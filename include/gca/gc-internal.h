#pragma once

#include <span>
#include <array>
#include <list>
#include <unordered_map>
#include <functional>

#include "gc-config.h"
#include "gc-util.h"
#include "gc.h"

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

        explicit page(std::pmr::vector<allocation> &&allocations) : allocations(std::move(allocations)) { }

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
        atomic_bit_set<object_flags> flags{};

        // starting state
        // this constructor exists solely because there was an issue with
        // handle->referencedBy = std::move(allocator->CreateReferencedByVectorForHandle())
        internal_handle(const page_allocation &allocation, std::pmr::vector<internal_handle*> &&referencedBy, const type_index type)
        : objectAllocation(allocation)
        , referencedBy(std::move(referencedBy))
        , rootHandleCount(1)
        , objectType(type)
        , flags(object_flags::uninitialized) {}

        /**
         * @brief Assigns a new value to the field
         *
         * @param field The field to assign a new value to (can be nullptr)
         * @param newValue The new value to assign to the field (can be nullptr)
         */
        void SetField(internal_handle **field, internal_handle *newValue) noexcept(false);
    };

    struct gc_impl;
    namespace debug {
        struct callback_data {
            gc_impl *impl{nullptr};

            callback_data(gc_impl *impl) : impl(impl) {
            }
        };

        using on_allocation_request_signature                                      = void (const callback_data &data, const object_type &type, size_t count);
        using on_before_pages_lock_acquire_for_begin_and_end_query_signature       = void (const callback_data& data);
        using on_pages_begin_and_end_query_finished_signature                      = void (const callback_data &data, std::pmr::list<page>::iterator begin, std::pmr::list<page>::iterator end);
        using on_before_page_lock_acquire_signature                                = void (const callback_data &data);
        using on_page_allocation_attempt_signature                                 = void (const callback_data &data, size_t size, size_t alignment, page_allocation result);
        using on_before_register_type_lock_ro_acquire_signature                    = void (const callback_data &data, const object_type &type);
        using on_before_register_types_ro_lock_upgrade_signature                   = void (const callback_data &data, const object_type &type);
        using on_before_register_object_types_ro_lock_acquire_signature            = void (const callback_data &data, page_allocation allocation, type_index type);
        using on_before_register_object_lookup_rw_lock_acquire_signature           = void (const callback_data &data, page_allocation allocation, type_index type);
        using on_object_registered_signature                                       = void (const callback_data &data, internal_handle *handle);
        using on_register_object_failed_signature                                  = void (const callback_data &data, page_allocation allocation);
        using on_allocate_returning_null_signature                                 = void (const callback_data &data);
        using on_before_allocation_to_handle_lookup_ro_lock_acquire_signature      = void (const callback_data &data, const void *allocation);
        using on_before_collection_start_signature                                 = void (const callback_data &data, page &page);
        using on_collection_start_signature                                        = void (const callback_data &data, page &page);
        using on_collection_finished_signature                                     = void (const callback_data &data, page &page);
        using on_before_collection_ro_lock_acquire_signature                       = void (const callback_data &data, internal_handle *handle);
        using on_before_collection_ro_lock_upgrade_signature                       = void (const callback_data &data, internal_handle *handle);
        using on_before_destroy_object_allocation_lookup_rw_lock_acquire_signature = void (const callback_data &data, internal_handle *handle);
        using on_before_destroy_object_types_ro_lock_acquire_signature             = void (const callback_data &data, internal_handle *handle);
        using on_before_destroy_object_field_old_value_acquire_rw_lock_signature   = void (const callback_data &data, internal_handle *handle, internal_handle *fieldOldValue, size_t fieldIndex);
        using on_before_object_destroyed_signature                                 = void (const callback_data &data, internal_handle *handle);
        using on_after_object_destroyed_signature                                  = void (const callback_data &data, internal_handle *invalidHandle);
        using on_object_destroy_failed_signature                                   = void (const callback_data &data, internal_handle *handle);
        using on_before_find_root_referenced_by_ro_lock_acquire_signature          = void (const callback_data &data, internal_handle *handle, internal_handle *referencedBy);
        using on_defragment_started_signature                                      = void (const callback_data &data, page &page);
        using on_defragment_finished_signature                                     = void (const callback_data &data, page &page);
        using on_before_defragment_page_types_ro_lock_acquire_signature            = void (const callback_data &data, internal_handle *handle);
        using on_before_defragment_page_object_rw_lock_try_acquire_signature       = void (const callback_data &data, internal_handle *handle);
        using on_before_defragment_page_lookup_lock_rw_acquire_signature           = void (const callback_data &data, internal_handle *handle);
        using on_before_defragment_object_relocate_signature                       = void (const callback_data &data, internal_handle *handle, void *newLocation);
        using on_before_new_page_allocation_lock_acquire_signature                 = void (const callback_data &data);
        using on_before_new_page_pages_lock_rw_acquire_signature                   = void (const callback_data &data);
        using on_new_page_page_created_signature                                   = void (const callback_data &data, page &page, bool acquireLockByDefault);
        using on_before_page_destroyed_signature                                   = void (const callback_data &data, page &page);
        using on_before_destructor_object_rw_lock_acquire_signature                = void (const callback_data &data, internal_handle *handle);

        struct debug_listeners {
            std::function<on_allocation_request_signature>                                      onAllocation                                       {[](auto&&...){}};
            std::function<on_before_pages_lock_acquire_for_begin_and_end_query_signature>       onBeforePagesQuery                                 {[](auto&&...){}};
            std::function<on_pages_begin_and_end_query_finished_signature>                      onPagesQueryFinished                               {[](auto&&...){}};
            std::function<on_before_page_lock_acquire_signature>                                onBeforePageLockAcquire                            {[](auto&&...){}};
            std::function<on_page_allocation_attempt_signature>                                 onPageAllocationAttempt                            {[](auto&&...){}};
            std::function<on_before_register_type_lock_ro_acquire_signature>                    onBeforeRegisterTypeROLockAcquire                  {[](auto&&...){}};
            std::function<on_before_register_types_ro_lock_upgrade_signature>                   onBeforeRegisterTypeLockUpgrade                    {[](auto&&...){}};
            std::function<on_before_register_object_types_ro_lock_acquire_signature>            onBeforeRegisterObjectTypesROLockAcquire           {[](auto&&...){}};
            std::function<on_before_register_object_lookup_rw_lock_acquire_signature>           onBeforeRegisterObjectLookupRWLockAcquire          {[](auto&&...){}};
            std::function<on_object_registered_signature>                                       onObjectRegistered                                 {[](auto&&...){}};
            std::function<on_register_object_failed_signature>                                  onObjectRegisterFailed                             {[](auto&&...){}};
            std::function<on_allocate_returning_null_signature>                                 onAllocateReturningNull                            {[](auto&&...){}};
            std::function<on_before_allocation_to_handle_lookup_ro_lock_acquire_signature>      onBeforeAllocationToHandleLookupROLockAcquire      {[](auto&&...){}};
            std::function<on_before_collection_start_signature>                                 onBeforeCollectionStart                            {[](auto&&...){}};
            std::function<on_collection_start_signature>                                        onCollectionStart                                  {[](auto&&...){}};
            std::function<on_collection_finished_signature>                                     onCollectionFinished                               {[](auto&&...){}};
            std::function<on_before_collection_ro_lock_acquire_signature>                       onBeforeCollectionROLockAcquire                    {[](auto&&...){}};
            std::function<on_before_collection_ro_lock_upgrade_signature>                       onBeforeCollectionROLockUpgrade                    {[](auto&&...){}};
            std::function<on_before_destroy_object_allocation_lookup_rw_lock_acquire_signature> onBeforeDestroyObjectAllocationLookupRWLockAcquire {[](auto&&...){}};
            std::function<on_before_destroy_object_types_ro_lock_acquire_signature>             onBeforeDestroyObjectTypesROLockAcquire            {[](auto&&...){}};
            std::function<on_before_destroy_object_field_old_value_acquire_rw_lock_signature>   onBeforeDestroyObjectFieldOldValueRWLockAcquire    {[](auto&&...){}};
            std::function<on_before_object_destroyed_signature>                                 onBeforeObjectDestroyed                            {[](auto&&...){}};
            std::function<on_after_object_destroyed_signature>                                  onAfterObjectDestroyed                             {[](auto&&...){}};
            std::function<on_object_destroy_failed_signature>                                   onObjectDestroyFailed                              {[](auto&&...){}};
            std::function<on_before_find_root_referenced_by_ro_lock_acquire_signature>          onBeforeFindRootReferencedByROLockAcquire          {[](auto&&...){}};
            std::function<on_defragment_started_signature>                                      onDefragmentStarted                                {[](auto&&...){}};
            std::function<on_defragment_finished_signature>                                     onDefragmentFinished                               {[](auto&&...){}};
            std::function<on_before_defragment_page_types_ro_lock_acquire_signature>            onBeforeDefragmentPageTypesROLockAcquire           {[](auto&&...){}};
            std::function<on_before_defragment_page_object_rw_lock_try_acquire_signature>       onBeforeDefragmentPageObjectRWLockTryAcquire       {[](auto&&...){}};
            std::function<on_before_defragment_page_lookup_lock_rw_acquire_signature>           onBeforeDefragmentPageLookupRWAcquire              {[](auto&&...){}};
            std::function<on_before_defragment_object_relocate_signature>                       onBeforeDefragmentObjectRelocate                   {[](auto&&...){}};
            std::function<on_before_new_page_allocation_lock_acquire_signature>                 onBeforeNewPageAllocationLockAcquire               {[](auto&&...){}};
            std::function<on_before_new_page_pages_lock_rw_acquire_signature>                   onBeforeNewPagePagesRWLockAcquire                  {[](auto&&...){}};
            std::function<on_new_page_page_created_signature>                                   onNewPageCreated                                   {[](auto&&...){}};
            std::function<on_before_page_destroyed_signature>                                   onBeforePageDestroyed                              {[](auto&&...){}};
            std::function<on_before_destructor_object_rw_lock_acquire_signature>                onBeforeDestructorObjectRWLockAcquire              {[](auto&&...){}};
        };
    }

    /**
     * @brief An interface for memory allocations for the gc
     */
    struct gc_allocator {
        /**
         * @brief Create the vector used by the implementation to hold the registered types
         * @return The type vector
         */
        virtual std::pmr::vector<object_type> CreateTypesVector() noexcept = 0;

        /**
         * @brief Create the list that stores the existing pages
         * @return The page list
         */
        virtual std::pmr::list<page> CreatePagesList()  noexcept = 0;

        /**
         * @brief Create an allocator used to allocate the memory for pages
         * @return The allocator
         */
        virtual std::pmr::polymorphic_allocator<page_memory> CreatePageMemoryAllocator() noexcept = 0;

        /**
         * @brief Create the container used to store the handles for allocations
         * @return The handle container
         */
        virtual ptr_safe_container<internal_handle> CreateObjectHandlesContainer()  noexcept= 0;

        /**
         * @brief Create the lookup for memory allocations to handles for them
         * @return The lookup
         */
        virtual std::pmr::unordered_map<const void*, internal_handle*> CreateAllocationToHandleLookup()  noexcept = 0;

        /**
         * @brief The vector used to store the allocations made on a page
         * @return The allocations vector
         */
        virtual std::pmr::vector<page::allocation> CreatePageAllocationsVectorForPage()  noexcept = 0;

        /**
         * @brief Create the vector used by the handle to track which other handles reference it
         * @return The vector to store references to a handle
         */
        virtual std::pmr::vector<internal_handle*> CreateReferencedByVectorForHandle()  noexcept = 0;

        virtual ~gc_allocator() = default;
    };

    struct gc_impl {
        gc_allocator *allocator = nullptr;
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
#ifdef _MSC_VER
        [[msvc::no_unique_address]] std::conditional_t<config::enable_debug_messages, debug::debug_listeners, std::monostate> debugListeners{};
#else
        [[no_unique_address]] std::conditional_t<config::enable_debug_messages, debug::debug_listeners, std::monostate> debugListeners{};
#endif

        /**
         * @brief constructs a gc_impl that cannot allocate
         *
         * construction with gc_init_args where the allocator is set to the default allocator
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
         * @throws ... Anything that the underlying memory allocator containers might throw (mainly std::bad_alloc)
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
            const page &page,
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
         * @note Every field (that the collector is aware of) will be set to null before the destructor is called.
         * (field only refers to field handles)
         */
        void DestroyObject(page &page, internal_handle *handle) noexcept(false);
        bool TryFindRootFor(internal_handle *handle, thread_count id) noexcept(true);
        internal_handle *TryGetHandleForObjectAllocation(const void *objAllocation) noexcept(true); // can return nullptr
        internal_handle *GetHandleForObjectAllocation(const void *objAllocation) noexcept(false); // throws instead of returning nullptr

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

        /**
         * @brief Allocates @p count instances of an object of @p type on a new page and returns a handle to the allocation
         *
         * Requests a new page and allocates @p count instances of objects of @p type in a continuous region
         * @param type The type of the object
         * @param count The number of instances
         * @return A handle to the allocation
         * @note The implementation should throw before letting this return a null handle
         * @throws ... Anything that the allocator and containers might throw
         */
        internal_handle *TryAllocateOnNewPage(const object_type &type, size_t count) noexcept(false);

        /**
         * @brief Gets the type index for the type or registers and then returns the registered index
         * @param type The type to get the index for
         * @return An index that can be used to access the type
         * @throws std::out_of_range If the value of the index for the type cannot fit into the type_index
         */
        type_index GetOrRegisterType(const object_type &type) noexcept(false);

        /**
         * @brief Requests a new page
         *
         * Requests a new page and initializes it
         *
         * @param acquireLock Defer the unlocking of the page to the caller
         * @return A new empty page
         * @throws ... Anything that the allocator might throw
         */
        page &NewPage(bool acquireLock = false) noexcept(false);

        /**
         * @brief Initialize a newly created page
         * @param page The page to initialize
         * @throws ... Anything the allocator might throw
         * @note This is a separate step to ensure proper clean-up in the implementation if anything throws
         */
        void InitPage(page &page) noexcept(false);

        /**
         * @brief Deallocates the memory of a page
         * @param page The page to destroy
         * @note This will not ensure nor check whether the objects allocated on this page have been cleaned up properly
         */
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