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

int main() {
    gc::Init();
    example();
    gc::Destroy();
    return 0;
}