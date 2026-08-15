#include "gca/gc-internal.h"

#include <algorithm>
#include <memory_resource>
#include <ranges>
#include <thread>

namespace gc {
    gc_impl::gc_impl() noexcept : gc_impl(gc_init_args{}) {}
    gc_impl::gc_impl(const gc_init_args &args)
    : allocator(args.allocator)
    , types(allocator->CreateTypesVector())
    , pages(allocator->CreatePagesList())
    , pageAllocator(allocator->CreatePageMemoryAllocator())
    , objectHandles(allocator->CreateObjectHandlesContainer())
    , allocationToHandleLookup(allocator->CreateAllocationToHandleLookup()) { }

    internal_handle *gc_impl::Allocate(const object_type &type, const size_t count) noexcept(false) {
        if constexpr (config::enable_debug_messages) {
            debugListeners.onAllocation(this, type, count);
        }

        // FIXME: handle large objects
        if (type.size * count > config::page_size) {
            throw bad_api_usage("large objects are not supported yet");
        }

        std::pmr::list<page>::iterator begin{}, end{};

        {
            if constexpr (config::enable_debug_messages) {
                debugListeners.onBeforePagesQuery(this);
            }
            scoped_rw_lock lock(pagesLock);
            begin = pages.begin();
            end = pages.end();
            if constexpr (config::enable_debug_messages) {
                debugListeners.onPagesQueryFinished(this, begin, end);
            }
        }

        for (auto it = begin; it != end; ++it) {
            page &page = *it;

            if constexpr (config::enable_debug_messages) {
                debugListeners.onBeforePageLockAcquire(this);
            }
            scoped_mutex_lock lock(page.allocationLock, scoped_mutex_lock::mode::try_only, config::page_acquire_attempts);
            if (!lock.Acquired()) {
                continue;
            }

            page_allocation allocation = page.TryAllocate(type.size * count, type.alignment);
            if constexpr (config::enable_debug_messages) {
                debugListeners.onPageAllocationAttempt(this, type.size * count, type.alignment, allocation);
            }

            if (allocation.empty()) {
                CollectOnPage(page);
                allocation = page.TryAllocate(type.size * count, type.alignment);
                if constexpr (config::enable_debug_messages) {
                    debugListeners.onPageAllocationAttempt(this, type.size * count, type.alignment, allocation);
                }
            }

            if (allocation.empty()) {
                DefragmentOnPage(page);
                allocation = page.TryAllocate(type.size * count, type.alignment);
                if constexpr (config::enable_debug_messages) {
                    debugListeners.onPageAllocationAttempt(this, type.size * count, type.alignment, allocation);
                }
            }

            if (allocation.empty()) {
                continue;
            }

            try {
                return RegisterObject(allocation, GetOrRegisterType(type));
            } catch (...) {
                page.RemoveAllocation(allocation);
                if constexpr (config::enable_debug_messages) {
                    debugListeners.onObjectRegisterFailed(this, allocation);
                }
                throw;
            }
        }

        return TryAllocateOnNewPage(type, count);
    }

    void gc_impl::DefragmentOnPage(page &page) noexcept(false) {
        if constexpr (config::enable_debug_messages) {
            debugListeners.onDefragmentStarted(this, page);
        }

        // assume write access to page
        const auto begin = reinterpret_cast<uintptr_t>(page.memory->data());
        auto lastAllocationEnd = begin;

        for (size_t i = 0; i < page.allocations.size(); ++i) {
            auto &allocation = page.allocations[i];
            internal_handle *handle = GetHandleForObjectAllocation(reinterpret_cast<void *>(begin + allocation.offset));

            if (!handle->flags.HasFlag(object_flags::immovable)) {
                const object_type *type = nullptr;
                {
                    if constexpr (config::enable_debug_messages) {
                        debugListeners.onBeforeDefragmentPageTypesROLockAcquire(this, handle);
                    }
                    scoped_rw_lock lock(typesLock);
                    type = &types[handle->objectType];
                }

                const auto alignment = type->alignment;
                const auto size = type->size;

                const auto padding = page::GetAlignmentCorrection(lastAllocationEnd, alignment);
                const auto potentialNewLocation = lastAllocationEnd + padding;
                const auto gap = reinterpret_cast<uintptr_t>(handle->objectAllocation.data()) - potentialNewLocation;

                if (gap >= size) {
                    // we have exclusive write access, so no read access that would point to the wrong address
                    // after this could exist
                    if constexpr (config::enable_debug_messages) {
                        debugListeners.onBeforeDefragmentPageObjectRWLockTryAcquire(this, handle);
                    }
                    scoped_rw_lock lock(handle->objectLock, scoped_rw_lock::mode::try_rw, 1);
                    // not gonna wait until it's released
                    if (lock.Acquired()) {
                        RelocateObject(page, handle, type, allocation, reinterpret_cast<void *>(potentialNewLocation));
                    }

                }
            }

            lastAllocationEnd = begin + allocation.offset + allocation.size; // allocation got modified by RelocateObject
        }

        if constexpr (config::enable_debug_messages) {
            debugListeners.onDefragmentFinished(this, page);
        }
    }

    void page::RemoveAllocation(const page_allocation &allocation) noexcept(true) {
        // assume rw access
        auto it = std::ranges::find(
            allocations,
            page::allocation{
                static_cast<allocation::size_type>(reinterpret_cast<uintptr_t>(allocation.data()) - reinterpret_cast<uintptr_t>(memory->data())),
                static_cast<allocation::size_type>(allocation.size())
            });

        if (it == allocations.end()) {
            return;
        }

        allocations.erase(it);
    }

    void gc_impl::RelocateObject(
        const page &page,
        internal_handle *handle,
        const object_type *type,
        page::allocation &allocation,
        void *newLocation
    ) noexcept(false) {
        if constexpr (config::enable_debug_messages) {
            debugListeners.onBeforeDefragmentObjectRelocate(this, handle, newLocation);
        }
        // assume write access to handle
        if constexpr (config::enable_debug_messages) {
            debugListeners.onBeforeDefragmentPageLookupRWAcquire(this, handle);
        }
        {
            // we have the rw lock, so no one else should access the allocation
            // so we can update the lookup before moving the objects
            scoped_rw_lock lock(allocationLookupLock, scoped_rw_lock::mode::rw);
            allocationToHandleLookup.erase(handle->objectAllocation.data());
            allocationToHandleLookup.insert(std::make_pair(newLocation, handle));
        }

        const auto begin = reinterpret_cast<uintptr_t>(handle->objectAllocation.data());
        const auto newBegin = reinterpret_cast<uintptr_t>(newLocation);
        const auto count = handle->objectAllocation.size() / type->size;
        if (!handle->flags.HasFlag(object_flags::uninitialized | object_flags::initialization_failed)) {
            for (size_t i = 0; i < count; ++i) {
                const auto offset = i * type->size;
                type->move(
                    reinterpret_cast<void*>(begin + offset),
                    reinterpret_cast<void*>(newBegin + offset)
                );
            }
        }

        handle->objectAllocation = { static_cast<std::byte *>(newLocation), allocation.size };
        allocation.offset = static_cast<page::allocation::size_type>(reinterpret_cast<uintptr_t>(newLocation) - reinterpret_cast<uintptr_t>(page.memory->data()));
    }

    void internal_handle::SetField(internal_handle **field, internal_handle *newValue) noexcept(false) {
        if (field == nullptr) {
            return;
        }

        // assume having rw access to this (the instance that SetField was called on)
        internal_handle *oldValue = *field;

        if (oldValue == newValue) {
            return;
        }

        if (newValue != nullptr) {
            // someone is trying to assign a new value to a field in the destructor
            // setting to null is allowed because that's what we do when destroying the object
            if (this->flags.HasFlag(object_flags::garbage)) {
                throw bad_api_usage("cannot set the fields of garbage objects to non-null values");
            }

            const auto hasNoRWAccessToNewValue = !newValue->objectLock.DoesThisThreadHaveRWAccess();
            if (hasNoRWAccessToNewValue) { newValue->objectLock.AcquireWrite(); }
            defer release([=]{ if (hasNoRWAccessToNewValue) { newValue->objectLock.Release(); } });
            newValue->referencedBy.emplace_back(this);
        }


        if (oldValue != nullptr) {
            const auto hasNoRWAccessToOldValue = !oldValue->objectLock.DoesThisThreadHaveRWAccess();
            if (hasNoRWAccessToOldValue) { oldValue->objectLock.AcquireWrite(); }
            defer release([=]{ if (hasNoRWAccessToOldValue) { oldValue->objectLock.Release(); } });

            auto it = std::ranges::find(oldValue->referencedBy, this);

            if (it == oldValue->referencedBy.end()) {
                throw library_bug("oldValue wasn't aware of being referenced by obj");
            }

            std::swap(*it, oldValue->referencedBy.back());
            oldValue->referencedBy.pop_back();

            oldValue->objectLock.Release();
        }

        *field = newValue;

        // assuming that obj is alive (since we are accessing its fields)
        // can old value become garbage when it's not supposed to?
        // no, it will be still referenced by other objects if it shouldn't become garbage
        // can new value become garbage when it is not supposed to?
        // no, new value either comes from a root handle
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

    void gc_impl::CollectOnPage(page &page) noexcept(false) {
        if constexpr (config::enable_debug_messages) {
            debugListeners.onBeforeCollectionStart(this, page);
        }

        thread_count id;
        while ((id = gcCount++) >= config::gc_max_collection_thread_count) {
            --gcCount;
        }

        if constexpr (config::enable_debug_messages) {
            debugListeners.onCollectionStart(this, page);
        }

        // assume having acquired the page mutex
        for (size_t i = 0; i < page.allocations.size(); ++i)
        {
            const auto &allocation = page.allocations[i];
            auto handle = GetHandleForObjectAllocation(page.memory->data() + allocation.offset);

            if (handle->rootHandleCount > 0) {
                continue;
            }

            // alive if acquisition fails
            if constexpr (config::enable_debug_messages) {
                debugListeners.onBeforeCollectionROLockAcquire(this, handle);
            }
            if (!handle->objectLock.TryAcquireRead(1)) {
                continue; // 1 attempt should be able to determine that
            }

            if (IsMarkedGarbage(handle) || !TryFindRootFor(handle, id)) {
                if constexpr (config::enable_debug_messages) {
                    debugListeners.onBeforeCollectionROLockUpgrade(this, handle);
                }
                // handle->objectLock.Upgrade(); // redundant but just to be safe
                if (!handle->objectLock.TryUpgrade()) {
                    // this could mean 2 things
                    // 1. forgot to release a lock (assume that non-library code does not mess with internal handles
                    //    directly and only uses the exposed functions to manipulate them)
                    // 2. this object is not dead
                    throw library_bug("unreleased lock(s) to dead object");
                }
                try {
                    // don't release the lock after this: the handle is no longer valid
                    DestroyObject(page, handle);
                } catch (...) {
                    if constexpr (config::enable_debug_messages) {
                        debugListeners.onObjectDestroyFailed(this, handle);
                    }
                    handle->objectLock.Release();
                    throw;
                }
                --i; // destroying the object will remove the allocation entry from the page
            }
            else {
                handle->objectLock.Release();
            }
        }

        if constexpr (config::enable_debug_messages) {
            debugListeners.onCollectionFinished(this, page);
        }
    }

    void gc_impl::DestroyObject(page &page, internal_handle *handle) noexcept(false) {
        // assume rw access for both the handle and the page
        MarkGarbage(handle); // in case, it wasn't already

        if (page.memory->data() > handle->objectAllocation.data()
            || page.memory->data() + page.memory->size() < handle->objectAllocation.data()) {
            throw library_bug("object not allocated on this page");
            }

        {
            if constexpr (config::enable_debug_messages) {
                debugListeners.onBeforeDestroyObjectAllocationLookupRWLockAcquire(this, handle);
            }
            scoped_rw_lock lock(allocationLookupLock, scoped_rw_lock::mode::rw);
            if (!allocationToHandleLookup.erase(handle->objectAllocation.data())) {
                throw library_bug("unregistered object");
            }
        }

        const object_type *type = nullptr;
        {
            if constexpr (config::enable_debug_messages) {
                debugListeners.onBeforeDestroyObjectTypesROLockAcquire(this, handle);
            }
            scoped_rw_lock lock(typesLock);
            type = &types[handle->objectType];
        }

        if constexpr (config::enable_debug_messages) {
            debugListeners.onBeforeObjectDestroyed(this, handle);
        }

        if (!handle->flags.HasFlag(object_flags::uninitialized))
        {
            const auto count = handle->objectAllocation.size() / type->size;
            const auto begin = reinterpret_cast<uintptr_t>(handle->objectAllocation.data());
            const auto end = begin + handle->objectAllocation.size();

            for (size_t i = 0; i < count; ++i) {
                auto* const obj = reinterpret_cast<void *>(begin + i * type->size);
                const auto fieldCount = type->getFieldCount(obj);
                for (size_t j = 0; j < fieldCount; ++j) {
                    // better than dangling references
                    // possible deadlock
                    // we are scanning the old value of field (acquire ro access)
                    // SetField will acquire rw access for the object of field to remove this from its references
                    // set field waits for the ro lock to release
                    // meanwhile, the scanning will wait for this to release the rw lock
                    // so it can get a ro lock and scan it for possible root references

                    internal_handle **field = type->getField(obj, j);
                    if (field == nullptr) {
                        continue;
                    }

                    internal_handle *fieldObj = *field;
                    if (fieldObj == nullptr) {
                        continue;
                    }

                    // solution to the deadlock: temporarily release the lock for this handle
                    if constexpr (config::enable_debug_messages) {
                        debugListeners.onBeforeDestroyObjectFieldOldValueRWLockAcquire(this, handle, fieldObj, j);
                    }
                    while (!fieldObj->objectLock.TryAcquireWrite()) {
                        handle->objectLock.Release();
                        std::this_thread::yield();
                        handle->objectLock.AcquireWrite();
                        // in case the object got reallocated
                        // which shouldn't happen since a defragmentation can only run after the collection has finished
                        // which is still ongoing at this point

                        const auto newBegin = reinterpret_cast<uintptr_t>(handle->objectAllocation.data());
                        const auto newEnd = begin + handle->objectAllocation.size();

                        if (newBegin != begin || newEnd != end) {
                            throw library_bug("object got relocated while being destroyed");
                        }
                    }

                    try {
                        handle->SetField(type->getField(obj, j), nullptr);
                        fieldObj->objectLock.Release();
                    } catch (...) {
                        fieldObj->objectLock.Release();
                        throw;
                    }
                }

                type->destructor(obj);
            }
        }

        page.RemoveAllocation(handle->objectAllocation);

        objectHandles.Remove(handle);
        if constexpr (config::enable_debug_messages) {
            debugListeners.onAfterObjectDestroyed(this, handle);
        }
    }

    bool gc_impl::TryFindRootFor(internal_handle *handle, thread_count id) noexcept(true) {
        // assume ro access to handle
        if (handle->markBits.ReadBit(id)) {
            // already tested this and already got false
            return false;
        }

        // no need to find one as it is already a root
        if (handle->rootHandleCount > 0) {
            return true;
        }

        if (IsMarkedGarbage(handle)) {
            return false;
        }

        handle->markBits.SetBit(id, true);

        const auto found = std::ranges::any_of(handle->referencedBy,[id, this, handle](internal_handle *referencedBy) {
            if (referencedBy->rootHandleCount > 0) {
                return true;
            }

            if (IsMarkedGarbage(referencedBy)) {
                return false;
            }

            if constexpr (config::enable_debug_messages) {
                debugListeners.onBeforeFindRootReferencedByROLockAcquire(this, handle, referencedBy);
            }
            if (!referencedBy->objectLock.TryAcquireRead(1)) {
                return true; // just assume that the object is being used, not destroyed
            }

            const auto found = TryFindRootFor(referencedBy, id);
            if (!found) {
                MarkGarbage(referencedBy); // might not be on this page, so just mark without collecting
            }

            referencedBy->objectLock.Release();

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

    internal_handle *gc_impl::TryGetHandleForObjectAllocation(const void *objAllocation) noexcept(true) {
        if constexpr (config::enable_debug_messages) {
            debugListeners.onBeforeAllocationToHandleLookupROLockAcquire(this, objAllocation);
        }
        scoped_rw_lock lock(allocationLookupLock);

        auto it = allocationToHandleLookup.find(objAllocation);
        if (it == allocationToHandleLookup.end()) {
            return nullptr;
        }

        return it->second;
    }

    internal_handle *gc_impl::GetHandleForObjectAllocation(const void *objAllocation) noexcept(false) {
        internal_handle *handle = TryGetHandleForObjectAllocation(objAllocation);
        if (handle == nullptr) {
            // should be a library bug as this function should only be called internally for valid objects
            throw library_bug("unregistered/invalid allocation");
        }

        return handle;
    }

    internal_handle *gc_impl::RegisterObject(page_allocation allocation, const type_index type) noexcept(false) {
        std::ranges::fill(allocation, static_cast<std::byte>(0));

        if constexpr (config::enable_debug_messages) {
            debugListeners.onBeforeRegisterObjectLookupRWLockAcquire(this, allocation, type);
        }
        scoped_rw_lock lockLookup(allocationLookupLock, scoped_rw_lock::mode::rw);

        const auto [it, success] = allocationToHandleLookup.insert(std::make_pair(allocation.data(), nullptr));

        if (!success) {
            throw library_bug("dead handle remained in lookup");
        }

        internal_handle *handle = nullptr;
        try {
            handle = &objectHandles.GetNextUninitialized();

            std::construct_at(handle, allocation, allocator->CreateReferencedByVectorForHandle(), type);
            {
                if constexpr (config::enable_debug_messages) {
                    debugListeners.onBeforeRegisterObjectTypesROLockAcquire(this, allocation, type);
                }
                scoped_rw_lock lockTypes(typesLock);
                if (types[type].move == nullptr) {
                    handle->flags.SetFlag(object_flags::immovable, true);
                }
            }

            it->second = handle;
            if constexpr (config::enable_debug_messages) {
                debugListeners.onObjectRegistered(this, handle);
            }
            return handle;
        } catch (...) {
            allocationToHandleLookup.erase(allocation.data());
            objectHandles.Remove(handle);
            throw;
        }
    }

    page_allocation page::TryAllocate(const size_t size, const size_t alignment) noexcept(false) {
        const auto begin = std::bit_cast<uintptr_t>(memory->data());
        auto lastAllocationEnd = begin;

        for (size_t i = 0; i < allocations.size(); ++i) {
            const auto &allocation = allocations[i];
            const auto allocationBegin = begin + allocation.offset;
            const auto gap = allocationBegin - lastAllocationEnd;
            const auto padding = GetAlignmentCorrection(lastAllocationEnd, alignment);

            if (gap >= size + padding) {
                constexpr size_t max_value = std::numeric_limits<ptrdiff_t>::max();
                if (i > max_value) [[unlikely]] {
                    break;
                }

                allocations.insert(allocations.begin() + static_cast<ptrdiff_t>(i),
                    page::allocation{
                        static_cast<allocation::size_type>(allocation.offset + allocation.size + padding),
                        static_cast<allocation::size_type>(size)
                    });

                return {reinterpret_cast<std::byte *>(lastAllocationEnd + padding), size};
            }

            lastAllocationEnd = allocationBegin + allocation.size;
        }

        const auto end = reinterpret_cast<uintptr_t>(memory->data()) + memory->size();
        const auto padding = GetAlignmentCorrection(lastAllocationEnd, alignment);
        const auto gap = end - lastAllocationEnd;

        if (gap >= size + padding) {
            allocations.emplace_back(
                static_cast<allocation::size_type>(lastAllocationEnd - begin + padding),
                static_cast<allocation::size_type>(size));

            return {reinterpret_cast<std::byte *>(lastAllocationEnd + padding), size};
        }

        return {};
    }

    internal_handle *gc_impl::TryAllocateOnNewPage(const object_type &type, const size_t count) noexcept(false) {
        page &page = NewPage(true);
        defer lockRelease ([&]{ page.allocationLock.Release(); });

        page_allocation allocation = page.TryAllocate(type.size * count, type.alignment);
        if constexpr (config::enable_debug_messages) {
            debugListeners.onPageAllocationAttempt(this, type.size * count, type.alignment, allocation);
        }
        if (!allocation.empty()) {
            try {
                return RegisterObject(allocation, GetOrRegisterType(type));
            } catch (...) {
                page.RemoveAllocation(allocation);
                if constexpr (config::enable_debug_messages) {
                    debugListeners.onObjectRegisterFailed(this, allocation);
                }
                throw;
            }
        }

        if constexpr (config::enable_debug_messages) {
            debugListeners.onAllocateReturningNull(this);
        }
        // should get a std::bad_alloc before getting here
        Unreachable();
    }

    type_index gc_impl::GetOrRegisterType(const object_type &type) noexcept(false) {
        if constexpr (config::enable_debug_messages) {
            debugListeners.onBeforeRegisterTypeROLockAcquire(this, type);
        }
        scoped_rw_lock lock(typesLock);
        auto found = std::ranges::find(types, type);
        if (found == types.end()) {
            if constexpr (config::enable_debug_messages) {
                debugListeners.onBeforeRegisterTypeLockUpgrade(this, type);
            }
            lock.Upgrade();
            // multiple threads might have tried to add the same type
            found = std::ranges::find(types, type);
            if (found == types.end()) {
                if (types.size() > static_cast<size_t>(std::numeric_limits<type_index>::max()) + 1) {
                    throw too_many_types("the amount of types exceed the the max value of the indexing type");
                }

                types.emplace_back(type);
                return static_cast<type_index>(types.size() - 1);
            }
        }

        const auto index = std::distance(types.begin(), found);
        if (index > std::numeric_limits<type_index>::max()) {
            throw std::out_of_range("index for type exceeds tha maximum value that the gc::type_index can hold");
        }

        return static_cast<type_index>(index);
    }

    page &gc_impl::NewPage(const bool acquireLock) noexcept(false) {
        if constexpr (config::enable_debug_messages) {
            debugListeners.onBeforeNewPageAllocationLockAcquire(this);
        }
        scoped_mutex_lock allocationLock(pageAllocationLock);
        if constexpr (config::enable_debug_messages) {
            debugListeners.onBeforeNewPagePagesRWLockAcquire(this);
        }
        scoped_rw_lock pageContainerLock(pagesLock, scoped_rw_lock::mode::rw);

        auto &page = pages.emplace_back(allocator->CreatePageAllocationsVectorForPage());
        try {
            InitPage(page);
        } catch (...) {
            pages.pop_back();
            throw;
        }

        if (acquireLock) {
            page.allocationLock.Acquire();
        }

        if constexpr (config::enable_debug_messages) {
            debugListeners.onNewPageCreated(this, page, acquireLock);
        }
        return page;
    }

    void gc_impl::InitPage(page &page) noexcept(false) {
        page.memory = pageAllocator.allocate(1);
        pageAllocator.construct(page.memory);
    }

    void gc_impl::DestroyPage(page &page) noexcept(true) {
        if constexpr (config::enable_debug_messages) {
            debugListeners.onBeforePageDestroyed(this, page);
        }
        // assume rw access
        page.memory->~page_memory();
        pageAllocator.deallocate(page.memory, 1);
    }

    gc_impl::~gc_impl() noexcept(false) {
        // force destroying objects
        // remove all references and set all fields to null
        for (page &page : pages) {
            scoped_mutex_lock lockPage(page.allocationLock);

            const auto begin = reinterpret_cast<uintptr_t>(page.memory->data());
            for (auto allocation : page.allocations) {
                internal_handle *handle = GetHandleForObjectAllocation(reinterpret_cast<const void *>(begin + allocation.offset));

                if constexpr (config::enable_debug_messages) {
                    debugListeners.onBeforeDestructorObjectRWLockAcquire(this, handle);
                }

                scoped_rw_lock lockObject(handle->objectLock, scoped_rw_lock::mode::rw);

                const object_type *type = nullptr;
                {
                    scoped_rw_lock lockTypes(typesLock);
                    type = &types[handle->objectType];
                }

                const auto objectCount = handle->objectAllocation.size() / type->size;
                const auto allocationBegin = reinterpret_cast<uintptr_t>(handle->objectAllocation.data());
                for (size_t i = 0; i < objectCount; ++i) {
                    auto *const obj = reinterpret_cast<void *>(allocationBegin + i * type->size);

                    const auto fieldCount = type->getFieldCount(obj);
                    for (size_t j = 0; j < fieldCount; ++j) {
                        internal_handle **field = type->getField(obj, j);
                        if (field != nullptr) {
                            *field = nullptr;
                        }
                    }
                }

                handle->referencedBy.clear();
            }
        }

        for (page &page : pages) {
            scoped_mutex_lock lock(page.allocationLock);

            const auto begin = reinterpret_cast<uintptr_t>(page.memory->data());
            while (!page.allocations.empty()) {
                const auto &allocation = page.allocations.back();
                internal_handle *handle = GetHandleForObjectAllocation(reinterpret_cast<const void *>(begin + allocation.offset));
                if constexpr (config::enable_debug_messages) {
                    debugListeners.onBeforeDestructorObjectRWLockAcquire(this, handle);
                }
                handle->objectLock.AcquireWrite();
                DestroyObject(page, handle);
            }

            DestroyPage(page);
        }
    }

    struct non_owning_memory_resource_allocator final : gc_allocator {
        std::pmr::memory_resource *resource = std::pmr::null_memory_resource();
        non_owning_memory_resource_allocator(std::pmr::memory_resource *resource) : resource(resource) {
            if (resource == nullptr) {
                throw library_bug("resource is nullptr");
            }
        }

        std::pmr::vector<object_type> CreateTypesVector() noexcept override {
            return std::pmr::vector<object_type>(resource);
        }

        std::pmr::list<page> CreatePagesList() noexcept override {
            return std::pmr::list<page>(resource);
        }

        std::pmr::polymorphic_allocator<page_memory> CreatePageMemoryAllocator() noexcept override {
            return {resource};
        }

        ptr_safe_container<internal_handle> CreateObjectHandlesContainer() noexcept override {
            return {resource, resource};
        }

        std::pmr::unordered_map<const void *, internal_handle *> CreateAllocationToHandleLookup() noexcept override {
            return std::pmr::unordered_map<const void*, internal_handle*>(resource);
        }

        std::pmr::vector<page::allocation> CreatePageAllocationsVectorForPage() noexcept override {
            return std::pmr::vector<page::allocation>(resource);
        }

        std::pmr::vector<internal_handle *> CreateReferencedByVectorForHandle() noexcept override {
            return std::pmr::vector<internal_handle*>(resource);
        }

        ~non_owning_memory_resource_allocator() override = default;
    };

    allocator_handle_t *GetNullAllocator() {
        static non_owning_memory_resource_allocator allocator(std::pmr::null_memory_resource());
        return &allocator;
    }

    allocator_handle_t *GetDefaultAllocator() {
        static non_owning_memory_resource_allocator allocator(std::pmr::get_default_resource());
        return &allocator;
    }

    size_t initCount = 0; // TODO: maybe debug assertions for being initialized (in debug mode only)
    // don't want the constructor to run before Init is called
    // and don't want to use new/delete
    alignas(alignof(gc_impl)) std::byte data[sizeof(gc_impl)];
    gc_impl *impl = nullptr;

    bool Init(const gc_init_args *args) {
        if (initCount++ == 0) {
            if (args) {
                impl = new (data) gc_impl(*args);
                std::construct_at(impl, *args);
                return true;
            }

            impl = new (data) gc_impl();
            return true;
        }

        return false;
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

    void Destroy() {
        if (--initCount == 0) {
            // NOTE: collections, allocation and defragmentation operations might still be ongoing,
            //       and it's not my job to ensure that there aren't any
            std::destroy_at(impl);
            impl = nullptr;
        }
    }
    namespace internal {
        // misleading name: each object has only 1 handle to it;
        // we are just simulating having multiple handles with different roles
        handle_t *Copy(handle_t *src, const handle_role srcRole, const handle_role dstRole) {
            if (src == nullptr) {
                return nullptr;
            }

            if (dstRole == handle_role::field) {
                throw bad_api_usage("use SetField to set a field");
            }

            switch (srcRole) {
                case handle_role::internal_initialization:
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
                            ++src->rootHandleCount;
                            break;
                    case handle_role::rw_pin:
                            src->objectLock.AcquireWrite();
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
                            [[fallthrough]]; // for the sake of consistency
                    case handle_role::rw_pin:
                            // cannot copy rw pins for a variety of reasons:
                            // double release of rw locks, either multiple read or only 1 write access can exist at a
                            // time (so copying write accesses is invalid), ...etc
                            throw bad_api_usage("invalid copy operation");
                    default:
                            break;
                    }
                    return src;
                default:
                    return src;
            }
        }

        handle_t *SetField(handle_t *obj, handle_t **field, handle_t *newValue, const handle_role newValueRole) {
            if (obj == nullptr) {
                // assuming no one is calling this directly
                throw library_bug("obj is nullptr");
            }

            const auto needsUpgrade = newValueRole == handle_role::ro_pin;
            if (needsUpgrade && newValue != nullptr) {
                newValue->objectLock.Upgrade();
            }
            defer downgrade([=]{ if (needsUpgrade) { newValue->objectLock.DownGrade(); } });

            obj->SetField(field, newValue);
            return newValue;
        }

        bool Equals(const handle_t *lhs, const handle_t *rhs) noexcept {
            return lhs == rhs;
        }

        void Destroy(handle_t *handle, const handle_role role) {
            if (handle == nullptr) {
                return;
            }

            switch (role) {
                case handle_role::ro_pin:
                    [[fallthrough]];
                case handle_role::rw_pin:
                    handle->objectLock.Release();
                    [[fallthrough]];
                case handle_role::root:
                    // would be problem: object is alive, but has 0 reachable references (referenced by)
                    // and isn't a root. But, a reachable object is always in either of the 2 states:
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

            if (handle == nullptr) {
                return nullptr;
            }

            return handle->objectAllocation.data();
        }

        size_t GetMemberCount(const handle_t *array) {
            if (array == nullptr) {
                return 0;
            }

            const object_type *type = nullptr;
            {
                scoped_rw_lock lock(impl->typesLock);
                type = &impl->types[array->objectType];
            }

            return array->objectAllocation.size() / type->size;
        }

        handle_t *LookupObject(const void *obj) noexcept {
            if (obj == nullptr) {
                return nullptr;
            }

            return impl->TryGetHandleForObjectAllocation(obj);
        }

        handle_t *GetOwningObject(const void *obj) noexcept {
            scoped_rw_lock lock(impl->allocationLookupLock);
            for (handle_t *handle : std::views::values(impl->allocationToHandleLookup)) {
                if (handle->objectAllocation.data() <= obj && handle->objectAllocation.data() + handle->objectAllocation.size() >= obj) {
                    return handle;
                }
            }

            return nullptr;
        }

        handle_t *LookupObjectOrGetOwningObject(const void *obj) noexcept {
            if (handle_t *handle = LookupObject(obj); handle != nullptr) {
                return handle;
            }

            scoped_rw_lock lock(impl->allocationLookupLock);
            for (handle_t *handle : std::views::values(impl->allocationToHandleLookup)) {
                if (handle->objectAllocation.data() <= obj && handle->objectAllocation.data() + handle->objectAllocation.size() >= obj) {
                    return handle;
                }
            }

            return nullptr;
        }

        const std::type_info *GetType(const handle_t *obj) noexcept {
            if (obj == nullptr) {
                return nullptr;
            }

            scoped_rw_lock lockTypes(impl->typesLock);
            return impl->types[obj->objectType].type;
        }

        void TemporaryROPin(handle_t *handle) {
            handle->objectLock.AcquireRead();
            ++handle->rootHandleCount;
        }

        void TemporaryUnpin(handle_t *handle) {
            handle->objectLock.Release();
            --handle->rootHandleCount;
        }

        handle_t *New(
            const size_t size,
            const size_t alignment,
            const size_t count,
            const std::type_info *type,
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
}
