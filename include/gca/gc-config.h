#pragma once
#include <cstdint>

namespace gc::config {
    using rw_lock_underlying_type = std::uint8_t;
    using mutex_underlying_type = bool;
    using object_type_index_underlying_type = std::uint16_t;
    using object_flags_underlying_type = std::uint16_t;
    using object_root_handle_count_underlying_type = std::uint16_t;
    using gc_impl_ongoing_gc_count_underlying_type = std::uint8_t;
    constexpr bool gc_throw_nonessential_exceptions = true; // AKA exceptions instead of debug assertions
    constexpr bool store_type_name_in_object_type = true;
    constexpr bool store_type_id_in_object_type = true;
    constexpr std::size_t page_size = 4096;
    constexpr std::size_t page_acquire_attempts = 10;
    constexpr std::size_t gc_ro_lock_acquire_attempts = 10;
    constexpr std::uint8_t gc_max_collection_thread_count = 8; // max is 64
}
