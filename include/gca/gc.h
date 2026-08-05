#pragma once

#include <memory>
#include <typeindex>
#include <utility>

namespace gc {
    typedef struct internal_handle handle_t;
    typedef struct gc_allocator allocator_handle_t;
    enum class handle_role { internal_initialization, root, field, ro_pin, rw_pin };

    allocator_handle_t *GetNullAllocator();
    allocator_handle_t *GetDefaultAllocator();

    struct gc_init_args {
        allocator_handle_t *allocator = GetDefaultAllocator();
        gc_init_args() noexcept = default;
        explicit gc_init_args(allocator_handle_t *allocator) : allocator(allocator) {
            if (allocator == nullptr) {
                throw bad_api_usage("allocator cannot be nullptr (use GetNullAllocatorInstead)");
            }
        }
    };


    /**
     * @brief Initialize the GC singleton
     * @param args The initialization arguments. Can be nullptr in which case, it will use the default configuration
     * @return Whether the initialization was successful
     */
    bool Init(const gc_init_args *args);
    inline bool Init(const gc_init_args& args) { return Init(&args);}
    inline bool Init() { return Init(nullptr); }

    /**
     * Force a GC
     * @param defragment Whether to also defragment the memory as well
     */
    void Collect(bool defragment = false);

    /**
     * @brief Cleans up the GC singleton
     * @note Calling this while a GC/defragmentation or any other operation is ongoing will result in undefined behaviour
     * (i.e. I was lazy to implement the safety checks to against deadlocks and all kinds of memory bugs that could occur)
     */
    void Destroy();

    using destructor_fn = void (*)(void *) noexcept; // too lazy to deal with throwing destructors and all the possible edge-cases it could present
    using get_field_count_fn = size_t (*)(const void *obj) noexcept; // just return an invalid index
    using get_field_fn = handle_t** (*)(void *obj, size_t index) noexcept; // return nullptr for invalid index
    using move_fn = void (*)(void *obj, void *newLocation) noexcept; // too lazy to handle one move throwing while moving an array allocation

    // AKA, functions you are not supposed to use
    namespace internal {
        /**
         * @brief Return a handle for the same object to which @p src points to but with a different role
         * @param src The source handle
         * @param srcRole The role of the source handle
         * @param dstRole The desired role of the returned handle
         * @return A handle for the same object as to which src points to
         */
        handle_t *Copy(handle_t *src, handle_role srcRole, handle_role dstRole);
        handle_t *SetField(handle_t *obj, handle_t **field, handle_t *newValue, handle_role newValueRole);

        /**
         * @brief Compare whether the two handles point to the same object
         * @param lhs The left-hand side
         * @param rhs The right-hand side
         * @return Whether @p lhs points to the same object as @p rhs
         */
        bool Equals(const handle_t *lhs, const handle_t *rhs) noexcept;

        void Destroy(handle_t *handle, handle_role role); // can handle nullptr for handle
        void *GetInstance(const handle_t *handle, handle_role role); // should only be called with a pin(ro/rw) role
        size_t GetMemberCount(const handle_t *array);
        handle_t *LookupObject(const void *obj) noexcept;
        handle_t *GetOwningObject(const void *obj) noexcept;
        handle_t *LookupObjectOrGetOwningObject(const void *obj) noexcept;
        const std::type_info *GetType(const handle_t *obj) noexcept; // returns the type of std::nullptr_t if obj is nullptr
        void TemporaryROPin(handle_t *handle); // used for dynamic handles only as a "temporary" solution
        void TemporaryUnpin(handle_t *handle); // ^ same thing

        // only allocates, does not initialize
        handle_t *New(
            size_t size,
            size_t alignment,
            size_t count,
            const std::type_info *type,
            destructor_fn destructor,
            get_field_count_fn getFieldCount,
            get_field_fn getField,
            move_fn move
        );

        /**
         * @brief Get the right to initialize an uninitialized object
         *
         * Objects should be initialized once. To ensure that, whether the object has been constructed is tracked.
         * By calling this, you request permission to initialize it and you may do so when it returns true.
         * False means either that the object is already initialized (failed or not does not matter) or another thread
         * got the permission before this thread did.
         *
         * @param handle The object you want to get the initialization right for
         * @return Whether you can initialize the object
         */
        bool TryAcquireInitializeRight(handle_t *handle);

        /**
         * @brief Notify the GC that the initialization of this object has failed
         * @param handle The handle for which the initialization failed
         * @note This should only be called if TryAcquireInitializeRight returned true for @p handle and the
         * initialization was attempted and failed
         */
        void SetInitializationFailed(handle_t *handle);
    }

    struct null_handle_t {
        constexpr explicit null_handle_t(std::nullptr_t) noexcept {}
    };

    inline constexpr null_handle_t null_handle { nullptr };

    /**
     * @brief A handle for an object that can only be used for roots.
     *
     * Roots refer to references to objects that are unquestionably reachable.
     *
     * @tparam T The type of the object this handle references
     * @note This can only be used for globals or for values on the stack, for fields in objects, use field
     */
    template <class T>
    struct root_handle;

    /**
     * @brief A handle for an object that can be used as a field in classes/structs
     * @tparam field_store_offset The offset to the field store on the object this field is located at
     * @tparam index The index of the field in the field store
     * @tparam T The type of the object this handle references
     * @note Probably the worst way to do this
     * @note Must be used with no_unique_address (or msvc::no_unique_address, because msvc wanted to feel special)
     * and hope that your compiler won't ignore it and makes the value of the this pointer the same as the object it is
     * the field of
     */
    template <ptrdiff_t field_store_offset, size_t index, class T>
    struct field;

    /**
     * @brief The only handle that lets you access the object it references
     * @tparam T The type of the object this handle references
     * @tparam ro Whether the access should be read-only or read-write
     * @note Shared reads and exclusive writes are enforced
     * @note This should be only used on the stack and cannot be moved nor copied
     * @note If @tp T is const, only read-only accesses are allowed
     * @note There is a specialization for arrays
     */
    template <class T, bool ro = std::is_const_v<T>>
    struct pin;

    namespace internal {
        template <class T, bool ro>
        handle_t *ToRootHandle(const pin<T, ro>& pin);

        template <ptrdiff_t field_store_offset, size_t index, class T>
        handle_t *ToRootHandle(const field<field_store_offset, index, T>& field);

        template <class T>
        handle_t *PinRW(const root_handle<T> &handle);

        template <ptrdiff_t field_store_offset, size_t index, class T>
        handle_t *PinRW(const field<field_store_offset, index, T> &field);

        template <class T>
        handle_t *PinRO(const root_handle<T> &handle);

        template <ptrdiff_t field_store_offset, size_t index, class T>
        handle_t *PinRO(const field<field_store_offset, index, T> &field);
    }

    template <class T>
    struct root_handle {
        handle_t *handle = nullptr;
        constexpr root_handle() noexcept = default;
        constexpr root_handle(std::nullptr_t) noexcept {}
        constexpr root_handle(null_handle_t) noexcept {}
        template <bool ro>
        root_handle(const pin<T, ro>& pin) : handle(internal::ToRootHandle(pin)) {}
        template <bool ro>
        root_handle(const pin<std::remove_const_t<T>, ro>& pin) requires(std::is_const_v<T>) : handle(internal::ToRootHandle(pin)) {}
        template <ptrdiff_t field_store_offset, size_t index>
        root_handle(const field<field_store_offset, index, T>& field) : handle(internal::ToRootHandle(field)) {}
        template <ptrdiff_t field_store_offset, size_t index>
        root_handle(const field<field_store_offset, index, std::remove_const_t<T>>& field) requires(std::is_const_v<T>) : handle(internal::ToRootHandle(field)) {}
        root_handle(handle_t *handle) : handle(internal::Copy(handle, handle_role::internal_initialization, handle_role::root)) {} // internal

        root_handle(const root_handle &other) : handle(internal::Copy(other.handle, handle_role::root, handle_role::root)) {}
        root_handle(const root_handle<std::remove_const_t<T>> &other) requires(std::is_const_v<T>) : handle(internal::Copy(other.handle, handle_role::root, handle_role::root)) {}
        root_handle &operator=(const root_handle &other) {
            if (&other == this) {
                return *this;
            }
            handle = internal::Copy(handle, handle_role::root, handle_role::root);
            return *this;
        }

        constexpr root_handle(root_handle &&other) noexcept : handle(std::exchange(other.handle, nullptr)) {}
        constexpr root_handle &operator=(root_handle &&other) noexcept {
            if (&other == this) {
                return *this;
            }

            handle = std::exchange(other.handle, handle);
            return *this;
        }

        constexpr root_handle &operator=(std::nullptr_t) noexcept {
            if (handle != nullptr) {
                internal::Destroy(handle, handle_role::root);
            }

            handle = nullptr;
            return *this;
        }

        constexpr root_handle &operator=(null_handle_t) noexcept {
            if (handle != nullptr) {
                internal::Destroy(handle, handle_role::root);
            }

            handle = nullptr;
            return *this;
        }

        explicit constexpr operator root_handle<const T>() requires(!std::is_const_v<T>) {
            root_handle<const T> other{};
            other.handle = internal::Copy(handle, handle_role::root, handle_role::root);
            return other;
        }

        [[nodiscard]] constexpr bool IsNull() const noexcept {
            return handle == nullptr;
        }

        constexpr explicit operator handle_t *() const noexcept {
            return handle;
        }

        constexpr explicit operator bool() const noexcept {
            return handle != nullptr;
        }

        ~root_handle() {
            if (handle != nullptr) {
                internal::Destroy(handle, handle_role::root);
            }
        }
    };

    template <class T=void, bool ro>
    struct pin {
        static_assert(ro || !std::is_const_v<T>, "cannot create read-write pin for const T");
        constexpr static handle_role role = ro ? handle_role::ro_pin : handle_role::rw_pin;

        using element_type = T;
        using rw_element_type = std::remove_const_t<T>;
        using ro_element_type = std::add_const_t<T>;
        using return_element_type = std::conditional_t<ro, ro_element_type, rw_element_type>;
        using return_pointer_type = std::add_pointer_t<return_element_type>;

        handle_t *handle = nullptr;
        constexpr pin() noexcept = default;
        constexpr pin(std::nullptr_t) noexcept {}
        constexpr pin(null_handle_t) noexcept {}
        pin(const root_handle<rw_element_type> &handle) : handle(ro ? internal::PinRO(handle) : internal::PinRW(handle)) {}
        template <ptrdiff_t field_store_offset, size_t index>
        pin(const field<field_store_offset, index, rw_element_type> &field) : handle(ro ? internal::PinRO(field) : internal::PinRW(field)) {}
        pin(const root_handle<ro_element_type> &handle) requires(ro) : handle(internal::PinRO(handle)) {}
        template <ptrdiff_t field_store_offset, size_t index>
        pin(const field<field_store_offset, index, ro_element_type> &field) requires(ro) : handle(internal::PinRO(field)) {}

        pin(const pin&) = delete;
        pin& operator=(const pin&) = delete;
        pin(pin&&) = delete;
        pin& operator=(pin&&) = delete;

        [[nodiscard]] constexpr bool IsNull() const noexcept {
            return handle == nullptr;
        }

        constexpr explicit operator handle_t *() const noexcept {
            return handle;
        }

        constexpr explicit operator bool() const noexcept {
            return handle != nullptr;
        }

        return_pointer_type Get() const {
            if (handle == nullptr) {
                return nullptr;
            }

            return std::launder(static_cast<return_pointer_type>(internal::GetInstance(handle, role)));
        }

        return_pointer_type operator ->() const {
            return Get();
        }

        std::add_lvalue_reference_t<return_element_type> operator *() const {
            return *Get();
        }

        ~pin() {
            if (handle != nullptr) {
                internal::Destroy(handle, role);
            }
        }
    };

    template <class T, bool ro>
    struct pin<T[], ro> {
        static_assert(ro || !std::is_const_v<T>, "cannot create read-write pin for const T");
        constexpr static handle_role role = ro ? handle_role::ro_pin : handle_role::rw_pin;

        using element_type = T;
        using rw_element_type = std::remove_const_t<T>;
        using ro_element_type = std::add_const_t<T>;
        using return_element_type = std::conditional_t<ro, ro_element_type, rw_element_type>;
        using return_pointer_type = std::add_pointer_t<return_element_type>;

        handle_t *handle = nullptr;
        constexpr pin() noexcept = default;
        constexpr pin(std::nullptr_t) noexcept {}
        constexpr pin(null_handle_t) noexcept {}
        pin(const root_handle<rw_element_type[]> &handle) : handle(ro ? PinRO(handle) : PinRW(handle)) {}
        template <ptrdiff_t field_store_offset, size_t index>
        pin(const field<field_store_offset, index, rw_element_type[]> &field) : handle(ro ? PinRO(field) : PinRW(field)) {}
        pin(const root_handle<ro_element_type[]> &handle) : handle(PinRO(handle)) {}
        template <ptrdiff_t field_store_offset, size_t index>
        pin(const field<field_store_offset, index, ro_element_type[]> &field) : handle(PinRO(field)) {}

        pin(const pin&) = delete;
        pin& operator=(const pin&) = delete;
        pin(pin&&) = delete;
        pin& operator=(pin&&) = delete;

        [[nodiscard]] constexpr bool IsNull() const noexcept {
            return handle == nullptr;
        }

        constexpr explicit operator handle_t *() const noexcept {
            return handle;
        }

        constexpr explicit operator bool() const noexcept {
            return handle != nullptr;
        }

        [[nodiscard]] size_t Count() const {
            return internal::GetMemberCount(handle);
        }

        return_pointer_type Get() const {
            if (handle == nullptr) {
                return nullptr;
            }

            return std::launder(static_cast<return_pointer_type>(internal::GetInstance(handle, role)));
        }

        constexpr std::add_lvalue_reference_t<return_element_type> operator [](size_t index) const
        {
            return Get()[index];
        }

        ~pin() {
            if (handle != nullptr) {
                internal::Destroy(handle, role);
            }
        }
    };

    /*
     * field spec v3
     *
     * fields are part of the object, but only the handle assigned to the field counts as part of the value
     * the fields themselves are tied to the object
     *
     * so during moves and copies, only the value gets moved/copied
     * the field is just an offset to the location of a handle_t* from the base pointer (this pointer of the object that has this field)
     *
     * 2 modes
     * 1. non-gc-allocated (unmanaged) mode (stack/heap, but not by the gc)
     * 2. gc-allocated (managed) mode
     *
     * in unmanaged mode, the field behaves like a root-handle
     * in managed mode, the field no longer counts as a root-handle. Additionally, it keeps updating which object is
     * referenced by which other objects (which is tracked by the gc)
     */

    struct unsized_field_store {
        handle_t **fields{nullptr};
    };

    template <size_t field_count>
    struct field_store {
        unsized_field_store store;
        std::array<handle_t*, field_count> fields{nullptr};
        field_store() :store(fields.data()) {}
        // move and copy should be a noop
        // the operation itself happens inside the fields
        // not sure whether using {} would be the same as =default, so I put an empty lambda being called inside
        field_store(const field_store&) { []{}(); }
        field_store(field_store&&) noexcept { store.fields = fields.data(); } // should've just relied on array decay
        field_store&operator=(const field_store&) {
            return *this;
        }
        field_store&operator=(field_store&&) noexcept {
            return *this;
        }
    };

    // use with no_unique_address
    template <ptrdiff_t field_store_offset, size_t index, class T>
    struct field {
        field() noexcept {
            Set(nullptr, handle_role::internal_initialization);
        }

        field(const field &other) { Set(other); }
        field& operator=(const field &other) { if (&other != this) { Set(other); } return *this; }
        field(field &&other) {Set(other); other.Set(nullptr, handle_role::field); }
        field& operator=(field &&other) { if (&other != this) { auto oldValue = Get(); Set(other); other.Set(oldValue, handle_role::field); } return *this; }

        field(std::nullptr_t) noexcept : field() {}
        field(null_handle_t) noexcept : field() {}

        field(const root_handle<T> &handle) {
            Set(handle);
        }

        template <bool ro>
        field(const pin<T, ro> &pin) {
            Set(pin);
        }

        template <bool ro>
        field(const pin<std::remove_const_t<T>, ro> &pin) requires(std::is_const_v<T>) {
            Set(pin);
        }

        template <ptrdiff_t other_field_store_offset, uint16_t other_index>
        field(const field<other_field_store_offset, other_index, T> &other) {
            Set(other);
        }

        template <ptrdiff_t other_field_store_offset, uint16_t other_index>
        field(field<other_field_store_offset, other_index, T> &&other) {
            Set(other);
        }

        template <ptrdiff_t other_field_store_offset, uint16_t other_index>
        field(const field<other_field_store_offset, other_index, std::remove_const_t<T>> &other) requires(std::is_const_v<T>) {
            Set(other);
        }

        template <ptrdiff_t other_field_store_offset, uint16_t other_index>
        field(field<other_field_store_offset, other_index, std::remove_const_t<T>> &&other) requires(std::is_const_v<T>) {
            Set(other);
        }

        field &operator=(std::nullptr_t) {
            Set(nullptr, handle_role::internal_initialization);
            return *this;
        }

        field &operator=(null_handle_t) {
            Set(nullptr, handle_role::internal_initialization);
            return *this;
        }

        field &operator=(const root_handle<T> &handle) {
            Set(handle);
            return *this;
        }

        template <bool ro>
        field &operator=(const pin<T, ro> &pin) {
            Set(pin);
            return *this;
        }

        template <bool ro>
        field &operator=(const pin<std::remove_const_t<T>, ro> &pin) requires(std::is_const_v<T>) {
            Set(pin);
            return *this;
        }

        template <ptrdiff_t other_field_store_offset, uint16_t other_index>
        field& operator=(const field<other_field_store_offset, other_index, T> &other) {
            if (this != &other) {
                Set(other);
            }

            return *this;
        }

        template <ptrdiff_t other_field_store_offset, uint16_t other_index>
        field& operator=(const field<other_field_store_offset, other_index, std::remove_const_t<T>> &other) requires(std::is_const_v<T>) {
            if (this != static_cast<const void *>(&other)) {
                Set(other);
            }

            return *this;
        }

        template <ptrdiff_t other_field_store_offset, uint16_t other_index>
        field& operator=(field<other_field_store_offset, other_index, T> &&other) {
            if (this != &other) {
                Set(other);
            }

            return *this;
        }

        template <ptrdiff_t other_field_store_offset, uint16_t other_index>
        field& operator=(field<other_field_store_offset, other_index, std::remove_const_t<T>> &&other) requires(std::is_const_v<T>) {
            if (this != static_cast<void *>(&other)) {
                Set(other);
            }

            return *this;
        }

        [[nodiscard]] constexpr const void *GetThisObject() const noexcept {
            return static_cast<const void *>(this);
        }

        [[nodiscard]] constexpr unsized_field_store *GetStore() const noexcept {
            return reinterpret_cast<unsized_field_store *>(reinterpret_cast<uintptr_t>(GetThisObject()) + field_store_offset);
        }

        [[nodiscard]] constexpr handle_t **GetField() const noexcept {
            return &GetStore()->fields[index];
        }

        [[nodiscard]] constexpr handle_t *Get() const noexcept {
            return *GetField();
        }

        void Set(const root_handle<T> &handle) {
            Set(static_cast<handle_t *>(handle), handle_role::root);
        }

        void Set(const root_handle<std::remove_const_t<T>> &handle) requires(std::is_const_v<T>) {
            Set(static_cast<handle_t *>(handle), handle_role::root);
        }

        template <bool ro>
        void Set(const pin<T, ro> &pin) {
            Set(static_cast<handle_t *>(pin), pin.role);
        }

        template <bool ro>
        void Set(const pin<std::remove_const_t<T>, ro> &pin) requires(std::is_const_v<T>) {
            Set(static_cast<handle_t *>(pin), pin.role);
        }

        template <ptrdiff_t other_field_store_offset, uint16_t other_index>
        void Set(const field<other_field_store_offset, other_index, T> &field) {
            Set(field.Get(), handle_role::field);
        }

        template <ptrdiff_t other_field_store_offset, uint16_t other_index>
        void Set(const field<other_field_store_offset, other_index, std::remove_const_t<T>> &field) requires(std::is_const_v<T>) {
            Set(field.Get(), handle_role::field);
        }

        void Set(handle_t *newValue, const handle_role newValueRole) {
            handle_t *obj = internal::LookupObject(GetThisObject());

            if (obj == nullptr) {
                // either unmanaged or a normal field (not gc::field) of a managed object
                obj = internal::GetOwningObject(GetThisObject());
            }

            if (obj == nullptr) {
                // unmanaged mode
                handle_t *oldValue = Get();
                if (internal::Equals(oldValue, newValue)) {
                    return;
                }

                *GetField() = internal::Copy(newValue, newValueRole, handle_role::root);
                if (oldValue != nullptr) {
                    internal::Destroy(oldValue, handle_role::root);
                }
            }
            else {
                // managed mode
                // or object owning this field is a normal field of a managed object
                internal::SetField(obj, GetField(), newValue, newValueRole);
            }
        }

        [[nodiscard]] bool IsNull() const noexcept {
            return Get() == nullptr;
        }


        constexpr explicit operator handle_t *() const noexcept {
            return Get();
        }

        constexpr operator handle_t **() noexcept {
            return GetField();
        }

        constexpr explicit operator bool() const noexcept {
            return Get() != nullptr;
        }

        ~field() {
            handle_t *obj = internal::LookupObjectOrGetOwningObject(GetThisObject());
            if (obj == nullptr) {
                // unmanaged mode
                internal::Destroy(Get(), handle_role::root);
            }
            else {
                // managed mode or indirectly managed
                internal::SetField(obj, GetField(), nullptr, handle_role::internal_initialization);
            }
        }
    };

    template <class T>
    concept supports_gc_equal = requires(T x)
    {
        static_cast<handle_t*>(x);
    };

    template <supports_gc_equal lhs, supports_gc_equal rhs>
    bool operator==(const lhs &l, const rhs &r) {
        return internal::Equals(static_cast<handle_t*>(l), static_cast<handle_t*>(r));
    }

    template <supports_gc_equal lhs, supports_gc_equal rhs>
    bool operator!=(const lhs &l, const rhs &r) {
        return internal::Equals(static_cast<handle_t*>(l), static_cast<handle_t*>(r));
    }

    template <supports_gc_equal T>
    bool operator==(const T &lhs, std::nullptr_t) {
        return static_cast<handle_t*>(lhs) == nullptr;
    }

    template <supports_gc_equal T>
    bool operator!=(const T &lhs, std::nullptr_t) {
        return static_cast<handle_t*>(lhs) != nullptr;
    }

    template <supports_gc_equal T>
    bool operator==(const T &lhs, null_handle_t) {
        return static_cast<handle_t*>(lhs) == nullptr;
    }

    template <supports_gc_equal T>
    bool operator!=(const T &lhs, null_handle_t) {
        return static_cast<handle_t*>(lhs) != nullptr;
    }

    template <class T>
    pin<T, false> RWPin(const root_handle<T> &handle) {
        return pin<T, false>(handle);
    }

    template <class T>
    pin<T, true> ROPin(const root_handle<T> &handle) {
        return pin<T, true>(handle);
    }

    template <ptrdiff_t field_store_offset, size_t index, class T>
    pin<T, false> RWPin(const field<field_store_offset, index, T> &field) {
        return pin<T, false>(field);
    }

    template <ptrdiff_t field_store_offset, size_t index, class T>
    pin<T, true> ROPin(const field<field_store_offset, index, T> &field) {
        return pin<T, true>(field);
    }

    namespace internal {
        template <class T, bool ro>
        handle_t *ToRootHandle(const pin<T, ro>& pin) {
            return internal::Copy(pin.handle, pin.role, handle_role::root);
        }

        template <ptrdiff_t field_store_offset, size_t index, class T>
        handle_t *ToRootHandle(const field<field_store_offset, index, T>& field) {
            return internal::Copy(field.Get(), handle_role::field, handle_role::root);
        }

        template <class T>
        handle_t *PinRW(const root_handle<T> &handle) {
            return internal::Copy(handle.handle, handle_role::root, handle_role::rw_pin);
        }

        template <ptrdiff_t field_store_offset, size_t index, class T>
        handle_t *PinRW(const field<field_store_offset, index, T> &field) {
            return internal::Copy(field.Get(), handle_role::field, handle_role::rw_pin);
        }

        template <class T>
        handle_t *PinRO(const root_handle<T> &handle) {
            return internal::Copy(handle.handle, handle_role::root, handle_role::ro_pin);
        }

        template <ptrdiff_t field_store_offset, size_t index, class T>
        handle_t *PinRO(const field<field_store_offset, index, T> &field) {
            return internal::Copy(field.Get(), handle_role::field, handle_role::ro_pin);
        }
    }

    // unlike regular field, root and pin handles, the handles here are able to handle polymorphism
    namespace dyn {
        template <class T, class U>
        concept dynamic_castable_from_u_to_t = requires(U *u) {
            dynamic_cast<T*>(u);
        };

        template <class T>
        struct polymorphic_handle {
            tagged_ptr<handle_t, int16_t> handle = nullptr;
            constexpr polymorphic_handle() = default;
            constexpr polymorphic_handle(std::nullptr_t) {}
            constexpr polymorphic_handle(null_handle_t) {}
            // internal
            polymorphic_handle(handle_t *handle) {
                if (handle == nullptr) {
                    return;
                }

                if (!internal::GetType(handle)->operator==(typeid(T))) {
                    throw library_bug("initial handle type mismatch");
                }

                this->handle = tagged_ptr(handle, static_cast<int16_t>(0));
            }
            // internal
            constexpr polymorphic_handle(handle_t *handle, const int16_t offset) : handle(handle, offset) {}

            template <dynamic_castable_from_u_to_t<T> U>
            polymorphic_handle(const polymorphic_handle<U> &other) {
                if (other.IsNull()) {
                    return;
                }

                internal::TemporaryROPin(other.handle.Get());
                U *original = nullptr;
                T *casted = nullptr;
                try {
                    original = other.Get(handle_role::ro_pin);
                    casted = dynamic_cast<T*>(original);
                    internal::TemporaryUnpin(other.handle.Get());
                } catch (...) {
                    internal::TemporaryUnpin(other.handle.Get());
                }

                if (casted == nullptr) {
                    return;
                }

                // const auto diff = reinterpret_cast<intptr_t>(casted)
                //                          - reinterpret_cast<intptr_t>(original)
                //                          + static_cast<intptr_t>(other.handle.GetData());

                const auto d = reinterpret_cast<intptr_t>(casted);
                const auto d2 = d - reinterpret_cast<intptr_t>(original);
                const auto d3 = d2 + static_cast<intptr_t>(other.handle.GetData());
                if (d < d2) {
                    throw std::runtime_error("underflow");
                }
                if (d2 > d3) {
                    throw std::runtime_error("overflow");
                }

                constexpr ptrdiff_t min = std::numeric_limits<int16_t>::min();
                constexpr ptrdiff_t max = std::numeric_limits<int16_t>::max();
                if (d3 < min || d3 > max) {
                    throw std::out_of_range("cannot store offset in int16_t");
                }

                handle = tagged_ptr(other.handle.Get(), static_cast<int16_t>(d3));
            }

            [[nodiscard]] constexpr bool IsNull() const noexcept {
                return handle == nullptr;
            }

            explicit constexpr operator bool() const noexcept {
                return !IsNull();
            }

            template <class U>
            [[nodiscard]] bool operator==(const polymorphic_handle<U> &rhs) const noexcept {
                return internal::Equals(handle.Get(), rhs.handle.Get());
            }

            [[nodiscard]] constexpr bool operator==(std::nullptr_t) const noexcept {
                return IsNull();
            }

            [[nodiscard]] constexpr bool operator==(null_handle_t) const noexcept {
                return IsNull();
            }

            [[nodiscard]] T *Get(const handle_role role) const {
                if (IsNull()) {
                    return nullptr;
                }

                return std::launder(reinterpret_cast<T*>(static_cast<std::byte *>(internal::GetInstance(handle.Get(), role)) + handle.GetData()));
            }

            [[nodiscard]] const std::type_info &GetType() const noexcept {
                if (IsNull()) {
                    return typeid(std::nullptr_t);
                }

                return *internal::GetType(handle.Get());
            }
        };

        template <class T>
        class root_handle;

        template <class T, bool ro = std::is_const_v<T>>
        class pin;

        template <class T>
        class field;

        template <class From>
        polymorphic_handle<void /* replace with the type of To handle */ > GetHandle(const From&) {
            // implement this via template specialization
            // do not execute the internal copying
            return nullptr;
        }

        // TODO: equality operators

        template <class T, bool ro>
        class pin {
            static_assert(ro || !std::is_const_v<T>, "cannot create read-write pin for const T");

            friend auto GetHandle<>(const pin&);
        public:
            constexpr static handle_role role = ro ? handle_role::ro_pin : handle_role::rw_pin;

            using element_type = T;
            using rw_element_type = std::remove_const_t<T>;
            using ro_element_type = std::add_const_t<T>;
            using return_element_type = std::conditional_t<ro, ro_element_type, rw_element_type>;
            using return_pointer_type = std::add_pointer_t<return_element_type>;
        private:
            polymorphic_handle<return_element_type> value{nullptr};
        public:
            pin() = delete;
            pin(const pin &) = delete;
            pin(pin &&) = delete;
            pin &operator=(const pin &) = delete;
            pin &operator=(pin &&) = delete;

            pin(const gc::root_handle<rw_element_type> &handle) : value(handle.handle) {}

            pin(const gc::root_handle<ro_element_type> &handle) requires(ro) : value(handle.handle) {}

            template <ptrdiff_t field_store_offset, size_t index>
            pin(const gc::field<field_store_offset, index, rw_element_type> &handle) : value(handle.handle) {}

            template <ptrdiff_t field_store_offset, size_t index>
            pin(const gc::field<field_store_offset, index, ro_element_type> &handle) requires(ro) : value(handle.handle) {}

            template <class U>
            pin(const root_handle<std::remove_const_t<U>> &handle) {
                value = GetHandle(handle);
                value.handle.Set(internal::Copy(value.handle.Get(), handle_role::root, role));
            }

            template <class U>
            pin(const root_handle<const U> &handle) requires(ro) {
                value = GetHandle(handle);
                value.handle.Set(internal::Copy(value.handle.Get(), handle_role::root, role));
            }

            template <class U>
            pin(const field<std::remove_const_t<U>> &field) {
                value = GetHandle(field);
                value.handle.Set(internal::Copy(value.handle.Get(), handle_role::field, role));
            }

            template <class U>
            pin(const field<const U> &field) requires(ro) {
                value = GetHandle(field);
                value.handle.Set(internal::Copy(value.handle.Get(), handle_role::field, role));
            }

            [[nodiscard]] constexpr bool IsNull() const noexcept {
                return value.IsNull();
            }

            [[nodiscard]] const std::type_info &GetType() const noexcept {
                return value.GetType();
            }

            constexpr operator bool() const noexcept {
                return !IsNull();
            }

            return_pointer_type Get() const {
                return value.Get();
            }

            return_pointer_type operator ->() const {
                return Get();
            }

            std::add_lvalue_reference_t<return_element_type> operator *() const {
                return *Get();
            }

            template <class U>
            [[nodiscard]] bool Equals(const U &rhs) const noexcept {
                return value == GetHandle(rhs);
            }

            [[nodiscard]] bool Equals(std::nullptr_t) const noexcept {
                return IsNull();
            }

            [[nodiscard]] bool Equals(null_handle_t) const noexcept {
                return IsNull();
            }

            template <class U>
            bool operator==(const U &rhs) const noexcept {
                return Equals(rhs);
            }

            template <class U>
            bool operator!=(const U &rhs) const noexcept {
                return !Equals(rhs);
            }

            ~pin() {
                internal::Destroy(value.handle.Get(), role);
            }
        };

        template <class T, bool ro>
        class pin<T[], ro> {
            static_assert(ro || !std::is_const_v<T>, "cannot create read-write pin for const T");

            friend auto GetHandle<>(const pin&);
        public:
            constexpr static handle_role role = ro ? handle_role::ro_pin : handle_role::rw_pin;

            using element_type = T;
            using rw_element_type = std::remove_const_t<T>;
            using ro_element_type = std::add_const_t<T>;
            using return_element_type = std::conditional_t<ro, ro_element_type, rw_element_type>;
            using return_pointer_type = std::add_pointer_t<return_element_type>;
        private:
            polymorphic_handle<return_element_type> value{nullptr};
        public:
            pin() = delete;
            pin(const pin &) = delete;
            pin(pin &&) = delete;
            pin &operator=(const pin &) = delete;
            pin &operator=(pin &&) = delete;

            pin(const gc::root_handle<rw_element_type[]> &handle) : value(handle.handle) {}

            pin(const gc::root_handle<ro_element_type[]> &handle) requires(ro) : value(handle.handle) {}

            template <ptrdiff_t field_store_offset, size_t index>
            pin(const gc::field<field_store_offset, index, rw_element_type[]> &handle) : value(handle.handle) {}

            template <ptrdiff_t field_store_offset, size_t index>
            pin(const gc::field<field_store_offset, index, ro_element_type[]> &handle) requires(ro) : value(handle.handle) {}

            template <class U>
            pin(const root_handle<std::remove_const_t<U>> &handle) {
                value = GetHandle(handle);
                value.handle.Set(internal::Copy(value.handle.Get(), handle_role::root, role));
            }

            template <class U>
            pin(const root_handle<const U> &handle) requires(ro) {
                value = GetHandle(handle);
                value.handle.Set(internal::Copy(value.handle.Get(), handle_role::root, role));
            }

            template <class U>
            pin(const field<std::remove_const_t<U>> &field) {
                value = GetHandle(field);
                value.handle.Set(internal::Copy(value.handle.Get(), handle_role::field, role));
            }

            template <class U>
            pin(const field<const U> &field) requires(ro) {
                value = GetHandle(field);
                value.handle.Set(internal::Copy(value.handle.Get(), handle_role::field, role));
            }

            [[nodiscard]] constexpr bool IsNull() const noexcept {
                return value.IsNull();
            }

            [[nodiscard]] const std::type_info &GetType() const noexcept {
                return value.GetType();
            }

            constexpr operator bool() const noexcept {
                return !IsNull();
            }

            [[nodiscard]] size_t Count() const {
                return internal::GetMemberCount(value.handle.Get());
            }

            return_pointer_type Get() const {
                return value.Get();
            }

            return_pointer_type operator ->() const {
                return Get();
            }

            std::add_lvalue_reference_t<return_element_type> operator *() const {
                return *Get();
            }

            std::add_lvalue_reference_t<element_type> operator[](const size_t index) const {
                return Get()[index];
            }

            template <class U>
            [[nodiscard]] bool Equals(const U &rhs) const noexcept {
                return value == GetHandle(rhs);
            }

            [[nodiscard]] bool Equals(std::nullptr_t) const noexcept {
                return IsNull();
            }

            [[nodiscard]] bool Equals(null_handle_t) const noexcept {
                return IsNull();
            }

            template <class U>
            bool operator==(const U &rhs) const noexcept {
                return Equals(rhs);
            }

            template <class U>
            bool operator!=(const U &rhs) const noexcept {
                return !Equals(rhs);
            }

            ~pin() {
                internal::Destroy(value.handle.Get(), role);
            }
        };

        template <class T>
        class root_handle {
            friend auto GetHandle<>(const root_handle&);

            polymorphic_handle<T> value{nullptr};

            template <class U>
            void Assign(polymorphic_handle<U> newValue, const handle_role newValueRole) {
                if (newValue == value) {
                    return;
                }

                auto oldHandle = value.handle.Get();
                value = polymorphic_handle<T>(newValue);
                // actually copy the handle
                value.handle.Set(internal::Copy(value.handle.Get(), newValueRole, handle_role::root));
                internal::Destroy(oldHandle, handle_role::root);
            }

        public:
            root_handle() = default;
            root_handle(std::nullptr_t) {}
            root_handle(null_handle_t) {}

            root_handle(const gc::root_handle<T> &other) {
                Assign(other.handle, handle_role::root);
            }

            template <bool ro>
            root_handle(gc::pin<T, ro> &&pin) {
                Assign(pin.handle, pin.role);
            }

            template <bool ro>
            root_handle(gc::pin<std::remove_const_t<T>, ro> &&pin) requires(std::is_const_v<T>) {
                Assign(pin.handle, pin.role);
            }

            template <ptrdiff_t field_store_offset, size_t index>
            root_handle(const gc::field<field_store_offset, index, T> &field) {
                Assign(field.handle, handle_role::field);
            }

            root_handle(const root_handle &other) {
                Assign(other.value, handle_role::root);
            }

            template <class U>
            root_handle(const root_handle<U> &other) {
                Assign(other.value, handle_role::root);
            }

            template <class U, bool ro>
            root_handle(pin<U, ro> &&pin) {
                Assign(GetHandle(pin), pin.role);
            }

            template <class U>
            root_handle(const field<U> &field) {
                Assign(GetHandle(field), handle_role::field);
            }

            root_handle(root_handle &&other) noexcept /* won't throw if there are no bugs */ {
                Assign(other.value, handle_role::root);
                other.Assign(nullptr, handle_role::root);
            }

            template <class U>
            root_handle(root_handle<U> &&other) {
                Assign(other.value, handle_role::root);
                other.Assign(nullptr, handle_role::root);
            }

            root_handle &operator=(std::nullptr_t) {
                Assign(nullptr, handle_role::internal_initialization);
                return *this;
            }

            root_handle &operator=(null_handle_t) {
                Assign(nullptr, handle_role::internal_initialization);
                return *this;
            }

            root_handle &operator=(const root_handle &other) {
                if (&other == this) {
                    return *this;
                }

                Assign(other.value, handle_role::root);
                return *this;
            }

            template <class U>
            root_handle &operator=(const root_handle<U> &other) {
                Assign(other.value, handle_role::root);
                return *this;
            }

            template <class U, bool ro>
            root_handle &operator=(pin<U, ro> &&pin) {
                Assign(GetHandle(pin), pin.role);
                return *this;
            }

            template <class U>
            root_handle &operator=(const field<U> &field) {
                Assign(GetHandle(field), handle_role::field);
                return *this;
            }

            root_handle &operator=(root_handle &&other) noexcept /* won't throw if there are no bugs */ {
                Assign(other.value, handle_role::root);
                other.Assign(nullptr, handle_role::root);
                return *this;
            }

            template <class U>
            root_handle &operator=(root_handle<U> &&other) {
                Assign(other.value, handle_role::root);
                other.Assign(nullptr, handle_role::root);
                return *this;
            }

            [[nodiscard]] constexpr bool IsNull() const noexcept {
                return value.IsNull();
            }

            [[nodiscard]] const std::type_info &GetType() const noexcept {
                return value.GetType();
            }

            constexpr operator bool() const noexcept {
                return !IsNull();
            }

            template <class U>
            [[nodiscard]] bool Equals(const U &rhs) const noexcept {
                return value == GetHandle(rhs);
            }

            [[nodiscard]] bool Equals(std::nullptr_t) const noexcept {
                return IsNull();
            }

            [[nodiscard]] bool Equals(null_handle_t) const noexcept {
                return IsNull();
            }

            template <class U>
            bool operator==(const U &rhs) const noexcept {
                return Equals(rhs);
            }

            template <class U>
            bool operator!=(const U &rhs) const noexcept {
                return !Equals(rhs);
            }

            pin<T, true> Pin() const {
                return pin<T, true>(*this);
            }

            pin<T, false> Pin() requires(!std::is_const_v<T>) {
                return pin<T, false>(*this);
            }

            ~root_handle() {
                internal::Destroy(value.handle.Get(), handle_role::root);
            }
        };

        template <class T>
        class field {
            friend auto GetHandle<>(const field&);

            handle_t *const obj{nullptr}; // the object this field belongs to
            handle_t *value{nullptr}; // the value of the field
            uint16_t offset{0}; // cannot use polymorphic_handle here because fields neet to expose a handle_t**

            template <class U>
            void Assign(polymorphic_handle<U> newValue, const handle_role newValueRole) {
                polymorphic_handle<T> handle(newValue);
                offset = handle.handle.GetData();
                if (obj == nullptr) {
                    // unmanaged
                    value = internal::Copy(newValue.handle.Get(), newValueRole, handle_role::root);
                }
                else {
                    // managed
                    internal::SetField(obj, &value, newValue.handle.Get(), newValueRole);
                }
            }

            constexpr polymorphic_handle<T> GetDynamicHandle() const noexcept {
                return polymorphic_handle<T>(value, offset);
            }

            [[nodiscard]] constexpr handle_role GetHandleRole() const noexcept {
                return obj == nullptr ? handle_role::root : handle_role::field;
            }
        public:
            field() noexcept : obj(internal::LookupObjectOrGetOwningObject(this)) {}
            field(std::nullptr_t) noexcept : field() {}
            field(null_handle_t) noexcept : field() {}

            field(const gc::root_handle<T> &handle) : field() {
                Assign(handle.handle, handle_role::root);
            }

            template <bool ro>
            field(gc::pin<T, ro> &&pin) : field() {
                Assign(pin.handle, pin.role);
            }

            template <bool ro>
            field(gc::pin<std::remove_const_t<T>, ro> &&pin) requires(std::is_const_v<T>) : field() {
                Assign(pin.handle, pin.role);
            }

            template <ptrdiff_t field_store_offset, size_t index>
            field(const gc::field<field_store_offset, index, T> &other) : field() {
                Assign(other.handle, handle_role::field);
            }

            template <class U>
            field(const root_handle<U> &handle) : field() {
                Assign(GetHandle(handle), handle_role::root);
            }

            template <class U, bool ro>
            field(pin<U, ro> &&pin) : field() {
                Assign(GetHandle(pin), pin.role);
            }

            field(const field &other) : field() {
                Assign(other.GetDynamicHandle(), other.GetHandleRole());
            }

            template <class U>
            field(const field<U> &other) : field() {
                Assign(other.GetDynamicHandle(), other.GetHandleRole());
            }

            field(field &&other) noexcept /* Assign could throw, but shouldn't if everything is correct */ : field() {
                Assign(other.GetDynamicHandle(), other.GetHandleRole());
                other.Assign(nullptr, handle_role::field);
            }

            template <class U>
            field(field<U> &&other) : field() {
                Assign(other.GetDynamicHandle(), other.GetHandleRole());
                other.Assign(nullptr, handle_role::field);
            }

            field &operator=(std::nullptr_t) {
                Assign(nullptr, handle_role::internal_initialization);
                return *this;
            }

            field &operator=(null_handle_t) {
                Assign(nullptr, handle_role::internal_initialization);
                return *this;
            }

            template <class U>
            field &operator=(const root_handle<U> &handle) {
                Assign(GetHandle(handle), handle_role::root);
                return *this;
            }

            template <class U, bool ro>
            field &operator=(pin<U, ro> &&pin) {
                Assign(GetHandle(pin), pin.role);
                return *this;
            }

            field &operator=(const field &other) {
                if (&other == this) {
                    return *this;
                }

                Assign(other.GetDynamicHandle(), other.GetHandleRole());
                return *this;
            }

            template <class U>
            field &operator=(const field<U> &other) {
                Assign(other.GetDynamicHandle(), other.GetHandleRole());
                return *this;
            }

            field &operator=(field &&other) noexcept /* Assign could throw, but shouldn't if everything is correct */ {
                if (&other == this) {
                    return *this;
                }

                Assign(other.GetDynamicHandle(), other.GetHandleRole());
                other.Assign(nullptr, handle_role::field);
                return *this;
            }

            template <class U>
            field &operator=(field<U> &&other) {
                Assign(other.GetDynamicHandle(), other.GetHandleRole());
                other.Assign(nullptr, handle_role::field);
                return *this;
            }

            [[nodiscard]] constexpr bool IsNull() const noexcept {
                return value == nullptr;
            }

            [[nodiscard]] const std::type_info &GetType() const noexcept {
                return GetDynamicHandle().GetType();
            }

            constexpr operator bool() const noexcept {
                return !IsNull();
            }

            template <class U>
            [[nodiscard]] bool Equals(const U &rhs) const noexcept {
                return value == GetHandle(rhs);
            }

            [[nodiscard]] bool Equals(std::nullptr_t) const noexcept {
                return IsNull();
            }

            [[nodiscard]] bool Equals(null_handle_t) const noexcept {
                return IsNull();
            }

            template <class U>
            bool operator==(const U &rhs) const noexcept {
                return Equals(rhs);
            }

            template <class U>
            bool operator!=(const U &rhs) const noexcept {
                return !Equals(rhs);
            }

            pin<T, true> Pin() const {
                return pin<T, true>(*this);
            }

            pin<T, false> Pin() requires(!std::is_const_v<T>) {
                return pin<T, false>(*this);
            }

            ~field() {
                if (obj == nullptr) {
                    // unmanaged
                    internal::Destroy(value, handle_role::root);
                }
                else {
                    // managed
                    internal::SetField(obj, &value, nullptr, handle_role::field);
                }
            }
        };

        template <class T>
        polymorphic_handle<T> GetHandle(const root_handle<T> &handle) noexcept {
            return handle.value;
        }

        template <class T>
        polymorphic_handle<T> GetHandle(const field<T> &field) noexcept {
            return field.GetDynamicHandle();
        }

        template <class T, bool ro>
        polymorphic_handle<typename pin<T, ro>::return_element_type> GetHandle(const pin<T, ro> &pin) noexcept {
            return pin.value;
        }
    }

    // assumes object has no gc fields
    template <class T>
    constexpr inline get_field_count_fn default_get_field_count = [](auto...) noexcept {return 0ULL;};

    // assumes that object has no gc fields, so every index is invalid
    template <class T>
    constexpr inline get_field_fn default_get_field = [](auto...) noexcept -> handle_t** { return nullptr; };

    template <class T>
    constexpr inline move_fn default_move_for_movable = [](void *obj, void *newLocation) noexcept {
        using type = std::remove_const_t<T>; // T might be const, but, we can move it because we know it is allocated by us
        auto from = std::launder(const_cast<type*>(static_cast<T*>(obj)));
        new (newLocation) T(std::move(*from));
    };

    template <class T>
    constexpr inline move_fn default_move_for_immovable = nullptr;

    template <class T>
    constexpr move_fn GetDefaultMoveFn() noexcept {
        if constexpr (std::is_nothrow_move_constructible_v<T>) {
            return default_move_for_movable<T>;
        }
        else {
            return default_move_for_immovable<T>;
        }
    }

    // move constructor if T has a move constructor that doesn't throw and T instances are considered immovable otherwise
    template <class T>
    constexpr inline move_fn default_move = GetDefaultMoveFn<T>();

    template <class T>
    constexpr inline destructor_fn default_destroy = [](void *obj) noexcept {
        using type = std::remove_const_t<T>;
        std::destroy_at(std::launder(const_cast<type*>(static_cast<T*>(obj))));
    };

    // make a specialization of this where this is set to true
    template <class>
    struct is_gc_supported {
        static constexpr bool supported = false;
    };

    template <class T>
    constexpr inline bool is_gc_supported_v = is_gc_supported<T>::supported;

    // when you only need to override a set of functions
    template <class T>
    struct partial_gc_object_traits {
        using type = T; // that's all you are required to define
    };

    template <class Traits>
    concept defines_get_field_count = requires() { Traits::get_field_count; };

    template <class Traits>
    concept defines_get_field = requires() { Traits::get_field; };

    template <class Traits>
    concept defines_move = requires() { Traits::move; };

    template <class Traits>
    concept defines_destructor = requires() { Traits::destructor; };

    template <class Traits>
    concept defines_type = requires() { typename Traits::type; };

    // chooses the implementation in Traits if it is defined and the default implementation otherwise
    template <defines_type Traits>
    struct gc_object_traits {
    private:
        static constexpr get_field_count_fn ChooseGetFieldCount() noexcept {
            if constexpr (defines_get_field_count<Traits>) {
                return Traits::get_field_count;
            }
            else {
                return default_get_field_count<typename Traits::type>;
            }
        }

        static constexpr get_field_fn ChooseGetField() noexcept {
            if constexpr (defines_get_field<Traits>) {
                return Traits::get_field;
            }
            else {
                return default_get_field<typename Traits::type>;
            }
        }

        static constexpr move_fn ChooseMove() noexcept {
            if constexpr (defines_move<Traits>) {
                return Traits::move;
            }
            else {
                return default_move<typename Traits::type>;
            }
        }

        static constexpr destructor_fn ChooseDestructor() noexcept {
            if constexpr (defines_destructor<Traits>) {
                return Traits::destructor;
            }
            else {
                return default_destroy<typename Traits::type>;
            }
        }
    public:
        using type = Traits::type;
        static constexpr get_field_count_fn get_field_count = ChooseGetFieldCount();
        static constexpr get_field_fn get_field = ChooseGetField();
        static constexpr move_fn move = ChooseMove();
        static constexpr destructor_fn destructor = ChooseDestructor();
        static constexpr bool supported = is_gc_supported_v<type>;
    };

    template <class T>
    concept gc_supported = is_gc_supported_v<T>;

    /**
     * @brief Allocates enough space for an instance of type @tp T and returns a handle to it
     * @tparam T The type to allocate an instance for
     * @tparam Traits The traits of type @tp T
     * @return A handle to the allocated instance
     * @throws ... Anything that the allocator or the containers might throw
     * @note The allocated space is uninitialized, call Initialize to actually construct the instance
     */
    template <gc_supported T, class Traits = partial_gc_object_traits<T>>
    root_handle<T> Allocate() {
        using object_traits = gc_object_traits<Traits>;

        return root_handle<T>(internal::New(
            sizeof(T),
            alignof(T),
            1,
            &typeid(T),
            object_traits::destructor,
            object_traits::get_field_count,
            object_traits::get_field,
            object_traits::move
        ));
    }

    /**
     * Allocates a continuous array of @tp T instances without initializing it
     * @tparam T The type to allocate instances for
     * @tparam Traits The traits of type @tp T
     * @param count The length of the array
     * @return A handle to the allocated array
     * @throws ... Anything that the allocator or the containers might throw
     * @note The allocated instances are uninitialized. Call initialize to construct them
     */
    template <gc_supported T, class Traits = partial_gc_object_traits<T>>
    root_handle<T[]> Allocate(const size_t count) {
        using object_traits = gc_object_traits<Traits>;

        return root_handle<T>(internal::New(
            sizeof(T),
            alignof(T),
            count,
            &typeid(T),
            object_traits::destructor,
            object_traits::get_field_count,
            object_traits::get_field,
            object_traits::move
        ));
    }

    /**
     * @brief Try to initialize an uninitialized instance
     * @tparam T The type of the instance
     * @tparam Args The types of the arguments to pass into the constructor of the instance
     * @param instance A handle to the uninitialized instance
     * @param args The arguments to pass into the constructor of the instance
     * @return Whether the initialization attempt was valid
     * @note If the instance is already or is being initialized, then the call does not initialize the instance a second
     * time and returns false
     * @throws ... Anything that the constructor of @tp T with @tp Args could throw
     */
    template <class T, class... Args>
    bool Initialize(pin<T, false> instance, Args&&... args) {
        if (!internal::TryAcquireInitializeRight(instance.handle)) {
            return false;
        }

        try {
            std::construct_at(instance.Get(), std::forward<Args>(args)...);
            return true;
        } catch (...) {
            internal::SetInitializationFailed(instance.handle);
            throw;
        }
    }

    /**
     * @brief Initialize each member of the array with @p initMember
     * @tparam T The type of the instances in the array
     * @tparam InitMemberFn A callable that takes in a @tp T pointer to the member and the index of the member (size_t)
     * @param instance The uninitialized array instance
     * @param initMember The initializer function
     * @return Whether the initialization attempt was valid
     * @note If the instance is already or is being initialized, then the call does not initialize the instance a second
     * time and returns false
     * @throws ... Anything that the @p initMember could throw
     */
    template <class T, class InitMemberFn>
    bool Initialize(pin<T[], false> instance, InitMemberFn&& initMember) {
        if (!internal::TryAcquireInitializeRight(instance.handle)) {
            return false;
        }

        try {
            T array[] = instance.Get();
            const size_t count = instance.Count();

            for (size_t i = 0; i < count; ++i) {
                initMember(&array[i], i);
            }

            return true;
        } catch (...) {
            internal::SetInitializationFailed(instance.handle);
            throw;
        }
    }

    /**
     * Initialize each member of the array with a value
     * @tparam T The type of the array members
     * @param instance The array
     * @param defaultValue The value to initialize the members with
    * @return Whether the initialization attempt was valid
     * @note If the instance is already or is being initialized, then the call does not initialize the instance a second
     * time and returns false
     * @throws ... Anything that the copy constructor of @tp T could throw
     */
    template <class T>
    bool Initialize(pin<T[], false> instance, const T& defaultValue) {
        if (!internal::TryAcquireInitializeRight(instance.handle)) {
            return false;
        }

        try {
            T array[] = instance.Get();
            const size_t count = instance.Count();

            for (size_t i = 0; i < count; ++i) {
                array[i] = defaultValue;
            }

            return true;
        } catch (...) {
            internal::SetInitializationFailed(instance.handle);
            throw;
        }
    }

    /**
      * @brief Allocates and initializes an instance of type @tp T
      * @tparam T The type of the instance
      * @tparam Traits The traits of type @tp T
      * @tparam Args The types of the arguments to pass use to construct the allocated instance
      * @param args The arguments to pass use to construct the allocated instance
      * @return A handle to the initialized instance
      * @throws ... Anything that the allocator or the containers might throw
      */
     template <gc_supported T, class Traits = partial_gc_object_traits<T>, class... Args>
     root_handle<T> New(Args&&... args) {
        auto handle = Allocate<T, Traits>();
        Initialize(RWPin(handle), std::forward<Args>(args)...);
        return handle;
    }

     /**
     * @brief Allocates an array of @tp T instances and initializes each member using @p initMember
     * @tparam T The type of the array members
     * @tparam Traits The traits of type @tp T
     * @tparam InitMemberFn The type of the initializer function. Takes in a pointer to an array member (@tp T*) and the
     * index of the member (size_t)
     * @param count The length of the array
     * @param initMember The initializer function for the array members
     * @return A handle to the initialized array instance
     * @throws ... Anything that the allocator, the containers and @p initMember could throw
     */
    template <gc_supported T, class Traits = partial_gc_object_traits<T>, class InitMemberFn>
    root_handle<T[]> New(const size_t count, InitMemberFn &&initMember) {
        auto handle = Allocate<T, Traits>(count);
        Initialize(RWPin(handle), std::forward<InitMemberFn>(initMember));
        return handle;
    }

    /**
     * @biref Allocates an array of @tp T instances and initializes each member to the value of @p defaultValue
     * @tparam T The type of the array members
     * @tparam Traits The traits of type @tp T
     * @param count The length of the array
     * @param defaultValue The value to initialize the array members to
     * @return A handle to the initialized array instance
     * @throws ... Anything that the allocator, the containers and the copy constructor of @tp T could throw
     */
    template <gc_supported T, class Traits = partial_gc_object_traits<T>>
    root_handle<T[]> New(const size_t count, const T& defaultValue) {
        auto handle = Allocate<T, Traits>(count);
        Initialize(RWPin(handle), defaultValue);
        return handle;
    }

    /**
     * @brief Allocates an array of @tp T instances and initializes each member using the default constructor
     * @tparam T The type of the members
     * @tparam Traits The traits of type @tp T
     * @param count The length of the array
     * @return A handle to the initialized array
     * @throws ... Anything that the allocator, the containers and the default constructor of @tp T could throw
     */
    template <gc_supported T, class Traits = partial_gc_object_traits<T>>
    root_handle<T[]> New(const size_t count) {
        return New<T, Traits>(count, [](T* member, size_t) {
            std::construct_at(member);
        });
    }
}
