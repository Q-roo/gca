# GCA (Garbage Collecting Allocator)

A toy allocator that aims for the simplest implementation possible (at the cost of performance) while having the following features:
- pinning objects
- reclamation of cyclic garbage
- polymorphic handles (AKA support for dynamic_cast for allocated objects)
- can execute allocation/collection across multiple threads safely
- can share allocated objects between threads safely
- defragmentation

## Building

No build system required, just add `gc.cpp` and `include/gca/*.h` to your project.
The tests and examples directories are not required.

The `CMakeLists.txt` can be ignored, it is only used to build the collector as a static library and the unit tests for
it as an executable and then run the tests.

## The API

The project has 3 APIs:
- internal: actual implementation
- "internal": exposes parts of the internal API for the public API to use
- public: The only API most people will need to care about

### The public API

The public API can further be divided into 2 categories: one that supports polymorphism (`gc::dyn` namespace) and one that doesn't (`gc` namespace).

Both have the same types: `root_handle`, `pin`, `field`.

#### `root_handle`

A handle for an allocated object that should only exist on the stack or in the globals, but never as a field in an object (use `field` for that).

#### `field`

A handle for an allocated object that can be used as a field in other objects.

#### `pin`

The only way to access an allocated object. Prevents the collector from relocating the object while the pin is alive.
It enforces shared read/exclusive write access on the allocated object.

#### `gc::Init`

Initialize the grabage collector.

#### `gc::Destroy`

Destroy the garbage collector. Note that the collector must be in an idle state when this is called.

#### `gc::New`

Allocate and initialize an instance of type `T`. Specializations for arrays exist as well.

#### Making the GC support a type

The collector can only work with types that are supported.
Support for a type can be implemented by creating template specializations of types.

##### Basic support

create a specialization of `is_gc_supported` for `T` where `is_gc_supported<T>::supported` is set to `true`.

In case `T` has fields, `default_get_field_count` and `default_get_field` has to be specialized as well
(the default implementation assumes no fields).

Optionally, specialize `default_move` (which either uses the move constructor or makes the object immovable) and
`default_destroy` (which is just `~T()`).

It is also possible to create a custom trait type and make the allocator use that
(`gc::New<gc_supported T, class Traits = partial_gc_object_traits<T>, class... Args>`)

The type needs to define the following values:
* get_field_count (`gc::get_field_count_fn`)
* get_field (`gc::get_field_fn`)
* move (`gc::move_fn`)
* destructor (`gc::destructor_fn`)

Apart from `move`, neither of these function pointers can be `nullptr` and must be `noexcept(true)`. If `move` is
`nullptr`, the object will be considered immovable. `get_field` may return `nullptr` for invalid field indices.

Alternatively, use `gc_object_traits<Traits>`, where you are only required to define `type` (`Traits::type=T`).
This generates a traits type that can be used by `gc::New`. Any value defined in `Traits` will be used instead of the
default implementation and the default implementation will be used for any missing values.

#### Finalization of garbage objects

Every GC field will be set to `null_handle` before the finalizer for `T` (the destructor in most cases) will be invoked.
This can happen on any thread at any time.

#### Example usage (basic)

```c++
#include <gca/gc.h>
#include <vector>

struct no_gc_fields {
    std::vector<int> vec;
};

template<> struct gc::is_gc_supported<no_gc_fields> { static constexpr bool supported = true; };

void example() {
    gc::root_handle<no_gc_fields> handle = gc::New<no_gc_fields>();
    auto pin = gc::RWPin(handle);
    pin->vec.emplace_back(0);
}
```

#### Example usage (with fields)

The implementation for field for the non-polymorphic API is very scuffed and its usage is not recommend.

```c++
#include <gca/gc.h>
#include <vector>

struct no_gc_fields {
    std::vector<int> vec;
};

template<> struct gc::is_gc_supported<no_gc_fields> { static constexpr bool supported = true; };

struct gc_fields {
    // do not access directly
    // not required to be the first field, but it is recommended
    gc::field_store<1 /* field count */ > _fields{}; // initialization is very important here
    [[no_unique_address /* or mscv::no_unique_address */ ]] gc::field<0 /* offset of the field store from the start of the object */, 0 /* field index */, no_gc_fields> field{gc::null_handle};
    gc_fields()
    : field(gc::New<no_gc_fields>()) {}
};

template<> struct gc::is_gc_supported<gc_fields> { static constexpr bool supported = true; };

// gc::New uses partial_gc_object_traits<T> with gc::object_traits to get all the required functionality for T
template<> struct gc::partial_gc_object_traits<gc_fields> {
    using type = gc_fields;
    static constexpr get_field_count_fn get_field_count = [](const void*) noexcept -> size_t { return 1ULL; };
    static constexpr get_field_fn get_field = [](void *obj /* gc_fields instance */ , const size_t idx /* field index */ ) noexcept -> handle_t** {
        return idx == 0
        ? std::launder(static_cast<gc_fields*>(obj))->field.GetField() // should only be used here
        : nullptr;
    };
};

void example() {
    gc::root_handle handle = gc::ROPin(gc::New<gc_fields>())->field; // cannot use auto because that would copy the field
    gc::RWPin(handle)->vec.emplace_back(0);
}
```

#### Example usage (gc::dyn with fields)

Note that when a cast fails, `std::bad_cast` will be thrown.

```c++
#include <gca/gc.h>

struct animal               { virtual ~animal()          = default; };
struct cat : virtual animal {         ~cat()    override = default; };
struct dog : virtual animal {         ~dog()    override = default; };
struct dogcat : dog, cat    {         ~dogcat() override = default; };

struct dog_owner { gc::dyn::field<dog> pet{gc::null_handle}; }; // no need to define is_gc_supported yet

template<> struct gc::is_gc_supported<dogcat>    { static constexpr bool supported = true; };
template<> struct gc::is_gc_supported<dog_owner> { static constexpr bool supported = true; };

template<> struct gc::partial_gc_object_traits<dog_owner> {
    using type = dog_owner;
    static constexpr get_field_count_fn get_field_count = [](const void*) noexcept -> size_t { return 1ULL; };
    static constexpr get_field_fn get_field = [](void *obj, const size_t idx) noexcept -> handle_t** {
        return idx == 0
        ? std::launder(static_cast<dog_owner*>(obj))->pet.GetField() // should only be used here
        : nullptr;
    };
};

void example() {
    // root_handle<T> can be converted to dyn::root_handle<T>, but not to dyn::root_handle<U> (T : U)
    gc::dyn::root_handle<animal> pet = static_cast<gc::dyn::root_handle<dogcat>>(gc::New<dogcat>());
    gc::dyn::root_handle owner = gc::New<dog_owner>();
    owner.PinRW()->pet = pet;
}
```
