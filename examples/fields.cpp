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

int main() {
    gc::Init();
    example();
    gc::Destroy();
    return 0;
}