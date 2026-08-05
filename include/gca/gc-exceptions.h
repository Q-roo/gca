#pragma once

#include <stdexcept>

namespace gc {
    class too_many_types : public std::range_error {
        using std::range_error::range_error;
    public:
        [[nodiscard]] const char *what() const noexcept override {
            return "number of types exceed the maximum value expressible by the indexing type";
        }
    };

    class out_of_memory : public std::bad_alloc {
        using std::bad_alloc::bad_alloc;
        public:
        [[nodiscard]] const char *what() const noexcept override {
            return "failed to allocate on existing pages and couldn't allocate a new page";
        }
    };

    class bad_api_usage : public std::logic_error {
        using std::logic_error::logic_error;
    };

    class library_bug : public std::logic_error {
        using std::logic_error::logic_error;
    };
}
