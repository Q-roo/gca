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

int main() {
    gc::Init();
    example();
    gc::Destroy();
    return 0;
}