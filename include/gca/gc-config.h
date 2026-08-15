#pragma once

#include <cstdint>

namespace gc::config {
    using rw_lock_underlying_type = std::uint8_t;
    using mutex_underlying_type = bool;
    using object_type_index_underlying_type = std::uint16_t;
    using object_flags_underlying_type = std::uint16_t;
    using object_root_handle_count_underlying_type = std::uint16_t;
    using gc_impl_ongoing_gc_count_underlying_type = std::uint8_t;
    constexpr bool gc_throw_nonessential_exceptions = true; // mostly for out-of-bound indices in gc-util.h
    constexpr bool enable_debug_messages = true;
    constexpr bool enable_assertions = true; // for bugs within gc_impl
    constexpr std::size_t page_size = 4096;
    constexpr std::size_t page_alignment = 4096;
    constexpr std::size_t page_acquire_attempts = 10;
    constexpr std::uint8_t gc_max_collection_thread_count = 8; // max config value is
}
