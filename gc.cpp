#include "gca/gc.h"

#include <algorithm>
#include <list>
#include <memory_resource>
#include <unordered_map>

#include <gca/gc-util.h>

struct object_type {
    size_t size, alignment;
    std::type_index type;
    gc::destructor_fn destructor;
    gc::get_field_count_fn getFieldCount;
    gc::get_field_fn getField;
    gc::move_fn move;

    bool operator==(const object_type &other) const noexcept {
        return destructor == other.destructor
        && move == other.move
        && getFieldCount == other.getFieldCount
        && getField == other.getField
        && type == other.type;
    }
};

namespace gc {
    using type_index = config::object_type_index_underlying_type;
}

using page_memory = std::array<std::byte, gc::config::page_size>;

struct page {
    page_memory *memory{nullptr};
    gc::mutex allocationLock{}; // allocated objects should be accessible safely without this
    struct allocation {
        using size_type = gc::smallest_unsigned_numeric_type_needed_for_t<gc::config::page_size>;
        size_type offset{0};
        size_type size{0};

        [[nodiscard]] constexpr bool Equals(const allocation &other) const noexcept {
            return offset == other.offset && size == other.size;
        }

        constexpr bool operator==(const allocation &other) const noexcept {
            return Equals(other);
        }

        constexpr bool operator!=(const allocation &other) const noexcept {
            return !Equals(other);
        }
    };
    std::pmr::vector<allocation> allocations;

    explicit page(std::pmr::memory_resource *allocationsBacking) : allocations(allocationsBacking) {}
};

using page_allocation = std::span<std::byte>;

static_assert(gc::config::gc_max_collection_thread_count <= 64, "garbage collection threads are only supported");

using thread_count = gc::smallest_unsigned_numeric_type_needed_for_t<gc::config::gc_max_collection_thread_count>;
using thread_mark_bitfield_type = gc::smallest_unsigned_numeric_type_needed_for_t<1 << (gc::config::gc_max_collection_thread_count - 1)>;
using atomic_mark_bitfield = gc::atomic_bit_set<thread_mark_bitfield_type>;

enum class object_flags : uint8_t {
    uninitialized         = 1 << 0,
    garbage               = 1 << 1,
    immovable             = 1 << 2,
    initialization_failed = 1 << 3,
};

template<>
struct gc::enum_flag_traits<object_flags> {
    using underlying_type = uint8_t;
    constexpr static bool is_flag = true;
    constexpr static uint8_t all_flags = 0b1111;
};

namespace gc {
    struct internal_handle {
        page_allocation objectAllocation{};
        std::pmr::vector<internal_handle*> referencedBy{};
        std::atomic<size_t> rootHandleCount{0};
        std::atomic<size_t> pinCount{0};
        type_index objectType{};
        atomic_mark_bitfield markBits{0};
        rw_lock objectLock{}; // only for field/allocation modifications
        atomic_bit_set<object_flags> flags;
    };
}

struct gc_impl {
    std::pmr::memory_resource *objectMemory = nullptr, *backingMemory = nullptr;
    std::pmr::vector<object_type> types{};
    gc::rw_lock typesLock{};
    std::pmr::list<page> pages{};
    gc::mutex pageAllocationLock{};
    gc::rw_lock pagesLock{};
    std::pmr::polymorphic_allocator<page_memory> pageAllocator{};
    gc::ptr_safe_container<gc::internal_handle> objectHandles{};
    std::pmr::unordered_map<const void*, gc::internal_handle*> allocationToHandleLookup{};
    gc::rw_lock allocationLookupLock{}; // for both the container and the lookup
    std::atomic<thread_count> gcCount{0};

    gc_impl() : gc_impl(gc::gc_init_args{std::pmr::null_memory_resource(), std::pmr::null_memory_resource()}) {}
    explicit gc_impl(const gc::gc_init_args &args)
    : objectMemory(args.objectMemory)
    , backingMemory(args.backingMemory)
    , types(backingMemory)
    , pages(backingMemory)
    , pageAllocator(objectMemory)
    , objectHandles(backingMemory)
    , allocationToHandleLookup(backingMemory) {}

    gc::internal_handle *Allocate(const object_type &type, const size_t count) noexcept(false) {
        // FIXME: handle large objects

        std::pmr::list<page>::iterator begin{}, end{};

        {
            gc::scoped_rw_lock lock(pagesLock);
            begin = pages.begin();
            end = pages.end();
        }

        for (auto it = begin; it != end; ++it) {
            page &page = *it;

            gc::scoped_mutex_lock lock(page.allocationLock, gc::scoped_mutex_lock::mode::try_only, gc::config::page_acquire_attempts);
            if (!lock.Acquired()) {
                continue;
            }

            page_allocation allocation = TryAllocateOnPage(page, type.size * count, type.alignment);
            if (!allocation.empty()) {
                try {
                    return RegisterObject(&page, allocation, count, GetOrRegisterType(type));
                } catch (std::exception&) {
                    RemoveAllocation(page, allocation);
                }
            }

            CollectOnPage(page);
            allocation = TryAllocateOnPage(page, type.size * count, type.alignment);

            if (!allocation.empty()) {
                try {
                    return RegisterObject(&page, allocation, count, GetOrRegisterType(type));
                } catch (std::exception&) {
                    RemoveAllocation(page, allocation);
                }
            }

            DefragmentOnPage(page);
            allocation = TryAllocateOnPage(page, type.size * count, type.alignment);

            if (!allocation.empty()) {
                try {
                    return RegisterObject(&page, allocation, count, GetOrRegisterType(type));
                } catch (std::exception&) {
                    RemoveAllocation(page, allocation);
                }
            }
        }

        return TryAllocateOnNewPage(type, count);
    }

    void DefragmentOnPage(page &page) noexcept(false) {
        // assume write access to page
        const auto begin = reinterpret_cast<uintptr_t>(page.allocations.data());
        auto lastAllocationEnd = begin;

        for (size_t i = 0; i < page.allocations.size(); ++i) {
            auto &allocation = page.allocations[i];
            gc::internal_handle *handle = GetHandleForObjectAllocation(reinterpret_cast<void *>(begin + allocation.offset));

            if (!handle->flags.HasFlag(object_flags::immovable)) {
                const object_type *type = nullptr;
                {
                    gc::scoped_rw_lock lock(typesLock);
                    type = &types[handle->objectType];
                }

                const auto alignment = type->alignment;
                const auto size = type->size;

                const auto padding = alignment - lastAllocationEnd % alignment;

                if (lastAllocationEnd + padding + size < allocation.offset) {
                    // we have exclusive write access, so no pin could exist
                    gc::scoped_rw_lock lock(handle->objectLock, gc::scoped_rw_lock::mode::try_rw, 1);
                    // not gonna wait until it's released
                    if (lock.Acquired()) {
                        RelocateObject(page, handle, type, allocation, reinterpret_cast<void *>(lastAllocationEnd + padding));
                    }

                }
            }

            lastAllocationEnd = begin + allocation.offset + allocation.size; // allocation got modified by RelocateObject
        }
    }

    static void RemoveAllocation(page &page, const page_allocation &allocation) noexcept(true) {
        // assume rw access
        auto it = std::ranges::find(
            page.allocations,
            page::allocation{
                static_cast<page::allocation::size_type>(reinterpret_cast<uintptr_t>(allocation.data()) - reinterpret_cast<uintptr_t>(page.memory->data())),
                static_cast<page::allocation::size_type>(allocation.size())
            });

        if (it == page.allocations.end()) {
            return;
        }

        page.allocations.erase(it);
    }

    void RelocateObject(
        page &page,
        gc::internal_handle *handle,
        const object_type *type,
        page::allocation &allocation,
        void *newLocation
    ) noexcept(false) {
        // assume write access to handle
        gc::scoped_rw_lock lock(allocationLookupLock, gc::scoped_rw_lock::mode::rw);
        allocationToHandleLookup.insert(std::make_pair(newLocation, handle));

        const auto begin = reinterpret_cast<uintptr_t>(handle->objectAllocation.data());
        const auto end = begin + handle->objectAllocation.size();
        for (
            auto obj = begin, newLoc = reinterpret_cast<uintptr_t>(newLocation);
            obj < end;
            obj += type->size, newLoc += type->size) {
            type->move(reinterpret_cast<void *>(obj), reinterpret_cast<void*>(newLoc));
        }
        handle->objectAllocation = { static_cast<std::byte *>(newLocation), allocation.size };
        allocation.offset = reinterpret_cast<uintptr_t>(newLocation) - reinterpret_cast<uintptr_t>(page.allocations.data());

        allocationToHandleLookup.erase(handle->objectAllocation.data());
    }

    static constexpr void MarkGarbage(gc::internal_handle *handle) noexcept(true) {
        handle->flags.SetFlag(object_flags::garbage, true);
    }

    static constexpr bool IsMarkedGarbage(const gc::internal_handle *handle) noexcept(true) {
        return handle->flags.HasFlag(object_flags::garbage);
    }

    static void SetField(gc::internal_handle *obj, gc::internal_handle **field, gc::internal_handle *newValue) noexcept(false) {
        if (field == nullptr) {
            return;
        }

        // assume having rw pin access to obj
        gc::internal_handle *oldValue = *field;

        if (oldValue == newValue) {
            return;
        }

        if (newValue != nullptr) {
            gc::scoped_rw_lock lock(newValue->objectLock, gc::scoped_rw_lock::mode::rw);
            newValue->referencedBy.emplace_back(obj);
        }

        if (oldValue != nullptr) {
            gc::scoped_rw_lock lock(oldValue->objectLock, gc::scoped_rw_lock::mode::rw);
            auto it = std::ranges::find(oldValue->referencedBy, obj);

            if (it == oldValue->referencedBy.end()) {
                throw gc::library_bug("oldValue wasn't aware of being referenced by obj");
            }

            std::swap(*it, oldValue->referencedBy.back());
            oldValue->referencedBy.pop_back();
        }

        *field = newValue;

        // assuming that obj is alive (since we are accessing its fields)
        // can old value become garbage when it's not supposed to?
        // no, it will be still referenced by other objects if it shouldn't become garbage
        // can new value become garbage when it is not supposed to?
        // no, new value either comes from a root handle (whether pinned or not is irrelevant)
        // or from a field of an object that is most definitely alive,
        // since we need to be able to reach the object to access the field

        // foo.field = new bar()
        // bar not yet scanned
        // foo scan finished
        // bar gets assigned to foo.field
        // bar root handle destroyed (new returns a root handle)
        // bar gets scanned: referenced by foo
        // conclusion: the scanner will see the bar object in one of the 2 possible states
        // 1: has at least 1 root handle
        // 2: is referenced by a reachable object (foo in this case) (assume alive)
        // when 2 happens, the referenced by objects get rescanned
        // (the mark bit only prevents recursion, not repeating the same computation, which works for us here)
    }

    // 0 attempts means try until you succeed
    static bool TryRoPin(gc::internal_handle *handle, const size_t attempts = 0) noexcept(true) {
        if (attempts == 0) {
            handle->objectLock.AcquireRead();
            ++handle->pinCount;
            return true;
        }

        auto acquired = attempts == 1 ? handle->objectLock.TryAcquireRead() : handle->objectLock.TryAcquireRead(attempts);
        if (!acquired) {
            return false;
        }

        ++handle->pinCount;
        return true;
    }

    static bool TryRwPin(gc::internal_handle *handle, const size_t attempts = 0) noexcept(true) {
        if (attempts == 0) {
            handle->objectLock.AcquireWrite();
            ++handle->pinCount;
            return true;
        }

        auto acquired = attempts == 1 ? handle->objectLock.TryAcquireWrite() : handle->objectLock.TryAcquireWrite(attempts);
        if (!acquired) {
            return false;
        }

        ++handle->pinCount;
        return true;
    }

    static void Unpin(gc::internal_handle *handle) noexcept(true) {
        handle->objectLock.Release();
        --handle->pinCount;
    }

    static void PinUpgrade(gc::internal_handle *handle) noexcept(true) {
        handle->objectLock.Upgrade();
    }

    void CollectOnPage(page &page) noexcept(false) {
        thread_count id;
        while ((id = gcCount++) >= gc::config::gc_max_collection_thread_count) {
            --gcCount;
        }

        // assume having acquired the page mutex
        for (size_t i = 0; i < page.allocations.size(); ++i)
        {
            const auto &allocation = page.allocations[i];
            auto handle = GetHandleForObjectAllocation(page.allocations.data() + allocation.offset);

            // alive if acquisition fails
            if (!TryRoPin(handle, 1)) {
                continue; // 1 attempt should be able to determine that
            }

            if (IsMarkedGarbage(handle) || !TryFindRootFor(handle, id)) {
                PinUpgrade(handle);
                try {
                    // don't unpin after this: the handle is no longer valid
                    DestroyObject(page, handle);
                } catch (std::exception&) {
                    Unpin(handle);
                    throw;
                }
                --i; // destroying the object will remove the allocation entry from the page
            }
            else {
                Unpin(handle);
            }
        }
    }

    void DestroyObject(page &page, gc::internal_handle *handle) noexcept(false) {
        // assume rw access for both the handle and the page

        if (page.memory->data() > handle->objectAllocation.data()
            || page.memory->data() + page.memory->size() < handle->objectAllocation.data()) {
            throw gc::library_bug("object not allocated on this page");
        }

        {
            gc::scoped_rw_lock lock(allocationLookupLock, gc::scoped_rw_lock::mode::rw);
            if (!allocationToHandleLookup.erase(handle->objectAllocation.data())) {
                throw gc::library_bug("unregistered object");
            }
        }

        const object_type *type = nullptr;
        {
            gc::scoped_rw_lock lock(typesLock);
            type = &types[handle->objectType];
        }

        for (auto obj = reinterpret_cast<uintptr_t>(handle->objectAllocation.data());
            obj < reinterpret_cast<uintptr_t>(handle->objectAllocation.data() + handle->objectAllocation.size());
            obj += type->size)
        {
            const auto fieldCount = type->getFieldCount(reinterpret_cast<const void *>(obj));
            for (size_t i = 0; i < fieldCount; ++i) {
                // better than dangling references
                SetField(handle, type->getField(reinterpret_cast<void *>(obj), i), nullptr);
            }

            type->destructor(reinterpret_cast<void *>(obj));
        }

        RemoveAllocation(page, handle->objectAllocation);

        objectHandles.Remove(handle);
    }

    bool TryFindRootFor(gc::internal_handle *handle, thread_count id) noexcept(true) {
        // assume ro pin access to handle
        if (handle->markBits.ReadBit(id)) {
            // already tested this and already got false
            return false;
        }

        handle->markBits.SetBit(id, true);

        const auto found = std::ranges::any_of(handle->referencedBy,[id, this](gc::internal_handle *referencedBy) {
            if (!TryRoPin(referencedBy, 1)) {
                return true;
            }

            const auto found = TryFindRootFor(referencedBy, id);
            if (!found) {
                MarkGarbage(referencedBy); // might not be on this page, so just mark without collecting
            }

            Unpin(referencedBy);

            // found a chain of references that lead to a root
            return found;
        });

        handle->markBits.SetBit(id, false);
        // this should be fine
        // scenario:
        // foo.bar = bar
        // bar.foo = foo
        // foo gets scanned and only after that will the flag get unset
        // if bar (which was already scanned alongside foo) were to be scanned again,
        // the bit would be set again, and unset after the scan
        // so no infinite recursion here either (only doing the same computation again)

        return found;
    }

    gc::internal_handle *GetHandleForObjectAllocation(const void *objAllocation) noexcept(false) {
        gc::scoped_rw_lock lock(allocationLookupLock);

        auto it = allocationToHandleLookup.find(objAllocation);
        if (it == allocationToHandleLookup.end()) {
            // should be a library bug as this function should only be called internally
            throw gc::library_bug("unregistered/invalid allocation");
        }

        return it->second;
    }

    gc::internal_handle *RegisterObject(page *page, page_allocation allocation, size_t count, gc::type_index type) noexcept(false) {
        std::ranges::fill(allocation, static_cast<std::byte>(0));

        gc::scoped_rw_lock lock(allocationLookupLock, gc::scoped_rw_lock::mode::rw);

        const auto [it, success] = allocationToHandleLookup.insert(std::make_pair(allocation.data(), nullptr));

        if (!success) {
            throw gc::library_bug("dead handle remained in lookup");
        }

        gc::internal_handle *handle = nullptr;
        try {
            handle = &objectHandles.GetNextUninitialized();

            std::construct_at(handle);
            handle->objectAllocation = allocation;
            handle->referencedBy = std::pmr::vector<gc::internal_handle*>(backingMemory);
            handle->objectType = type;
            handle->rootHandleCount = 1;
            handle->flags.ClearAll();
            handle->flags.SetFlag(object_flags::uninitialized, true);

            {
                gc::scoped_rw_lock lock(typesLock);
                if (types[type].move == nullptr) {
                    handle->flags.SetFlag(object_flags::immovable, true);
                }
            }

            it->second = handle;

            return handle;
        } catch (std::exception&) {
            allocationToHandleLookup.erase(allocation.data());
            objectHandles.Remove(handle);
            throw;
        }
    }

    static page_allocation TryAllocateOnPage(page &page, const size_t size, const size_t alignment) noexcept(false) {
        const auto begin = std::bit_cast<uintptr_t>(page.memory->data());
        auto lastAllocationEnd = begin;

        for (size_t i = 0; i < page.allocations.size(); ++i) {
            const auto &allocation = page.allocations[i];
            const auto allocationBegin = begin + allocation.offset;
            const auto gap = allocationBegin - lastAllocationEnd;
            const auto padding = alignment - lastAllocationEnd % alignment;

            if (gap >= size + padding) {
                if (i > std::numeric_limits<ptrdiff_t>::max()) [[unlikely]] {
                    break;
                }

                page.allocations.insert(page.allocations.begin() + static_cast<ptrdiff_t>(i),
                    page::allocation{
                        static_cast<page::allocation::size_type>(allocation.offset + allocation.size + padding),
                        static_cast<page::allocation::size_type>(size)
                    });

                return {reinterpret_cast<std::byte *>(lastAllocationEnd + padding), size};
            }

            lastAllocationEnd = allocationBegin + allocation.size;
        }

        const auto end = reinterpret_cast<uintptr_t>(page.memory->data()) + page.memory->size();
        const auto padding = alignment - lastAllocationEnd % alignment;
        const auto gap = end - lastAllocationEnd;

        if (gap >= size + padding) {
            page.allocations.emplace_back(
                static_cast<page::allocation::size_type>(lastAllocationEnd - begin + padding),
                static_cast<page::allocation::size_type>(size));

            return {reinterpret_cast<std::byte *>(lastAllocationEnd + padding), size};
        }

        return {};
    }

    gc::internal_handle *TryAllocateOnNewPage(const object_type &type, const size_t count) noexcept(false) {
        page &page = NewPage(true);
        gc::defer lockRelease ([&]{ page.allocationLock.Release(); });

        page_allocation allocation = TryAllocateOnPage(page, type.size * count, type.alignment);
        if (!allocation.empty()) {
            try {
                return RegisterObject(&page, allocation, count, GetOrRegisterType(type));
            } catch (std::exception&) {
                RemoveAllocation(page, allocation);
                throw;
            }
        }

        return nullptr;
    }

    gc::type_index GetOrRegisterType(const object_type &type) noexcept(false) {
        gc::scoped_rw_lock lock(typesLock);
        auto found = std::ranges::find(types, type);
        if (found == types.end()) {
            lock.Upgrade();
            // multiple threads might have tried to add the same type
            found = std::ranges::find(types, type);
            if (found == types.end()) {
                types.emplace_back(type);
                return types.size() - 1;
            }
        }

        const auto index = std::distance(types.begin(), found);
        if (index > std::numeric_limits<gc::type_index>::max()) {
            throw std::out_of_range("index for type exceeds tha maximum value that the gc::type_index can hold");
        }

        return static_cast<gc::type_index>(index);
    }

    page &NewPage(bool acquireLock = false) noexcept(false) {
        gc::scoped_mutex_lock allocationLock(pageAllocationLock);
        gc::scoped_rw_lock pageContainerLock(pagesLock, gc::scoped_rw_lock::mode::rw);

        auto &page = pages.emplace_back(backingMemory);
        try {
            InitPage(page);
        } catch (std::exception&) {
            pages.pop_back();
            throw;
        }

        if (acquireLock) {
            page.allocationLock.Acquire();
        }

        return page;
    }

    void InitPage(page &page) noexcept(false) {
        page.memory = pageAllocator.allocate(1);
        pageAllocator.construct(page.memory);
        page.memory->fill(static_cast<std::byte>(0));
    }

    void DestroyPage(page &page) noexcept(true) {
        // assume rw access
        page.memory->~page_memory();
        pageAllocator.deallocate(page.memory, 1);
    }

    ~gc_impl() noexcept(false) {
        for (page &page : pages) {
            gc::scoped_mutex_lock lock(page.allocationLock);

            const auto begin = reinterpret_cast<uintptr_t>(page.memory->data());
            while (!page.allocations.empty()) {
                const auto &allocation = page.allocations.back();
                gc::internal_handle *handle = GetHandleForObjectAllocation(reinterpret_cast<const void *>(begin + allocation.offset));
                TryRwPin(handle); // not just trying in this call,as misleading as it may be
                DestroyObject(page, handle);
            }

            DestroyPage(page);
        }
    }
};

size_t initCount = 0; // TODO: maybe debug assertions for being initialized (in debug mode only)
// don't want the constructor to run before Init is called
// and don't want to use new/delete
alignas(alignof(gc_impl)) std::byte data[sizeof(gc_impl)];
gc_impl *impl = reinterpret_cast<gc_impl *>(&data[0]);

namespace gc {
    bool Init(const gc_init_args *args) {
        if (initCount++ == 0) {
            if (args) {
                std::construct_at(impl, *args);
                return true;
            }

            std::construct_at(impl);
            return true;
        }

        return false;
    }

    void Destroy() {
        if (--initCount == 0) {
            // NOTE: collections, allocation and defragmentation operations might still be ongoing,
            //       and it's not my job to ensure that there aren't any
            std::destroy_at(impl);
        }
    }

    // misleading name: each object has only 1 handle to it;
    // we are just simulating having multiple handles with different roles
    handle_t *Copy(handle_t *src, handle_role srcRole, handle_role dstRole) {
        switch (srcRole) {
            case handle_role::unknown:
                if (dstRole != handle_role::root) {
                    throw bad_api_usage("this operation is only meant to be used for creating a root handle from a newly allocated handle");
                }
                if (src->rootHandleCount != 1) {
                    throw library_bug("header should have been just initialized to have 1 root reference");
                }
                return src;
            case handle_role::root:
                [[fallthrough]];
            case handle_role::field:
                switch (dstRole) {
                case handle_role::ro_pin:
                        src->objectLock.AcquireRead();
                        ++src->pinCount;
                        ++src->rootHandleCount;
                        break;
                case handle_role::rw_pin:
                        src->objectLock.AcquireWrite();
                        ++src->pinCount;
                        ++src->rootHandleCount;
                        break;
                case handle_role::root:
                        ++src->rootHandleCount;
                        break;
                default:
                        break;
                }
                return src;
            case handle_role::ro_pin:
                [[fallthrough]];
            case handle_role::rw_pin:
                switch (dstRole) {
                    case handle_role::root:
                        ++src->rootHandleCount;
                        break;
                    case handle_role::ro_pin:
                        [[fallthrough]];
                    case handle_role::rw_pin:
                        // cannot copy pins
                        // double free, either multiple read or only 1 write access can exist at a time, ...etc
                        throw bad_api_usage("invalid copy operation");
                        default:
                        break;
                }
                return src;
            default:
                return src;
        }
    }

    bool Equals(const handle_t *lhs, const handle_t *rhs) {
        return lhs == rhs;
    }

    void Destroy(handle_t *handle, handle_role role) {
        switch (role) {
            case handle_role::ro_pin:
                [[fallthrough]];
            case handle_role::rw_pin:
                handle->objectLock.Release();
                --handle->pinCount;
                [[fallthrough]];
            case handle_role::root:
                // would be problem: object is alive, but has 0 reachable references (referenced by) and isn't a root
                // but a reachable object is always in either of the 2 states:
                // 1. is root
                // is a field of a reachable object
                // and (atomic) transitions between being a field and a root ensure a proper order
                // where this invariance is not broken
                // root -> field:
                // gets added as a field before the root handle is destroyed
                --handle->rootHandleCount;
                break;
            default:
                break;
        }
    }
    void *GetInstance(const handle_t *handle, const handle_role role) {
        if (role != handle_role::ro_pin && role != handle_role::rw_pin) {
            throw bad_api_usage("instance can only be accessed from pinned handles");
        }

        return handle->objectAllocation.data();
    }

    void Collect(const bool defragment) {
        std::pmr::list<page>::iterator begin{}, end{};

        {
            scoped_rw_lock lock(impl->pagesLock);
            begin = impl->pages.begin();
            end = impl->pages.end();
        }

        for (auto it = begin; it != end; ++it) {
            scoped_mutex_lock lock(it->allocationLock);
            impl->CollectOnPage(*it);
            if (defragment) {
                impl->DefragmentOnPage(*it);
            }
        }
    }

    handle_t *New(
        const size_t size,
        const size_t alignment,
        const size_t count,
        const std::type_index type,
        const destructor_fn destructor,
        const get_field_count_fn getFieldCount,
        const get_field_fn getField,
        const move_fn move
    ) {
        return impl->Allocate(object_type{ size, alignment, type, destructor, getFieldCount, getField, move }, count);
    }
    bool TryAcquireInitializeRight(handle_t *handle) {
        return handle->flags.SetFlag(object_flags::uninitialized, false);
    }
    void SetInitializationFailed(handle_t *handle) {
        handle->flags.SetFlag(object_flags::initialization_failed, true);
    }
}