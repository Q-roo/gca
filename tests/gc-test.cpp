#ifdef _MSC_VER
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#include <ranges>
#include <memory_resource>

#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>

#include <gca/gc-internal.h>

#ifdef _MSC_VER
#define no_unique_address msvc::no_unique_address
#endif

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

template <typename T>
std::string demangled_type_name() {
#ifdef  _MSC_VER
    return typeid(T).name();
#else
    int status = 0;
    std::unique_ptr<char, void(*)(void*)> res {
        abi::__cxa_demangle(typeid(T).name(), nullptr, nullptr, &status),
        std::free
    };

    return status == 0 ? res.get() : typeid(T).name();
#endif
}

class simulated_out_of_storage_resource : public std::pmr::memory_resource {
    size_t maxAllocationCount = 0, currentAllocationCount = 0;
    std::pmr::unsynchronized_pool_resource resource{};
public:
    simulated_out_of_storage_resource(size_t maxAllocationCount) : maxAllocationCount(maxAllocationCount) {}
    simulated_out_of_storage_resource() = default;

    ~simulated_out_of_storage_resource() override = default;

private:
    void * do_allocate(std::size_t __bytes, std::size_t __alignment) override {
        if (currentAllocationCount++ >= maxAllocationCount) {
            throw std::bad_alloc();
        }
        return resource.allocate(__bytes, __alignment);
    }

    void do_deallocate(void *__p, std::size_t __bytes, std::size_t __alignment) override {
        return resource.deallocate(__p, __bytes, __alignment);
    }

    bool do_is_equal(const memory_resource &__other) const noexcept override {
        return resource.is_equal(__other);
    }
};

struct allocation_failure_test_allocator : gc::gc_allocator {
    simulated_out_of_storage_resource types, pages, pageMemories, ptrSafeContainerNodes,
                                      lookupNodes, pageAllocations, referencedBy;

    allocation_failure_test_allocator(
        const size_t maxTypes = 0,
        const size_t maxPages = 0,
        const size_t maxPageMemories = 0, // to simulate allocation failure after the page was allocated
        const size_t maxPageAllocations = 0,
        const size_t maxObjectNodes = 0, // 1 node contains 64 objects
        const size_t maxObjectLookupNodes = 0, // not specified
        const size_t maxReferencedBy = 0
    )
#ifdef _MSC_VER
#define MAYBE_DEBUG_OVERHEAD(n) (std::is_same_v<std::_Container_base, std::_Container_base12> ? (n) : 0)
#define GUARANTEED_OVERHEAD(n) n
#define OVERHEAD(dbg, guaranteed) (MAYBE_DEBUG_OVERHEAD((dbg)) + GUARANTEED_OVERHEAD((guaranteed)))
    // +1 for allocating the proxy in std::vector
    // +2 for std::list (proxy + head) (head gets allocated in release as well)
    // +4 for unordered_map (list + vector + initial bucket) (list and initial bucket gets allocated in release as well)
    // but only when debug utilities are enabled
    : types(maxTypes + std::max(1ULL, maxTypes) * OVERHEAD(1, 0))
    , pages(maxPages + std::max(1ULL, maxPages) * OVERHEAD(1, 1))
    , pageMemories(maxPageMemories)
    , ptrSafeContainerNodes(maxObjectNodes) // underlying vector uses the new/delete resource for this
    , lookupNodes(maxObjectLookupNodes + std::max(1ULL, maxObjectLookupNodes) * OVERHEAD(2, 2))
    , pageAllocations(maxPageAllocations + std::max(1ULL, maxPageAllocations) * OVERHEAD(1, 0))
    , referencedBy(maxReferencedBy + std::max(1ULL, maxReferencedBy) * OVERHEAD(1, 0)) {}
#undef OVERHEAD
#undef GUARANTEED_OVERHEAD
#undef MAYBE_DEBUG_OVERHEAD
#else
    : types(maxTypes)
    , pages(maxPages)
    , pageMemories(maxPageMemories)
    , ptrSafeContainerNodes(maxObjectNodes)
    , lookupNodes(maxObjectLookupNodes)
    , pageAllocations(maxPageAllocations)
    , referencedBy(maxReferencedBy) {}
#endif

    std::pmr::vector<gc::object_type> CreateTypesVector() noexcept override {
        return std::pmr::vector<gc::object_type>(&types);
    }

    std::pmr::list<gc::page> CreatePagesList() noexcept override {
        return std::pmr::list<gc::page>(&pages);
    }

    std::pmr::polymorphic_allocator<gc::page_memory> CreatePageMemoryAllocator() noexcept override {
        return {&pageMemories};
    }

    gc::ptr_safe_container<gc::internal_handle> CreateObjectHandlesContainer() noexcept override {
        return {&ptrSafeContainerNodes, std::pmr::new_delete_resource()};
    }

    std::pmr::unordered_map<const void *, gc::internal_handle *> CreateAllocationToHandleLookup() noexcept override {
        return std::pmr::unordered_map<const void *, gc::internal_handle *>(&lookupNodes);
    }

    std::pmr::vector<gc::page::allocation> CreatePageAllocationsVectorForPage() noexcept override {
        return std::pmr::vector<gc::page::allocation>(&pageAllocations);
    }

    std::pmr::vector<gc::internal_handle *> CreateReferencedByVectorForHandle() noexcept override {
        return std::pmr::vector<gc::internal_handle *>(&referencedBy);
    }

    ~allocation_failure_test_allocator() override = default;
};

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

TEST(GC_utility__rw_lock, DoesThisThreadHaveRWAccess_correctness) {
    TerminateOnDeadlockGuard guard;
    gc::rw_lock lock;
    EXPECT_FALSE(lock.DoesThisThreadHaveRWAccess());
    lock.AcquireRead();
    EXPECT_FALSE(lock.DoesThisThreadHaveRWAccess());
    lock.Upgrade();
    EXPECT_TRUE(lock.DoesThisThreadHaveRWAccess());
    lock.Release();
    EXPECT_FALSE(lock.DoesThisThreadHaveRWAccess());
    lock.AcquireWrite();
    EXPECT_TRUE(lock.DoesThisThreadHaveRWAccess());
    auto hasRWAccessOnOtherThread = false;
    std::thread([&] {
        hasRWAccessOnOtherThread = lock.DoesThisThreadHaveRWAccess();
    }).join();
    EXPECT_FALSE(hasRWAccessOnOtherThread);
    lock.Release();
    EXPECT_FALSE(lock.DoesThisThreadHaveRWAccess());
}

TEST(GC_utility__rw_lock, Downgrade_degrades_write_access_to_a_single_read_access) {
    TerminateOnDeadlockGuard guard;
    {
        gc::rw_lock lock;
        lock.AcquireWrite();
        lock.DownGrade();
        lock.Upgrade();
    }
    {
        gc::rw_lock lock;
        lock.AcquireWrite();
        lock.DownGrade();
        lock.AcquireRead();
    }
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

TEST(GC_utility__ptr_safe_container, does_not_allocate_the_first_node_on_construction) {
#ifdef _MSC_VER
    // Microsoft (R) C/C++ Optimizing Compiler Version 19.44.35228 for x86
    // can't do anything about this:
    // std::pmr::vector<T>(std::pmr::null_memory_resource()) ->
    //  _CONSTEXPR20_CONTAINER explicit vector(const _Alloc& _Al) noexcept ->
    //  _Mypair._Myval2._Alloc_proxy(_GET_PROXY_ALLOCATOR(_Alty, _Getal())) -> (vector)
    // _Container_proxy* const _New_proxy = _Unfancy(_Al.allocate(1)) (xmemory)
    // should fail because std::terminate will be called
    // because the constructor is noexcept
    // actual exit code will be 3
    // best of all, it only does so in debug
    if (std::is_same_v<std::_Container_base, std::_Container_base12>) {
        simulated_out_of_storage_resource res{1}; // for the proxy
        EXPECT_NO_THROW(gc::ptr_safe_container<uint8_t>(&res, &res));
        // should fail by termination (exit code 3)
        EXPECT_EXIT({
            EXPECT_NO_THROW(gc::ptr_safe_container<uint8_t>(std::pmr::null_memory_resource(), std::pmr::null_memory_resource()));
            std::exit(0);
        }, testing::ExitedWithCode(3), "");
    }
    else {
        EXPECT_NO_THROW(gc::ptr_safe_container<uint8_t>(std::pmr::null_memory_resource(), std::pmr::null_memory_resource()));
    }
#else
    // msvc hates this
    EXPECT_NO_THROW(gc::ptr_safe_container<uint8_t>(std::pmr::null_memory_resource(), std::pmr::null_memory_resource()));
#endif
}

namespace gc {
    class ptr_safe_container_test_access {
public:
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
    size_t allocationCount{0};
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
        std::array<uint8_t, sizeof(gc::ptr_safe_container<uint8_t>::node)> buffer{0};
        auto res = std::pmr::monotonic_buffer_resource(buffer.data(), buffer.size(), std::pmr::null_memory_resource());
        ASSERT_NO_THROW(gc::ptr_safe_container<uint8_t>(&res, std::pmr::get_default_resource())) << "less than the minimal amount of memory required for construction";
    }

    // first, the node is allocated and then, the pointer to it gets added to nodes
    {
        std::array<uint8_t, sizeof(gc::ptr_safe_container<uint8_t>::node)> buffer{0};
        auto res = std::pmr::monotonic_buffer_resource(buffer.data(), buffer.size(), std::pmr::null_memory_resource());
        auto data = gc::ptr_safe_container<uint8_t>(&res, std::pmr::get_default_resource());

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
        void *buff = nullptr;
        size_t buffSize = 0;
#ifdef _MSC_VER
        if constexpr (std::is_same_v<std::_Container_base, std::_Container_base12>) {
            // std::_Container_proxy is what vector allocates on construction
            constexpr size_t greater_alignment = alignof(void*) > alignof(std::_Container_proxy)
                                               ? alignof(void*)
                                               : alignof(std::_Container_proxy);
            alignas(greater_alignment) std::array<
                std::byte,
                sizeof(void *) * 2 + sizeof(std::_Container_proxy) // what vector allocates on construction
                > buffer {static_cast<std::byte>(0)};
            buff = buffer.data();
            buffSize = sizeof(buffer);
        }
        else {
            std::array<void*, 2> buffer{nullptr};
            buff = buffer.data();
            buffSize = sizeof(buffer);
        }

#else
        std::array<void*, 2> buffer{nullptr};
        buff = buffer.data();
        buffSize = sizeof(buffer);
#endif
        auto res = std::pmr::monotonic_buffer_resource(buff, buffSize, std::pmr::null_memory_resource());
        auto nodeRes = allocation_tracking_resource();
        auto data = gc::ptr_safe_container<uint8_t>(&nodeRes, &res);

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
struct gc::is_gc_supported<int> {
    static constexpr bool supported = true;
};

template <class T>
struct default_gc_object_type {
    static constexpr gc::object_type value{
        .size = sizeof(T),
        .alignment = alignof(T),
        .type = &typeid(T), // static storage duration
        .destructor = gc::default_destroy<T>,
        .getFieldCount = gc::default_get_field_count<T>,
        .getField = gc::default_get_field<T>,
        .move = gc::default_move<T>
    };
};

template <class T>
constexpr gc::object_type default_gc_object_type_v = default_gc_object_type<T>::value;

TEST(GC_collecting_allocator, this_should_work) { // temporary test
    bool success = false;
    gc::root_handle<int> handle = gc::null_handle;
    EXPECT_NO_THROW(success = gc::Init(gc::gc_init_args{gc::GetDefaultAllocator()}));
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
    gc::page page{std::pmr::vector<gc::page::allocation>(std::pmr::get_default_resource())};
    page.memory = new gc::page_memory;
    gc::defer d([&]{delete page.memory;});

    auto cAllocation = page.TryAllocate(1, 1);
    auto iAllocation = page.TryAllocate(4, 4);

    EXPECT_FALSE(cAllocation.empty());
    EXPECT_FALSE(iAllocation.empty());

    EXPECT_GT(iAllocation.data(), cAllocation.data()) << "allocations should be incrementing";
}

TEST(GC_collecting_allocator__page, allocation_strategy_respects_alignment) {
    gc::page page{std::pmr::vector<gc::page::allocation>(std::pmr::get_default_resource())};
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
    gc::page page{std::pmr::vector<gc::page::allocation>(std::pmr::get_default_resource())};
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
    gc::page page{std::pmr::vector<gc::page::allocation>(std::pmr::get_default_resource())};
    page.memory = new gc::page_memory;
    gc::defer d([&]{delete page.memory;});

    EXPECT_EQ(gc::page::GetAlignmentCorrection(reinterpret_cast<uintptr_t>(page.memory), gc::config::page_alignment), 0);
}

TEST(GC_collecting_allocator__page, memory_is_zeroed_out) {
    gc::page page{std::pmr::vector<gc::page::allocation>(std::pmr::get_default_resource())};
    page.memory = new gc::page_memory;
    gc::defer d([&]{delete page.memory;});

    EXPECT_EQ(std::ranges::find_if(page.memory->memory, [](const std::byte it) { return it != static_cast<std::byte>(0); }), page.memory->memory.end());
}

TEST(GC_collecting_allocator__page, allocation_strategy_can_handle_page_size_allocations) {
    gc::page page{std::pmr::vector<gc::page::allocation>(std::pmr::get_default_resource())};
    page.memory = new gc::page_memory;
    gc::defer d([&]{delete page.memory;});

    const auto allocation = page.TryAllocate(gc::config::page_size, gc::config::page_alignment);
    EXPECT_FALSE(allocation.empty());
}

TEST(GC_collecting_allocator__page, allocation_strategy_gracefully_fails_large_allocations) {
    gc::page page{std::pmr::vector<gc::page::allocation>(std::pmr::get_default_resource())};
    page.memory = new gc::page_memory;
    gc::defer d([&]{delete page.memory;});
    EXPECT_TRUE(page.TryAllocate(gc::config::page_size + 1, 1).empty());
}

TEST(GC_collecting_allocator__page, RemoveAllocation_removes_valid_allocation) {
    gc::page page{std::pmr::vector<gc::page::allocation>(std::pmr::get_default_resource())};
    page.memory = new gc::page_memory;
    gc::defer d([&]{delete page.memory;});

    auto allocation = page.TryAllocate(1, 1);
    EXPECT_FALSE(allocation.empty());
    EXPECT_EQ(page.allocations.size(), 1);
    page.RemoveAllocation(allocation);
    EXPECT_EQ(page.allocations.size(), 0);
}

TEST(GC_collecting_allocator__page, RemoveAllocation_does_not_remove_invalid_allocation) {
    gc::page page{std::pmr::vector<gc::page::allocation>(std::pmr::get_default_resource())};
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
#ifdef _MSC_VER
    if (std::is_same_v<std::_Container_base, std::_Container_base12>) {
        GTEST_FAIL() << "in msvc's implementation, most containers (including the ons used in the implementation) allocate debug proxies when enabled (and they currently are)";
    }
    else {
        GTEST_FAIL() << "in msvc's implementation, some containers allocate space on construction (e.g.: std::list allocates the first node)";
    }
#else
    EXPECT_NO_THROW(gc::gc_impl(gc::gc_init_args{gc::GetNullAllocator()}));
#endif
}

TEST(GC_collecting_allocator__gc_impl, constructor_does_not_accept_null_memory_resource_pointer) {
    EXPECT_THROW(gc::gc_impl(gc::gc_init_args{nullptr}), gc::bad_api_usage);
}

TEST(GC_collecting_allocator__gc_impl, default_constructor_does_not_throw) {
    EXPECT_EXIT({gc::gc_impl GC{}; std::exit(0);}, testing::ExitedWithCode(0), "");
}

TEST(GC_collecting_allocator__gc_impl, default_constructor_can_allocate) {
    EXPECT_NO_THROW(gc::gc_impl().Allocate(default_gc_object_type_v<int>, 1));
}

TEST(GC_collecting_allocator__gc_impl, Allocate_count_allocations_are_sequential) {
    gc::gc_impl GC(gc::gc_init_args{gc::GetDefaultAllocator()});
    auto handle = GC.Allocate(default_gc_object_type_v<int>, 10);
    EXPECT_EQ(handle->objectAllocation.size(), sizeof(int) * 10);
}

TEST(GC_collecting_allocator__gc_impl, Allocate_allocated_handles_count_as_root_handles) {
    gc::gc_impl GC(gc::gc_init_args{gc::GetDefaultAllocator()});
    auto handle = GC.Allocate(default_gc_object_type_v<int>, 10);
    EXPECT_EQ(handle->rootHandleCount, 1);
}

TEST(GC_collecting_allocator__gc_impl, Allocate_triggers_collection_when_page_is_full) {
    gc::gc_impl GC(gc::gc_init_args{gc::GetDefaultAllocator()});
    auto collected = false;
    GC.debugListeners.onBeforeCollectionStart = [&](const gc::debug::callback_data&, gc::page&) {collected = true;};
    GC.Allocate(default_gc_object_type_v<std::byte>, gc::config::page_size);
    EXPECT_FALSE(collected); // no gc yet
    GC.Allocate(default_gc_object_type_v<int>, 1);
    EXPECT_TRUE(collected);
}

TEST(GC_collecting_allocator__gc_impl, Collect_collects_dead_objects_that_are_not_roots) {
    gc::gc_impl GC(gc::gc_init_args{gc::GetDefaultAllocator()});
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
    gc::gc_impl GC(gc::gc_init_args{gc::GetDefaultAllocator()});
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
    gc::gc_impl GC(gc::gc_init_args{gc::GetDefaultAllocator()});
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
    gc::gc_impl GC(gc::gc_init_args{gc::GetDefaultAllocator()});
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

TEST(GC_collecting_allocator__gc_impl, scanning_a_field_of_an_object_being_destroyed_wont_cause_a_deadlock) {
    EXPECT_EXIT({
        TerminateOnDeadlockGuard guard{};
        {
            gc::gc_impl GC(gc::gc_init_args{gc::GetDefaultAllocator()});
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
    gc::gc_impl GC(gc::gc_init_args{gc::GetDefaultAllocator()});
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
    gc::gc_impl GC(gc::gc_init_args{gc::GetDefaultAllocator()});
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
    gc::gc_impl GC(gc::gc_init_args{gc::GetDefaultAllocator()});
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
    gc::gc_impl GC(gc::gc_init_args{gc::GetDefaultAllocator()});
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
    gc::gc_impl GC(gc::gc_init_args{gc::GetDefaultAllocator()});
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
    gc::gc_impl GC(gc::gc_init_args{gc::GetDefaultAllocator()});
    GC.Allocate(default_gc_object_type_v<std::byte>, 1);
    GC.Allocate(default_gc_object_type_v<std::byte>, sizeof(int))->rootHandleCount = 0; // allocation that could fit an int but is not aligned to the alignment of int
    auto hInt = GC.Allocate(default_gc_object_type_v<int>, 1); // there are padding bytes before this allocation
    GC.Allocate(default_gc_object_type_v<std::byte>, gc::config::page_size);
    EXPECT_EQ(gc::page::GetAlignmentCorrection(reinterpret_cast<uintptr_t>(hInt->objectAllocation.data()), alignof(int)), 0) << "allocation is no longer aligned";
}

TEST(GC_collecting_allocator__gc_impl, defragmentation_respects_alignment_2) {
    gc::gc_impl GC(gc::gc_init_args{gc::GetDefaultAllocator()});
    GC.Allocate(default_gc_object_type_v<uint32_t>, 1);
    GC.Allocate(default_gc_object_type_v<uint32_t>, 2)->rootHandleCount = 0;
    GC.Allocate(default_gc_object_type_v<uint32_t>, 1)->rootHandleCount = 0;
    auto hInt = GC.Allocate(default_gc_object_type_v<uint64_t>, 1); // no padding this time
    GC.Allocate(default_gc_object_type_v<std::byte>, gc::config::page_size);
    EXPECT_EQ(gc::page::GetAlignmentCorrection(reinterpret_cast<uintptr_t>(hInt->objectAllocation.data()), alignof(int)), 0) << "allocation is no longer aligned";
}

TEST(GC_collecting_allocator__gc_impl, defragmentation_does_not_overwrite_memory_while_moving) {
    gc::gc_impl GC(gc::gc_init_args{gc::GetDefaultAllocator()});

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
    gc::gc_impl GC(gc::gc_init_args{gc::GetDefaultAllocator()});
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
    gc::gc_impl GC(gc::gc_init_args{gc::GetDefaultAllocator()});
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
    gc::gc_impl GC(gc::gc_init_args{gc::GetDefaultAllocator()});
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
    gc::gc_impl GC(gc::gc_init_args{gc::GetDefaultAllocator()});
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
    gc::gc_impl GC(gc::gc_init_args{gc::GetDefaultAllocator()});
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
    gc::gc_impl GC(gc::gc_init_args{gc::GetDefaultAllocator()});
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
    Base b{};
    EXPECT_EQ(&b, b.member.GetThis());
}

TEST(GC_assumption, ptr_is_64_bits) {
    EXPECT_EQ(sizeof(void*), 8);
}

TEST(GC_assumption, virtual_address_size_is_48) {
    constexpr unsigned int leaf = 0x80000008;
#ifdef _MSC_VER
    int registers[4];
    __cpuid(registers, std::bit_cast<int>(leaf));
    const auto eax = std::bit_cast<unsigned int>(registers[0]);
#else
    unsigned int eax, ebx, ecx, edx;
    ASSERT_NE(__get_cpuid(leaf, &eax, &ebx, &ecx, &edx), 0) << "cpu not supported";

#endif
    const auto addressBits = eax & 0x000000FF;
    EXPECT_EQ(addressBits, 48);
}

TEST(GC_assumption, termainate_exit_code_is_3) {
    EXPECT_EXIT({
        std::terminate();
    }, testing::ExitedWithCode(3), "");
}

TEST(GC_assumption, msvc_vector_allocates_only_the_proxy_during_construction_when_debug_utilities_are_enabled) {
#ifdef _MSC_VER
    // std::_Container_proxy is what vector allocates on construction
    if (std::is_same_v<std::_Container_base, std::_Container_base0>) {
        GTEST_SKIP() << "debug proxies are not enabled";
    }
    else {
        ASSERT_EXIT({
            std::_Container_proxy p;
            auto vectorInitBuffer = std::pmr::monotonic_buffer_resource(&p, sizeof(p) - 1, std::pmr::null_memory_resource());
            std::pmr::vector<std::byte> vec(&vectorInitBuffer);
            std::exit(0);
        }, testing::ExitedWithCode(3), "") << "std::vector allocates less than just an std::_Container_proxy instance on construction";

        ASSERT_EXIT({
            std::_Container_proxy p;
            auto vectorInitBuffer = std::pmr::monotonic_buffer_resource(&p, sizeof(p), std::pmr::null_memory_resource());
            std::pmr::vector<std::byte> vec(&vectorInitBuffer);
            std::exit(0);
        }, testing::ExitedWithCode(0), "") << "std::vector allocates more than just an std::_Container_proxy instance on construction";
    }
#else
    GTEST_SKIP() << "not compiling with msvc";
#endif
}

TEST(GC_assumption, msvc_list_only_allocates_a_proxy_during_construction) {
#ifdef _MSC_VER
    using list = std::pmr::list<int>; // not noexcept
    struct allocation {
        std::_Container_proxy _{};
        // linked list (int) node approximation
        void * _next{nullptr}, *_prev{nullptr};
        int _data{0};
    };
    if (std::is_same_v<std::_Container_base, std::_Container_base0>) {
        GTEST_SKIP() << "debug proxies are not enabled";
    }
    else {
        ASSERT_THROW({
        allocation p;
        auto initBuffer = std::pmr::monotonic_buffer_resource(&p, sizeof(p) - 1, std::pmr::null_memory_resource());
        list unorderedMap(&initBuffer);
    }, std::bad_alloc) << "std::list allocates less than expected on construction";
        ASSERT_NO_THROW({
            allocation p;
            auto initBuffer = std::pmr::monotonic_buffer_resource(&p, sizeof(p), std::pmr::null_memory_resource());
            list unorderedMap(&initBuffer);
        }) << "std::list allocates more than expected on construction";
    }
#else
    GTEST_SKIP() << "not compiling with msvc";
#endif
}

TEST(GC_assumption, msvc_unordered_map_allocates_only_the_proxy_during_construction) {
#ifdef _MSC_VER
    // list + vector allocation
    // not noexcept (but "explicit _Hash_vec(_Any_alloc&& _Al) noexcept" is)
    // and then a resize (which will allocate) for the first bucket
    using unordered_map = std::pmr::unordered_map<int, int>;

    // approximation
    struct allocation {
        std::_Container_proxy _pList{};
        void *_next{nullptr}, *_prev{nullptr};
        std::pair<int, int> _data{};
        std::_Container_proxy _pVec{};
        // bucket size is 8
        struct {
            size_t _hash{0};
            int _key{0};
            int _value{0};
        } buckets[8];
    };

    if (std::is_same_v<std::_Container_base, std::_Container_base0>) {
        GTEST_SKIP() << "debug proxies are not enabled";
    }
    else {
        simulated_out_of_storage_resource resFail{3};
        EXPECT_THROW(unordered_map unorderedMap(&resFail), std::bad_alloc);
        simulated_out_of_storage_resource resSuccess{4};
        EXPECT_NO_THROW(unordered_map unorderedMap(&resSuccess));

        ASSERT_EXIT({
            allocation p;
            auto initBuffer = std::pmr::monotonic_buffer_resource(&p, sizeof(p) - 1, std::pmr::null_memory_resource());
            EXPECT_THROW(unordered_map unorderedMap(&initBuffer), std::bad_alloc);
            std::exit(0);
        }, testing::ExitedWithCode(0), "") << "std::unordered_map allocates less than expected on construction";

        EXPECT_EXIT({
            allocation p;
            auto initBuffer = std::pmr::monotonic_buffer_resource(&p, sizeof(p), std::pmr::null_memory_resource());
            EXPECT_NO_THROW(unordered_map unorderedMap(&initBuffer));
            std::exit(0);
        }, testing::ExitedWithCode(0), "") << "std::unordered_map allocates more than expected on construction";
    }
#else
    GTEST_SKIP() << "not compiling with msvc";
#endif
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

template <> struct gc::is_gc_supported<ClassA> {
    static constexpr bool supported = true;
};

template <> struct gc::is_gc_supported<ClassB> {
    static constexpr bool supported = true;
};

template <> struct gc::partial_gc_object_traits<ClassB> {
    using type = ClassB;
    static constexpr get_field_count_fn get_field_count = [](const void*) noexcept { return 1ULL; };
    static constexpr get_field_fn get_field = [](void*obj, const size_t idx) noexcept -> internal_handle** {
        return idx == 0
        ? std::launder(static_cast<ClassB*>(obj))->a.GetField()
        : nullptr;
    };
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

template <class T> struct gc::is_gc_supported<ClassC<T>> {
    static constexpr bool supported = true;
};

template <class T> struct gc::partial_gc_object_traits<ClassC<T>> {
    using type = ClassC<T>;
    static constexpr get_field_count_fn get_field_count = [](const void*) noexcept -> size_t { return 1ULL; };
    static constexpr get_field_fn get_field = [](void *obj, const size_t idx) noexcept -> internal_handle** {
        return idx == 0
        ? std::launder(static_cast<ClassC<T>*>(obj))->field.GetField()
        : nullptr;
    };
};

TEST(GC_collecting_allocator__public_API, compiles) {
    TerminateOnDeadlockGuard guard{};
    ASSERT_TRUE(gc::Init(gc::gc_init_args{gc::GetDefaultAllocator()}));
    gc::defer destroy([]{gc::Destroy();});

    gc::root_handle<ClassB> h0 = gc::New<ClassB>(gc::New<ClassA>());
    gc::root_handle<ClassB> h1 = gc::New<ClassB>(gc::ROPin(h0)->a);
    gc::root_handle<ClassB> h2 = gc::New<ClassB>(gc::RWPin(h0)->a);
    gc::root_handle<ClassB> h3 = gc::New<ClassB>(gc::ROPin(gc::ROPin(h0)->a));
    gc::root_handle<ClassB> h4 = gc::New<ClassB>(gc::RWPin(gc::ROPin(h0)->a));
    gc::root_handle<ClassB> h5 = gc::New<ClassB>(gc::ROPin(gc::RWPin(h0)->a));
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
}

TEST(GC_collecting_allocator__public_API, move_does_not_break_invariance) {
    TerminateOnDeadlockGuard guard{};
    ASSERT_TRUE(gc::Init(gc::gc_init_args{gc::GetDefaultAllocator()}));
    gc::defer destroy([]{gc::Destroy();});

    auto handle = gc::root_handle<const int>(std::move(gc::New<int>(0)));
    EXPECT_EQ(handle.handle->rootHandleCount, 1);

    auto h = gc::New<ClassB>(std::move(*gc::RWPin(gc::New<ClassB>(gc::New<ClassA>())).Get()));
    auto h2 = std::move(h);
    EXPECT_EQ(h2.handle->rootHandleCount, 1);
    EXPECT_FALSE(gc::ROPin(h2).Get()->a.IsNull());
}

class ClassE;

// indirectly managed
class ClassD {
    gc::field_store<1> fields{};
    public:
    [[no_unique_address]]gc::field<0, 0, ClassE> field;
    ClassD(const gc::root_handle<ClassE> &value) : field{value} {}
};

class ClassE {
    uint64_t _{};
    ClassD d{gc::null_handle};
public:
    void Set(const gc::root_handle<ClassE> &value) {
        d = ClassD{value};
    }
};

class ClassH;

// doubly indirectly managed
class ClassF {
    gc::field_store<1> fields{};
public:
    [[no_unique_address]]gc::field<0, 0, ClassH> field;
    ClassF(const gc::root_handle<ClassH> &value) : field{value} {}
};

class ClassG {
    uint64_t _{};
    ClassF f{gc::null_handle};
public:
    void Set(const gc::root_handle<ClassH> &value) {
        f = ClassF{value};
    }
};

class ClassH {
    uint64_t _{};
public:
    ClassG g{};
};

template <> struct gc::is_gc_supported<ClassH> {
    static constexpr bool supported = true;
};

template <> struct gc::is_gc_supported<ClassF> {
    static constexpr bool supported = true;
};

template <> struct gc::is_gc_supported<ClassE> {
    static constexpr bool supported = true;
};

template <> struct gc::is_gc_supported<ClassD> {
    static constexpr bool supported = true;
};

template <> struct gc::partial_gc_object_traits<ClassD> {
    using type = ClassD;
    static constexpr get_field_count_fn get_field_count = [](const void*) noexcept { return 1ULL; };
    static constexpr get_field_fn get_field = [](void *obj, size_t idx) noexcept -> internal_handle** {
        return idx == 0
        ? std::launder(static_cast<ClassD *>(obj))->field.GetField()
        : nullptr;
    };
};

namespace gc {
    extern gc_impl *impl;
}
TEST(GC_collecting_allocator__public_API, non_gc_allocated_member_does_not_cause_circular_references) {
    TerminateOnDeadlockGuard guard{};
    ASSERT_TRUE(gc::Init(gc::gc_init_args{gc::GetDefaultAllocator()}));
    gc::defer destroy([]{gc::Destroy();});

    {
        auto h = gc::New<ClassE>();
        gc::RWPin(h)->Set(h);
    }

    gc::Collect();
    EXPECT_EQ(gc::impl->pages.front().allocations.size(), 0);

    {
        auto h = gc::New<ClassH>();
        gc::RWPin(h)->g.Set(h);
    }

    gc::Collect();
    EXPECT_EQ(gc::impl->pages.front().allocations.size(), 0);
}

TEST(GC_collecting_allocator__public_API, field_assignment_from_another_field_does_not_break_invariance_on_copy) {
    TerminateOnDeadlockGuard guard{};
    ASSERT_TRUE(gc::Init(gc::gc_init_args{gc::GetDefaultAllocator()}));
    gc::defer destroy([]{gc::Destroy();});

    gc::internal_handle *h1 = nullptr, *h2 = nullptr;

    {
        ClassG g;
        {
            auto h = gc::New<ClassH>();
            h1 = h.handle;
            gc::RWPin(h)->g.Set(h);
            g = gc::ROPin(h)->g;
        }

        gc::Collect();
        EXPECT_EQ(gc::impl->pages.front().allocations.size(), 1);
        EXPECT_FALSE(h1->referencedBy.empty()) << "h.g no longer references h after copy";
        EXPECT_EQ(h1->rootHandleCount, 1);

        {
            auto h = gc::New<ClassH>();
            h2 = h.handle;
            gc::RWPin(h)->g = g;
            EXPECT_FALSE(h1->referencedBy.empty()) << "h1 should only be referenced by g, which should behave as a root";
            EXPECT_EQ(h1->referencedBy.size(), 2) << "should be referenced by itself and h2";
            EXPECT_EQ(h2->referencedBy.size(), 0);
            EXPECT_EQ(h2->rootHandleCount, 1);
        }
    }

    gc::Collect();
    EXPECT_EQ(gc::impl->pages.front().allocations.size(), 0);
}

TEST(GC_collecting_allocator__public_API, field_assignment_from_another_field_does_not_break_invariance_on_move) {
    TerminateOnDeadlockGuard guard{};
    ASSERT_TRUE(gc::Init(gc::gc_init_args{gc::GetDefaultAllocator()}));
    gc::defer destroy([]{gc::Destroy();});

    gc::internal_handle *h1 = nullptr;
    {
        ClassG g;
        {
            auto h = gc::New<ClassH>();
            h1 = h.handle;
            gc::RWPin(h)->g.Set(h);
            g = std::move(gc::RWPin(h)->g);
        }

        gc::Collect();
        EXPECT_EQ(gc::impl->pages.front().allocations.size(), 1);
        EXPECT_TRUE(h1->referencedBy.empty()) << "h.g not got copied, not moved";
        EXPECT_EQ(h1->rootHandleCount, 1);

        {
            auto h = gc::New<ClassH>();
            gc::RWPin(h)->g = std::move(g);
            EXPECT_EQ(h1->referencedBy.size(), 1);
            EXPECT_EQ(h1->rootHandleCount, 0);
        }
    }

    gc::Collect();
    EXPECT_EQ(gc::impl->pages.front().allocations.size(), 0);
}

TEST(GC_collecting_allocator__public_API, strong_exception_guarantee_on_page_allocation_failure) {
    TerminateOnDeadlockGuard guard{};
    allocation_failure_test_allocator allocator{
        0, // types (vector)
        0, // pages (linked list)
        0, // pages (object memory)
        0, // page allocations (vector)
        0, // object nodes (n * 64)
        0, // lookup (unordered map) (happens before the handle allocation)
        0, // referenced by (vector)
    };
    ASSERT_TRUE(gc::Init(gc::gc_init_args{&allocator}));
    gc::defer destroy(&gc::Destroy);

    EXPECT_THROW(gc::New<int>(), std::bad_alloc);
    EXPECT_TRUE(gc::impl->pagesLock.TryAcquireWrite()) << "lock not released";
    gc::impl->pagesLock.Release();
    EXPECT_TRUE(gc::impl->pageAllocationLock.TryAcquire()) << "mutex not released";
    gc::impl->pageAllocationLock.Release();
}

TEST(GC_collecting_allocator__public_API, strong_exception_guarantee_on_page_node_allocation_failure) {
    TerminateOnDeadlockGuard guard{};
    allocation_failure_test_allocator allocator{
        0, // types (vector)
        1, // pages (linked list)
        0, // pages (object memory)
#ifdef  _MSC_VER
        std::is_same_v<std::_Container_base, std::_Container_base12> ? 1 : 0, // page allocations (vector)
        // msvc's vector implementation will allocate a new proxy to exchange in the move constructor
        // in debug mode
#else
        0, // page allocations (vector)
#endif
        0, // object nodes (n * 64)
        0, // lookup (unordered map) (happens before the handle allocation)
        0, // referenced by (vector)
    };
    ASSERT_TRUE(gc::Init(gc::gc_init_args{&allocator}));
    gc::defer destroy(&gc::Destroy);

    EXPECT_THROW(gc::New<int>(), std::bad_alloc);
    EXPECT_TRUE(gc::impl->pagesLock.TryAcquireWrite()) << "lock not released";
    gc::impl->pagesLock.Release();
    EXPECT_TRUE(gc::impl->pageAllocationLock.TryAcquire()) << "mutex not released";
    gc::impl->pageAllocationLock.Release();
    EXPECT_EQ(gc::impl->pages.size(), 0) << "uninitialized page not removed";
}

TEST(GC_collecting_allocator__public_API, strong_exception_guarantee_on_page_allocation_entry_allocation_failure) {
    TerminateOnDeadlockGuard guard{};
    allocation_failure_test_allocator allocator{
        0, // types (vector)
        1, // pages (linked list)
        1, // pages (object memory)
#ifdef  _MSC_VER
        std::is_same_v<std::_Container_base, std::_Container_base12> ? 1 : 0, // page allocations (vector)
        // msvc's vector implementation will allocate a new proxy to exchange in the move constructor
        // in debug mode
#else
        0, // page allocations (vector)
#endif
        0, // object nodes (n * 64)
        0, // lookup (unordered map) (happens before the handle allocation)
        0, // referenced by (vector)
    };
    ASSERT_TRUE(gc::Init(gc::gc_init_args{&allocator}));
    gc::defer destroy(&gc::Destroy);

    // same problem as previously (msvc only)
    EXPECT_THROW(gc::New<int>(), std::bad_alloc);
    EXPECT_TRUE(gc::impl->pagesLock.TryAcquireWrite()) << "lock not released";
    gc::impl->pagesLock.Release();
    EXPECT_TRUE(gc::impl->pageAllocationLock.TryAcquire()) << "mutex not released";
    gc::impl->pageAllocationLock.Release();
    EXPECT_EQ(gc::impl->pages.size(), 1) << "initialized page not removed";
}

TEST(GC_collecting_allocator__public_API, strong_exception_guarantee_on_type_allocation_failure) {
    TerminateOnDeadlockGuard guard{};
    allocation_failure_test_allocator allocator{
        0, // types (vector)
        1, // pages (linked list)
        1, // pages (object memory)
        1, // page allocations (vector)
        0, // object nodes (n * 64)
        0, // lookup (unordered map) (happens before the handle allocation)
        0, // referenced by (vector)
    };
    ASSERT_TRUE(gc::Init(gc::gc_init_args{&allocator}));
    gc::defer destroy(&gc::Destroy);

    EXPECT_THROW(gc::New<int>(), std::bad_alloc);
    EXPECT_TRUE(gc::impl->pagesLock.TryAcquireWrite()) << "lock not released";
    gc::impl->pagesLock.Release();
    EXPECT_TRUE(gc::impl->pageAllocationLock.TryAcquire()) << "mutex not released";
    gc::impl->pageAllocationLock.Release();
    EXPECT_EQ(gc::impl->pages.size(), 1) << "initialized page not removed";
    EXPECT_TRUE(gc::impl->pages.front().allocationLock.TryAcquire()) << "mutex not released";
    gc::impl->pages.front().allocationLock.Release();
    EXPECT_TRUE(gc::impl->typesLock.TryAcquireWrite()) << "lock not released";
    gc::impl->typesLock.Release();
    EXPECT_EQ(gc::impl->pages.front().allocations.size(), 0) << "allocation not released";
}

TEST(GC_collecting_allocator__public_API, strong_exception_guarantee_on_lookup_allocation_failure) {
    TerminateOnDeadlockGuard guard{};
    allocation_failure_test_allocator allocator{
        1, // types (vector)
        1, // pages (linked list)
        1, // pages (object memory)
        1, // page allocations (vector)
        1, // object nodes (n * 64)
        0, // lookup (unordered map) (happens before the handle allocation)
        0, // referenced by (vector)
    };
    ASSERT_TRUE(gc::Init(gc::gc_init_args{&allocator}));
    gc::defer destroy(&gc::Destroy);

    EXPECT_THROW(gc::New<int>(), std::bad_alloc);
    EXPECT_TRUE(gc::impl->pagesLock.TryAcquireWrite()) << "lock not released";
    gc::impl->pagesLock.Release();
    EXPECT_TRUE(gc::impl->pageAllocationLock.TryAcquire()) << "mutex not released";
    gc::impl->pageAllocationLock.Release();
    EXPECT_EQ(gc::impl->pages.size(), 1) << "initialized page not removed";
    EXPECT_TRUE(gc::impl->pages.front().allocationLock.TryAcquire()) << "mutex not released";
    gc::impl->pages.front().allocationLock.Release();
    EXPECT_TRUE(gc::impl->typesLock.TryAcquireWrite()) << "lock not released";
    gc::impl->typesLock.Release();
    EXPECT_EQ(gc::impl->pages.front().allocations.size(), 0) << "allocation not released";
    EXPECT_TRUE(gc::impl->allocationLookupLock.TryAcquireWrite()) << "lock not released";
    gc::impl->allocationLookupLock.Release();
}

TEST(GC_collecting_allocator__public_API, strong_exception_guarantee_on_handle_allocation_failure) {
    TerminateOnDeadlockGuard guard{};
    allocation_failure_test_allocator allocator{
        1, // types (vector)
        1, // pages (linked list)
        1, // pages (object memory)
        1, // page allocations (vector)
        0, // object nodes (n * 64)
        1, // lookup (unordered map) (happens before the handle allocation)
        0, // referenced by (vector)
    };
    ASSERT_TRUE(gc::Init(gc::gc_init_args{&allocator}));
    gc::defer destroy(&gc::Destroy);

    EXPECT_THROW(gc::New<int>(), std::bad_alloc);
    EXPECT_TRUE(gc::impl->pagesLock.TryAcquireWrite()) << "lock not released";
    gc::impl->pagesLock.Release();
    EXPECT_TRUE(gc::impl->pageAllocationLock.TryAcquire()) << "mutex not released";
    gc::impl->pageAllocationLock.Release();
    EXPECT_EQ(gc::impl->pages.size(), 1) << "initialized page not removed";
    EXPECT_TRUE(gc::impl->pages.front().allocationLock.TryAcquire()) << "mutex not released";
    gc::impl->pages.front().allocationLock.Release();
    EXPECT_TRUE(gc::impl->typesLock.TryAcquireWrite()) << "lock not released";
    gc::impl->typesLock.Release();
    EXPECT_EQ(gc::impl->pages.front().allocations.size(), 0) << "allocation not released";
    EXPECT_TRUE(gc::impl->allocationLookupLock.TryAcquireWrite()) << "lock not released";
    gc::impl->allocationLookupLock.Release();
    EXPECT_TRUE(gc::impl->allocationToHandleLookup.empty()) << "handle registration for allocation not rolled back";
}

TEST(GC_collecting_allocator__public_API, strong_exception_guarantee_on_referenced_by_allocation_failure) {
    TerminateOnDeadlockGuard guard{};
    allocation_failure_test_allocator allocator{
        2, // types (vector)
        1, // pages (linked list)
        1, // pages (object memory)
#ifdef _MSC_VER
        (std::is_same_v<std::_Container_base, std::_Container_base12> ? 1 : 0) + // debug proxy allocation
#endif
        2, // page allocations (vector)
        1, // object nodes (n * 64)
        3, // lookup (unordered map) (happens before the handle allocation)
#ifdef  _MSC_VER
        std::is_same_v<std::_Container_base, std::_Container_base12> ? 2 : 0, // referenced by (vector)
        // 2 allocations for 2 moves
#else
        0, // referenced by (vector)
#endif
    };
    ASSERT_TRUE(gc::Init(gc::gc_init_args{&allocator}));
    gc::defer destroy(&gc::Destroy);

    gc::root_handle<ClassA> a{};
    // vector move ctor, courtesy of msvc's implementation
    EXPECT_NO_THROW(a = gc::New<ClassA>());
    EXPECT_THROW(gc::New<ClassB>(a), std::bad_alloc); // will throw in initialization, which happens after object allocation
    EXPECT_EQ(gc::impl->allocationToHandleLookup.size(), 2) << "object that failed to initialize did not become floating garbage";
    gc::internal_handle *hClassB = nullptr;
    for (gc::internal_handle *handle : std::ranges::views::values(gc::impl->allocationToHandleLookup)) {
        if (handle != a.handle) {
            hClassB = handle;
        }
    }
    ASSERT_NE(hClassB, nullptr);
    EXPECT_FALSE(gc::internal::Equals(*gc::partial_gc_object_traits<ClassB>::get_field(hClassB->objectAllocation.data(), 0), a.handle)) << "field assignment not rolled back";
    EXPECT_TRUE(a.handle->objectLock.TryAcquireWrite()) << "lock not released";
    a.handle->objectLock.Release();
    EXPECT_TRUE(hClassB->objectLock.TryAcquireWrite()) << "lock not released";
    hClassB->objectLock.Release();
}

class PolymorphicBase { uint64_t _{}; public: virtual ~PolymorphicBase() = default; };
class PolymorphicDerived : public PolymorphicBase { uint64_t _{}; public: ~PolymorphicDerived() override = default; };

TEST(GC_collecting_allocator__public_API, handle_cast_base_to_base) {
    TerminateOnDeadlockGuard guard{};
    ASSERT_TRUE(gc::Init(gc::gc_init_args{gc::GetDefaultAllocator()}));
    gc::defer destroy([]{gc::Destroy();});

    const auto h = gc::impl->Allocate(default_gc_object_type_v<PolymorphicBase>, 1);
    std::construct_at(reinterpret_cast<PolymorphicDerived *>(h->objectAllocation.data())); // init vtable
    const gc::dyn::polymorphic_handle<PolymorphicBase> hP = h;
    EXPECT_EQ(static_cast<ptrdiff_t>(hP.handle.GetData()), 0);
}

TEST(GC_collecting_allocator__public_API, handle_cast_derived_to_base) {
    TerminateOnDeadlockGuard guard{};
    ASSERT_TRUE(gc::Init(gc::gc_init_args{gc::GetDefaultAllocator()}));
    gc::defer destroy([]{gc::Destroy();});

    const auto h = gc::impl->Allocate(default_gc_object_type_v<PolymorphicDerived>, 1);
    const auto original = std::construct_at(reinterpret_cast<PolymorphicDerived *>(h->objectAllocation.data())); // init vtable
    const auto casted = dynamic_cast<PolymorphicBase*>(original);
    const gc::dyn::polymorphic_handle<PolymorphicDerived> hP = h;
    const gc::dyn::polymorphic_handle<PolymorphicBase> hP2 = hP;
    EXPECT_EQ(hP.Get(gc::handle_role::ro_pin), original);
    EXPECT_EQ(hP2.Get(gc::handle_role::ro_pin), casted);
}

TEST(GC_collecting_allocator__public_API, handle_cast_base_to_derived) {
    TerminateOnDeadlockGuard guard{};
    ASSERT_TRUE(gc::Init(gc::gc_init_args{gc::GetDefaultAllocator()}));
    gc::defer destroy([]{gc::Destroy();});

    const auto h = gc::impl->Allocate(default_gc_object_type_v<PolymorphicDerived>, 1);
    const auto original = std::construct_at(reinterpret_cast<PolymorphicDerived *>(h->objectAllocation.data())); // init vtable
    const auto casted = dynamic_cast<PolymorphicBase*>(original);
    const gc::dyn::polymorphic_handle<PolymorphicDerived> hP = h;
    const gc::dyn::polymorphic_handle<PolymorphicBase> hP2 = hP;
    EXPECT_EQ(hP.Get(gc::handle_role::ro_pin), original);
    EXPECT_EQ(hP2.Get(gc::handle_role::ro_pin), casted);
}

class Parent1 {uint64_t _{}; virtual void A() = 0;};
class Parent2 {uint64_t _{}; virtual void B() = 0;};
class Child : public Parent1, public Parent2 {uint64_t _{}; void A() override {} void B() override {}};

TEST(GC_collecting_allocator__public_API, handle_cast_cross_cast) {
    TerminateOnDeadlockGuard guard{};
    ASSERT_TRUE(gc::Init(gc::gc_init_args{gc::GetDefaultAllocator()}));
    gc::defer destroy([]{gc::Destroy();});

    const auto h = gc::impl->Allocate(default_gc_object_type_v<Child>, 1);
    const auto original = std::construct_at(reinterpret_cast<Child *>(h->objectAllocation.data())); // init vtable
    const auto casted1 = dynamic_cast<Parent1*>(original);
    const auto casted2 = dynamic_cast<Parent2*>(casted1);
    const gc::dyn::polymorphic_handle<Child> hP = h;
    const gc::dyn::polymorphic_handle<Parent1> hP1 = hP;
    const gc::dyn::polymorphic_handle<Parent2> hP2 = hP1;

    EXPECT_EQ(hP.Get(gc::handle_role::ro_pin), original);
    EXPECT_EQ(hP1.Get(gc::handle_role::ro_pin), casted1);
    EXPECT_EQ(hP2.Get(gc::handle_role::ro_pin), casted2);
}

class Animal {public: virtual ~Animal() = default;};
class Dog : virtual public Animal {public: ~Dog() override = default;};
class Cat : virtual public Animal {public: ~Cat() override = default;};
class DogCat : public Dog, public Cat {public: ~DogCat() override = default;};

// alternatively
/*
 * interface Foo {...}
 * interface Bar requires Foo {...}
 * interface Baz requires Foo {...}
 * class Faz implements Bar, Baz {...}
 */

TEST(GC_collecting_allocator__public_API, handle_cast_virtual_diamond) {
    TerminateOnDeadlockGuard guard{};
    ASSERT_TRUE(gc::Init(gc::gc_init_args{gc::GetDefaultAllocator()}));
    gc::defer destroy([]{gc::Destroy();});

    const auto hDogCat = gc::impl->Allocate(default_gc_object_type_v<DogCat>, 1);
    const auto dogCat = std::construct_at(std::launder(reinterpret_cast<DogCat *>(hDogCat->objectAllocation.data())));
    const auto hPDogCat = gc::dyn::polymorphic_handle<DogCat>(hDogCat);
    EXPECT_EQ(hPDogCat.Get(gc::handle_role::ro_pin), dogCat);

    // cast to bases
    const auto dog = dynamic_cast<Dog*>(dogCat);
    const auto hPDog = gc::dyn::polymorphic_handle<Dog>(hPDogCat);
    EXPECT_EQ(hPDog.Get(gc::handle_role::ro_pin), dog);

    const auto cat = dynamic_cast<Cat*>(dogCat);
    const auto hPCat = gc::dyn::polymorphic_handle<Cat>(hPDogCat);
    EXPECT_EQ(hPCat.Get(gc::handle_role::ro_pin), cat);

    const auto animal = dynamic_cast<Animal*>(dogCat); // ambiguous
    const auto hPAnimal = gc::dyn::polymorphic_handle<Animal>(hPDogCat);
    EXPECT_EQ(hPAnimal.Get(gc::handle_role::ro_pin), animal);

    const auto dAnimal = dynamic_cast<Animal*>(dog);
    const auto hPDAnimal = gc::dyn::polymorphic_handle<Animal>(hPDog);
    EXPECT_EQ(hPDAnimal.Get(gc::handle_role::ro_pin), dAnimal);

    const auto cAnimal = dynamic_cast<Animal*>(cat);
    const auto hPCAnimal = gc::dyn::polymorphic_handle<Animal>(hPCat);
    EXPECT_EQ(hPCAnimal.Get(gc::handle_role::ro_pin), cAnimal);

    // cast to sibling/derived
    const auto dog2D = dynamic_cast<Dog*>(dAnimal);
    const auto hPDog2D = gc::dyn::polymorphic_handle<Dog>(hPDAnimal);
    EXPECT_EQ(hPDog2D.Get(gc::handle_role::ro_pin), dog2D);

    const auto dog2S = dynamic_cast<Dog*>(cAnimal);
    const auto hPDog2S = gc::dyn::polymorphic_handle<Dog>(hPCAnimal);
    EXPECT_EQ(hPDog2S.Get(gc::handle_role::ro_pin), dog2S);

    const auto cat2D = dynamic_cast<Cat*>(cAnimal);
    const auto hPCat2D = gc::dyn::polymorphic_handle<Cat>(hPCAnimal);
    EXPECT_EQ(hPCat2D.Get(gc::handle_role::ro_pin), cat2D);

    const auto cat2S = dynamic_cast<Cat*>(dAnimal);
    const auto hPCat2S = gc::dyn::polymorphic_handle<Cat>(hPDAnimal);
    EXPECT_EQ(hPCat2S.Get(gc::handle_role::ro_pin), cat2S);
}

TEST(GC_collecting_allocator__public_API, dyn_api_pin_types) {
    using rw_pin = gc::dyn::pin<int>;
    using ro_pin = gc::dyn::pin<const int>;

    using rw_array_pin = gc::dyn::pin<int[]>;
    using ro_array_pin = gc::dyn::pin<const int[]>;

    EXPECT_TYPE(decltype(rw_pin().Get()), int*);
    EXPECT_TYPE(decltype(rw_pin().operator->()), int*);
    EXPECT_TYPE(decltype(rw_pin().operator*()), int&);

    EXPECT_TYPE(decltype(ro_pin().Get()), const int*);
    EXPECT_TYPE(decltype(ro_pin().operator->()), const int*);
    EXPECT_TYPE(decltype(ro_pin().operator*()), const int&);

    // EXPECT_TYPE(decltype(rw_array_pin().Get()), int*);
    // EXPECT_TYPE(decltype(rw_array_pin().operator[](0)), int&);
    // EXPECT_TYPE(decltype(rw_array_pin().Count()), size_t);
    //
    // EXPECT_TYPE(decltype(ro_array_pin().Get()), const int*);
    // EXPECT_TYPE(decltype(ro_array_pin().operator[](0)), const int&);
    // EXPECT_TYPE(decltype(ro_array_pin().Count()), size_t);
}

class DClassA {
    std::vector<int> toMove;
};

class DClassB {
    public:
    DClassB(DClassB&&) = default;
    gc::dyn::field<DClassA> a;
    DClassB(const gc::dyn::root_handle<DClassA> &h) : a{h} {}
    DClassB(gc::dyn::pin<DClassA> &&p) : a{std::forward<gc::dyn::pin<DClassA>>(p)} {}
    DClassB(const gc::dyn::field<DClassA> &f) : a{f} {}
};

template <> struct gc::is_gc_supported<DClassA> {
    static constexpr bool supported = true;
};

template <> struct gc::is_gc_supported<DClassB> {
    static constexpr bool supported = true;
};

template <> struct gc::partial_gc_object_traits<DClassB> {
    using type = DClassB;
    static constexpr get_field_count_fn get_field_count = [](const void*) noexcept { return 1ULL; };
    static constexpr get_field_fn get_field = [](void*obj, const size_t idx) noexcept -> internal_handle** {
        return idx == 0
        ? std::launder(static_cast<DClassB*>(obj))->a.GetField()
        : nullptr;
    };
};

template<class T>
class DClassC {
public:
    gc::dyn::field<T> field;

    DClassC(const gc::dyn::root_handle<T> &h) :field(h) {}
    DClassC(gc::dyn::pin<T> &&p) :field(std::forward<gc::dyn::pin<T>>(p)) {}
    DClassC(const gc::dyn::field<T> &f) :field(f) {}

    DClassC(std::nullptr_t) :field(nullptr) {}
    DClassC(gc::null_handle_t) :field(gc::null_handle) {}

    DClassC(const gc::dyn::root_handle<std::remove_const_t<T>> &h) requires(std::is_const_v<T>) :field(h) {}
    DClassC(gc::dyn::pin<std::remove_const_t<T>> &&p) requires(std::is_const_v<T>) :field(std::forward<gc::dyn::pin<std::remove_const_t<T>>>(p)) {}
    DClassC(const gc::dyn::field<std::remove_const_t<T>> &f) requires(std::is_const_v<T>) :field(f) {}
};

template <class T> struct gc::is_gc_supported<DClassC<T>> {
    static constexpr bool supported = true;
};

template <class T> struct gc::partial_gc_object_traits<DClassC<T>> {
    using type = DClassC<T>;
    static constexpr get_field_count_fn get_field_count = [](const void*) noexcept -> size_t { return 1ULL; };
    static constexpr get_field_fn get_field = [](void *obj, const size_t idx) noexcept -> internal_handle** {
        return idx == 0
        ? std::launder(static_cast<DClassC<T>*>(obj))->field.GetField()
        : nullptr;
    };
};

TEST(GC_collecting_allocator__public_API, dyn_api_compiles) {
    TerminateOnDeadlockGuard guard{};
    ASSERT_TRUE(gc::Init(gc::gc_init_args{gc::GetDefaultAllocator()}));
    gc::defer destroy([]{gc::Destroy();});

    gc::dyn::root_handle<DClassB> h0 = gc::New<DClassB>(static_cast<gc::dyn::root_handle<DClassA>>(gc::New<DClassA>()));
    gc::dyn::root_handle<DClassB> h1 = gc::New<DClassB>(h0.PinRO()->a);
    gc::dyn::root_handle<DClassB> h2 = gc::New<DClassB>(h0.PinRW()->a);
    gc::dyn::root_handle<DClassB> h7{nullptr};
    gc::dyn::root_handle<DClassB> h8{gc::null_handle};
    gc::dyn::root_handle<DClassB> h9{DClassC<DClassB>(gc::null_handle).field};
    gc::dyn::root_handle<DClassB> h10{std::move(gc::dyn::pin<const DClassB>())};
    gc::dyn::root_handle<DClassB> h11{std::move(gc::dyn::pin<DClassB>())};

    h0 = nullptr;
    h0 = gc::null_handle;
    h0 = h1;
    h0 = std::move(h1);
    h0 = DClassC<DClassB>(gc::null_handle).field;
    h0 = std::move(gc::dyn::pin<const DClassB>());
    h0 = std::move(gc::dyn::pin<DClassB>());

    gc::dyn::root_handle<const DClassB> hc0{gc::dyn::root_handle<DClassB>()};
    gc::dyn::root_handle<const DClassB> hc1{DClassC<DClassB>(gc::null_handle).field};
    gc::dyn::root_handle<const DClassB> hc2{std::move(gc::dyn::pin<const DClassB>())};
    gc::dyn::root_handle<const DClassB> hc3{std::move(gc::dyn::pin<DClassB>())};
    gc::dyn::root_handle<const DClassB> hc4{gc::dyn::root_handle<const DClassB>()};
    gc::dyn::root_handle<const DClassB> hc5{DClassC<const DClassB>(gc::null_handle).field};
    gc::dyn::root_handle<const DClassB> hc6{std::move(gc::dyn::pin<const DClassB>())};
    gc::dyn::root_handle<const DClassB> hc7{nullptr};
    gc::dyn::root_handle<const DClassB> hc8{gc::null_handle};

    hc0 = nullptr;
    hc0 = gc::null_handle;
    hc0 = h0;
    hc0 = std::move(h0);
    hc0 = hc1;
    hc0 = std::move(hc1);
    hc0 = DClassC<DClassB>(gc::null_handle).field;
    hc0 = std::move(gc::dyn::pin<const DClassB>());
    hc0 = std::move(gc::dyn::pin<DClassB>());
    hc0 = DClassC<const DClassB>(gc::null_handle).field;
    hc0 = std::move(gc::dyn::pin<const DClassB>());

    DClassC<DClassB> f0{gc::dyn::root_handle<DClassB>()};
    DClassC<DClassB> f1{DClassC<DClassB>(gc::null_handle).field};
    DClassC<const DClassB> f2{std::move(gc::dyn::pin<const DClassB>())};
    DClassC<DClassB> f3{std::move(gc::dyn::pin<DClassB>())};
    DClassC<DClassB> f4{nullptr};
    DClassC<DClassB> f5{gc::null_handle};

    f0.field = nullptr;
    f0.field = gc::null_handle;
    f0.field = f1.field;
    f0.field = std::move(f1.field);
    f0.field = DClassC<DClassB>(gc::null_handle).field;
    f0.field = std::move(gc::dyn::pin<const DClassB>());
    f0.field = std::move(gc::dyn::pin<DClassB>());

    DClassC<const DClassB> fc0{gc::dyn::root_handle<DClassB>()};
    DClassC<const DClassB> fc1{DClassC<DClassB>(gc::null_handle).field};
    DClassC<const DClassB> fc2{gc::dyn::pin<const DClassB>()};
    DClassC<const DClassB> fc3{gc::dyn::pin<DClassB>()};
    DClassC<const DClassB> fc4{gc::dyn::root_handle<const DClassB>()};
    DClassC<const DClassB> fc5{DClassC<const DClassB>(gc::null_handle).field};
    DClassC<const DClassB> fc6{gc::dyn::pin<const DClassB>()};
    DClassC<const DClassB> fc7{nullptr};
    DClassC<const DClassB> fc8{gc::null_handle};

    fc0.field = nullptr;
    fc0.field = gc::null_handle;
    fc0.field = f0.field;
    fc0.field = std::move(f0.field);
    fc0.field = fc1.field;
    fc0.field = std::move(fc1.field);
    fc0.field = DClassC<DClassB>(gc::null_handle).field;
    fc0.field = gc::dyn::pin<const DClassB>();
    fc0.field = gc::dyn::pin<DClassB>();
    fc0.field = DClassC<const DClassB>(gc::null_handle).field;
    fc0.field = gc::dyn::pin<const DClassB>();

    gc::dyn::pin<const DClassB> pro0{gc::root_handle<DClassB>()};
    gc::dyn::pin<const DClassB> pro1{DClassC<DClassB>(gc::null_handle).field};

    gc::dyn::pin<DClassB> prw0{gc::root_handle<DClassB>()};
    gc::dyn::pin<DClassB> prw1{DClassC<DClassB>(gc::null_handle).field};

    gc::dyn::pin<const DClassB> proc0{gc::dyn::root_handle<DClassB>()};
    gc::dyn::pin<const DClassB> proc1{DClassC<DClassB>(gc::null_handle).field};
    gc::dyn::pin<const DClassB> proc2{gc::dyn::root_handle<const DClassB>()};
    gc::dyn::pin<const DClassB> proc3{DClassC<const DClassB>(gc::null_handle).field};
}

int main(int argc, char **argv) {
#ifdef _MSC_VER
    // there's something ironic about not having this precisely where I need it
#else
    std::set_terminate(__gnu_cxx::__verbose_terminate_handler);
#endif
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}