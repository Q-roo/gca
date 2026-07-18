#pragma once

#include <list>
#include <unordered_map>

#include "gc-config.h"
#include "gc-util.h"
#include "gca/gc.h"

namespace gc {
    struct object_type {
        size_t size, alignment;
        std::type_index type;
        destructor_fn destructor;
        get_field_count_fn getFieldCount;
        get_field_fn getField;
        move_fn move;

        bool operator==(const object_type &other) const noexcept {
            return destructor == other.destructor
            && move == other.move
            && getFieldCount == other.getFieldCount
            && getField == other.getField
            && type == other.type;
        }
    };

    using type_index = config::object_type_index_underlying_type;

    using page_memory = std::array<std::byte, config::page_size>;

    struct page {
        page_memory *memory{nullptr};
        mutex allocationLock{}; // allocated objects should be accessible safely without this
        struct allocation {
            using size_type = smallest_unsigned_numeric_type_needed_for_t<config::page_size>;
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

    static_assert(config::gc_max_collection_thread_count <= 64, "garbage collection threads are only supported");

    using thread_count = smallest_unsigned_numeric_type_needed_for_t<config::gc_max_collection_thread_count>;
    using thread_mark_bitfield_type = smallest_unsigned_numeric_type_needed_for_t<1 << (config::gc_max_collection_thread_count - 1)>;
    using atomic_mark_bitfield = atomic_bit_set<thread_mark_bitfield_type>;

    enum class object_flags : uint8_t {
        uninitialized         = 1 << 0,
        garbage               = 1 << 1,
        immovable             = 1 << 2,
        initialization_failed = 1 << 3,
    };

    template<>
    struct enum_flag_traits<object_flags> {
        using underlying_type = uint8_t;
        constexpr static bool is_flag = true;
        constexpr static uint8_t all_flags = 0b1111;
    };

    struct internal_handle {
        page_allocation objectAllocation{};
        std::pmr::vector<internal_handle*> referencedBy{};
        std::atomic<size_t> rootHandleCount{0};
        std::atomic<size_t> pinCount{0};
        type_index objectType{};
        atomic_mark_bitfield markBits{0};
        rw_lock objectLock{}; // only for field/allocation modifications or for moving object during defragmentation
        atomic_bit_set<object_flags> flags;
    };

    struct gc_impl {
        std::pmr::memory_resource *objectMemory = nullptr, *backingMemory = nullptr;
        std::pmr::vector<object_type> types{};
        rw_lock typesLock{};
        std::pmr::list<page> pages{};
        mutex pageAllocationLock{};
        rw_lock pagesLock{};
        std::pmr::polymorphic_allocator<page_memory> pageAllocator{};
        ptr_safe_container<internal_handle> objectHandles{};
        std::pmr::unordered_map<const void*, internal_handle*> allocationToHandleLookup{};
        rw_lock allocationLookupLock{}; // for both the container and the lookup
        std::atomic<thread_count> gcCount{0};

        gc_impl();
        explicit gc_impl(const gc_init_args &args);

        internal_handle *Allocate(const object_type &type, size_t count) noexcept(false);
        void DefragmentOnPage(page &page) noexcept(false);
        static void RemoveAllocation(page &page, const page_allocation &allocation) noexcept(true);
        void RelocateObject(
            page &page,
            internal_handle *handle,
            const object_type *type,
            page::allocation &allocation,
            void *newLocation
        ) noexcept(false);

        static constexpr void MarkGarbage(internal_handle *handle) noexcept(true) {
            handle->flags.SetFlag(object_flags::garbage, true);
        }

        static constexpr bool IsMarkedGarbage(const internal_handle *handle) noexcept(true) {
            return handle->flags.HasFlag(object_flags::garbage);
        }

        static void SetField(internal_handle *obj, internal_handle **field, internal_handle *newValue) noexcept(false);
        static bool TryRoPin(internal_handle *handle, size_t attempts = 0) noexcept(true);
        static bool TryRwPin(internal_handle *handle, size_t attempts = 0) noexcept(true);
        static void Unpin(internal_handle *handle) noexcept(true);
        static void PinUpgrade(internal_handle *handle) noexcept(true);
        void CollectOnPage(page &page) noexcept(false);
        void DestroyObject(page &page, internal_handle *handle) noexcept(false);
        bool TryFindRootFor(internal_handle *handle, thread_count id) noexcept(true);
        internal_handle *GetHandleForObjectAllocation(const void *objAllocation) noexcept(false);
        internal_handle *RegisterObject(page_allocation allocation, type_index type) noexcept(false);
        static page_allocation TryAllocateOnPage(page &page, size_t size, size_t alignment) noexcept(false);
        internal_handle *TryAllocateOnNewPage(const object_type &type, size_t count) noexcept(false);
        type_index GetOrRegisterType(const object_type &type) noexcept(false);
        page &NewPage(bool acquireLock = false) noexcept(false);
        void InitPage(page &page) noexcept(false);
        void DestroyPage(page &page) noexcept(true);
        ~gc_impl() noexcept(false);
    };
}