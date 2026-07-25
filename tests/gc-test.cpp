#include <memory_resource>
#include <cpuid.h>

#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>

#include <gca/gc-internal.h>
#include <gca/gc.h>

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

TEST(GC_utility__ptr_safe_container, does_not_allocates_the_first_node_on_construction) {
    EXPECT_NO_THROW(gc::ptr_safe_container<uint8_t>({std::pmr::null_memory_resource()}, {std::pmr::null_memory_resource()}));
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

template <class T>
struct default_gc_object_type {
    static constexpr gc::object_type value{
        .size = sizeof(T),
        .alignment = alignof(T),
        .type = &typeid(T), // static storage duration
        .destructor = [](void *obj) noexcept { if constexpr (std::is_destructible_v<T>) {
            std::destroy_at<T>(static_cast<T *>(obj));
        }},
        .getFieldCount = [](const void*) noexcept -> size_t {return 0;},
        .getField = [](void*, size_t) noexcept -> gc::internal_handle** {return nullptr;},
        .move = [](void *obj, void *newLocation) noexcept {
            *(T*)newLocation = std::move(*(T*)obj);
        }
            //static_cast<gc::move_fn>(nullptr) // FIXME: pointers and move do not play nice with each other
    };
};

template <class T>
constexpr gc::object_type default_gc_object_type_v = default_gc_object_type<T>::value;

TEST(GC_collecting_allocator, this_should_work) { // temporary test
    bool success = false;
    gc::root_handle<int> handle = gc::null_handle;
    EXPECT_NO_THROW(success = gc::Init(gc::gc_init_args{std::pmr::get_default_resource(), std::pmr::get_default_resource()}));
    ASSERT_TRUE(success);
    EXPECT_NO_THROW(handle = gc::New<int>());
    EXPECT_NE(handle, gc::null_handle);
    EXPECT_NO_THROW(gc::Destroy());
}

TEST(GC_collecting_allocator__page, GetAlignmentCorrection_is_correct) {
    EXPECT_EQ(gc::page::GetAlignmentCorrection(0, 1), 0);
    EXPECT_EQ(gc::page::GetAlignmentCorrection(0, 2), 0);
    EXPECT_EQ(gc::page::GetAlignmentCorrection(1, 1), 0);
    EXPECT_EQ(gc::page::GetAlignmentCorrection(1, 2), 1);
    EXPECT_EQ(gc::page::GetAlignmentCorrection(2, 1), 0);
    EXPECT_EQ(gc::page::GetAlignmentCorrection(2, 2), 0);
    EXPECT_EQ(gc::page::GetAlignmentCorrection(2, 4), 2);
}

TEST(GC_collecting_allocator__page, allocation_strategy_allocations_are_incrementing) {
    gc::page page{std::pmr::get_default_resource()};
    page.memory = new gc::page_memory;
    gc::defer d([&]{delete page.memory;});

    auto cAllocation = page.TryAllocate(1, 1);
    auto iAllocation = page.TryAllocate(4, 4);

    EXPECT_FALSE(cAllocation.empty());
    EXPECT_FALSE(iAllocation.empty());

    EXPECT_GT(iAllocation.data(), cAllocation.data()) << "allocations should be incrementing";
}

TEST(GC_collecting_allocator__page, allocation_strategy_respects_alignment) {
    gc::page page{std::pmr::get_default_resource()};
    page.memory = new gc::page_memory;
    gc::defer d([&]{delete page.memory;});

    auto cAllocation = page.TryAllocate(1, 1);
    auto iAllocation = page.TryAllocate(4, 4);

    EXPECT_FALSE(cAllocation.empty());
    EXPECT_FALSE(iAllocation.empty());

    EXPECT_EQ(cAllocation.data(), page.memory->data()) << "wasted 1 byte when aligning to 1";
    EXPECT_EQ(reinterpret_cast<uintptr_t>(cAllocation.data()) + cAllocation.size() + 3, reinterpret_cast<uintptr_t>(iAllocation.data())) << "there should be a 3 byte gap between the allocations if the alignment is to be respected";
}

TEST(GC_collecting_allocator__page, allocation_strategy_does_not_waste_space) {
    gc::page page{std::pmr::get_default_resource()};
    page.memory = new gc::page_memory;
    gc::defer d([&]{delete page.memory;});

    auto cAllocation = page.TryAllocate(1, 1);
    auto iAllocation = page.TryAllocate(4, 4);
    auto fAllocation = page.TryAllocate(3, 1);

    EXPECT_FALSE(cAllocation.empty());
    EXPECT_FALSE(iAllocation.empty());
    EXPECT_FALSE(fAllocation.empty());

    EXPECT_GT(fAllocation.data(), cAllocation.data()) << "fAllocation should fit between the gap of cAllocation and iAllocation";
    EXPECT_LT(fAllocation.data(), iAllocation.data()) << "fAllocation should fit between the gap of cAllocation and iAllocation";
}

TEST(GC_collecting_allocator__page, memory_is_aligned_to_page_alignment) {
    gc::page page{std::pmr::get_default_resource()};
    page.memory = new gc::page_memory;
    gc::defer d([&]{delete page.memory;});

    EXPECT_EQ(gc::page::GetAlignmentCorrection(reinterpret_cast<uintptr_t>(page.memory), gc::config::page_alignment), 0);
}

TEST(GC_collecting_allocator__page, memory_is_zeroed_out) {
    gc::page page{std::pmr::get_default_resource()};
    page.memory = new gc::page_memory;
    gc::defer d([&]{delete page.memory;});

    EXPECT_EQ(std::ranges::find_if(page.memory->memory, [](const std::byte it) { return it != static_cast<std::byte>(0); }), page.memory->memory.end());
}

TEST(GC_collecting_allocator__page, allocation_strategy_can_handle_page_size_allocations) {
    gc::page page{std::pmr::get_default_resource()};
    page.memory = new gc::page_memory;
    gc::defer d([&]{delete page.memory;});

    const auto allocation = page.TryAllocate(gc::config::page_size, gc::config::page_alignment);
    EXPECT_FALSE(allocation.empty());
}

TEST(GC_collecting_allocator__page, allocation_strategy_gracefully_fails_large_allocations) {
    gc::page page{std::pmr::get_default_resource()};
    page.memory = new gc::page_memory;
    gc::defer d([&]{delete page.memory;});
    EXPECT_TRUE(page.TryAllocate(gc::config::page_size + 1, 1).empty());
}

TEST(GC_collecting_allocator__page, RemoveAllocation_removes_valid_allocation) {
    gc::page page{std::pmr::get_default_resource()};
    page.memory = new gc::page_memory;
    gc::defer d([&]{delete page.memory;});

    auto allocation = page.TryAllocate(1, 1);
    EXPECT_FALSE(allocation.empty());
    EXPECT_EQ(page.allocations.size(), 1);
    page.RemoveAllocation(allocation);
    EXPECT_EQ(page.allocations.size(), 0);
}

TEST(GC_collecting_allocator__page, RemoveAllocation_does_not_remove_invalid_allocation) {
    gc::page page{std::pmr::get_default_resource()};
    page.memory = new gc::page_memory;
    gc::defer d([&]{delete page.memory;});

    auto allocation = page.TryAllocate(1, 1);
    EXPECT_FALSE(allocation.empty());
    EXPECT_EQ(page.allocations.size(), 1);
    page.RemoveAllocation({});
    EXPECT_EQ(page.allocations.size(), 1);
    page.RemoveAllocation({page.memory->data() - 1, 1});
    EXPECT_EQ(page.allocations.size(), 1);
    page.RemoveAllocation({allocation.data(), allocation.size() + 1});
    EXPECT_EQ(page.allocations.size(), 1);
}

TEST(GC_collecting_allocator__gc_impl, constructor_does_not_allocate) {
    EXPECT_NO_THROW(gc::gc_impl({std::pmr::null_memory_resource(), std::pmr::null_memory_resource()}));
}

TEST(GC_collecting_allocator__gc_impl, constructor_does_not_accept_null_memory_resource_pointer) {
    EXPECT_THROW(gc::gc_impl({std::pmr::get_default_resource(), nullptr}), gc::bad_api_usage);
    EXPECT_THROW(gc::gc_impl({nullptr, std::pmr::get_default_resource()}), gc::bad_api_usage);
    EXPECT_THROW(gc::gc_impl({nullptr, nullptr}), gc::bad_api_usage);
}

TEST(GC_collecting_allocator__gc_impl, default_constructor_does_not_throw) {
    EXPECT_EXIT({gc::gc_impl GC{}; std::exit(0);}, testing::ExitedWithCode(0), "");
}

TEST(GC_collecting_allocator__gc_impl, default_constructor_cannot_allocate) {
    EXPECT_THROW(gc::gc_impl().Allocate(default_gc_object_type_v<int>, 1), std::bad_alloc);
}

TEST(GC_collecting_allocator__gc_impl, Allocate_count_allocations_are_sequential) {
    gc::gc_impl GC({std::pmr::get_default_resource(), std::pmr::get_default_resource()});
    auto handle = GC.Allocate(default_gc_object_type_v<int>, 10);
    EXPECT_EQ(handle->objectAllocation.size(), sizeof(int) * 10);
}

TEST(GC_collecting_allocator__gc_impl, Allocate_allocated_handles_count_as_root_handles) {
    gc::gc_impl GC({std::pmr::get_default_resource(), std::pmr::get_default_resource()});
    auto handle = GC.Allocate(default_gc_object_type_v<int>, 10);
    EXPECT_EQ(handle->rootHandleCount, 1);
}

TEST(GC_collecting_allocator__gc_impl, Allocate_triggers_collection_when_page_is_full) {
    gc::gc_impl GC({std::pmr::get_default_resource(), std::pmr::get_default_resource()});
    auto collected = false;
    GC.debugListeners.onBeforeCollectionStart = [&](const gc::debug::callback_data&, gc::page&) {collected = true;};
    GC.Allocate(default_gc_object_type_v<std::byte>, gc::config::page_size);
    EXPECT_FALSE(collected); // no gc yet
    GC.Allocate(default_gc_object_type_v<int>, 1);
    EXPECT_TRUE(collected);
}

TEST(GC_collecting_allocator__gc_impl, Collect_collects_dead_objects_that_are_not_roots) {
    gc::gc_impl GC({std::pmr::get_default_resource(), std::pmr::get_default_resource()});
    auto collected = false;
    auto handleCollected = false;
    GC.debugListeners.onBeforeCollectionStart = [&](const gc::debug::callback_data&, gc::page&) {collected = true;};
    auto handle = GC.Allocate(default_gc_object_type_v<std::byte>, gc::config::page_size);
    GC.debugListeners.onBeforeObjectDestroyed = [&](const gc::debug::callback_data&, const gc::internal_handle *invalidHandle) {if (invalidHandle == handle) { handleCollected = true; }};
    auto allocationPos = handle->objectAllocation.data();
    EXPECT_FALSE(collected); // no gc yet
    handle->rootHandleCount = 0;
    auto otherHandle = GC.Allocate(default_gc_object_type_v<int>, 1);
    EXPECT_TRUE(collected);
    EXPECT_TRUE(handleCollected);
    EXPECT_EQ(otherHandle->objectAllocation.data(), allocationPos + gc::page::GetAlignmentCorrection(reinterpret_cast<uintptr_t>(allocationPos), alignof(int)));
}

struct field_test {
    gc::internal_handle *field = nullptr;
};

constexpr gc::object_type field_test_type = {
    .size = sizeof(field_test),
    .alignment = alignof(field_test),
    .type = &typeid(field_test),
    .destructor = [](void*) noexcept {},
    .getFieldCount = [](const void*) noexcept {return 1ULL;},
    .getField = [](void* obj, const size_t index) noexcept { return index == 0 ? &static_cast<field_test *>(obj)->field : nullptr; },
    .move = [](void *obj, void *newLocation) noexcept { *static_cast<field_test *>(newLocation) = *static_cast<field_test *>(obj); }
};

TEST(GC_collecting_allocator__gc_impl, Collect_does_not_collect_alive_root_objects) {
    gc::gc_impl GC({std::pmr::get_default_resource(), std::pmr::get_default_resource()});
    auto collected = false;
    auto handleCollected = false;
    GC.debugListeners.onBeforeCollectionStart = [&](const gc::debug::callback_data&, gc::page&) {collected = true;};
    auto handle = GC.Allocate(default_gc_object_type_v<std::byte>, gc::config::page_size);
    GC.debugListeners.onBeforeObjectDestroyed = [&](const gc::debug::callback_data&, const gc::internal_handle *invalidHandle) {if (invalidHandle == handle) { handleCollected = true; }};
    EXPECT_FALSE(collected); // no gc yet
    GC.Allocate(default_gc_object_type_v<int>, 1);
    EXPECT_TRUE(collected);
    EXPECT_FALSE(handleCollected);
}

TEST(GC_collecting_allocator__gc_impl, Collect_does_not_collect_reachable_non_root_objects) {
    gc::gc_impl GC({std::pmr::get_default_resource(), std::pmr::get_default_resource()});
    auto collected = false;
    auto hIntCollected = false;
    GC.debugListeners.onBeforeCollectionStart = [&](const gc::debug::callback_data&, gc::page&) {collected = true;};
    const auto hFieldTest = GC.Allocate(field_test_type, 1);
    const auto hInt = GC.Allocate(default_gc_object_type_v<int>, 1);
    hFieldTest->SetField(field_test_type.getField(hFieldTest->objectAllocation.data(), 0), hInt);
    hInt->rootHandleCount = 0;
    GC.debugListeners.onBeforeObjectDestroyed = [&](const gc::debug::callback_data&, const gc::internal_handle *handle) { if (handle == hInt) { hIntCollected = true; } };
    EXPECT_FALSE(collected); // no gc yet
    GC.Allocate(default_gc_object_type_v<std::byte>, gc::config::page_size);
    EXPECT_TRUE(collected);
    EXPECT_FALSE(hIntCollected);
}

TEST(GC_collecting_allocator__gc_impl, Collect_collects_dead_objects_that_are_not_roots_and_not_fields_of_any_reachable_objects_but_are_fields_of_dead_objects) {
    gc::gc_impl GC({std::pmr::get_default_resource(), std::pmr::get_default_resource()});
    auto collected = false;
    auto hIntCollected = false;
    auto hFieldTestCollected = false;
    GC.debugListeners.onBeforeCollectionStart = [&](const gc::debug::callback_data&, gc::page&) {collected = true;};
    const auto hFieldTest = GC.Allocate(field_test_type, 1);
    const auto hInt = GC.Allocate(default_gc_object_type_v<int>, 1);
    hFieldTest->SetField(field_test_type.getField(hFieldTest->objectAllocation.data(), 0), hInt);
    hInt->rootHandleCount = 0;
    hFieldTest->rootHandleCount = 0;
    GC.debugListeners.onBeforeObjectDestroyed = [&](const gc::debug::callback_data&, const gc::internal_handle *handle) {
        if (handle == hInt) {
            hIntCollected = true;
        }
        if (handle == hFieldTest) {
            hFieldTestCollected = true;
        }
    };
    EXPECT_FALSE(collected); // no gc yet
    GC.Allocate(default_gc_object_type_v<std::byte>, gc::config::page_size);
    EXPECT_TRUE(collected);
    EXPECT_TRUE(hIntCollected);
    EXPECT_TRUE(hFieldTestCollected);
}

// Source - https://stackoverflow.com/a/71009896
// Posted by Marek R, modified by community. See post 'Timeline' for change history
// Retrieved 2026-07-21, License - CC BY-SA 4.0

class TerminateOnDeadlockGuard final
{
public:
    using Clock = std::chrono::system_clock;
    using Duration = Clock::duration;

    explicit TerminateOnDeadlockGuard(Duration timeout = std::chrono::seconds{30});

    ~TerminateOnDeadlockGuard();

private:
    void waitForCompletion();

private:
    const Duration m_timeout;
    std::mutex m_mutex;
    std::condition_variable m_wasCompleted;
    std::condition_variable m_guardStarted;
    std::thread m_guardThread;
};
//---------
TerminateOnDeadlockGuard::TerminateOnDeadlockGuard(Duration timeout) : m_timeout{timeout}
{
    m_guardThread = std::thread(&TerminateOnDeadlockGuard::waitForCompletion, this);
    std::unique_lock lock{m_mutex};
    m_guardStarted.wait(lock);
}

TerminateOnDeadlockGuard::~TerminateOnDeadlockGuard()
{
    m_wasCompleted.notify_one();
    m_guardThread.join();
}

void TerminateOnDeadlockGuard::waitForCompletion()
{
    std::unique_lock lock{m_mutex};
    m_guardStarted.notify_one();
    if (std::cv_status::timeout == m_wasCompleted.wait_for(lock, m_timeout))
    {
        std::cerr << "Test timeout!!!" << std::endl;
        std::exit(-1); // you can use `abort` here to create crash log.
    }
}

TEST(GC_collecting_allocator__gc_impl, scanning_a_field_of_an_object_being_destroyed_wont_cause_a_deadlock) {
    EXPECT_EXIT({
        TerminateOnDeadlockGuard guard{};
        {
            gc::gc_impl GC({std::pmr::get_default_resource(), std::pmr::get_default_resource()});
            auto hFieldTest = GC.Allocate(field_test_type, 1);
            auto hInt = GC.TryAllocateOnNewPage(default_gc_object_type_v<int>, 1);
            auto hFieldTestCollected = false;

            hFieldTest->SetField(field_test_type.getField(hFieldTest->objectAllocation.data(), 0), hInt);
            hFieldTest->rootHandleCount = 0;

            GC.debugListeners.onBeforeDestroyObjectFieldOldValueRWLockAcquire = [](
                const gc::debug::callback_data& data,
                gc::internal_handle *,
                gc::internal_handle *fieldOldValue,
                size_t) {
                fieldOldValue->objectLock.AcquireRead();
                // scanning done on another thread
                std::thread t([=] {
                    std::this_thread::sleep_for(std::chrono::milliseconds{100}); // make sure we don't finish before we try to acquire the rw lock in DestroyObject
                    data.impl->TryFindRootFor(fieldOldValue, 1); // collection thread ID should be 1
                    fieldOldValue->objectLock.Release();
                });
                t.detach();
            };

            GC.debugListeners.onAfterObjectDestroyed = [&](const gc::debug::callback_data&, const gc::internal_handle *handle) {
                if (handle == hFieldTest) {
                    hFieldTestCollected = true;
                }
            };

            GC.Allocate(default_gc_object_type_v<std::byte>, gc::config::page_size); // will trigger a collection on the page where hFieldTest is allocated
            EXPECT_TRUE(hFieldTestCollected);
        }
        std::exit(::testing::Test::HasFailure() ? 2 : 0);
    }, testing::ExitedWithCode(0), "");
}

TEST(GC_collecting_allocator__gc_impl, DestroyObject_will_not_destroy_uninitialized_objects) {
    static size_t destructionCount;
    gc::gc_impl GC({std::pmr::get_default_resource(), std::pmr::get_default_resource()});
    const gc::object_type tInt {
        .size = sizeof(int),
        .alignment = alignof(int),
        .type = &typeid(int),
        .destructor = [](void*) noexcept {++destructionCount;},
        .getFieldCount = [](const void*) noexcept { return 0ULL; },
        .getField = [](void*, size_t) noexcept -> gc::internal_handle** { return nullptr; },
        .move = nullptr
    };

    destructionCount = 0;
    GC.Allocate(tInt, 1)->rootHandleCount = 0;
    GC.Allocate(default_gc_object_type_v<std::byte>, gc::config::page_size);
    EXPECT_EQ(destructionCount, 0);
}

TEST(GC_collecting_allocator__gc_impl, DestroyObject_sitll_frees_the_space_for_uninitialized_objects) {
    gc::gc_impl GC({std::pmr::get_default_resource(), std::pmr::get_default_resource()});
    const gc::object_type tInt {
        .size = sizeof(int),
        .alignment = alignof(int),
        .type = &typeid(int),
        .destructor = [](void*) noexcept {},
        .getFieldCount = [](const void*) noexcept { return 0ULL; },
        .getField = [](void*, size_t) noexcept -> gc::internal_handle** { return nullptr; },
        .move = nullptr
    };

    GC.Allocate(tInt, 1)->rootHandleCount = 0;
    GC.Allocate(default_gc_object_type_v<std::byte>, gc::config::page_size);
    EXPECT_EQ(GC.pages.size(), 1) << "uninitialized garbage object not deallocated";
}

TEST(GC_collecting_allocator__gc_impl, DestroyObject_will_destroy_initialized_objects) {
    static size_t destructionCount;
    gc::gc_impl GC({std::pmr::get_default_resource(), std::pmr::get_default_resource()});
    const gc::object_type tInt {
        .size = sizeof(int),
        .alignment = alignof(int),
        .type = &typeid(int),
        .destructor = [](void*) noexcept {++destructionCount;},
        .getFieldCount = [](const void*) noexcept { return 0ULL; },
        .getField = [](void*, size_t) noexcept -> gc::internal_handle** { return nullptr; },
        .move = nullptr
    };

    destructionCount = 0;
    auto hInt = GC.Allocate(tInt, 1);
    hInt->rootHandleCount = 0;
    hInt->flags.SetFlag(gc::object_flags::uninitialized, false);
    GC.Allocate(default_gc_object_type_v<std::byte>, gc::config::page_size);
    EXPECT_EQ(destructionCount, 1);
}

TEST(GC_collecting_allocator__gc_impl, DestroyObject_will_destroy_each_member_of_an_initialized_array) {
    static size_t destructionCount;
    gc::gc_impl GC({std::pmr::get_default_resource(), std::pmr::get_default_resource()});
    const gc::object_type tInt {
        .size = sizeof(int),
        .alignment = alignof(int),
        .type = &typeid(int),
        .destructor = [](void*) noexcept {++destructionCount;},
        .getFieldCount = [](const void*) noexcept { return 0ULL; },
        .getField = [](void*, size_t) noexcept -> gc::internal_handle** { return nullptr; },
        .move = nullptr
    };

    destructionCount = 0;
    auto hIntArray = GC.Allocate(tInt, 2);
    hIntArray->rootHandleCount = 0;
    hIntArray->flags.SetFlag(gc::object_flags::uninitialized, false);
    GC.Allocate(default_gc_object_type_v<std::byte>, gc::config::page_size);
    EXPECT_EQ(destructionCount, 2);
}

TEST(GC_collecting_allocator__gc_impl, DestroyObject_will_destroy_objects_that_have_attempted_initialization) {
    static size_t destructionCount;
    gc::gc_impl GC({std::pmr::get_default_resource(), std::pmr::get_default_resource()});
    const gc::object_type tInt {
        .size = sizeof(int),
        .alignment = alignof(int),
        .type = &typeid(int),
        .destructor = [](void*) noexcept {++destructionCount;},
        .getFieldCount = [](const void*) noexcept { return 0ULL; },
        .getField = [](void*, size_t) noexcept -> gc::internal_handle** { return nullptr; },
        .move = nullptr
    };

    destructionCount = 0;
    auto hInt = GC.Allocate(tInt, 1);
    hInt->rootHandleCount = 0;
    hInt->flags.SetFlag(gc::object_flags::uninitialized, false);
    hInt->flags.SetFlag(gc::object_flags::initialization_failed, true);
    GC.Allocate(default_gc_object_type_v<std::byte>, gc::config::page_size);
    EXPECT_EQ(destructionCount, 1);
}

TEST(GC_collecting_allocator__gc_impl, defragmentation_respects_alignment) {
    gc::gc_impl GC({std::pmr::get_default_resource(), std::pmr::get_default_resource()});
    GC.Allocate(default_gc_object_type_v<std::byte>, 1);
    GC.Allocate(default_gc_object_type_v<std::byte>, sizeof(int))->rootHandleCount = 0; // allocation that could fit an int but is not aligned to the alignment of int
    auto hInt = GC.Allocate(default_gc_object_type_v<int>, 1); // there are padding bytes before this allocation
    GC.Allocate(default_gc_object_type_v<std::byte>, gc::config::page_size);
    EXPECT_EQ(gc::page::GetAlignmentCorrection(reinterpret_cast<uintptr_t>(hInt->objectAllocation.data()), alignof(int)), 0) << "allocation is no longer aligned";
}

TEST(GC_collecting_allocator__gc_impl, defragmentation_respects_alignment_2) {
    gc::gc_impl GC({std::pmr::get_default_resource(), std::pmr::get_default_resource()});
    GC.Allocate(default_gc_object_type_v<uint32_t>, 1);
    GC.Allocate(default_gc_object_type_v<uint32_t>, 2)->rootHandleCount = 0;
    GC.Allocate(default_gc_object_type_v<uint32_t>, 1)->rootHandleCount = 0;
    auto hInt = GC.Allocate(default_gc_object_type_v<uint64_t>, 1); // no padding this time
    GC.Allocate(default_gc_object_type_v<std::byte>, gc::config::page_size);
    EXPECT_EQ(gc::page::GetAlignmentCorrection(reinterpret_cast<uintptr_t>(hInt->objectAllocation.data()), alignof(int)), 0) << "allocation is no longer aligned";
}

TEST(GC_collecting_allocator__gc_impl, defragmentation_does_not_overwrite_memory_while_moving) {
    gc::gc_impl GC({std::pmr::get_default_resource(), std::pmr::get_default_resource()});

    const auto hToBeMovedHere = GC.Allocate(default_gc_object_type_v<int>, 1);
    hToBeMovedHere->rootHandleCount = 0;
    const auto hExpectedNewLocation = hToBeMovedHere->objectAllocation.data();

    const auto hIntArray = GC.Allocate(default_gc_object_type_v<int>, 10);
    hIntArray->flags.SetFlag(gc::object_flags::uninitialized, false);
    EXPECT_EQ(hIntArray->objectAllocation.size(), 10 * sizeof(int));
    auto content = std::span{reinterpret_cast<int *>(hIntArray->objectAllocation.data()), 10};
    for (auto i = 0; i < content.size(); ++i) {
        content[i] = i;
    }

    GC.Allocate(default_gc_object_type_v<std::byte>, gc::config::page_size); // will trigger a collection and defragmentation
    EXPECT_EQ(hIntArray->objectAllocation.data(), hExpectedNewLocation) << "allocation did not get shifted by 1 place";

    content = std::span{reinterpret_cast<int *>(hIntArray->objectAllocation.data()), 10};
    for (auto i = 0; i < content.size(); ++i) {
        EXPECT_EQ(content[i], i) << "object got overwritten during move";
    }
}

TEST(GC_collecting_allocator__gc_impl, defarmentation_does_not_move_immovable_types) {
    gc::gc_impl GC({std::pmr::get_default_resource(), std::pmr::get_default_resource()});
    const gc::object_type immovable {
        .size = sizeof(int),
        .alignment = alignof(int),
        .type = &typeid(int),
        .destructor = [](void*) noexcept{},
        .getFieldCount = [](const void*) noexcept -> size_t { return 0; },
        .getField = [](void*,size_t) noexcept -> gc::internal_handle** { return nullptr; },
        .move = nullptr
    };

    GC.Allocate(immovable, 1)->rootHandleCount = 0;
    auto *const hInt = GC.Allocate(immovable, 1);
    const auto oldLocation = hInt->objectAllocation.data();
    GC.Allocate(default_gc_object_type_v<std::byte>, gc::config::page_size);
    EXPECT_EQ(oldLocation, hInt->objectAllocation.data());
}

TEST(GC_collecting_allocator__gc_impl, defarmentation_does_not_move_the_data_of_uninitialized_objects) {
    static size_t moveCount;
    gc::gc_impl GC({std::pmr::get_default_resource(), std::pmr::get_default_resource()});
    const gc::object_type tInt {
        .size = sizeof(int),
        .alignment = alignof(int),
        .type = &typeid(int),
        .destructor = [](void*) noexcept{},
        .getFieldCount = [](const void*) noexcept -> size_t { return 0; },
        .getField = [](void*,size_t) noexcept -> gc::internal_handle** { return nullptr; },
        .move = [](void*,void*) noexcept { ++moveCount; }
    };

    moveCount = 0;
    GC.Allocate(tInt, 1)->rootHandleCount = 0;
    GC.Allocate(tInt, 1);
    GC.Allocate(default_gc_object_type_v<std::byte>, gc::config::page_size);
    EXPECT_EQ(moveCount, 0);
}

TEST(GC_collecting_allocator__gc_impl, defarmentation_moves_the_data_of_initialized_objects) {
    static size_t moveCount;
    gc::gc_impl GC({std::pmr::get_default_resource(), std::pmr::get_default_resource()});
    const gc::object_type tInt {
        .size = sizeof(int),
        .alignment = alignof(int),
        .type = &typeid(int),
        .destructor = [](void*) noexcept{},
        .getFieldCount = [](const void*) noexcept -> size_t { return 0; },
        .getField = [](void*,size_t) noexcept -> gc::internal_handle** { return nullptr; },
        .move = [](void*,void*) noexcept { ++moveCount; }
    };

    moveCount = 0;
    GC.Allocate(tInt, 1)->rootHandleCount = 0;
    GC.Allocate(tInt, 1)->flags.SetFlag(gc::object_flags::uninitialized, false);
    GC.Allocate(default_gc_object_type_v<std::byte>, gc::config::page_size);
    EXPECT_EQ(moveCount, 1);
}

TEST(GC_collecting_allocator__gc_impl, defarmentation_does_not_move_the_data_of_objects_that_failed_to_initialize) {
    static size_t moveCount;
    gc::gc_impl GC({std::pmr::get_default_resource(), std::pmr::get_default_resource()});
    const gc::object_type tInt {
        .size = sizeof(int),
        .alignment = alignof(int),
        .type = &typeid(int),
        .destructor = [](void*) noexcept{},
        .getFieldCount = [](const void*) noexcept -> size_t { return 0; },
        .getField = [](void*,size_t) noexcept -> gc::internal_handle** { return nullptr; },
        .move = [](void*,void*) noexcept { ++moveCount; }
    };

    moveCount = 0;
    GC.Allocate(tInt, 1)->rootHandleCount = 0;
    auto flags = &GC.Allocate(tInt, 1)->flags;
    flags->SetFlag(gc::object_flags::uninitialized, false);
    flags->SetFlag(gc::object_flags::initialization_failed, true);
    GC.Allocate(default_gc_object_type_v<std::byte>, gc::config::page_size);
    EXPECT_EQ(moveCount, 0);
}

TEST(GC_collecting_allocator__gc_impl, defarmentation_moves_the_data_of_every_slot_if_the_object_is_an_array) {
    static size_t moveCount;
    gc::gc_impl GC({std::pmr::get_default_resource(), std::pmr::get_default_resource()});
    const gc::object_type tInt {
        .size = sizeof(int),
        .alignment = alignof(int),
        .type = &typeid(int),
        .destructor = [](void*) noexcept{},
        .getFieldCount = [](const void*) noexcept -> size_t { return 0; },
        .getField = [](void*,size_t) noexcept -> gc::internal_handle** { return nullptr; },
        .move = [](void*,void*) noexcept { ++moveCount; }
    };

    moveCount = 0;
    GC.Allocate(tInt, 1)->rootHandleCount = 0;
    GC.Allocate(tInt, 2)->flags.SetFlag(gc::object_flags::uninitialized, false);
    GC.Allocate(default_gc_object_type_v<std::byte>, gc::config::page_size);
    EXPECT_EQ(moveCount, 2);
}

TEST(GC_collecting_allocator__gc_impl, defarmentation_still_relocates_uninitialized_objects) {
    static size_t moveCount;
    gc::gc_impl GC({std::pmr::get_default_resource(), std::pmr::get_default_resource()});
    const gc::object_type tInt {
        .size = sizeof(int),
        .alignment = alignof(int),
        .type = &typeid(int),
        .destructor = [](void*) noexcept{},
        .getFieldCount = [](const void*) noexcept -> size_t { return 0; },
        .getField = [](void*,size_t) noexcept -> gc::internal_handle** { return nullptr; },
        .move = [](void*,void*) noexcept { ++moveCount; }
    };

    moveCount = 0;
    GC.Allocate(tInt, 1)->rootHandleCount = 0;
    auto hInt = GC.Allocate(tInt, 1);
    auto oldLocation = hInt->objectAllocation.data();
    GC.Allocate(default_gc_object_type_v<std::byte>, gc::config::page_size);
    EXPECT_EQ(moveCount, 0) << "uninitialized object moved";
    EXPECT_NE(oldLocation, hInt->objectAllocation.data()) << "uninitialized movable object not relocated";
}

template <class T> // can be anything as long as it's non-const
concept const_preserving_pin = requires(gc::root_handle<T> h, gc::field<0, 0, T> f, gc::root_handle<const T> cH, gc::field<0, 0, const T> cF)
{
    { h=gc::pin<T, true>{h} };
    { h=gc::pin<T, false>{h} };
    { f=gc::pin<T, true>{f} };
    { f=gc::pin<T, false>{f} };
    { cH=gc::pin<const T, true>{cH} };
    { cH=gc::pin<const T, true>{h} };
    { cF=gc::pin<const T, true>{f} };
    { cF=gc::pin<const T, true>{cF} };
} && !requires(gc::root_handle<const T> cH, gc::field<0, 0, const T> cF) {
    { cH=gc::pin<T, true>{cH} };
    { cH=gc::pin<T, false>{cH} };
    { cF=gc::pin<T, true>{cF} };
    { cF=gc::pin<T, false>{cF} };
    { cH=gc::pin<const T, false>{cH} };
    { cF=gc::pin<const T, false>{cF} };
};

TEST(GC_collecting_allocator__public_API, const_preservance) {
    static_assert(const_preserving_pin<int>);
}

TEST(GC_collecting_allocator__public_API, pin_types) {
    using non_const_ro_pin = gc::pin<int, true>;
    using non_const_rw_pin = gc::pin<int, false>;
    using const_ro_pin = gc::pin<const int, true>;
    // invalid
    // using const_rw_pin = gc::pin<const int, false>;

    using non_const_ro_array_pin = gc::pin<int[], true>;
    using non_const_rw_array_pin = gc::pin<int[], false>;
    // invalid
    // using const_rw_array_pin = gc::pin<const int[], false>;
    using const_ro_array_pin = gc::pin<const int[], true>;

    EXPECT_TYPE(decltype(non_const_ro_pin().Get()), const int*);
    EXPECT_TYPE(decltype(non_const_ro_pin().operator->()), const int*);
    EXPECT_TYPE(decltype(non_const_ro_pin().operator*()), const int&);

    EXPECT_TYPE(decltype(non_const_rw_pin().Get()), int*);
    EXPECT_TYPE(decltype(non_const_rw_pin().operator->()), int*);
    EXPECT_TYPE(decltype(non_const_rw_pin().operator*()), int&);

    EXPECT_TYPE(decltype(const_ro_pin().Get()), const int*);
    EXPECT_TYPE(decltype(const_ro_pin().operator->()), const int*);
    EXPECT_TYPE(decltype(const_ro_pin().operator*()), const int&);

    EXPECT_TYPE(decltype(non_const_ro_array_pin().Get()), const int*);
    EXPECT_TYPE(decltype(non_const_ro_array_pin().operator[](0)), const int&);
    EXPECT_TYPE(decltype(non_const_ro_array_pin().Count()), size_t);

    EXPECT_TYPE(decltype(non_const_rw_array_pin().Get()), int*);
    EXPECT_TYPE(decltype(non_const_rw_array_pin().operator[](0)), int&);
    EXPECT_TYPE(decltype(non_const_rw_array_pin().Count()), size_t);

    EXPECT_TYPE(decltype(const_ro_array_pin().Get()), const int*);
    EXPECT_TYPE(decltype(const_ro_array_pin().operator[](0)), const int&);
    EXPECT_TYPE(decltype(const_ro_array_pin().Count()), size_t);
}

struct BaseMember { [[nodiscard]] constexpr const void *GetThis() const noexcept { return this; } };
struct Base {
    uint64_t _;
    [[no_unique_address]]BaseMember member;
};

TEST(GC_assumption, this_in_a_member_with_no_unique_address_equals_to_the_pointer_to_the_object_that_has_the_member) {
    Base b;
    EXPECT_EQ(&b, b.member.GetThis());
}

TEST(GC_assumption, ptr_is_64_bits) {
    EXPECT_EQ(sizeof(void*), 8);
}

TEST(GC_assumption, virtual_address_size_is_48) {
    unsigned int eax, ebx, ecx, edx;
    ASSERT_NE(__get_cpuid(0x80000008, &eax, &ebx, &ecx, &edx), 0) << "cpu not supported";
    const auto addressBits = eax & 0x000000FF;
    EXPECT_EQ(addressBits, 48);
}

class ClassA {
    std::vector<int> toMove;
};

class ClassB {
    gc::field_store<1> fields{};
    public:
    ClassB(ClassB&&) = default;
    [[no_unique_address]]gc::field<0,0, ClassA> a;
    ClassB(const gc::root_handle<ClassA> &h) : a{h} {}
    template <bool ro>
    ClassB(const gc::pin<ClassA, ro> &p) : a{p} {}
    template <ptrdiff_t field_store_offset, size_t index>
    ClassB(const gc::field<field_store_offset, index, ClassA> &f) : a{f} {}
};

template <> struct gc::gc_object_traits<ClassA> {
    static constexpr get_field_count_fn get_field_count = [](const void*) noexcept -> size_t { return 0; };
    static constexpr get_field_fn get_field = [](void*,size_t) noexcept -> internal_handle** { return nullptr; };
    static constexpr move_fn move = [](void* obj, void* newLocation) noexcept {
        ClassA &a = *static_cast<ClassA *>(obj), &newA = *static_cast<ClassA *>(newLocation);
        std::construct_at(&newA, std::move(a));
    };
    static constexpr destructor_fn destructor = [](void *obj) noexcept { std::destroy_at(static_cast<ClassA *>(obj)); };
    static constexpr bool supported = true;
};

template <> struct gc::gc_object_traits<ClassB> {
    static constexpr get_field_count_fn get_field_count = [](const void*) noexcept -> size_t { return 1; };
    static constexpr get_field_fn get_field = [](void *obj, size_t idx) noexcept -> internal_handle** { return idx == 0 ? static_cast<ClassB *>(obj)->a.GetField() : nullptr; };
    static constexpr move_fn move = [](void* obj, void* newLocation) noexcept {
        ClassB &b = *static_cast<ClassB *>(obj), &newB = *static_cast<ClassB *>(newLocation);
        std::construct_at(&newB, std::move(b));
    };
    static constexpr destructor_fn destructor = [](void *obj) noexcept { std::destroy_at(static_cast<ClassB *>(obj)); };
    static constexpr bool supported = true;
};

template<class T>
class ClassC {
    gc::field_store<1> fields;
    constexpr static ptrdiff_t fields_offset = 0;
public:
    [[no_unique_address]]gc::field<fields_offset, 0, T> field;

    ClassC(const gc::root_handle<T> &h) :field(h) {}
    template <bool ro>
    ClassC(const gc::pin<T, ro> &p) :field(p) {}
    template <auto offset, auto index>
    ClassC(const gc::field<offset, index, T> &f) :field(f) {}

    ClassC(std::nullptr_t) :field(nullptr) {}
    ClassC(gc::null_handle_t) :field(gc::null_handle) {}

    ClassC(const gc::root_handle<std::remove_const_t<T>> &h) requires(std::is_const_v<T>) :field(h) {}
    template <bool ro>
    ClassC(const gc::pin<std::remove_const_t<T>, ro> &p) requires(std::is_const_v<T>) :field(p) {}
    template <auto offset, auto index>
    ClassC(const gc::field<offset, index, std::remove_const_t<T>> &f) requires(std::is_const_v<T>) :field(f) {}
};

template <class T> struct gc::gc_object_traits<ClassC<T>> {
    static constexpr get_field_count_fn get_field_count = [](const void*) noexcept -> size_t { return 1; };
    static constexpr get_field_fn get_field = [](void *obj, size_t idx) noexcept -> internal_handle** { return idx == 0 ? static_cast<ClassC<T> *>(obj)->field.GetField() : nullptr; };
    static constexpr move_fn move = [](void* obj, void* newLocation) noexcept {
        ClassB &b = *static_cast<ClassB *>(obj), &newB = *static_cast<ClassB *>(newLocation);
        std::construct_at(&newB, std::move(b));
    };
    static constexpr destructor_fn destructor = [](void *obj) noexcept { std::destroy_at(static_cast<ClassB *>(obj)); };
    static constexpr bool supported = true;
};

TEST(GC_collecting_allocator__public_API, compiles) {
    TerminateOnDeadlockGuard guard{};
    ASSERT_TRUE(gc::Init({std::pmr::get_default_resource(), std::pmr::get_default_resource()}));
    gc::defer destroy([]{gc::Destroy();});

    gc::root_handle<ClassB> h0 = gc::New<ClassB>(gc::New<ClassA>());
    gc::root_handle<ClassB> h1 = gc::New<ClassB>(gc::ROPin(h0)->a);
    gc::root_handle<ClassB> h2 = gc::New<ClassB>(gc::RWPin(h0)->a);
    EXPECT_THROW(gc::root_handle<ClassB> h3 = gc::New<ClassB>(gc::ROPin(gc::ROPin(h0)->a)), gc::bad_api_usage);
    gc::root_handle<ClassB> h4 = gc::New<ClassB>(gc::RWPin(gc::ROPin(h0)->a));
    EXPECT_THROW(gc::root_handle<ClassB> h5 = gc::New<ClassB>(gc::ROPin(gc::RWPin(h0)->a)), gc::bad_api_usage);
    gc::root_handle<ClassB> h6 = gc::New<ClassB>(gc::RWPin(gc::RWPin(h0)->a));
    gc::root_handle<ClassB> h7{nullptr};
    gc::root_handle<ClassB> h8{gc::null_handle};
    gc::root_handle<ClassB> h9{ClassC<ClassB>(gc::null_handle).field};
    gc::root_handle<ClassB> h10{gc::pin<ClassB, true>()};
    gc::root_handle<ClassB> h11{gc::pin<ClassB, false>()};

    h0 = nullptr;
    h0 = gc::null_handle;
    h0 = h1;
    h0 = std::move(h1);
    h0 = ClassC<ClassB>(gc::null_handle).field;
    h0 = gc::pin<ClassB, true>();
    h0 = gc::pin<ClassB, false>();

    gc::root_handle<const ClassB> hc0{gc::root_handle<ClassB>()};
    gc::root_handle<const ClassB> hc1{ClassC<ClassB>(gc::null_handle).field};
    gc::root_handle<const ClassB> hc2{gc::pin<ClassB, true>()};
    gc::root_handle<const ClassB> hc3{gc::pin<ClassB, false>()};
    gc::root_handle<const ClassB> hc4{gc::root_handle<const ClassB>()};
    gc::root_handle<const ClassB> hc5{ClassC<const ClassB>(gc::null_handle).field};
    gc::root_handle<const ClassB> hc6{gc::pin<const ClassB, true>()};
    gc::root_handle<const ClassB> hc7{nullptr};
    gc::root_handle<const ClassB> hc8{gc::null_handle};

    hc0 = nullptr;
    hc0 = gc::null_handle;
    hc0 = h0;
    hc0 = std::move(h0);
    hc0 = hc1;
    hc0 = std::move(hc1);
    hc0 = ClassC<ClassB>(gc::null_handle).field;
    hc0 = gc::pin<ClassB, true>();
    hc0 = gc::pin<ClassB, false>();
    hc0 = ClassC<const ClassB>(gc::null_handle).field;
    hc0 = gc::pin<const ClassB, true>();

    ClassC<ClassB> f0{gc::root_handle<ClassB>()};
    ClassC<ClassB> f1{ClassC<ClassB>(gc::null_handle).field};
    ClassC<ClassB> f2{gc::pin<ClassB, true>()};
    ClassC<ClassB> f3{gc::pin<ClassB, false>()};
    ClassC<ClassB> f4{nullptr};
    ClassC<ClassB> f5{gc::null_handle};

    f0.field = nullptr;
    f0.field = gc::null_handle;
    f0.field = f1.field;
    f0.field = std::move(f1.field);
    f0.field = ClassC<ClassB>(gc::null_handle).field;
    f0.field = gc::pin<ClassB, true>();
    f0.field = gc::pin<ClassB, false>();

    ClassC<const ClassB> fc0{gc::root_handle<ClassB>()};
    ClassC<const ClassB> fc1{ClassC<ClassB>(gc::null_handle).field};
    ClassC<const ClassB> fc2{gc::pin<ClassB, true>()};
    ClassC<const ClassB> fc3{gc::pin<ClassB, false>()};
    ClassC<const ClassB> fc4{gc::root_handle<const ClassB>()};
    ClassC<const ClassB> fc5{ClassC<const ClassB>(gc::null_handle).field};
    ClassC<const ClassB> fc6{gc::pin<const ClassB, true>()};
    ClassC<const ClassB> fc7{nullptr};
    ClassC<const ClassB> fc8{gc::null_handle};

    fc0.field = nullptr;
    fc0.field = gc::null_handle;
    fc0.field = f0.field;
    fc0.field = std::move(f0.field);
    fc0.field = fc1.field;
    fc0.field = std::move(fc1.field);
    fc0.field = ClassC<ClassB>(gc::null_handle).field;
    fc0.field = gc::pin<ClassB, true>();
    fc0.field = gc::pin<ClassB, false>();
    fc0.field = ClassC<const ClassB>(gc::null_handle).field;
    fc0.field = gc::pin<const ClassB, true>();

    gc::pin<ClassB, true> pro0{gc::root_handle<ClassB>()};
    gc::pin<ClassB, true> pro1{ClassC<ClassB>(gc::null_handle).field};
    gc::pin<ClassB, true> pro2{nullptr};
    gc::pin<ClassB, true> pro3{gc::null_handle};

    gc::pin<ClassB, false> prw0{gc::root_handle<ClassB>()};
    gc::pin<ClassB, false> prw1{ClassC<ClassB>(gc::null_handle).field};
    gc::pin<ClassB, false> prw2{nullptr};
    gc::pin<ClassB, false> prw3{gc::null_handle};

    gc::pin<const ClassB, true> proc0{gc::root_handle<ClassB>()};
    gc::pin<const ClassB, true> proc1{ClassC<ClassB>(gc::null_handle).field};
    gc::pin<const ClassB, true> proc2{gc::root_handle<const ClassB>()};
    gc::pin<const ClassB, true> proc3{ClassC<const ClassB>(gc::null_handle).field};
    gc::pin<const ClassB, true> proc4{nullptr};
    gc::pin<const ClassB, true> proc5{gc::null_handle};

    // TODO: tests for move behaviour
}

TEST(GC_collecting_allocator__public_API, move_does_not_break_invariance) {
    TerminateOnDeadlockGuard guard{};
    ASSERT_TRUE(gc::Init({std::pmr::get_default_resource(), std::pmr::get_default_resource()}));
    gc::defer destroy([]{gc::Destroy();});

    auto handle = gc::root_handle<const int>(std::move(gc::New<int>(0)));
    EXPECT_EQ(handle.handle->rootHandleCount, 1);

    auto h = gc::New<ClassB>(std::move(*gc::RWPin(gc::New<ClassB>(gc::New<ClassA>())).Get()));
    auto h2 = std::move(h);
    EXPECT_EQ(h2.handle->rootHandleCount, 1);
    EXPECT_FALSE(gc::ROPin(h2).Get()->a.IsNull());
}

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}