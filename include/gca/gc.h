#pragma once
#include <memory>
#include <typeindex>
#include <utility>

#include "gc.h"

namespace gc {
    typedef struct internal_handle handle_t;
    typedef struct gc_allocator allocator_handle_t;
    enum class handle_role { unknown, root, field, ro_pin, rw_pin }; // unknown is to create a root handle internally

    allocator_handle_t *GetNullAllocator();
    allocator_handle_t *GetDefaultAllocator();

    struct gc_init_args {
        allocator_handle_t *allocator = GetNullAllocator();
        gc_init_args() noexcept = default;
        explicit gc_init_args(allocator_handle_t *allocator) : allocator(allocator) {
            if (allocator == nullptr) {
                throw bad_api_usage("allocator cannot be nullptr (use GetNullAllocatorInstead)");
            }
        }
    };


    bool Init(const gc_init_args *args /* can be nullptr */ );
    inline bool Init(const gc_init_args& args) { return Init(&args);}
    inline bool Init() { return Init(nullptr); }
    void Collect(bool defragment = false);
    void Destroy();

    using destructor_fn = void (*)(void *) noexcept; // too lazy to deal with throwing destructors and all the possible edge-cases it could present
    using get_field_count_fn = size_t (*)(const void *obj) noexcept; // just return an invalid index
    using get_field_fn = handle_t** (*)(void *obj, size_t index) noexcept; // return nullptr for invalid index
    using move_fn = void (*)(void *obj, void *newLocation) noexcept; // too lazy to handle one move throwing while moving an array allocation

    // AKA, functions you are not supposed to use
    namespace internal {
        handle_t *Copy(handle_t *src, handle_role srcRole, handle_role dstRole);
        handle_t *SetField(handle_t *obj, handle_t **field, handle_t *newValue, handle_role newValueRole);

        bool Equals(const handle_t *lhs, const handle_t *rhs);

        void Destroy(handle_t *handle, handle_role role);
        void *GetInstance(const handle_t *handle, handle_role role); // should only be called with a pin(ro/rw) role
        size_t GetMemberCount(const handle_t *array);
        handle_t *LookupObject(const void *obj) noexcept;
        handle_t *GetOwningObject(const void *obj) noexcept;
        handle_t *LookupObjectOrGetOwningObject(const void *obj) noexcept;

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

        bool TryAcquireInitializeRight(handle_t *handle);
        void SetInitializationFailed(handle_t *handle);
    }

    struct null_handle_t {
        constexpr explicit null_handle_t(std::nullptr_t) noexcept {}
    };

    inline constexpr null_handle_t null_handle { nullptr };

    template <class T>
    struct root_handle;

    template <ptrdiff_t field_store_offset, size_t index, class T>
    struct  field;

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
        root_handle(handle_t *handle) : handle(internal::Copy(handle, handle_role::unknown, handle_role::root)) {} // internal

        root_handle(const root_handle &other) : handle(internal::Copy(other.handle, handle_role::root, handle_role::root)) {}
        root_handle(const root_handle<std::remove_const_t<T>> &other) requires(std::is_const_v<T>) : handle(internal::Copy(other.handle, handle_role::root, handle_role::root)) {}
        root_handle &operator=(const root_handle &other) {
            if (&other == this) {
                return *this;
            }
            handle = internal::Copy(handle, handle_role::root, handle_role::root);
            return *this;
        }

        constexpr root_handle(root_handle &&other) noexcept : handle(std::move(other.handle)) {}
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

            return static_cast<return_pointer_type>(internal::GetInstance(handle, role));
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
        pin(const root_handle<rw_element_type> &handle) : handle(ro ? PinRO(handle) : PinRW(handle)) {}
        template <ptrdiff_t field_store_offset, size_t index>
        pin(const field<field_store_offset, index, rw_element_type> &field) : handle(ro ? PinRO(field) : PinRW(field)) {}
        pin(const root_handle<ro_element_type> &handle) : handle(PinRO(handle)) {}
        template <ptrdiff_t field_store_offset, size_t index>
        pin(const field<field_store_offset, index, ro_element_type> &field) : handle(PinRO(field)) {}

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

        size_t Count() const {
            return internal::GetMemberCount(handle);
        }

        return_pointer_type Get() const {
            if (handle == nullptr) {
                return nullptr;
            }

            return static_cast<return_pointer_type>(internal::GetInstance(handle, role));
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
            Set(nullptr, handle_role::unknown);
        }

        field(const field &other) { Set(other); }
        field& operator=(const field &other) { if (&other != this) { Set(other); } return *this; }
        field(field &&other) {auto oldValue = Get(); Set(other); other.Set(oldValue, handle_role::field); }
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
            Set(nullptr, handle_role::unknown);
            return *this;
        }

        field &operator=(null_handle_t) {
            Set(nullptr, handle_role::unknown);
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
                // managed mode or indicrectly managed
                internal::SetField(obj, GetField(), nullptr, handle_role::unknown);
            }
        }
    };

    // relies on 64 bit pointers and 48-bit canonical space
    template <class T, class U=uint16_t>
    struct tagged_ptr {
        static constexpr uint8_t canonical_address_bits = 48;
        static constexpr uint64_t pointer_mask = 0x0000FFFFFFFFFFFF;

        T *ptr = nullptr;
        constexpr tagged_ptr() noexcept = default;

        constexpr tagged_ptr(std::nullptr_t) noexcept {} // already initialized to that

        constexpr tagged_ptr(T *p) noexcept : ptr(p) {}

        constexpr tagged_ptr(T *p, U data={}) noexcept : ptr(p) {
            Pack(data);
        }

        constexpr void Pack(const U data) noexcept requires(!std::is_same_v<U, uint16_t>) {
            Pack(std::bit_cast<uint16_t>(data));
        }

        constexpr void Pack(const uint16_t data) noexcept {
            ptr = reinterpret_cast<T *>((static_cast<uint64_t>(data) << canonical_address_bits) |
                                        (reinterpret_cast<uint64_t>(ptr) & pointer_mask));
        }

        constexpr U GetData() const noexcept {
            return std::bit_cast<U>(static_cast<uint16_t>(reinterpret_cast<uint64_t>(ptr) >> canonical_address_bits));
        }

        constexpr T *Get() const noexcept {
            auto masked = reinterpret_cast<uint64_t>(ptr) & pointer_mask;
#if __x86_64__ || _M_X64
            // set all top bits to ones if 47th bit is enabled
            // (sign-extending)
            constexpr uint64_t sign_bit = 1ULL << 47ULL;
            constexpr uint64_t sign_extension_bits = 0xFFFF000000000000ULL;
            if (masked & sign_bit) {
                masked |= sign_extension_bits;
            }

            return reinterpret_cast<T *>(masked);
#endif
        }

        constexpr void Set(T* ptr) noexcept {
            uint16_t data = GetData();
            this->ptr = ptr;
            Pack(data);
        }

        constexpr T* operator->() const noexcept {
            return Get();
        }

        constexpr std::add_lvalue_reference_t<T> operator*() const noexcept {
            return *Get();
        }

        constexpr bool operator==(const tagged_ptr &other) const noexcept {
            return Get() == other.Get();
        }

        constexpr bool operator!=(const tagged_ptr &other) const noexcept {
            return Get() != other.Get();
        }

        constexpr bool operator==(std::nullptr_t) const noexcept {
            return Get() == nullptr;
        }

        constexpr bool operator!=(std::nullptr_t) const noexcept {
            return Get() != nullptr;
        }

        constexpr operator bool() const noexcept {
            return static_cast<bool>(Get());
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

    template <class T>
    struct gc_object_traits {
        static constexpr get_field_count_fn get_field_count = {}; // implement this
        static constexpr get_field_fn get_field = {}; // and this
        static constexpr move_fn move = std::is_move_constructible_v<T>
        ? [](void *obj, void *newLocation) {
            std::construct_at(static_cast<T*>(newLocation), std::move<T>(*static_cast<T*>(obj)));
        }
        : nullptr; // this one can be nullptr if move construction is not supported for T
        // in this case, the allocated object will be pinned at all times
        static constexpr destructor_fn destructor = [](void *obj) noexcept {
            static_assert(std::is_nothrow_destructible_v<T>);
            std::destroy_at(static_cast<T*>(obj));
        }; // and this
        static constexpr bool supported = false; // and set this to true
    };

    template <class T>
    concept gc_supported = gc_object_traits<T>::supported;

    template <gc_supported T>
    root_handle<T> Allocate() {
        return root_handle<T>(internal::New(
            sizeof(T),
            alignof(T),
            1,
            &typeid(T),
            gc_object_traits<T>::destructor,
            gc_object_traits<T>::get_field_count,
            gc_object_traits<T>::get_field,
            gc_object_traits<T>::move
        ));
    }

    template <gc_supported T>
    root_handle<T[]> Allocate(const size_t count) {
        return root_handle<T>(internal::New(
            sizeof(T),
            alignof(T),
            count,
            &typeid(T),
            gc_object_traits<T>::destructor,
            gc_object_traits<T>::get_field_count,
            gc_object_traits<T>::get_field,
            gc_object_traits<T>::move
        ));
    }

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

     template <gc_supported T, class... Args>
     root_handle<T> New(Args&&... args) {
        auto handle = Allocate<T>();
        Initialize(RWPin(handle), std::forward<Args>(args)...);
        return handle;
    }

    template <gc_supported T, class InitMemberFn>
    root_handle<T[]> New(const size_t count, InitMemberFn &&initMember) {
        auto handle = Allocate<T>(count);
        Initialize(RWPin(handle), std::forward<InitMemberFn>(initMember));
        return handle;
    }

    template <gc_supported T>
    root_handle<T[]> New(const size_t count, const T& defaultValue) {
        auto handle = Allocate<T>(count);
        Initialize(RWPin(handle), defaultValue);
        return handle;
    }

    template <gc_supported T>
    root_handle<T[]> New(const size_t count) {
        return New<T>(count, T{});
    }
}
