#include <memory_resource>

#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>

#include <gca/gc.h>
#include "gca/gc-util.h"

/*
 * things to test:
 * - general:
 * - overflow/underflow
 * - invalid values
 * - null pointers
 * - threading primitives:
 * - should lock the thread if a thread tries to acquire a lock/mutex again after acquiring it
 * - thread safety (TODO: test for memory and instruction ordering)
 * - utility classes/structs/functions
 * - GC:
 * - can handle all exceptions that can be thrown during allocation/collection
 * - thread safety (TODO: test for memory and instruction ordering)
 * - being able to handle fields/root handles changing during collection
 * - memory safety
 * - polymorphism
 */

template <typename T>
std::string demangled_type_name() {
    int status = 0;
    std::unique_ptr<char, void(*)(void*)> res {
        abi::__cxa_demangle(typeid(T).name(), nullptr, nullptr, &status),
        std::free
    };

    return status == 0 ? res.get() : typeid(T).name();
}

enum class test_flags_for_atomic_bit_set : uint8_t {
    none = 0,
    f1 = 1,
    f2 = 2,
    f3 = 4,
    f4 = 8,
};

template<>
struct gc::enum_flag_traits<test_flags_for_atomic_bit_set> {
    using underlying_type = std::underlying_type_t<test_flags_for_atomic_bit_set>;
    constexpr static bool is_flag = true;
    constexpr static underlying_type all_flags = 0b1111;
};

#define EXPECT_TYPE(type_expr_actual, type_expr_expected) EXPECT_TRUE((std::is_same_v<type_expr_actual, type_expr_expected>)) << "Expected type \"" << demangled_type_name<type_expr_expected>() << "\" but got \"" << demangled_type_name<type_expr_actual>() << "\""

TEST(GC_utility__smallest_unsigned_numeric_type_needed_for, correct_types_for_sizes) {
    EXPECT_TYPE(gc::smallest_unsigned_numeric_type_needed_for_t<0>, uint8_t);
    EXPECT_TYPE(gc::smallest_unsigned_numeric_type_needed_for_t<std::numeric_limits<uint8_t>::min()>, uint8_t);

    EXPECT_TYPE(gc::smallest_unsigned_numeric_type_needed_for_t<std::numeric_limits<uint8_t>::max()>, uint8_t);
    EXPECT_TYPE(gc::smallest_unsigned_numeric_type_needed_for_t<std::numeric_limits<uint8_t>::max() + 1ULL>, uint16_t);

    EXPECT_TYPE(gc::smallest_unsigned_numeric_type_needed_for_t<std::numeric_limits<uint16_t>::max()>, uint16_t);
    EXPECT_TYPE(gc::smallest_unsigned_numeric_type_needed_for_t<std::numeric_limits<uint16_t>::max() + 1ULL>, uint32_t);

    EXPECT_TYPE(gc::smallest_unsigned_numeric_type_needed_for_t<std::numeric_limits<uint32_t>::max()>, uint32_t);
    // and this is when you realize you need to use 1ull instead of just 1
    EXPECT_TYPE(gc::smallest_unsigned_numeric_type_needed_for_t<std::numeric_limits<uint32_t>::max() + 1ULL>, uint64_t);

    EXPECT_TYPE(gc::smallest_unsigned_numeric_type_needed_for_t<std::numeric_limits<uint64_t>::max()>, uint64_t);
}

TEST(GC_utility__atomic_bitset, read_and_write_bit_positions_are_correct) {
    gc::atomic_bit_set<uint8_t> bits = 0b10101010;
    EXPECT_EQ(bits.ReadBit(gc::atomic_bit_set<uint8_t>::min_pos), false);
    EXPECT_EQ(bits.ReadBit(gc::atomic_bit_set<uint8_t>::max_pos), true);
    bits.SetBit(gc::atomic_bit_set<uint8_t>::min_pos, true);
    EXPECT_EQ(bits.ReadAll(), 0b10101011);
    bits.SetBit(gc::atomic_bit_set<uint8_t>::max_pos, false);
    EXPECT_EQ(bits.ReadAll(), 0b00101011);
}

TEST(GC_utility__atomic_bitset, read_all_and_write_all_are_correct) {
    gc::atomic_bit_set<uint8_t> bits = 0b10101010;
    EXPECT_EQ(bits.ReadAll(), 0b10101010);
    bits.SetAll(0b01010101);
    EXPECT_EQ(bits.ReadAll(), 0b01010101);
}

TEST(GC_utility__atomic_bitset, flagset_specialization_flag_operations) {
    EXPECT_FALSE(gc::atomic_bit_set(test_flags_for_atomic_bit_set::none).HasFlag(test_flags_for_atomic_bit_set::f1));

    gc::atomic_bit_set bits(test_flags_for_atomic_bit_set::none);

    bits.SetFlag(test_flags_for_atomic_bit_set::f1, true);
    EXPECT_TRUE(bits.HasFlag(test_flags_for_atomic_bit_set::f1));

    bits.SetFlag(test_flags_for_atomic_bit_set::f1, false);
    EXPECT_FALSE(bits.HasFlag(test_flags_for_atomic_bit_set::f1));

    bits.SetFlag(test_flags_for_atomic_bit_set::f1, true);
    bits.ClearAll();
    EXPECT_FALSE(bits.HasFlag(test_flags_for_atomic_bit_set::f1));
}

TEST(GC_utility__atomic_bitset, flagset_specialization_flag_operations_with_multiple_flags) {
    gc::atomic_bit_set bits(test_flags_for_atomic_bit_set::none);

    bits.SetFlag(static_cast<test_flags_for_atomic_bit_set>(gc::enum_flag_traits<test_flags_for_atomic_bit_set>::all_flags), true);
    EXPECT_EQ(bits.ReadAll(), static_cast<test_flags_for_atomic_bit_set>(gc::enum_flag_traits<test_flags_for_atomic_bit_set>::all_flags));

    bits.SetFlag(static_cast<test_flags_for_atomic_bit_set>(gc::enum_flag_traits<test_flags_for_atomic_bit_set>::all_flags), false);
    EXPECT_EQ(bits.ReadAll(), static_cast<test_flags_for_atomic_bit_set>(0));
}

TEST(GC_utility__atomic_bitset, size_types_and_min_max_positions_are_correct) {
    EXPECT_EQ(gc::atomic_bit_set<uint8_t>::min_pos, 0);
    EXPECT_EQ(gc::atomic_bit_set<uint8_t>::max_pos, 7);

    EXPECT_EQ(gc::atomic_bit_set<uint16_t>::min_pos, 0);
    EXPECT_EQ(gc::atomic_bit_set<uint16_t>::max_pos, 15);

    EXPECT_EQ(gc::atomic_bit_set<uint32_t>::min_pos, 0);
    EXPECT_EQ(gc::atomic_bit_set<uint32_t>::max_pos, 31);

    EXPECT_EQ(gc::atomic_bit_set<uint64_t>::min_pos, 0);
    EXPECT_EQ(gc::atomic_bit_set<uint64_t>::max_pos, 63);

    EXPECT_TYPE(gc::enum_flag_traits<test_flags_for_atomic_bit_set>::underlying_type, std::underlying_type_t<test_flags_for_atomic_bit_set>);
    EXPECT_EQ(gc::atomic_bit_set<test_flags_for_atomic_bit_set>::min_pos, gc::atomic_bit_set<gc::enum_flag_traits<test_flags_for_atomic_bit_set>::underlying_type>::min_pos);
    EXPECT_EQ(gc::atomic_bit_set<test_flags_for_atomic_bit_set>::max_pos, gc::atomic_bit_set<gc::enum_flag_traits<test_flags_for_atomic_bit_set>::underlying_type>::max_pos);
}

TEST(GC_utility__atomic_bitset, bounds_check_exceptions_when_enabled) {
    if constexpr (!gc::config::gc_throw_nonessential_exceptions) {
        GTEST_SKIP() << "non-essential exceptions are disabled";
    }

    EXPECT_THROW(gc::atomic_bit_set<uint8_t>().ReadBit(gc::atomic_bit_set<uint8_t>::min_pos - 1), std::out_of_range);
    EXPECT_THROW(gc::atomic_bit_set<uint8_t>().ReadBit(gc::atomic_bit_set<uint8_t>::max_pos + 1), std::out_of_range);

    EXPECT_THROW(gc::atomic_bit_set<uint8_t>().SetBit(gc::atomic_bit_set<uint8_t>::min_pos - 1, false), std::out_of_range);
    EXPECT_THROW(gc::atomic_bit_set<uint8_t>().SetBit(gc::atomic_bit_set<uint8_t>::max_pos + 1, false), std::out_of_range);
}

TEST(GC_utility__atomic_bitset, flagset_specialization_bounds_check_exceptions_when_enabled) {
    if constexpr (!gc::config::gc_throw_nonessential_exceptions) {
        GTEST_SKIP() << "non-essential exceptions are disabled";
    }

    EXPECT_THROW(gc::atomic_bit_set<test_flags_for_atomic_bit_set>().HasFlag(static_cast<test_flags_for_atomic_bit_set>(static_cast<int>(test_flags_for_atomic_bit_set::f4) << 1)), std::out_of_range);
    EXPECT_THROW(gc::atomic_bit_set<test_flags_for_atomic_bit_set>().HasFlag(static_cast<test_flags_for_atomic_bit_set>(gc::enum_flag_traits<test_flags_for_atomic_bit_set>::all_flags << 1)), std::out_of_range);
    EXPECT_THROW(gc::atomic_bit_set<test_flags_for_atomic_bit_set>().HasFlag(static_cast<test_flags_for_atomic_bit_set>(gc::enum_flag_traits<test_flags_for_atomic_bit_set>::all_flags << 4)), std::out_of_range);

    EXPECT_THROW(gc::atomic_bit_set<test_flags_for_atomic_bit_set>().SetFlag(static_cast<test_flags_for_atomic_bit_set>(static_cast<int>(test_flags_for_atomic_bit_set::f4) << 1), false), std::out_of_range);
    EXPECT_THROW(gc::atomic_bit_set<test_flags_for_atomic_bit_set>().SetFlag(static_cast<test_flags_for_atomic_bit_set>(gc::enum_flag_traits<test_flags_for_atomic_bit_set>::all_flags << 1), false), std::out_of_range);
    EXPECT_THROW(gc::atomic_bit_set<test_flags_for_atomic_bit_set>().SetFlag(static_cast<test_flags_for_atomic_bit_set>(gc::enum_flag_traits<test_flags_for_atomic_bit_set>::all_flags << 4), false), std::out_of_range);
}

TEST(GC_utility__atomic_bitset, no_bounds_check_exceptions_when_disabled) {
    if constexpr (gc::config::gc_throw_nonessential_exceptions) {
        GTEST_SKIP() << "non-essential exceptions are enabled";
    }

    EXPECT_NO_THROW(gc::atomic_bit_set<uint8_t>().ReadBit(gc::atomic_bit_set<uint8_t>::min_pos - 1));
    EXPECT_NO_THROW(gc::atomic_bit_set<uint8_t>().ReadBit(gc::atomic_bit_set<uint8_t>::max_pos + 1));

    EXPECT_NO_THROW(gc::atomic_bit_set<uint8_t>().SetBit(gc::atomic_bit_set<uint8_t>::min_pos - 1, false));
    EXPECT_NO_THROW(gc::atomic_bit_set<uint8_t>().SetBit(gc::atomic_bit_set<uint8_t>::max_pos + 1, false));
}

TEST(GC_utility__atomic_bitset, flagset_specialization_bounds_check_exceptions_when_disabled) {
    if constexpr (gc::config::gc_throw_nonessential_exceptions) {
        GTEST_SKIP() << "non-essential exceptions are enabled";
    }

    EXPECT_THROW(gc::atomic_bit_set<test_flags_for_atomic_bit_set>().HasFlag(static_cast<test_flags_for_atomic_bit_set>(static_cast<int>(test_flags_for_atomic_bit_set::f4) << 1)), std::out_of_range);
    EXPECT_THROW(gc::atomic_bit_set<test_flags_for_atomic_bit_set>().HasFlag(static_cast<test_flags_for_atomic_bit_set>(gc::enum_flag_traits<test_flags_for_atomic_bit_set>::all_flags << 1)), std::out_of_range);
    EXPECT_THROW(gc::atomic_bit_set<test_flags_for_atomic_bit_set>().HasFlag(static_cast<test_flags_for_atomic_bit_set>(gc::enum_flag_traits<test_flags_for_atomic_bit_set>::all_flags << 4)), std::out_of_range);

    EXPECT_THROW(gc::atomic_bit_set<test_flags_for_atomic_bit_set>().SetFlag(static_cast<test_flags_for_atomic_bit_set>(static_cast<int>(test_flags_for_atomic_bit_set::f4) << 1), false), std::out_of_range);
    EXPECT_THROW(gc::atomic_bit_set<test_flags_for_atomic_bit_set>().SetFlag(static_cast<test_flags_for_atomic_bit_set>(gc::enum_flag_traits<test_flags_for_atomic_bit_set>::all_flags << 1), false), std::out_of_range);
    EXPECT_THROW(gc::atomic_bit_set<test_flags_for_atomic_bit_set>().SetFlag(static_cast<test_flags_for_atomic_bit_set>(gc::enum_flag_traits<test_flags_for_atomic_bit_set>::all_flags << 4), false), std::out_of_range);
}

TEST(GC_utility__atomic_bitset, set_returns_false_when_call_did_not_have_to_set_the_value) {
    gc::atomic_bit_set<uint8_t>bits(0);
    EXPECT_TRUE(bits.SetBit(0, true));
    EXPECT_FALSE(bits.SetBit(0, true));
    EXPECT_TRUE(bits.SetBit(0, false));
    EXPECT_FALSE(bits.SetBit(1, false));

    gc::atomic_bit_set flags(test_flags_for_atomic_bit_set::none);
    EXPECT_TRUE(flags.SetFlag(test_flags_for_atomic_bit_set::f1, true));
    EXPECT_FALSE(flags.SetFlag(test_flags_for_atomic_bit_set::f1, true));
    EXPECT_TRUE(flags.SetFlag(test_flags_for_atomic_bit_set::f1, false));
    EXPECT_FALSE(flags.SetFlag(test_flags_for_atomic_bit_set::f2, false));

    flags.ClearAll();
    using gc::operator|;
    EXPECT_TRUE(flags.SetFlag(test_flags_for_atomic_bit_set::f1 | test_flags_for_atomic_bit_set::f2, true));
    EXPECT_FALSE(flags.SetFlag(test_flags_for_atomic_bit_set::f1 | test_flags_for_atomic_bit_set::f2, true));
    EXPECT_TRUE(flags.SetFlag(test_flags_for_atomic_bit_set::f1 | test_flags_for_atomic_bit_set::f2, false));
    EXPECT_FALSE(flags.SetFlag(test_flags_for_atomic_bit_set::f3 | test_flags_for_atomic_bit_set::f4, false));

    flags.ClearAll();
    flags.SetFlag(test_flags_for_atomic_bit_set::f1, true);
    EXPECT_TRUE(flags.SetFlag(test_flags_for_atomic_bit_set::f1 | test_flags_for_atomic_bit_set::f2, true));
    EXPECT_FALSE(flags.SetFlag(test_flags_for_atomic_bit_set::f1 | test_flags_for_atomic_bit_set::f2, true));

    flags.ClearAll();
    flags.SetFlag(test_flags_for_atomic_bit_set::f1, true);
    EXPECT_TRUE(flags.SetFlag(test_flags_for_atomic_bit_set::f1 | test_flags_for_atomic_bit_set::f2, false));
    EXPECT_FALSE(flags.SetFlag(test_flags_for_atomic_bit_set::f1 | test_flags_for_atomic_bit_set::f2, false));
}

TEST(GC_utility__rw_lock, acquire_succeeds) {
    gc::rw_lock().AcquireRead();
    EXPECT_EXIT({
        std::atomic done = false;

        std::thread thread([&]() {
            gc::rw_lock lock;
            lock.AcquireRead();
            done = true;
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::exit(done);
    }, testing::ExitedWithCode(1), "");

    EXPECT_EXIT({
        std::atomic done = false;

        std::thread thread([&]() {
            gc::rw_lock lock;
            lock.AcquireWrite();
            done = true;
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::exit(done);
    }, testing::ExitedWithCode(1), "");
}

TEST(GC_utility__rw_lock, write_reacquire_fails) { // AKA, only 1 write access can exist, even on the same thread
    EXPECT_EXIT({
        std::atomic done = false;

        std::thread thread([&]() {
            gc::rw_lock lock;
            lock.AcquireWrite();
            lock.AcquireWrite();
            done = true;
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::exit(done);
    }, testing::ExitedWithCode(0), "");
}

TEST(GC_utility__rw_lock, write_acquire_fails_when_at_least_one_read_access_exists) {
    EXPECT_EXIT({
        std::atomic done = false;

        std::thread thread([&]() {
            gc::rw_lock lock;
            lock.AcquireRead();
            lock.AcquireWrite();
            done = true;
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::exit(done);
    }, testing::ExitedWithCode(0), "");
}

TEST(GC_utility__rw_lock, read_acquire_fails_when_write_access_exists) {
    EXPECT_EXIT({
        std::atomic done = false;

        std::thread thread([&]() {
            gc::rw_lock lock;
            lock.AcquireWrite();
            lock.AcquireRead();
            done = true;
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::exit(done);
    }, testing::ExitedWithCode(0), "");
}

TEST(GC_utility__rw_lock, multiple_read_accesses_can_exist_at_the_same_time) {
    EXPECT_EXIT({
        std::atomic done = false;

        std::thread thread([&]() {
            gc::rw_lock lock;
            lock.AcquireRead();
            lock.AcquireRead();
            done = true;
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::exit(done);
    }, testing::ExitedWithCode(1), "");
}

TEST(GC_utility__rw_lock, write_access_allowed_after_all_read_accesses_are_released) {
    EXPECT_EXIT({
        std::atomic done = false;
        std::atomic acquired = false;
        gc::rw_lock lock;

        std::thread read([&]() {
            lock.AcquireRead();
            lock.AcquireRead();

            acquired = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            lock.Release();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            lock.Release();
        });

        while (!acquired);

        std::thread thread([&]() {
            lock.AcquireWrite();
            done = true;
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::exit(done);
    }, testing::ExitedWithCode(1), "");
}

TEST(GC_utility__rw_lock, read_accesses_are_allowed_after_write_access_is_released) {
    EXPECT_EXIT({
        std::atomic done = false;
        std::atomic acquired = false;
        gc::rw_lock lock;

        std::thread read([&]() {
            lock.AcquireWrite();
            acquired = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            lock.Release();
        });

        while (!acquired);

        std::thread thread([&]() {
            lock.AcquireRead();
            done = true;
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::exit(done);
    }, testing::ExitedWithCode(1), "");
}

TEST(GC_utility__rw_lock, read_access_max_read_access_is_correct) {
    EXPECT_EXIT({
        std::atomic done = false;
        std::thread thread([&]() {
            gc::rw_lock lock;
            for (size_t i = 0; i < gc::rw_lock::max_read_access_count; ++i) {
                lock.AcquireRead();
            }
            done = true;
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::exit(done);
    }, testing::ExitedWithCode(1), "");

    EXPECT_EXIT({
        std::atomic done = false;
        std::thread thread([&]() {
            gc::rw_lock lock;
            for (size_t i = 0; i < gc::rw_lock::max_read_access_count; ++i) {
                lock.AcquireRead();
            }
            // the write bit is set due to the last iteration
            lock.AcquireRead();
            done = true;
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::exit(done);
    }, testing::ExitedWithCode(0), "");
}

TEST(GC_utility__rw_lock, release_handles_overflow_correctly) {
    EXPECT_EXIT({
        std::atomic done = false;
        std::thread thread([&]() {
            gc::rw_lock lock;
            for (size_t i = 0; i < gc::rw_lock::max_read_access_count; ++i) {
                lock.AcquireRead();
            }

            lock.Release();
            lock.AcquireRead();
            done = true;
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::exit(done);
    }, testing::ExitedWithCode(1), "");

    EXPECT_EXIT({
        std::atomic done = false;
        std::thread thread([&]() {
            gc::rw_lock lock;
            for (size_t i = 0; i < gc::rw_lock::max_read_access_count; ++i) {
                lock.AcquireRead();
            }

            lock.Release();
            lock.AcquireWrite();
            done = true;
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::exit(done);
    }, testing::ExitedWithCode(0), "");

    EXPECT_EXIT({
        std::atomic done = false;
        std::thread thread([&]() {
            gc::rw_lock lock;
            for (size_t i = 0; i < gc::rw_lock::max_read_access_count; ++i) {
                lock.AcquireRead();
            }

            for (size_t i = 0; i < gc::rw_lock::max_read_access_count; ++i) {
                lock.Release();
            }

            lock.AcquireWrite();
            done = true;
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::exit(done);
    }, testing::ExitedWithCode(1), "");
}

TEST(GC_utility__rw_lock, cannot_overflow) {
    EXPECT_EXIT({
        std::atomic done = false;
        std::thread thread([&]() {
            gc::rw_lock lock;
            for (size_t i = 0; i < gc::rw_lock::max_read_access_count; ++i) {
                lock.AcquireRead();
            }

            // the write bit is set due to the last iteration
            // so no further read accesses are possible (not to mention write ones)
            lock.AcquireRead();
            done = true;
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::exit(done);
    }, testing::ExitedWithCode(0), "");
}


TEST(GC_utility__rw_lock, release_does_not_underflow) {
    EXPECT_EXIT({
        std::atomic done = false;
        std::thread thread([&]() {
            gc::rw_lock lock;
            lock.Release();

            lock.AcquireWrite();
            done = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::exit(done);
    }, testing::ExitedWithCode(1), "");

    EXPECT_EXIT({
        std::atomic done = false;
        std::thread thread([&]() {
            gc::rw_lock lock;
            lock.AcquireWrite();
            lock.Release();

            lock.Release();

            lock.AcquireWrite();
            done = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::exit(done);
    }, testing::ExitedWithCode(1), "");

    EXPECT_EXIT({
        std::atomic done = false;
        std::thread thread([&]() {
            gc::rw_lock lock;
            lock.AcquireWrite();
            lock.Release();

            lock.Release();
            lock.Release();

            lock.AcquireWrite();
            done = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::exit(done);
    }, testing::ExitedWithCode(1), "");
}

TEST(GC_utility__rw_lock, upgrade_acquisition_works) {
    EXPECT_EXIT({
        std::atomic done = false;
        std::thread thread([&]() {
            gc::rw_lock lock;
            lock.AcquireRead();
            lock.Upgrade();
            done = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::exit(done);
    }, testing::ExitedWithCode(1), "");
}

TEST(GC_utility__rw_lock, upgrade_acquisition_does_nothing_for_write_lock) {
    EXPECT_EXIT({
        std::atomic done = false;
        std::thread thread([&]() {
            gc::rw_lock lock;
            lock.AcquireWrite();
            lock.Upgrade();
            done = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::exit(done);
    }, testing::ExitedWithCode(1), "");
}

TEST(GC_utility__rw_lock, upgrade_acquisition_is_only_possible_when_there_is_only_one_read_access) {
    EXPECT_EXIT({
        std::atomic done = false;
        std::thread thread([&]() {
            gc::rw_lock lock;
            lock.AcquireRead();
            lock.AcquireRead();
            lock.Upgrade();
            done = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::exit(done);
    }, testing::ExitedWithCode(0), "");
}

TEST(GC_utility__rw_lock, releasing_upgraded_lock_works) {
    EXPECT_EXIT({
        std::atomic done = false;
        std::thread thread([&]() {
            gc::rw_lock lock;
            lock.AcquireRead();
            lock.Upgrade();
            lock.Release();
            lock.AcquireRead();
            done = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::exit(done);
    }, testing::ExitedWithCode(1), "");

    EXPECT_EXIT({
        std::atomic done = false;
        std::thread thread([&]() {
            gc::rw_lock lock;
            lock.AcquireRead();
            lock.Upgrade();
            lock.Release();
            lock.AcquireWrite();
            done = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::exit(done);
    }, testing::ExitedWithCode(1), "");
}

TEST(GC_utility__rw_lock, cannot_have_more_accesses_to_an_upgraded_access) {
    EXPECT_EXIT({
        std::atomic done = false;
        std::thread thread([&]() {
            gc::rw_lock lock;
            lock.AcquireRead();
            lock.Upgrade();
            lock.AcquireRead();
            done = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::exit(done);
    }, testing::ExitedWithCode(0), "");

    EXPECT_EXIT({
        std::atomic done = false;
        std::thread thread([&]() {
            gc::rw_lock lock;
            lock.AcquireRead();
            lock.Upgrade();
            lock.AcquireWrite();
            done = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::exit(done);
    }, testing::ExitedWithCode(0), "");
}

TEST(GC_utility__rw_lock, try_access_acquisitions_do_not_hang) {
    EXPECT_EXIT({
        std::atomic done = false;
        std::thread thread([&]() {
            gc::rw_lock lock;
            lock.AcquireWrite();
            lock.TryAcquireRead();
            lock.TryAcquireWrite();
            done = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::exit(done);
    }, testing::ExitedWithCode(1), "");

    EXPECT_EXIT({
        std::atomic done = false;
        std::thread thread([&]() {
            gc::rw_lock lock;
            lock.AcquireRead();
            lock.AcquireRead();
            lock.TryUpgrade();
            done = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::exit(done);
    }, testing::ExitedWithCode(1), "");
}

TEST(GC_utility__scoped_rw_lock, unlocks_after_going_out_of_scope) {
    EXPECT_EXIT({
        std::atomic done = false;
        std::thread thread([&]() {
            gc::rw_lock lock;
            {
                gc::scoped_rw_lock sLock(lock);
            }
            lock.AcquireWrite();

            done = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::exit(done);
    }, testing::ExitedWithCode(1), "");
}

TEST(GC_utility__scoped_rw_lock, throws_when_trying_to_upgrade_an_unacquired_lock) {
    EXPECT_EXIT({
        std::atomic done = false;
        std::thread thread([&]() {
            gc::rw_lock lock;
            lock.AcquireWrite();
            {
                gc::scoped_rw_lock sLock(lock, gc::scoped_rw_lock::mode::try_ro);
                if (sLock.Acquired()) {
                    std::exit(2);
                }

                try {
                    sLock.Upgrade();
                } catch (gc::bad_api_usage&) {
                    std::exit(3);
                } catch (std::exception&) {
                    std::exit(4);
                }
            }

            done = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::exit(done);
    }, testing::ExitedWithCode(3), "");
}

TEST(GC_utility__mutex, only_one_access_can_exist_at_a_time) {
    EXPECT_EXIT({
        std::atomic done = false;
        std::thread thread([&]() {
            gc::mutex lock;
            lock.Acquire();
            done = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::exit(done);
    }, testing::ExitedWithCode(1), "");

    EXPECT_EXIT({
        std::atomic done = false;
        std::thread thread([&]() {
            gc::mutex lock;
            lock.Acquire();
            lock.Acquire();
            done = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::exit(done);
    }, testing::ExitedWithCode(0), "");
}

TEST(GC_utility__mutex, release_works) {
    EXPECT_EXIT({
        std::atomic done = false;
        std::thread thread([&]() {
            gc::mutex lock;
            lock.Acquire();
            lock.Release();
            lock.Acquire();
            done = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::exit(done);
    }, testing::ExitedWithCode(1), "");
}

TEST(GC_utility__scoped_mutex_lock, get_will_hang_when_reacquiring) {
    EXPECT_EXIT({
        std::atomic done = false;
        std::thread thread([&]() {
            gc::mutex m;
            m.Acquire();
            {
                gc::scoped_mutex_lock lock(m, gc::scoped_mutex_lock::mode::get);
            }
            m.Acquire();
            done = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::exit(done);
    }, testing::ExitedWithCode(0), "");
}

TEST(GC_utility__scoped_mutex_lock, try_will_not_hang) {
    EXPECT_EXIT({
        std::atomic done = false;
        std::thread thread([&]() {
            gc::mutex m;
            m.Acquire();
            {
                gc::scoped_mutex_lock lock(m, gc::scoped_mutex_lock::mode::try_only);
            }
            done = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::exit(done);
    }, testing::ExitedWithCode(1), "");
}

TEST(GC_utility__scoped_mutex_lock, release_works) {
    EXPECT_EXIT({
        std::atomic done = false;
        std::thread thread([&]() {
            gc::mutex m;
            {
                gc::scoped_mutex_lock lock(m);
            }
            m.Acquire();
            done = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::exit(done);
    }, testing::ExitedWithCode(1), "");
}

TEST(GC_utility__ptr_safe_container__node, slot_correctness) {
    EXPECT_EQ(gc::ptr_safe_container<uint8_t>::node::capacity, sizeof(gc::ptr_safe_container<uint8_t>::node::takenFlags) * CHAR_BIT) << "every slot should have a bit in the freeFlags";
    ASSERT_EQ(gc::ptr_safe_container<uint8_t>::node::capacity, 64) << "rest of the tests assume this to be 64";

    gc::ptr_safe_container<uint8_t>::node node{};

    node.takenFlags = 0;
    EXPECT_TRUE(node.HasFreeSlot());

    node.takenFlags = std::numeric_limits<uint64_t>::max();
    EXPECT_FALSE(node.HasFreeSlot());

    node.takenFlags = 0;
    EXPECT_EQ(node.NextFreeSlot(), 0);

    node.takenFlags = 1;
    EXPECT_EQ(node.NextFreeSlot(), 1);

    node.takenFlags = 3;
    EXPECT_EQ(node.NextFreeSlot(), 2);

    node.takenFlags = 0;
    EXPECT_TRUE(node.GetSlotIsFree(0));

    node.takenFlags = 1;
    EXPECT_FALSE(node.GetSlotIsFree(0));
    EXPECT_TRUE(node.GetSlotIsFree(1));

    node.takenFlags = 0;
    node.SetSlotIsFree(0, false);
    EXPECT_EQ(node.takenFlags, 1);
    node.SetSlotIsFree(1, false);
    EXPECT_EQ(node.takenFlags, 3);
    node.SetSlotIsFree(2, false);
    EXPECT_EQ(node.takenFlags, 7);

    node.SetSlotIsFree(3, true);
    EXPECT_EQ(node.takenFlags, 7);
    node.SetSlotIsFree(2, true);
    EXPECT_EQ(node.takenFlags, 3);
    node.SetSlotIsFree(1, true);
    EXPECT_EQ(node.takenFlags, 1);
    node.SetSlotIsFree(0, true);
    EXPECT_EQ(node.takenFlags, 0);

    node.takenFlags = 0;
    EXPECT_EQ(node.NextFreeSlot(), 0);
    node.SetSlotIsFree(0, false);
    EXPECT_EQ(node.NextFreeSlot(), 1);
    node.SetSlotIsFree(1, false);
    EXPECT_EQ(node.NextFreeSlot(), 2);
    node.SetSlotIsFree(2, false);
    EXPECT_EQ(node.NextFreeSlot(), 3);
    node.SetSlotIsFree(3, false);
    EXPECT_EQ(node.NextFreeSlot(), 4);
}

TEST(GC_utility__ptr_safe_container__node, throws_on_out_of_bounds_slot_operation_when_exceptions_are_enabled) {
    if constexpr (!gc::config::gc_throw_nonessential_exceptions) {
        GTEST_SKIP() << "non-essential exceptions are disabled";
    }

    EXPECT_THROW((void)gc::ptr_safe_container<uint8_t>::node().GetSlotIsFree(64), std::out_of_range);
    EXPECT_THROW(gc::ptr_safe_container<uint8_t>::node().SetSlotIsFree(64, false), std::out_of_range);
}

TEST(GC_utility__ptr_safe_container__node, does_not_throw_on_out_of_bounds_slot_operation_when_exceptions_are_disabled) {
    if constexpr (gc::config::gc_throw_nonessential_exceptions) {
        GTEST_SKIP() << "non-essential exceptions are enabled";
    }

    EXPECT_NO_THROW((void)gc::ptr_safe_container<uint8_t>::node().GetSlotIsFree(64));
    EXPECT_NO_THROW(gc::ptr_safe_container<uint8_t>::node().SetSlotIsFree(64, false));
}

TEST(GC_utility__ptr_safe_container, allocates_the_first_node_on_construction) {
    EXPECT_THROW(gc::ptr_safe_container<uint8_t>({std::pmr::null_memory_resource()}, {std::pmr::null_memory_resource()}), std::bad_alloc);
}

namespace gc {
    class ptr_safe_container_test_access {
public:
        template <class T>
        using actual_node_type = ptr_safe_container<T>::node_mimick;

        template <class T>
        static auto &GetNodes(ptr_safe_container<T>& container) {
            return container.nodes;
        }
    };
}

class ptr_safe_container_no_double_free {
    int &ref;
public:
    ptr_safe_container_no_double_free(int& ref) : ref(ref) {}
    ~ptr_safe_container_no_double_free() {
        ++ref;
    }
};

class allocation_tracking_resource : public std::pmr::monotonic_buffer_resource {
public:
    size_t allocationCount;
private:
    void *do_allocate(std::size_t __bytes, std::size_t __alignment) override {
        allocationCount++;
        return monotonic_buffer_resource::do_allocate(__bytes, __alignment);
    }

    void do_deallocate(void *__ptr, std::size_t __bytes, std::size_t __alignment) override {
        allocationCount--;
        monotonic_buffer_resource::do_deallocate(__ptr, __bytes, __alignment);
    }
};

TEST(GC_utility__ptr_safe_container, strong_exception_guarantee_for_allocator_failure) {
    {
        std::array<uint8_t, sizeof(gc::ptr_safe_container_test_access::actual_node_type<uint8_t>)> buffer{0};
        auto res = std::pmr::monotonic_buffer_resource(buffer.data(), buffer.size(), std::pmr::null_memory_resource());
        ASSERT_NO_THROW(gc::ptr_safe_container<uint8_t>({&res}, {})) << "less than the minimal amount of memory required for construction";
    }

    // first, the node is allocated and then, the pointer to it gets added to nodes
    {
        std::array<uint8_t, sizeof(gc::ptr_safe_container_test_access::actual_node_type<uint8_t>)> buffer{0};
        auto res = std::pmr::monotonic_buffer_resource(buffer.data(), buffer.size(), std::pmr::null_memory_resource());
        auto data = gc::ptr_safe_container<uint8_t>({&res}, {});

        for (size_t i = 0; i < gc::ptr_safe_container<uint8_t>::node::capacity; ++i) {
            EXPECT_NO_THROW(data.GetNextUninitialized()) << "shouldn't throw node fullness is only " << i << "/" << static_cast<size_t>(gc::ptr_safe_container<uint8_t>::node::capacity);
        }

        // throwing when allocating the new node
        auto prev = gc::ptr_safe_container_test_access::GetNodes(data);
        EXPECT_THROW(data.GetNextUninitialized(), std::bad_alloc);
        EXPECT_EQ(prev.size(), gc::ptr_safe_container_test_access::GetNodes(data).size());
        EXPECT_THAT(prev, testing::ContainerEq(gc::ptr_safe_container_test_access::GetNodes(data)));
    }

    {
        std::array<uint8_t, sizeof(void*) * 2> buffer{0};
        auto res = std::pmr::monotonic_buffer_resource(buffer.data(), buffer.size(), std::pmr::null_memory_resource());
        auto nodeRes = allocation_tracking_resource();
        auto data = gc::ptr_safe_container<uint8_t>({&nodeRes}, {&res});

        for (size_t i = 0; i < gc::ptr_safe_container<uint8_t>::node::capacity; ++i) {
            EXPECT_NO_THROW(data.GetNextUninitialized()) << "shouldn't throw node fullness is only " << i << "/" << static_cast<size_t>(gc::ptr_safe_container<uint8_t>::node::capacity);
        }

        // throwing after allocating the new node
        EXPECT_EQ(nodeRes.allocationCount, 1); // initial node
        EXPECT_THROW(data.GetNextUninitialized(), std::bad_alloc); // 2nd node
        EXPECT_EQ(nodeRes.allocationCount, 1); // 2nd node deallocated
        // make sure the node was deallocated
    }
}

TEST(GC_utility__ptr_safe_container, object_destructor_gets_called_only_once) {
    int i = 0;
    {
        gc::ptr_safe_container<ptr_safe_container_no_double_free>().Emplace(i);
    }
    EXPECT_EQ(i, 1);
}

template<>
struct gc::gc_object_traits<int> {
    constexpr static bool supported = true;
    constexpr static get_field_count_fn get_field_count = [](const void*) noexcept -> size_t{ return 0; };
    constexpr static get_field_fn get_field = [](void*, size_t) noexcept -> handle_t**{ return nullptr; };
    constexpr static move_fn move = [](void *obj, void *newLocation) noexcept { *(int*)newLocation = *(int*)obj;}; // trivial
    constexpr static destructor_fn destructor = [](void*) noexcept {}; // trivial
};

TEST(GC_collecting_allocator, this_should_work) { // temporary test
    bool success = false;
    gc::root_handle<int> handle = gc::null_handle;
    EXPECT_NO_THROW(success = gc::Init(gc::gc_init_args{std::pmr::get_default_resource(), std::pmr::get_default_resource()}));
    ASSERT_TRUE(success);
    EXPECT_NO_THROW(handle = gc::New<int>());
    EXPECT_NE(handle, gc::null_handle);
    EXPECT_NO_THROW(gc::Destroy());
}

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}