#pragma once

#include <list>
#include <unordered_map>
#include <functional>

#include "gc-config.h"
#include "gc-util.h"
#include "gca/gc.h"

namespace gc {
    struct object_type {
        size_t size, alignment;
        const std::type_info *type;
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

    //using page_memory = std::array<std::byte, config::page_size>;
    struct page_memory {
        alignas(config::page_alignment) std::array<std::byte, config::page_size> memory{static_cast<std::byte>(0)};

        constexpr std::byte *data() noexcept { return memory.data(); }
        [[nodiscard]] constexpr size_t size() const noexcept {return memory.size(); }
    };
    using page_allocation = std::span<std::byte>;

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


        /**
         * @brief get the number of bytes needed to pad @p ptr to satisfy the alignment constraint by @p alignment
         *
         * usage: @code ptr += GetAlignmentCorrection(ptr, alignment) @endcode
         *
         * @param ptr the pointer to align
         * @param alignment the alignment constraint for @p ptr
         * @return the padding bytes needed for @p ptr to be aligned to @p alignment
         * @note trying to align to 0 is undefined behaviour
         */
        static constexpr size_t GetAlignmentCorrection(const uintptr_t ptr, const size_t alignment) {
            const auto correction = ptr % alignment;
            return correction != 0 ? alignment - correction : 0;
        }

        explicit page(std::pmr::memory_resource *allocationsBacking) : allocations(allocationsBacking) {}

        /**
         * @biref Try allocating on this page
         *
         * Attempt reserving a part of the memory region owned by this page for the requested size
         * and return an empty allocation upon failure instead of std::bad_alloc
         *
         * @param size The size of the allocation
         * @param alignment The alignment of the allocation
         * @return an empty allocation if the allocation doesn't fit into the page and a proper allocation otherwise
         * @throws ... The underlying memory_resource might throw anything when resizing allocations
         * @note This function assumes having the allocationLock of this page when calling it
         */
        page_allocation TryAllocate(size_t size, size_t alignment) noexcept(false);


        /**
         * @brief Frees the region reserved region of memory if it exists
         *
         * Searches for an allocation with a matching size and start address among the allocations
         * and removes it when found
         *
         * @param allocation The allocation to free
         * @note This function assumes having the allocationLock of this page when calling it
         */
        void RemoveAllocation(const page_allocation &allocation) noexcept(true);
    };

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
        // std::atomic<size_t> pinCount{0};
        // redundant: pinning is only needed because relocating the object would result in current accesses pointing to
        // the wrong location, but moving an object already requires acquiring the write lock which means no other read
        // accesses can exist while it is acquired
        type_index objectType{};
        atomic_mark_bitfield markBits{0};
        rw_lock objectLock{}; // only for field/allocation modifications or for moving object during defragmentation
        atomic_bit_set<object_flags> flags;

        enum class set_field_fags : uint8_t {
            none                       = 0,
            has_rw_access_to_old_value = 1 << 0, // don't acquire the rw lock again for the value stored in field
            has_rw_access_to_new_value = 1 << 1, // don't acquire the rw lock again for newValue
        };

        /**
         * @brief Assigns a new value to the field
         *
         * @param field The field to assign a new value to (can be nullptr)
         * @param newValue The new value to assign to the field (can be nullptr)
         * @param flags Additional arguments to modify the behaviour of the call
         * @note assumes not having rw access to the old value of the field unless @p hasRWAccessToOldValue is set to true
         */
        void SetField(internal_handle **field, internal_handle *newValue, set_field_fags flags = set_field_fags::none) noexcept(false);
    };

    template <>
    struct enum_flag_traits<internal_handle::set_field_fags> {
        using underlying_type = uint8_t;
        constexpr static bool is_flag = true;
        constexpr static uint8_t all_flags = 0b11;
    };

    struct gc_impl;
    namespace debug {
        struct callback_data {
            gc_impl *impl{nullptr};

            callback_data(gc_impl *impl) : impl(impl) {
            }
        };

        using on_allocation_request_callback                                      = void (*)(const callback_data &data, const object_type &type, size_t count);
        using on_before_pages_lock_acquire_for_begin_and_end_query_callback       = void (*)(const callback_data& data);
        using on_pages_begin_and_end_query_finished_callback                      = void (*)(const callback_data &data, std::pmr::list<page>::iterator begin, std::pmr::list<page>::iterator end);
        using on_before_page_lock_acquire_callback                                = void (*)(const callback_data &data);
        using on_page_allocation_attempt_callback                                 = void (*)(const callback_data &data, size_t size, size_t alignment, page_allocation result);
        using on_before_register_type_lock_ro_acquire_callback                    = void (*)(const callback_data &data, const object_type &type);
        using on_before_register_types_ro_lock_upgrade_callback                   = void (*)(const callback_data &data, const object_type &type);
        using on_before_register_object_types_ro_lock_acquire_callback            = void (*)(const callback_data &data, page_allocation allocation, type_index type);
        using on_before_register_object_lookup_rw_lock_acquire_callback           = void (*)(const callback_data &data, page_allocation allocation, type_index type);
        using on_object_registered_callback                                       = void (*)(const callback_data &data, internal_handle *handle);
        using on_register_object_failed_callback                                  = void (*)(const callback_data &data, page_allocation allocation);
        using on_allocate_returning_null_callback                                 = void (*)(const callback_data &data);
        using on_before_allocation_to_handle_lookup_ro_lock_acquire_callback      = void (*)(const callback_data &data, const void *allocation);
        using on_before_collection_start_callback                                 = void (*)(const callback_data &data, page &page);
        using on_collection_start_callback                                        = void (*)(const callback_data &data, page &page);
        using on_collection_finished_callback                                     = void (*)(const callback_data &data, page &page);
        using on_before_collection_ro_lock_acquire_callback                       = void (*)(const callback_data &data, internal_handle *handle);
        using on_before_collection_ro_lock_upgrade_callback                       = void (*)(const callback_data &data, internal_handle *handle);
        using on_before_destroy_object_allocation_lookup_rw_lock_acquire_callback = void (*)(const callback_data &data, internal_handle *handle);
        using on_before_destroy_object_types_ro_lock_acquire_callback             = void (*)(const callback_data &data, internal_handle *handle);
        using on_before_destroy_object_field_old_value_acquire_rw_lock_callback   = void (*)(const callback_data &data, internal_handle *handle, internal_handle *fieldOldValue, size_t fieldIndex);
        using on_before_object_destroyed_callback                                 = void (*)(const callback_data &data, internal_handle *handle);
        using on_after_object_destroyed_callback                                  = void (*)(const callback_data &data, internal_handle *invalidHandle);
        using on_object_destroy_failed_callback                                   = void (*)(const callback_data &data, internal_handle *handle);
        using on_before_find_root_referenced_by_ro_lock_acquire_callback          = void (*)(const callback_data &data, internal_handle *handle, internal_handle *referencedBy);
        using on_defragment_started_callback                                      = void (*)(const callback_data &data, page &page);
        using on_defragment_finished_callback                                        = void (*)(const callback_data &data, page &page);
        using on_before_defragment_page_types_ro_lock_acquire_callback            = void (*)(const callback_data &data, internal_handle *handle);
        using on_before_defragment_page_object_rw_lock_try_acquire_callback       = void (*)(const callback_data &data, internal_handle *handle);
        using on_before_defragment_page_lookup_lock_rw_acquire_callback           = void (*)(const callback_data &data, internal_handle *handle);
        using on_before_defragment_object_relocate_callback                       = void (*)(const callback_data &data, internal_handle *handle, void *newLocation);
        using on_before_new_page_allocation_lock_acquire_callback                 = void (*)(const callback_data &data);
        using on_before_new_page_pages_lock_rw_acquire_callback                   = void (*)(const callback_data &data);
        using on_new_page_page_created_callback                                   = void (*)(const callback_data &data, page &page, bool acquireLockByDefault);
        using on_before_page_destroyed_callback                                   = void (*)(const callback_data &data, page &page);
        using on_before_destructor_object_rw_lock_acquire_callback                = void (*)(const callback_data &data, internal_handle *handle);

        struct debug_listeners {
            std::function<std::remove_pointer_t<on_allocation_request_callback>>                                      onAllocation{[](const callback_data&, const object_type&, size_t){}};
            std::function<std::remove_pointer_t<on_before_pages_lock_acquire_for_begin_and_end_query_callback>>       onBeforePagesQuery{[](const callback_data&){}};
            std::function<std::remove_pointer_t<on_pages_begin_and_end_query_finished_callback>>                      onPagesQueryFinished{[](const callback_data&, std::pmr::list<page>::iterator, std::pmr::list<page>::iterator){}};
            std::function<std::remove_pointer_t<on_before_page_lock_acquire_callback>>                                onBeforePageLockAcquire{[](const callback_data&){}};
            std::function<std::remove_pointer_t<on_page_allocation_attempt_callback>>                                 onPageAllocationAttempt{[](const callback_data&, size_t, size_t, page_allocation){}};
            std::function<std::remove_pointer_t<on_before_register_type_lock_ro_acquire_callback>>                    onBeforeRegisterTypeROLockAcquire{[](const callback_data&, const object_type&){}};
            std::function<std::remove_pointer_t<on_before_register_types_ro_lock_upgrade_callback>>                   onBeforeRegisterTypeLockUpgrade{[](const callback_data&, const object_type&){}};
            std::function<std::remove_pointer_t<on_before_register_object_types_ro_lock_acquire_callback>>            onBeforeRegisterObjectTypesROLockAcquire{[](const callback_data&, page_allocation, type_index){}};
            std::function<std::remove_pointer_t<on_before_register_object_lookup_rw_lock_acquire_callback>>           onBeforeRegisterObjectLookupRWLockAcquire{[](const callback_data&, page_allocation, type_index){}};
            std::function<std::remove_pointer_t<on_object_registered_callback>>                                       onObjectRegistered{[](const callback_data&, internal_handle*){}};
            std::function<std::remove_pointer_t<on_register_object_failed_callback>>                                  onObjectRegisterFailed{[](const callback_data&, page_allocation){}};
            std::function<std::remove_pointer_t<on_allocate_returning_null_callback>>                                 onAllocateReturningNull{[](const callback_data&){}};
            std::function<std::remove_pointer_t<on_before_allocation_to_handle_lookup_ro_lock_acquire_callback>>      onBeforeAllocationToHandleLookupROLockAcquire{[](const callback_data&, const void*){}};
            std::function<std::remove_pointer_t<on_before_collection_start_callback>>                                 onBeforeCollectionStart{[](const callback_data&, page&){}};
            std::function<std::remove_pointer_t<on_collection_start_callback>>                                        onCollectionStart{[](const callback_data&, page&){}};
            std::function<std::remove_pointer_t<on_collection_finished_callback>>                                     onCollectionFinished{[](const callback_data&, page&){}};
            std::function<std::remove_pointer_t<on_before_collection_ro_lock_acquire_callback>>                       onBeforeCollectionROLockAcquire{[](const callback_data&, internal_handle*){}};
            std::function<std::remove_pointer_t<on_before_collection_ro_lock_upgrade_callback>>                       onBeforeCollectionROLockUpgrade{[](const callback_data&, internal_handle*){}};
            std::function<std::remove_pointer_t<on_before_destroy_object_allocation_lookup_rw_lock_acquire_callback>> onBeforeDestroyObjectAllocationLookupRWLockAcquire{[](const callback_data&, internal_handle*){}};
            std::function<std::remove_pointer_t<on_before_destroy_object_types_ro_lock_acquire_callback>>             onBeforeDestroyObjectTypesROLockAcquire{[](const callback_data&, internal_handle*){}};
            std::function<std::remove_pointer_t<on_before_destroy_object_field_old_value_acquire_rw_lock_callback>>   onBeforeDestroyObjectFieldOldValueRWLockAcquire{[](const callback_data&, internal_handle*, internal_handle*, size_t){}};
            std::function<std::remove_pointer_t<on_before_object_destroyed_callback>>                                 onBeforeObjectDestroyed{[](const callback_data&, internal_handle*){}};
            std::function<std::remove_pointer_t<on_after_object_destroyed_callback>>                                  onAfterObjectDestroyed{[](const callback_data&, internal_handle*){}};
            std::function<std::remove_pointer_t<on_object_destroy_failed_callback>>                                   onObjectDestroyFailed{[](const callback_data&, internal_handle*){}};
            std::function<std::remove_pointer_t<on_before_find_root_referenced_by_ro_lock_acquire_callback>>          onBeforeFindRootReferencedByROLockAcquire{[](const callback_data&, internal_handle*, internal_handle*){}};
            std::function<std::remove_pointer_t<on_defragment_started_callback>>                                      onDefragmentStarted{[](const callback_data&, page&){}};
            std::function<std::remove_pointer_t<on_defragment_finished_callback>>                                     onDefragmentFinished{[](const callback_data&, page&){}};
            std::function<std::remove_pointer_t<on_before_defragment_page_types_ro_lock_acquire_callback>>            onBeforeDefragmentPageTypesROLockAcquire{[](const callback_data&, internal_handle*){}};
            std::function<std::remove_pointer_t<on_before_defragment_page_object_rw_lock_try_acquire_callback>>       onBeforeDefragmentPageObjectRWLockTryAcquire{[](const callback_data&, internal_handle*){}};
            std::function<std::remove_pointer_t<on_before_defragment_page_lookup_lock_rw_acquire_callback>>           onBeforeDefragmentPageLookupRWAcquire{[](const callback_data&, internal_handle*){}};
            std::function<std::remove_pointer_t<on_before_defragment_object_relocate_callback>>                       onBeforeDefragmentObjectRelocate{[](const callback_data&, internal_handle*, void*){}};
            std::function<std::remove_pointer_t<on_before_new_page_allocation_lock_acquire_callback>>                 onBeforeNewPageAllocationLockAcquire{[](const callback_data&){}};
            std::function<std::remove_pointer_t<on_before_new_page_pages_lock_rw_acquire_callback>>                   onBeforeNewPagePagesRWLockAcquire{[](const callback_data&){}};
            std::function<std::remove_pointer_t<on_new_page_page_created_callback>>                                   onNewPageCreated{[](const callback_data&, page&, bool){}};
            std::function<std::remove_pointer_t<on_before_page_destroyed_callback>>                                   onBeforePageDestroyed{[](const callback_data&, page&){}};
            std::function<std::remove_pointer_t<on_before_destructor_object_rw_lock_acquire_callback>>                onBeforeDestructorObjectRWLockAcquire{[](const callback_data&, internal_handle*){}};
        };
    }

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
        [[no_unique_address]] std::conditional_t<config::enable_debug_messages, debug::debug_listeners, std::monostate> debugListeners{};

        /**
         * @brief constructs a gc_impl that cannot allocate
         *
         * construction with gc_init_args where objectMemory and backingMemory is set to std::pmr::null_memory_resource
         */
        gc_impl() noexcept;
        explicit gc_impl(const gc_init_args &args);


        /**
         * @biref Allocates @p count instances of @p type and may launch a garbage collection and may even defragment.
         *
         * Tries to sequentially allocate @p count instances of @p type. On one of the existing pages.
         * When it fails to allocate on a page, it will collect garbage on that page and retry. If even that failed,
         * the page will be defragmented. If the allocation fails even after that, the allocator will try allocating on
         * a different page. If none of the existing pages can accommodate the allocation, a new page will be requested.
         *
         * @param type The type of the object
         * @param count The number of instances
         * @return A handle to the allocated instances
         * @throws ... Anything that the underlying memory_resource might throw (mainly std::bad_alloc)
         * @throws std::out_of_range When registering the type failed due to the resulting index being larger than what
         * type_index can hold
         * @note This only allocates space for the object (which it will track), but it won't initialize it.
         * @note setting @p count to 0 returns nullptr
         * @note @p count being larger than 1 implies allocating an array instead of a single object. In this case,
         * initializing the object means initializing every element of the array
         */
        internal_handle *Allocate(const object_type &type, size_t count) noexcept(false);

        /**
         * @brief defragments the page by compacting the allocations
         * @param page The page to defragment
         * @note assumes having acquired the lock for the page
         */
        void DefragmentOnPage(page &page) noexcept(false);

        /**
         * @brief Move an allocation on a page to a new location on the page
         * @param page The page that contains the allocation
         * @param handle The handle to the allocation
         * @param type The type of the object that is allocated in @p allocation
         * @param allocation The allocation
         * @param newLocation The new location for @p allocation
         * @note Assumes having acquired the lock for @p page and having the rw lock for @p handle. Also assumes
         * @p allocation to be on this page and @p newLocation to be within the bounds of this page (with the allocation
         * size included).
         * @note Objects will be relocated to @p newLocation, but the data will only be moved if the object
         * has been successfully initialized
         * @note For arrays, each element will be moved individually
         * @note Immovable objects will not be moved
         */
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

        /**
         * @brief Performs a garbage collection on @p page
         * @param page The page to perform the garbage collection on
         */
        void CollectOnPage(page &page) noexcept(false);

        /**
         * @brief Destroy an object
         *
         * Deallocates the space and calls the destructor for the object(s)
         *
         * @param page The page the object is located on
         * @param handle The handle to the object
         * @note If the allocation is an array allocation, then each object of the array will be destroyed individually
         * @note The object (or objects in the cases of arrays) will only be destroyed if they have been initialized.
         * @note Object(s) that have attempted and failed to initialize will still be destroyed.
         */
        void DestroyObject(page &page, internal_handle *handle) noexcept(false);
        bool TryFindRootFor(internal_handle *handle, thread_count id) noexcept(true);
        internal_handle *TryGetHandleForObjectAllocation(const void *objAllocation) noexcept(true);
        internal_handle *GetHandleForObjectAllocation(const void *objAllocation) noexcept(false);

        /**
         * @brief registers an allocation to the garbage collector
         *
         * Begin the tracking of @p allocation
         *
         * @param allocation The allocation to track
         * @param type The type of the object that was allocated
         * @return A handle to the allocation
         */
        internal_handle *RegisterObject(page_allocation allocation, type_index type) noexcept(false);
        internal_handle *TryAllocateOnNewPage(const object_type &type, size_t count) noexcept(false);
        type_index GetOrRegisterType(const object_type &type) noexcept(false);
        page &NewPage(bool acquireLock = false) noexcept(false);
        void InitPage(page &page) noexcept(false);
        void DestroyPage(page &page) noexcept(true);

        /**
         * @brief forcefully destroy all allocated objects
         *
         * Destroys all tracked objects regardless of whether they are reachable (destructors are only called when the
         * object has been initialized)
         *
         * @note This assumes that no collection, defragmentation, allocation, ...etc operations are currently ongoing,
         * nor will they be launched while the destructor executes.
         * @note There are currently no debug checks for these assumptions in the implementation
         */
        ~gc_impl() noexcept(false);
    };
}