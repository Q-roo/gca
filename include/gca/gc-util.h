#pragma once

#include <atomic>
#include <complex>
#include <format>
#include <limits>
#include <unordered_set>
#include <vector>
#include <utility>

#include "gc-config.h"
#include "gc-exceptions.h"

// FIXME: learn the difference between strong and weak memory ordering and std::memory_order

namespace gc {
    template <size_t max>
    struct smallest_unsigned_numeric_type_needed_for {
        static_assert(max <= std::numeric_limits<uint64_t>::max());
        using type = std::conditional_t<max <= std::numeric_limits<uint8_t >::max(), uint8_t ,
                     std::conditional_t<max <= std::numeric_limits<uint16_t>::max(), uint16_t,
                     std::conditional_t<max <= std::numeric_limits<uint32_t>::max(), uint32_t, uint64_t>>>;
    };
    template <auto max>
    using smallest_unsigned_numeric_type_needed_for_t = smallest_unsigned_numeric_type_needed_for<max>::type;

    template<class T>
    concept enumeration = std::is_enum_v<T>;

    template <enumeration T>
    struct enum_flag_traits {
        using underlying_type = std::underlying_type_t<T>;
        constexpr static bool is_flag = false;
        constexpr static underlying_type all_flags = std::numeric_limits<underlying_type>::is_signed
                                                   ? std::numeric_limits<underlying_type>::min()
                                                   : std::numeric_limits<underlying_type>::max();
    };

    template <class T>
    concept enum_flags = enum_flag_traits<T>::is_flag;

    template <enum_flags T>
    auto operator | (const T lhs, const T rhs) noexcept {
        using type = enum_flag_traits<T>::underlying_type;
        return T(type(lhs) | type(rhs));
    }

    template <enum_flags T>
    auto operator |= (T &lhs, const T rhs) noexcept {
        using type = enum_flag_traits<T>::underlying_type;
        return reinterpret_cast<T &>(reinterpret_cast<type &>(lhs)|=type(rhs));
    }

    template <enum_flags T>
    auto operator & (const T lhs, const T rhs) noexcept {
        using type = enum_flag_traits<T>::underlying_type;
        return T(type(lhs) & type(rhs));
    }

    template <enum_flags T>
    auto operator &= (T &lhs, const T rhs) noexcept {
        using type = enum_flag_traits<T>::underlying_type;
        return reinterpret_cast<T &>(reinterpret_cast<type &>(lhs)&=type(rhs));
    }

    template <enum_flags T>
    auto operator ^ (const T lhs, const T rhs) noexcept {
        using type = enum_flag_traits<T>::underlying_type;
        return T(type(lhs) ^ type(rhs));
    }

    template <enum_flags T>
    auto operator ^= (T &lhs, const T rhs) noexcept {
        using type = enum_flag_traits<T>::underlying_type;
        return reinterpret_cast<T &>(reinterpret_cast<type &>(lhs)^=type(rhs));
    }

    template <enum_flags T>
    auto operator ~ (const T v) noexcept {
        using type = enum_flag_traits<T>::underlying_type;
        return T(type(v) ^ enum_flag_traits<T>::all_flags);
    }

    template <class T> class atomic_bit_set {
        std::atomic<T> m_Flags = 0;
        // template <enum_flags U> friend class atomic_bit_set<U>; // things you wish would work
    public:
        using bit_pos_type = smallest_unsigned_numeric_type_needed_for_t<sizeof(T) * CHAR_BIT>;
        static constexpr bit_pos_type min_pos = 0;
        static constexpr bit_pos_type max_pos = sizeof(T) * CHAR_BIT - 1;

        constexpr atomic_bit_set() noexcept = default;
        constexpr atomic_bit_set(T flags) noexcept : m_Flags(flags) {}

        constexpr void SetAll(T value) noexcept {
            m_Flags = value;
        }

        constexpr T ReadAll() const noexcept {
            return m_Flags.load();
        }

        constexpr bool ReadBit(bit_pos_type bit) const noexcept(!config::gc_throw_nonessential_exceptions) {
            if constexpr (config::gc_throw_nonessential_exceptions) {
                BoundsCheck(bit);
            }

            return ReadMask(1 << bit);
        }

        constexpr bool SetBit(bit_pos_type bit, bool value) noexcept(!config::gc_throw_nonessential_exceptions) {
            return value ? EnableBit(bit) : ClearBit(bit);
        }

        constexpr bool ClearBit(bit_pos_type bit) noexcept(!config::gc_throw_nonessential_exceptions) {
            if constexpr (config::gc_throw_nonessential_exceptions) {
                BoundsCheck(bit);
            }

            // src bit: 0 0 1 1
            // msk bit: 0 1 0 1
            // res bit: 0 0 0 1

            return ClearMask(~(1 << bit));
        }

        constexpr bool EnableBit(bit_pos_type bit) noexcept(!config::gc_throw_nonessential_exceptions) {
            if constexpr (config::gc_throw_nonessential_exceptions) {
                BoundsCheck(bit);
            }

            // src bit: 0 0 1 1
            // msk bit: 0 1 0 1
            // res bit: 0 1 1 1

            return EnableMask(1 << bit);
        }

    protected:
        constexpr bool ReadMask(const T mask) const noexcept {
            return m_Flags.load() & mask;
        }

        constexpr bool ClearMask(const T mask) noexcept {
            auto v = m_Flags.load();
            do {
                if (v == (v & mask)) return false;
            }
            while (!m_Flags.compare_exchange_weak(v, v & mask));

            return true;
        }

        constexpr bool EnableMask(const T mask) noexcept {
            auto v = m_Flags.load();
            do {
                if (v == (v | mask)) return false;
            }
            while (!m_Flags.compare_exchange_weak(v, v | mask));

            return true;
        }

    private:
        template <std::enable_if_t<config::gc_throw_nonessential_exceptions, bool> = true>
        static void BoundsCheck(bit_pos_type bit) {
            if (bit < min_pos || bit > max_pos) {
                throw std::out_of_range(std::format("bit_pos_type {} out of range [{}; {}]", bit, min_pos, max_pos));
            }
        }
    };

    template <enum_flags T>
    class atomic_bit_set<T> : private atomic_bit_set<typename enum_flag_traits<T>::underlying_type> {
        using base_type = atomic_bit_set<typename enum_flag_traits<T>::underlying_type>;

        public:
        using underlying_type = enum_flag_traits<T>::underlying_type;
        using bit_pos_type = base_type::bit_pos_type;
        static constexpr bit_pos_type min_pos = base_type::min_pos;
        static constexpr bit_pos_type max_pos = base_type::max_pos;

        constexpr atomic_bit_set() noexcept = default;
        constexpr atomic_bit_set(T flags) noexcept : base_type(static_cast<underlying_type>(flags)) {}

        constexpr T ReadAll() const noexcept {
            return static_cast<T>(base_type::ReadAll());
        }

        constexpr bool HasFlag(T flag) const noexcept(!config::gc_throw_nonessential_exceptions) {
            if constexpr (config::gc_throw_nonessential_exceptions) {
                KnownFlagCheck(flag);
            }

            return base_type::ReadMask(static_cast<underlying_type>(flag));
        }

        constexpr bool SetFlag(T flag, bool value) noexcept(!config::gc_throw_nonessential_exceptions) {
            if constexpr (config::gc_throw_nonessential_exceptions) {
                KnownFlagCheck(flag);
            }

            return value
                   ? base_type::EnableMask(static_cast<underlying_type>(flag))
                   : base_type::ClearMask(static_cast<underlying_type>(~flag));
        }

        void ClearAll() noexcept {
            base_type::SetAll(0);
        }
    private:
        template <std::enable_if_t<config::gc_throw_nonessential_exceptions, bool> = true>
        static void KnownFlagCheck(T flag) {
            // src 0 0 1 1 (flag)
            // msk 0 1 0 1 (all flags)
            // res 0 0 0 1

            if (flag != (flag & static_cast<T>(enum_flag_traits<T>::all_flags))) {
                throw std::out_of_range(std::format(R"(flag "{:0b}" has flags not defined in "all flags" ({:0b}))", static_cast<underlying_type>(flag), enum_flag_traits<T>::all_flags));
            }
        }
    };

    // single write, multiple read
    class rw_lock {
        using counter_type = config::rw_lock_underlying_type;
        constexpr static counter_type write_mask = 1 << (sizeof(counter_type) * CHAR_BIT - 1);
        // g++ is not happy with this combination of keywords
        // static inline thread_local  std::pmr::unordered_set<const rw_lock*> acquiredWriteAccessesOnThisThread{};
        // and yet, this is fine
        static inline std::pmr::unordered_set<const rw_lock*>& GetAcquiredWriteAccessesOnThisThread() {
            static thread_local std::pmr::unordered_set<const rw_lock*> acquiredWriteAccessesOnThisThread{}; // assuming that every thread could become a collector thread
            return acquiredWriteAccessesOnThisThread;
        }
        std::atomic<counter_type> m_Lock = 0;

        static constexpr counter_type ClearWriteBit(const counter_type v) {
            return v & ~write_mask;
        }

        public:
        constexpr static counter_type max_read_access_count = write_mask;
        [[nodiscard]] bool DoesThisThreadHaveRWAccess() const noexcept {
            return GetAcquiredWriteAccessesOnThisThread().contains(this);
        }

        bool TryAcquireWrite() noexcept {
            // no need to load since in order to get the write lock, no other locks can exist
            counter_type v = 0;
            auto success = m_Lock.compare_exchange_weak(v, write_mask + 1); // set it to 0b10...01 (in case read overflows)
            if (success) {
                GetAcquiredWriteAccessesOnThisThread().emplace(this);
            }
            return success;
        }

        bool TryAcquireWrite(const size_t attempts) noexcept {
            for (size_t i = 0; i < attempts; ++i) {
                if (TryAcquireWrite()) {
                    return true;
                }
            }

            return false;
        }

        void AcquireWrite() noexcept {
            while (!TryAcquireWrite());
        }

        bool TryAcquireRead() noexcept {
            counter_type v = ClearWriteBit(m_Lock.load());
            return m_Lock.compare_exchange_weak(v, v + 1);
        }

        bool TryAcquireRead(const size_t attempts) noexcept {
            counter_type v = ClearWriteBit(m_Lock.load());
            for (size_t i = 0; i < attempts; ++i) {
                if (m_Lock.compare_exchange_weak(v, v + 1)) {
                    return true;
                }
                v = ClearWriteBit(v);
            }

            return false;
        }

        void AcquireRead() noexcept {
            // similar to atomic increment, but ensure that the bit for write access is 0
            for (counter_type v = ClearWriteBit(m_Lock.load());
                 !m_Lock.compare_exchange_weak(v, v + 1);
                 v = ClearWriteBit(v));
        }

        bool TryUpgrade() noexcept {
            counter_type v = 1;
            auto success = m_Lock.compare_exchange_weak(v, write_mask + 1) || v == write_mask + 1; // already rw
            if (success && v != write_mask + 1) {
                // when this throws, you probably have bigger problems than the noexcept specification of this function
                GetAcquiredWriteAccessesOnThisThread().emplace(this);
            }
            return success;
        }

        bool TryUpgrade(const size_t attempts) noexcept {
            for (size_t i = 0; i < attempts; ++i) {
                if (TryUpgrade()) {
                    return true;
                }
            }

            return false;
        }

        void Upgrade() noexcept {
            // upgrade to write access (only possible when this is the only read access)
            for (counter_type v = 1; !m_Lock.compare_exchange_weak(v, write_mask + 1); v = 1) {
                if (v == write_mask + 1) {
                    return; // already have write access
                }
            }

            GetAcquiredWriteAccessesOnThisThread().emplace(this);
        }

        void DownGrade() noexcept {
            counter_type v = write_mask + 1;
            if (m_Lock.compare_exchange_weak(v, 1)) {
                GetAcquiredWriteAccessesOnThisThread().erase(this);
            }
        }

        void Release() noexcept {
            // 3(+1) cases
            // 1: 0b10...01: write lock
            // 2: 0b00...01: read lock
            // 3: 0b10...00: read lock overflown
            // 4: 0b00...00: no lock

            // if not case 1
            if (counter_type v = write_mask + 1; !m_Lock.compare_exchange_weak(v, 0)) {
                // then just simply decrement
                // without under-flowing
                while (!m_Lock.compare_exchange_weak(v, v != 0 ? v - 1 : v));
            }

            GetAcquiredWriteAccessesOnThisThread().erase(this);
        }

        ~rw_lock() {
            GetAcquiredWriteAccessesOnThisThread().erase(this);
        }
    };

    class scoped_rw_lock {
        rw_lock *m_Lock = nullptr;
    public:
        enum class mode { ro, rw, try_ro, try_rw };
        explicit scoped_rw_lock(rw_lock &lock, const mode mode = mode::ro, const size_t try_attempts = 1) noexcept : m_Lock(&lock) {
            switch (mode) {
                case mode::ro:
                    m_Lock->AcquireRead();
                    break;
                    case mode::rw:
                    m_Lock->AcquireWrite();
                    break;
                    case mode::try_ro:
                    if (!m_Lock->TryAcquireRead(try_attempts)) {
                        m_Lock = nullptr;
                    }
                    break;
                    case mode::try_rw:
                    if (!m_Lock->TryAcquireWrite(try_attempts)) {
                        m_Lock = nullptr;
                    }
                    break;
            }
        }

        [[nodiscard]] constexpr bool Acquired() const noexcept {
            return m_Lock != nullptr;
        }

        [[nodiscard]] constexpr bool TryUpgrade(const size_t attempts = 1) const noexcept {
            if (!m_Lock) {
                return false;
            }

            return m_Lock->TryUpgrade(attempts);
        }

        void Upgrade() const {
            if (m_Lock) {
                m_Lock->Upgrade();
            }
            else {
                throw bad_api_usage("called upgrade when the acquisition failed");
            }
        }

        ~scoped_rw_lock() {
            if (m_Lock) {
                m_Lock->Release();
            }
        }
    };

    class mutex {
        std::atomic<config::mutex_underlying_type> m_Lock = false;

        public:
        bool TryAcquire() noexcept {
            config::mutex_underlying_type v = false;
            return m_Lock.compare_exchange_weak(v, true);
        }

        void Acquire() noexcept {
            while (!TryAcquire());
        }

        void Release() noexcept {
            m_Lock.store(false);
        }
    };

    class scoped_mutex_lock {
        mutex *m_Mutex = nullptr;
        public:
        enum class mode { try_only, get };
        explicit scoped_mutex_lock(mutex &mutex, const mode mode = mode::get, const size_t tries = 1) noexcept : m_Mutex(&mutex) {
            if (mode == mode::try_only) {
                for (size_t i = 0; i < tries; ++i) {
                    if (m_Mutex->TryAcquire()) {
                        return;
                    }
                }

                m_Mutex = nullptr;
            }
            else {
                m_Mutex->Acquire();
            }
        }

        [[nodiscard]] constexpr bool Acquired() const noexcept {
            return m_Mutex != nullptr;
        }

        ~scoped_mutex_lock() {
            if (m_Mutex) {
                m_Mutex->Release();
            }
        }
    };


    template <class T>
    class ptr_safe_container {
        friend class ptr_safe_container_test_access;
    public:
        // TODO: copying
        ptr_safe_container(const ptr_safe_container &) = delete;
        ptr_safe_container &operator=(const ptr_safe_container &) = delete;

        ptr_safe_container(ptr_safe_container &&other) noexcept
        : allocator(std::move(other.allocator))
        , nodes(std::move(other.nodes)) {}

        ptr_safe_container &operator=(ptr_safe_container &&other) noexcept {
            if (this == &other) {
                return *this;
            }

            other.allocator = std::exchange(allocator, other.allocator);
            other.nodes = std::exchange(nodes, other.nodes);
            return *this;
        }

        struct node {
            constexpr static uint8_t capacity = 64;
            uint64_t takenFlags = 0;
            // T data[capacity];
            T *data = nullptr; // trailing array

            [[nodiscard]] constexpr bool HasFreeSlot() const noexcept {
                return takenFlags != std::numeric_limits<uint64_t>::max();
            }

            [[nodiscard]] constexpr uint8_t NextFreeSlot() const noexcept {
                return 64 - std::countl_zero(takenFlags);
            }

            constexpr void SetSlotIsFree(const uint8_t slot, const bool isFree) noexcept(!config::gc_throw_nonessential_exceptions) {
                if constexpr (config::gc_throw_nonessential_exceptions) {
                    if (slot >= capacity) {
                        throw std::out_of_range(std::format("slot index {} is outside of the range of [{}; {})", slot, 0, capacity));
                    }
                }

                if (isFree) {
                    if (takenFlags & 1ULL << slot) {
                        takenFlags ^= 1ULL << slot;
                    }
                }
                else {
                    takenFlags |= 1ULL << slot;
                }
            }

            [[nodiscard]] constexpr bool GetSlotIsFree(const uint8_t slot) const noexcept(!config::gc_throw_nonessential_exceptions) {
                if constexpr (config::gc_throw_nonessential_exceptions) {
                    if (slot >= capacity) {
                        throw std::out_of_range(std::format("slot index {} is outside of the range of [{}; {})", slot, 0, capacity));
                    }
                }

                return (takenFlags & 1ULL << slot) == 0;
            }
        };

        using node_allocator = std::pmr::polymorphic_allocator<node>;
        using node_ptr_allocator = std::pmr::vector<node*>::allocator_type;

    private:
        node_allocator allocator;
        std::pmr::vector<node*> nodes;

        struct node_mimick {node node; T data[node::capacity];};
        node *AllocateNode() {
            auto *nodeExtended = static_cast<node_mimick*>(allocator.allocate_bytes(sizeof(node_mimick), alignof(node_mimick)));
            nodeExtended->node.takenFlags = 0;
            nodeExtended->node.data = nodeExtended->data;
            return reinterpret_cast<node*>(nodeExtended);
        }

        void DestroyNode(node *node) noexcept {
            for (size_t i = 0; i < node::capacity; ++i) {
                if (node->GetSlotIsFree(i)) {
                    continue;
                }

                std::destroy_at(&node->data[i]);
            }

            allocator.deallocate_bytes(node, sizeof(node_mimick), alignof(node_mimick));
        }

        T& GetNextWritable() {
            node *node = nullptr;
            for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
                struct node *n = *it;
                if (n->HasFreeSlot()) {
                    node = n;
                    break;
                }
            }

            if (node == nullptr) {
                node = AllocateNode();
                try {
                    nodes.push_back(node);
                } catch (std::exception&) {
                    DestroyNode(node);
                    throw;
                }
            }

            auto slot = node->NextFreeSlot();
            node->SetSlotIsFree(slot, false);
            return node->data[slot];
        }
    public:
        ptr_safe_container(const node_allocator &allocator = node_allocator(),
                        const node_ptr_allocator &ptr_allocator = node_ptr_allocator())
            : allocator(allocator), nodes(ptr_allocator) {}

        T& Insert(const T& v) {
            return GetNextWritable() = v;
        }

        T& Insert(T&& v) {
            return GetNextWritable() = std::move(v);
        }

        T& GetNextUninitialized() {
            return GetNextWritable();
        }

        template <class ...Args>
        T& Emplace(Args&&... args) {
            auto &writable = GetNextWritable();
            std::construct_at(&writable, std::forward<Args>(args)...);
            return writable;
        }

        void RemoveAt(size_t pos) {
            auto &node = nodes[pos / node::capacity];
            auto slot = static_cast<uint8_t>(pos % node::capacity);

            if (node->GetSlotIsFree(slot)) {
                return;
            }

            std::destroy_at(&node->data[slot]);
            node->SetSlotIsFree(slot, true);
        }

        void Remove(const T *value) {
            for (auto &node : nodes) {
                for (size_t i = 0; i < node::capacity; ++i) {
                    if (node->GetSlotIsFree(i)) {
                        continue;
                    }

                    if (&node->data[i] == value) {
                        std::destroy_at(&node->data[i]);
                        node->SetSlotIsFree(i, true);
                        return;
                    }
                }
            }
        }

        ~ptr_safe_container() {
            for (node *node : nodes) {
                DestroyNode(node);
            }
        }
    };

    template <std::invocable Action>
    class defer {
        Action action;
        public:
        explicit defer(Action &&action) : action(std::forward<Action>(action)) {}

        defer(const defer&) = delete;
        defer(defer&&) = delete;
        defer &operator=(const defer&) = delete;
        defer &operator=(defer&&) = delete;

        ~defer() noexcept(std::is_nothrow_invocable_v<Action>) {
            action();
        }
    };
}
