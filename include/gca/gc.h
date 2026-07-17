#pragma once
#include <memory>
#include <typeindex>
#include <utility>

namespace gc {
    typedef struct internal_handle handle_t;
    enum class handle_role { unknown, root, field, ro_pin, rw_pin }; // unknown is to create a root handle internally

    struct gc_init_args {
        std::pmr::memory_resource *objectMemory, *backingMemory;
    };

    bool Init(const gc_init_args *args /* can be nullptr */ );
    inline bool Init(const gc_init_args& args) { return Init(&args);}
    inline bool Init() { return Init(nullptr); }
    void Destroy();

    handle_t *Copy(handle_t *src, handle_role srcRole, handle_role dstRole);

    bool Equals(const handle_t *lhs, const handle_t *rhs);

    void Destroy(handle_t *handle, handle_role role);
    void *GetInstance(const handle_t *handle, handle_role role); // should only be called with a pin(ro/rw) role

    void Collect(bool defragment = false);

    using destructor_fn = void (*)(void *) noexcept; // too lazy to deal with throwing destructors and all the possible edge-cases it could present
    using get_field_count_fn = size_t (*)(const void *obj) noexcept; // just return an invalid index
    using get_field_fn = handle_t** (*)(void *obj, size_t index) noexcept; // return nullptr for invalid index
    using move_fn = void (*)(void *obj, void *newLocation) noexcept; // too lazy to handle one move throwing while moving an array allocation

    handle_t *New(
        size_t size,
        size_t alignment,
        size_t count,
        std::type_index type,
        destructor_fn destructor,
        get_field_count_fn getFieldCount,
        get_field_fn getField,
        move_fn move
    );

    bool TryAcquireInitializeRight(handle_t *handle);
    void SetInitializationFailed(handle_t *handle);

    struct null_handle_t {
        constexpr explicit null_handle_t(std::nullptr_t) noexcept {}
    };

    inline constexpr null_handle_t null_handle { nullptr };

    template <class T>
    struct root_handle;

    template <class T>
    struct  field;

    template <class T>
    struct rw_pin;

    template <class T>
    struct ro_pin;

    template <class T>
    handle_t *ToRootHandle(const ro_pin<T>& pin);

    template <class T>
    handle_t *ToRootHandle(const rw_pin<T>& pin);

    template <class T>
    handle_t *ToRootHandle(const field<T>& field);

    template <class T>
    handle_t *ToField(const root_handle<T> &handle);

    template <class T>
    handle_t *ToField(const ro_pin<T> &pin);

    template <class T>
    handle_t *ToField(const rw_pin<T> &pin);

    template <class T>
    handle_t *PinRW(const root_handle<T> &handle);

    template <class T>
    handle_t *PinRW(const field<T> &field);

    template <class T>
    handle_t *PinRO(const root_handle<T> &handle);

    template <class T>
    handle_t *PinRO(const field<T> &field);

    template <class T>
    struct field {
        handle_t *handle = nullptr;
        constexpr field() noexcept = default;
        constexpr field(std::nullptr_t) noexcept {}
        constexpr field(null_handle_t) noexcept {}
        field(const root_handle<T> &handle) : handle(ToField(handle)) {}
        field(const ro_pin<T> &pin) : handle(ToField(pin)) {}
        field(const rw_pin<T> &pin) : handle(ToField(pin)) {}

        field(const field &other) : handle(Copy(other.handle, handle_role::field, handle_role::field)) {}
        field &operator=(const field &other) {
            if (&other == this) {
                return *this;
            }

            handle = Copy(other.handle, handle_role::field, handle_role::field);
            return *this;
        }

        field(field &&other) noexcept : handle(std::move(other.handle)) {}
        field &operator=(field &&other) noexcept {
            if (&other == this) {
                return *this;
            }

            handle = std::exchange(other.handle, handle);
            return *this;
        }

        constexpr bool IsNull() const noexcept {
            return handle == nullptr;
        }

        constexpr explicit operator handle_t *() const noexcept {
            return handle;
        }

        constexpr explicit operator bool() const noexcept {
            return handle != nullptr;
        }

        ~field() {
            if (handle != nullptr) {
                Destroy(handle, handle_role::field);
            }
        }
    };

    template <class T>
    struct root_handle {
        handle_t *handle = nullptr;
        constexpr root_handle() noexcept = default;
        constexpr root_handle(std::nullptr_t) noexcept {}
        constexpr root_handle(null_handle_t) noexcept {}
        root_handle(const ro_pin<T>& pin) : handle(ToRootHandle(pin)) {}
        root_handle(const rw_pin<T>& pin) : handle(ToRootHandle(pin)) {}
        root_handle(const field<T>& field) : handle(ToRootHandle(field)) {}
        root_handle(handle_t *handle) : handle(Copy(handle, handle_role::unknown, handle_role::root)) {} // internal

        root_handle(const root_handle &other) : handle(Copy(other.handle, handle_role::root, handle_role::root)) {}
        root_handle &operator=(const root_handle &other) {
            if (&other == this) {
                return *this;
            }
            handle = Copy(handle, handle_role::root, handle_role::root);
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

        constexpr bool IsNull() const noexcept {
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
                Destroy(handle, handle_role::root);
            }
        }
    };

    template <class T>
    struct ro_pin {
        handle_t *handle = nullptr;
        constexpr ro_pin() noexcept = default;
        constexpr ro_pin(std::nullptr_t) noexcept {}
        constexpr ro_pin(null_handle_t) noexcept {}
        ro_pin(const root_handle<T> &handle) : handle(PinRO(handle)) {}
        ro_pin(const field<T> &field) : handle(PinRO(field)) {}

        ro_pin(const ro_pin&) = delete;
        ro_pin& operator=(const ro_pin&) = delete;
        ro_pin(ro_pin&&) = delete;
        ro_pin& operator=(ro_pin&&) = delete;

        constexpr bool IsNull() const noexcept {
            return handle == nullptr;
        }

        constexpr explicit operator handle_t *() const noexcept {
            return handle;
        }

        constexpr explicit operator bool() const noexcept {
            return handle != nullptr;
        }

        explicit operator field<T>() const noexcept {
            return field<T>(*this);
        }

        explicit operator root_handle<T>() const noexcept {
            return root_handle<T>(*this);
        }

        const T* Get() {
            if (handle == nullptr) {
                return nullptr;
            }

            return static_cast<const T*>(GetInstance(handle, handle_role::rw_pin));
        }

        const T* operator ->() {
            return Get();
        }

        ~ro_pin() {
            if (handle != nullptr) {
                Destroy(handle, handle_role::ro_pin);
            }
        }
    };

    template <class T>
    struct rw_pin {
        handle_t *handle = nullptr;
        constexpr rw_pin() noexcept = default;
        constexpr rw_pin(std::nullptr_t) noexcept {}
        constexpr rw_pin(null_handle_t) noexcept {}
        rw_pin(const root_handle<T> &handle) : handle(PinRW(handle)) {}
        rw_pin(const field<T> &field) : handle(PinRW(field)) {}

        rw_pin(const rw_pin&) = delete;
        rw_pin& operator=(const rw_pin&) = delete;
        rw_pin(rw_pin&&) = delete;
        rw_pin& operator=(rw_pin&&) = delete;

        constexpr bool IsNull() const noexcept {
            return handle == nullptr;
        }

        constexpr explicit operator handle_t *() const noexcept {
            return handle;
        }

        constexpr explicit operator bool() const noexcept {
            return handle != nullptr;
        }

        explicit operator field<T>() const noexcept {
            return field<T>(*this);
        }

        explicit operator root_handle<T>() const noexcept {
            return root_handle<T>(*this);
        }

        T* Get() {
            if (handle == nullptr) {
                return nullptr;
            }

            return static_cast<T*>(GetInstance(handle, handle_role::rw_pin));
        }

        T* operator ->() {
            return Get();
        }

        ~rw_pin() {
            if (handle != nullptr) {
                Destroy(handle, handle_role::rw_pin);
            }
        }
    };

    template <class T>
    bool operator == (const root_handle<T> &lhs, const root_handle<T> &rhs) {
        return Equals(lhs.handle, rhs.handle);
    }

    template <class T>
    bool operator != (const root_handle<T> &lhs, const root_handle<T> &rhs) {
        return !Equals(lhs.handle, rhs.handle);
    }

    template <class T>
    bool operator == (const root_handle<T> &lhs, const field<T> &rhs) {
        return Equals(lhs.handle, rhs.handle);
    }

    template <class T>
    bool operator != (const root_handle<T> &lhs, const field<T> &rhs) {
        return !Equals(lhs.handle, rhs.handle);
    }

    template <class T>
    bool operator == (const root_handle<T> &lhs, const ro_pin<T> &rhs) {
        return Equals(lhs.handle, rhs.handle);
    }

    template <class T>
    bool operator != (const root_handle<T> &lhs, const ro_pin<T> &rhs) {
        return !Equals(lhs.handle, rhs.handle);
    }

    template <class T>
    bool operator == (const root_handle<T> &lhs, const rw_pin<T> &rhs) {
        return Equals(lhs.handle, rhs.handle);
    }

    template <class T>
    bool operator != (const root_handle<T> &lhs, const rw_pin<T> &rhs) {
        return !Equals(lhs.handle, rhs.handle);
    }

    template <class T>
    bool operator == (const root_handle<T> &lhs, const std::nullptr_t&) {
        return lhs.IsNull();
    }

    template <class T>
    bool operator != (const root_handle<T> &lhs, const std::nullptr_t &) {
        return !lhs.IsNull();
    }

    template <class T>
    bool operator == (const root_handle<T> &lhs, const null_handle_t&) {
        return lhs.IsNull();
    }

    template <class T>
    bool operator != (const root_handle<T> &lhs, const null_handle_t&) {
        return !lhs.IsNull();
    }

    template <class T>
    bool operator == (const field<T> &lhs, const root_handle<T> &rhs) {
        return Equals(lhs.handle, rhs.handle);
    }

    template <class T>
    bool operator != (const field<T> &lhs, const root_handle<T> &rhs) {
        return !Equals(lhs.handle, rhs.handle);
    }

    template <class T>
    bool operator == (const field<T> &lhs, const field<T> &rhs) {
        return Equals(lhs.handle, rhs.handle);
    }

    template <class T>
    bool operator != (const field<T> &lhs, const field<T> &rhs) {
        return !Equals(lhs.handle, rhs.handle);
    }

    template <class T>
    bool operator == (const field<T> &lhs, const ro_pin<T> &rhs) {
        return Equals(lhs.handle, rhs.handle);
    }

    template <class T>
    bool operator != (const field<T> &lhs, const ro_pin<T> &rhs) {
        return !Equals(lhs.handle, rhs.handle);
    }

    template <class T>
    bool operator == (const field<T> &lhs, const rw_pin<T> &rhs) {
        return Equals(lhs.handle, rhs.handle);
    }

    template <class T>
    bool operator != (const field<T> &lhs, const rw_pin<T> &rhs) {
        return !Equals(lhs.handle, rhs.handle);
    }

    template <class T>
    bool operator == (const field<T> &lhs, const std::nullptr_t&) {
        return lhs.IsNull();
    }

    template <class T>
    bool operator != (const field<T> &lhs, const std::nullptr_t &) {
        return !lhs.IsNull();
    }

    template <class T>
    bool operator == (const field<T> &lhs, const null_handle_t&) {
        return lhs.IsNull();
    }

    template <class T>
    bool operator != (const field<T> &lhs, const null_handle_t&) {
        return !lhs.IsNull();
    }

    template <class T>
    bool operator == (const ro_pin<T> &lhs, const root_handle<T> &rhs) {
        return Equals(lhs.handle, rhs.handle);
    }

    template <class T>
    bool operator != (const ro_pin<T> &lhs, const root_handle<T> &rhs) {
        return !Equals(lhs.handle, rhs.handle);
    }

    template <class T>
    bool operator == (const ro_pin<T> &lhs, const field<T> &rhs) {
        return Equals(lhs.handle, rhs.handle);
    }

    template <class T>
    bool operator != (const ro_pin<T> &lhs, const field<T> &rhs) {
        return !Equals(lhs.handle, rhs.handle);
    }

    template <class T>
    bool operator == (const ro_pin<T> &lhs, const ro_pin<T> &rhs) {
        return Equals(lhs.handle, rhs.handle);
    }

    template <class T>
    bool operator != (const ro_pin<T> &lhs, const ro_pin<T> &rhs) {
        return !Equals(lhs.handle, rhs.handle);
    }

    template <class T>
    bool operator == (const ro_pin<T> &lhs, const rw_pin<T> &rhs) {
        return Equals(lhs.handle, rhs.handle);
    }

    template <class T>
    bool operator != (const ro_pin<T> &lhs, const rw_pin<T> &rhs) {
        return !Equals(lhs.handle, rhs.handle);
    }

    template <class T>
    bool operator == (const ro_pin<T> &lhs, const std::nullptr_t&) {
        return lhs.IsNull();
    }

    template <class T>
    bool operator != (const ro_pin<T> &lhs, const std::nullptr_t &) {
        return !lhs.IsNull();
    }

    template <class T>
    bool operator == (const ro_pin<T> &lhs, const null_handle_t&) {
        return lhs.IsNull();
    }

    template <class T>
    bool operator != (const ro_pin<T> &lhs, const null_handle_t&) {
        return !lhs.IsNull();
    }

    template <class T>
    bool operator == (const rw_pin<T> &lhs, const root_handle<T> &rhs) {
        return Equals(lhs.handle, rhs.handle);
    }

    template <class T>
    bool operator != (const rw_pin<T> &lhs, const root_handle<T> &rhs) {
        return !Equals(lhs.handle, rhs.handle);
    }

    template <class T>
    bool operator == (const rw_pin<T> &lhs, const field<T> &rhs) {
        return Equals(lhs.handle, rhs.handle);
    }

    template <class T>
    bool operator != (const rw_pin<T> &lhs, const field<T> &rhs) {
        return !Equals(lhs.handle, rhs.handle);
    }

    template <class T>
    bool operator == (const rw_pin<T> &lhs, const ro_pin<T> &rhs) {
        return Equals(lhs.handle, rhs.handle);
    }

    template <class T>
    bool operator != (const rw_pin<T> &lhs, const ro_pin<T> &rhs) {
        return !Equals(lhs.handle, rhs.handle);
    }

    template <class T>
    bool operator == (const rw_pin<T> &lhs, const rw_pin<T> &rhs) {
        return Equals(lhs.handle, rhs.handle);
    }

    template <class T>
    bool operator != (const rw_pin<T> &lhs, const rw_pin<T> &rhs) {
        return !Equals(lhs.handle, rhs.handle);
    }

    template <class T>
    bool operator == (const rw_pin<T> &lhs, const std::nullptr_t&) {
        return lhs.IsNull();
    }

    template <class T>
    bool operator != (const rw_pin<T> &lhs, const std::nullptr_t &) {
        return !lhs.IsNull();
    }

    template <class T>
    bool operator == (const rw_pin<T> &lhs, const null_handle_t&) {
        return lhs.IsNull();
    }

    template <class T>
    bool operator != (const rw_pin<T> &lhs, const null_handle_t&) {
        return !lhs.IsNull();
    }

    template <class T>
    rw_pin<T> RWPin(const root_handle<T> &handle) {
        return rw_pin<T>(handle);
    }

    template <class T>
    ro_pin<T> ROPin(const root_handle<T> &handle) {
        return ro_pin<T>(handle);
    }

    template <class T>
    rw_pin<T> RWPin(const field<T> &field) {
        return rw_pin<T>(field);
    }

    template <class T>
    ro_pin<T> ROPin(const field<T> &field) {
        return ro_pin<T>(field);
    }

    template <class T>
    handle_t *ToRootHandle(const ro_pin<T>& pin) {
        return Copy(pin.handle, handle_role::ro_pin, handle_role::root);
    }

    template <class T>
    handle_t *ToRootHandle(const rw_pin<T>& pin) {
        return Copy(pin.handle, handle_role::rw_pin, handle_role::root);
    }

    template <class T>
    handle_t *ToRootHandle(const field<T>& field) {
        return Copy(field.handle, handle_role::field, handle_role::root);
    }

    template <class T>
    handle_t *ToField(const root_handle<T> &handle) {
        return Copy(handle.handle, handle_role::root, handle_role::field);
    }

    template <class T>
    handle_t *ToField(const ro_pin<T> &pin) {
        return Copy(pin.handle, handle_role::ro_pin, handle_role::field);
    }

    template <class T>
    handle_t *ToField(const rw_pin<T> &pin) {
        return Copy(pin.handle, handle_role::rw_pin, handle_role::field);
    }

    template <class T>
    handle_t *PinRW(const root_handle<T> &handle) {
        return Copy(handle.handle, handle_role::root, handle_role::rw_pin);
    }

    template <class T>
    handle_t *PinRW(const field<T> &field) {
        return Copy(field.handle, handle_role::field, handle_role::rw_pin);
    }

    template <class T>
    handle_t *PinRO(const root_handle<T> &handle) {
        return Copy(handle.handle, handle_role::root, handle_role::ro_pin);
    }

    template <class T>
    handle_t *PinRO(const field<T> &field) {
        return Copy(field.handle, handle_role::field, handle_role::ro_pin);
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
    root_handle<T> Allocate(const size_t count = 1) {
        return root_handle<T>(New(
            sizeof(T),
            alignof(T),
            count,
            typeid(T),
            gc_object_traits<T>::destructor,
            gc_object_traits<T>::get_field_count,
            gc_object_traits<T>::get_field,
            gc_object_traits<T>::move
        ));
    }

    template <class T, class... Args>
    bool Initialize(rw_pin<T> instance, Args&&... args) {
        if (!TryAcquireInitializeRight(instance.handle)) {
            return false;
        }

        try {
            std::construct_at(instance.Get(), std::forward<Args>(args)...);
            return true;
        } catch (std::exception&) {
            SetInitializationFailed(instance.handle);
            throw;
        }
    }

     template <gc_supported T, class... Args>
     root_handle<T> New(Args&&... args) {
        auto handle = Allocate<T>();
        Initialize(RWPin(handle), std::forward<Args>(args)...);
        return handle;
    }
}