#ifndef SETBENCH_TRIE_FATNODE_NOVC_H
#define SETBENCH_TRIE_FATNODE_NOVC_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace trie_novc {

#ifndef FATNODE_ARRAY_SIZE
#define FATNODE_ARRAY_SIZE 10
#endif
static constexpr std::size_t ARRAY_SIZE = FATNODE_ARRAY_SIZE;

#ifdef MEASURE_VERSIONS
#define COUNT_VERSION(tid) GSTATS_ADD((tid), versions_created, 1)
#else
#define COUNT_VERSION(tid) ((void)(tid))
#endif

template <typename Value> struct Version;

template <typename Value> struct ArraySlot {
    int sum = 0;
    Version<Value>* owner = nullptr;
    ArraySlot* left_slot_ptr = nullptr;
    ArraySlot* right_slot_ptr = nullptr;
};

template <typename Value> struct Version {
    ArraySlot<Value> array[ARRAY_SIZE];
    std::atomic<std::uint64_t> next_empty_slot{1};
    Version() {
        for (auto& slot : array) slot.owner = this;
    }
};

template <typename Key, typename Value> struct Node {
    Key key;
    Value value{};
    Node* left = nullptr;
    Node* right = nullptr;
    Node* parent = nullptr;
    std::atomic<ArraySlot<Value>*> slot_ptr{nullptr};

    explicit Node(Key k, Node* p) : key(k), parent(p) {}
};

template <typename Key, typename Value, class RecMgr>
class Trie {

    RecMgr* recmgr_;
    const Key key_min_;
    const Key key_max_;
    const Value no_value_;
    std::size_t logical_size_;
    Node<Key, Value>* root_ = nullptr;
    std::vector<Node<Key, Value>*> leaves_;
    std::vector<std::atomic<bool>> initialized_;

    static std::size_t checked_range(Key lo, Key hi) {
        if (hi < lo) throw std::invalid_argument("empty trie key range");
        using U = typename std::make_unsigned<Key>::type;
        const U distance = static_cast<U>(hi) - static_cast<U>(lo);
        if (static_cast<std::uintmax_t>(distance) >=
            static_cast<std::uintmax_t>(std::numeric_limits<int>::max()))
            throw std::length_error("trie range exceeds int sum capacity");
        return static_cast<std::size_t>(distance) + 1;
    }

    bool index_of(Key k, std::size_t& idx) const {
        if (k < key_min_ || k > key_max_) return false;
        using U = typename std::make_unsigned<Key>::type;
        idx = static_cast<std::size_t>(static_cast<U>(k) -
                                       static_cast<U>(key_min_));
        return idx < logical_size_;
    }

    // Range-split over the inclusive leaf range [lo, hi], the same shape as
    // Trie_Baseline::build_tree.
    Node<Key, Value>* build(std::size_t lo, std::size_t hi, Node<Key, Value>* parent) {
        auto* x = new Node<Key, Value>(static_cast<Key>(-1), parent);
        if (lo == hi) {
            if constexpr (std::is_signed<Key>::value)
                x->key = static_cast<Key>(
                    static_cast<std::int64_t>(key_min_) +
                    static_cast<std::int64_t>(lo));
            else
                x->key = static_cast<Key>(
                    static_cast<std::uint64_t>(key_min_) +
                    static_cast<std::uint64_t>(lo));
            leaves_[lo] = x;
        } else {
            const std::size_t mid = lo + (hi - lo) / 2;
            x->left = build(lo, mid, x);
            x->right = build(mid + 1, hi, x);
        }
        auto* v = new Version<Value>;
        auto& slot = v->array[0];
        if (x->left) {
            slot.left_slot_ptr = x->left->slot_ptr.load(std::memory_order_relaxed);
            slot.right_slot_ptr = x->right->slot_ptr.load(std::memory_order_relaxed);
        }
        x->slot_ptr.store(&slot, std::memory_order_relaxed);
        return x;
    }

    bool publish_leaf(int tid, Node<Key, Value>* leaf,
                      ArraySlot<Value>* expected, int sum) {
        Version<Value>* old_v = expected->owner;
        const auto index = old_v->next_empty_slot.fetch_add(
            1, std::memory_order_relaxed);
        Version<Value>* new_v = nullptr;
        ArraySlot<Value>* desired;
        if (index < ARRAY_SIZE) {
            desired = &old_v->array[index];
        } else {
            new_v = new Version<Value>;
            COUNT_VERSION(tid);
            desired = &new_v->array[0];
        }
        desired->sum = sum;
        desired->left_slot_ptr = nullptr;
        desired->right_slot_ptr = nullptr;
        const bool installed = leaf->slot_ptr.compare_exchange_strong(
            expected, desired, std::memory_order_release,
            std::memory_order_acquire);
        if (new_v && !installed) delete new_v;
        return installed;
    }

    bool refresh(int tid, Node<Key, Value>* x, ArraySlot<Value>* expected,
                 std::uint64_t index, Version<Value>* owner) {
        if (!x || !x->left || !x->right) return true;
        ArraySlot<Value>* left = x->left->slot_ptr.load(std::memory_order_acquire);
        ArraySlot<Value>* right = x->right->slot_ptr.load(std::memory_order_acquire);
        const int sum = left->sum + right->sum;
        Version<Value>* new_v = nullptr;
        ArraySlot<Value>* desired;
        if (index < ARRAY_SIZE) {
            desired = &owner->array[index];
        } else {
            new_v = new Version<Value>;
            COUNT_VERSION(tid);
            desired = &new_v->array[0];
        }
        desired->sum = sum;
        desired->left_slot_ptr = left;
        desired->right_slot_ptr = right;
        const bool installed = x->slot_ptr.compare_exchange_strong(
            expected, desired, std::memory_order_release,
            std::memory_order_acquire);
        if (new_v && !installed) delete new_v;
        return installed;
    }

    void propagate(int tid, Node<Key, Value>* x) {
        while (x) {
            ArraySlot<Value>* expected = x->slot_ptr.load(std::memory_order_acquire);
            Version<Value>* owner = expected->owner;
            auto index = owner->next_empty_slot.fetch_add(
                1, std::memory_order_relaxed);
            if (!refresh(tid, x, expected, index, owner)) {
                expected = x->slot_ptr.load(std::memory_order_acquire);
                owner = expected->owner;
                index = owner->next_empty_slot.fetch_add(
                    1, std::memory_order_relaxed);
                refresh(tid, x, expected, index, owner);
            }
            x = x->parent;
        }
    }

    bool validate_node(Node<Key, Value>* x) const {
        if (!x) return true;
        ArraySlot<Value>* s = x->slot_ptr.load(std::memory_order_acquire);
        if (!s || !s->owner || s->sum < 0) return false;
        if (!x->left) return s->sum == 0 || s->sum == 1;
        if (!s->left_slot_ptr || !s->right_slot_ptr ||
            s->sum != s->left_slot_ptr->sum + s->right_slot_ptr->sum)
            return false;
        return validate_node(x->left) && validate_node(x->right);
    }

public:
    Trie(RecMgr* recmgr, int num_threads, Key key_min, Key key_max,
         Value no_value)
        : recmgr_(recmgr), key_min_(key_min), key_max_(key_max),
          no_value_(no_value), logical_size_(checked_range(key_min, key_max)),
          initialized_(num_threads) {
        leaves_.resize(logical_size_, nullptr);
        for (auto& initialized : initialized_)
            initialized.store(false, std::memory_order_relaxed);
        root_ = build(0, logical_size_ - 1, nullptr);
    }

    void initThread(int tid) {
        if (!initialized_[tid].exchange(true)) recmgr_->initThread(tid);
    }
    void deinitThread(int tid) {
        if (initialized_[tid].exchange(false)) recmgr_->deinitThread(tid);
    }

    Value find(int, Key k) const {
        std::size_t idx;
        if (!index_of(k, idx)) return no_value_;
        ArraySlot<Value>* current = root_->slot_ptr.load(std::memory_order_acquire);
        if (!current || current->sum < 1) return no_value_;
        std::size_t lo = 0, hi = logical_size_ - 1;
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (idx <= mid) {
                current = current->left_slot_ptr;
                hi = mid;
            } else {
                current = current->right_slot_ptr;
                lo = mid + 1;
            }
            if (!current || current->sum < 1) return no_value_;
        }
        return leaves_[idx]->value;
    }

    // Returns NO_VALUE when k was inserted, (Value)k otherwise (Trie_Baseline
    // convention).
    Value insertIfAbsent(int tid, Key k, Value value) {
        std::size_t idx;
        if (!index_of(k, idx)) return no_value_;
        Node<Key, Value>* leaf = leaves_[idx];
        ArraySlot<Value>* old = leaf->slot_ptr.load(std::memory_order_acquire);
        if (old->sum == 1) {
            propagate(tid, leaf->parent);
            return (Value)k;
        }
        leaf->value = value;
        const bool installed = publish_leaf(tid, leaf, old, 1);
        propagate(tid, leaf->parent);
        return installed ? no_value_ : (Value)k;
    }

    // Returns (Value)k when k was erased, NO_VALUE otherwise (Trie_Baseline
    // convention).
    Value erase(int tid, Key k) {
        std::size_t idx;
        if (!index_of(k, idx)) return no_value_;
        Node<Key, Value>* leaf = leaves_[idx];
        ArraySlot<Value>* old = leaf->slot_ptr.load(std::memory_order_acquire);
        if (old->sum != 1) {
            propagate(tid, leaf->parent);
            return no_value_;
        }
        const bool installed = publish_leaf(tid, leaf, old, 0);
        propagate(tid, leaf->parent);
        return installed ? (Value)k : no_value_;
    }

    std::int64_t keySum() const {
        std::int64_t total = 0;
        for (std::size_t i = 0; i < logical_size_; ++i) {
            ArraySlot<Value>* s = leaves_[i]->slot_ptr.load(std::memory_order_acquire);
            if (s->sum == 1) total += static_cast<std::int64_t>(leaves_[i]->key);
        }
        return total;
    }
    std::int64_t size(int) const { return keySum(); }
    int count() const {
        return root_->slot_ptr.load(std::memory_order_acquire)->sum;
    }
    bool validateStructure() const {
        if (!validate_node(root_)) return false;
        int count_leaves = 0;
        for (std::size_t i = 0; i < logical_size_; ++i)
            count_leaves += leaves_[i]->slot_ptr.load(std::memory_order_acquire)->sum;
        return count() == count_leaves;
    }
    Node<Key, Value>* get_root() const { return root_; }
};

} // namespace trie_novc

#endif
