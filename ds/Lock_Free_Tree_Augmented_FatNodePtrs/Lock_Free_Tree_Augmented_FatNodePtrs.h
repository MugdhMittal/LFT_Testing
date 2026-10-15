#ifndef SETBENCH_LOCK_FREE_TREE_AUGMENTED_FATNODEPTRS_H
#define SETBENCH_LOCK_FREE_TREE_AUGMENTED_FATNODEPTRS_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stack>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bst_fatptr {

#ifndef FATNODE_ARRAY_SIZE
#define FATNODE_ARRAY_SIZE 10
#endif
static constexpr std::size_t ARRAY_SIZE = FATNODE_ARRAY_SIZE;
enum class State : std::uintptr_t { CLEAN, IFLAG, DFLAG, MARK };

#ifdef MEASURE_VERSIONS
#define COUNT_VERSION(tid) GSTATS_ADD((tid), versions_created, 1)
#else
#define COUNT_VERSION(tid) ((void)(tid))
#endif

template <typename Key> struct VersionOwner {
    Key key;
    explicit VersionOwner(Key key_) : key(key_) {}
    virtual ~VersionOwner() = default;
};

template <typename Key> struct VersionRef {
    int sum = 0;
    VersionOwner<Key>* owner = nullptr;
};

template <typename Key> struct ArraySlot : VersionRef<Key> {
    VersionRef<Key>* left_slot_ptr = nullptr;
    VersionRef<Key>* right_slot_ptr = nullptr;
};

template <typename Key, typename Value>
struct VersionLeaf : VersionRef<Key>, VersionOwner<Key> {
    const Value value;
    VersionLeaf(Key key, Value value_, int sum_)
        : VersionOwner<Key>(key), value(value_) {
        this->sum = sum_;
        this->owner = this;
    }
};

template <typename Key> struct VersionInternal : VersionOwner<Key> {
    ArraySlot<Key> array[ARRAY_SIZE];
    std::atomic<std::uint64_t> next_empty_slot{0};
    explicit VersionInternal(Key key)
        : VersionOwner<Key>(key) {
        for (auto& slot : array) slot.owner = this;
    }
};

template <typename Key> struct Node {
    const Key key;
    explicit Node(Key key_) : key(key_) {}
    virtual ~Node() = default;
};

struct Info {};

template <typename Key> struct Update {
    State state;
    Info* info;
    bool operator==(const Update& other) const {
        return state == other.state && info == other.info;
    }
};

template <typename Key> struct Internal : Node<Key> {
    std::atomic<Update<Key>> update;
    std::atomic<Node<Key>*> left;
    std::atomic<Node<Key>*> right;
    std::atomic<ArraySlot<Key>*> slot_ptr{nullptr};
    Internal(Key key, Node<Key>* l, Node<Key>* r)
        : Node<Key>(key), left(l), right(r) {
        update.store(Update<Key>{State::CLEAN, nullptr});
    }
};

template <typename Key, typename Value> struct Leaf : Node<Key> {
    VersionLeaf<Key, Value>* version;
    Leaf(Key key, VersionLeaf<Key, Value>* version_)
        : Node<Key>(key), version(version_) {}
};

template <typename Key> struct IInfo : Info {
    Internal<Key>* p;
    Internal<Key>* new_internal;
    Node<Key>* old_leaf;
    IInfo(Internal<Key>* p_, Internal<Key>* n_, Node<Key>* l_)
        : p(p_), new_internal(n_), old_leaf(l_) {}
};

template <typename Key> struct DInfo : Info {
    Internal<Key>* gp;
    Internal<Key>* p;
    Node<Key>* leaf;
    Update<Key> pupdate;
    DInfo(Internal<Key>* gp_, Internal<Key>* p_, Node<Key>* l_,
          Update<Key> pupdate_)
        : gp(gp_), p(p_), leaf(l_), pupdate(pupdate_) {}
};

template <typename Key, typename Value, class RecMgr>
class Tree {
    struct SearchResult {
        Internal<Key>* gp;
        Internal<Key>* p;
        Leaf<Key, Value>* leaf;
        Update<Key> pupdate;
        Update<Key> gpupdate;
    };
    const Key key_min_;
    const Key key_max_;
    const Key inf1_;
    const Key inf2_;
    const Value no_value_;
    RecMgr* recmgr_;
    Internal<Key>* root_;
    std::vector<std::atomic<bool>> initialized_;
    std::vector<std::stack<Node<Key>*>*> paths_;

    bool valid_key(Key k) const {
        return k >= key_min_ && k <= key_max_ && k < inf1_;
    }
    static VersionRef<Key>* snapshot(Node<Key>* n) {
        if (auto* leaf = dynamic_cast<Leaf<Key, Value>*>(n))
            return leaf->version;
        return static_cast<Internal<Key>*>(n)->slot_ptr.load(std::memory_order_acquire);
    }

    SearchResult search(Key k, int tid) {
        auto& path = *paths_[tid];
        while (!path.empty()) path.pop();
        Internal<Key>* gp = nullptr;
        Internal<Key>* p = nullptr;
        Update<Key> gpupdate{State::CLEAN, nullptr};
        Update<Key> pupdate{State::CLEAN, nullptr};
        Node<Key>* current = root_;
        while (auto* internal = dynamic_cast<Internal<Key>*>(current)) {
            path.push(current);
            gp = p;
            p = internal;
            gpupdate = pupdate;
            pupdate = p->update.load();
            current = k < p->key
                          ? p->left.load(std::memory_order_acquire)
                          : p->right.load(std::memory_order_acquire);
        }
        path.push(current);
        return {gp, p, static_cast<Leaf<Key, Value>*>(current), pupdate, gpupdate};
    }

    void cas_child(Internal<Key>* parent, Node<Key>* old, Node<Key>* replacement) {
        Node<Key>* left = parent->left.load(std::memory_order_acquire);
        if (left == old) {
            parent->left.compare_exchange_strong(
                old, replacement, std::memory_order_acq_rel,
                std::memory_order_acquire);
        } else {
            parent->right.compare_exchange_strong(
                old, replacement, std::memory_order_acq_rel,
                std::memory_order_acquire);
        }
    }

    void help_insert(IInfo<Key>* op) {
        if (!op) return;
        cas_child(op->p, op->old_leaf, op->new_internal);
        Update<Key> expected{State::IFLAG, op};
        op->p->update.compare_exchange_strong(
            expected, Update<Key>{State::CLEAN, op});
    }

    void help_marked(DInfo<Key>* op) {
        if (!op) return;
        Node<Key>* other = op->p->left.load(std::memory_order_acquire) == op->leaf
                       ? op->p->right.load(std::memory_order_acquire)
                       : op->p->left.load(std::memory_order_acquire);
        cas_child(op->gp, op->p, other);
        Update<Key> expected{State::DFLAG, op};
        op->gp->update.compare_exchange_strong(
            expected, Update<Key>{State::CLEAN, op});
    }

    bool help_delete(DInfo<Key>* op) {
        if (!op) return false;
        Update<Key> expected = op->pupdate;
        Update<Key> desired{State::MARK, op};
        if (op->p->update.compare_exchange_strong(expected, desired) ||
            expected == desired) {
            help_marked(op);
            return true;
        }
        help(expected);
        Update<Key> dflag{State::DFLAG, op};
        op->gp->update.compare_exchange_strong(
            dflag, Update<Key>{State::CLEAN, op});
        return false;
    }

    void help(Update<Key> u) {
        switch (u.state) {
            case State::IFLAG: help_insert(static_cast<IInfo<Key>*>(u.info)); break;
            case State::DFLAG: help_delete(static_cast<DInfo<Key>*>(u.info)); break;
            case State::MARK: help_marked(static_cast<DInfo<Key>*>(u.info)); break;
            case State::CLEAN: break;
        }
    }

    bool refresh(int tid, Internal<Key>* x) {
        ArraySlot<Key>* expected = x->slot_ptr.load(std::memory_order_acquire);
        VersionInternal<Key>* owner = static_cast<VersionInternal<Key>*>(expected->owner);
        const auto index = owner->next_empty_slot.fetch_add(
            1, std::memory_order_relaxed);

        Node<Key>* child;
        VersionRef<Key>* left;
        do {
            child = x->left.load(std::memory_order_acquire);
            left = snapshot(child);
        } while (x->left.load(std::memory_order_acquire) != child);
        VersionRef<Key>* right;
        do {
            child = x->right.load(std::memory_order_acquire);
            right = snapshot(child);
        } while (x->right.load(std::memory_order_acquire) != child);

        VersionInternal<Key>* fresh = nullptr;
        ArraySlot<Key>* desired;
        if (index < ARRAY_SIZE) {
            desired = &owner->array[index];
        } else {
            fresh = new VersionInternal<Key>(x->key);
            COUNT_VERSION(tid);
            const auto first = fresh->next_empty_slot.fetch_add(
                1, std::memory_order_relaxed);
            desired = &fresh->array[first];
        }
        desired->sum = left->sum + right->sum;
        desired->left_slot_ptr = left;
        desired->right_slot_ptr = right;
        const bool installed = x->slot_ptr.compare_exchange_strong(
            expected, desired, std::memory_order_release,
            std::memory_order_acquire);
        if (fresh && !installed) delete fresh;
        return installed;
    }

    void propagate(int tid) {
        auto& path = *paths_[tid];
        while (!path.empty()) {
            Node<Key>* node = path.top();
            path.pop();
            if (node == nullptr) continue;
            // The leaf is popped before propagate, so only internals remain.
            auto* internal = static_cast<Internal<Key>*>(node);
            if (!refresh(tid, internal)) refresh(tid, internal);
        }
    }

    static std::int64_t key_sum(VersionRef<Key>* current) {
        if (current->sum == 0) return 0;
        if (!dynamic_cast<VersionInternal<Key>*>(current->owner))
            return static_cast<std::int64_t>(current->owner->key);
        auto* slot = static_cast<ArraySlot<Key>*>(current);
        return key_sum(slot->left_slot_ptr) + key_sum(slot->right_slot_ptr);
    }

    static bool validate_snapshot(VersionRef<Key>* current) {
        if (!current || !current->owner || current->sum < 0) return false;
        if (!dynamic_cast<VersionInternal<Key>*>(current->owner))
            return current->sum <= 1;
        auto* slot = static_cast<ArraySlot<Key>*>(current);
        return slot->left_slot_ptr && slot->right_slot_ptr &&
               slot->sum == slot->left_slot_ptr->sum +
                                slot->right_slot_ptr->sum &&
               validate_snapshot(slot->left_slot_ptr) &&
               validate_snapshot(slot->right_slot_ptr);
    }

    static int live_count(Node<Key>* node) {
        if (auto* leaf = dynamic_cast<Leaf<Key, Value>*>(node))
            return leaf->version->sum;
        auto* x = static_cast<Internal<Key>*>(node);
        return live_count(x->left.load(std::memory_order_acquire)) +
               live_count(x->right.load(std::memory_order_acquire));
    }

public:
    Tree(int num_threads, Key key_min, Key key_max, Value no_value,
         RecMgr* recmgr)
        : key_min_(key_min), key_max_(key_max),
          inf1_(std::numeric_limits<Key>::max() - 1),
          inf2_(std::numeric_limits<Key>::max()), no_value_(no_value),
          recmgr_(recmgr), root_(nullptr), initialized_(num_threads),
          paths_(num_threads) {
        for (auto& path : paths_) path = new std::stack<Node<Key>*>();
        if (key_min > key_max || key_max >= inf1_)
            throw std::invalid_argument("BST key range overlaps sentinel");
        for (auto& initialized : initialized_)
            initialized.store(false, std::memory_order_relaxed);
        auto* left_v = new VersionLeaf<Key, Value>(inf1_, no_value_, 0);
        auto* right_v = new VersionLeaf<Key, Value>(inf2_, no_value_, 0);
        auto* left = new Leaf<Key, Value>(inf1_, left_v);
        auto* right = new Leaf<Key, Value>(inf2_, right_v);
        root_ = new Internal<Key>(inf2_, left, right);
        auto* root_v = new VersionInternal<Key>(inf2_);
        const auto first = root_v->next_empty_slot.fetch_add(
            1, std::memory_order_relaxed);
        auto* slot = &root_v->array[first];
        slot->sum = 0;
        slot->left_slot_ptr = left_v;
        slot->right_slot_ptr = right_v;
        root_->slot_ptr.store(slot, std::memory_order_relaxed);
    }

    ~Tree() {
        for (auto* path : paths_) delete path;
    }

    void initThread(int tid) {
        if (!initialized_[tid].exchange(true)) recmgr_->initThread(tid);
    }
    void deinitThread(int tid) {
        if (initialized_[tid].exchange(false)) recmgr_->deinitThread(tid);
    }

    Value find(int, Key k) const {
        if (!valid_key(k)) return no_value_;
        VersionRef<Key>* current = root_->slot_ptr.load(std::memory_order_acquire);
        if (!current || current->sum == 0) return no_value_;
        while (auto* internal = dynamic_cast<VersionInternal<Key>*>(current->owner)) {
            auto* slot = static_cast<ArraySlot<Key>*>(current);
            current = k < internal->key
                          ? slot->left_slot_ptr : slot->right_slot_ptr;
            if (!current || current->sum == 0) return no_value_;
        }
        auto* leaf = static_cast<VersionLeaf<Key, Value>*>(current);
        return leaf->key == k ? leaf->value : no_value_;
    }

    Value insertIfAbsent(int tid, Key k, Value value) {
        if (!valid_key(k)) return value;
        for (;;) {
            SearchResult found = search(k, tid);
            auto& path = *paths_[tid];
            path.pop(); // exclude leaf from Propagate
            if (found.leaf->key == k) {
                propagate(tid);
                return found.leaf->version->value;
            }
            if (found.pupdate.state != State::CLEAN) {
                help(found.pupdate);
                continue;
            }
            auto* leaf_v = new VersionLeaf<Key, Value>(k, value, 1);
            COUNT_VERSION(tid);
            auto* new_leaf = new Leaf<Key, Value>(k, leaf_v);
            auto* sibling = new Leaf<Key, Value>(found.leaf->key, found.leaf->version);
            Node<Key>* left = k < found.leaf->key ? static_cast<Node<Key>*>(new_leaf)
                                            : static_cast<Node<Key>*>(sibling);
            Node<Key>* right = k < found.leaf->key ? static_cast<Node<Key>*>(sibling)
                                             : static_cast<Node<Key>*>(new_leaf);
            auto* new_internal = new Internal<Key>(k < found.leaf->key
                                           ? found.leaf->key : k, left, right);
            auto* version = new VersionInternal<Key>(new_internal->key);
            COUNT_VERSION(tid);
            const auto first = version->next_empty_slot.fetch_add(
                1, std::memory_order_relaxed);
            auto* slot = &version->array[first];
            // Both children of a freshly split leaf are Leaves.
            slot->left_slot_ptr = static_cast<Leaf<Key, Value>*>(left)->version;
            slot->right_slot_ptr = static_cast<Leaf<Key, Value>*>(right)->version;
            slot->sum = slot->left_slot_ptr->sum + slot->right_slot_ptr->sum;
            new_internal->slot_ptr.store(slot, std::memory_order_relaxed);
            auto* op = new IInfo<Key>(found.p, new_internal, found.leaf);
            Update<Key> expected = found.pupdate;
            if (found.p->update.compare_exchange_strong(
                    expected, Update<Key>{State::IFLAG, op})) {
                help_insert(op);
                propagate(tid);
                return no_value_;
            }
            help(expected);
            recmgr_->deallocate(tid, new_leaf);
            recmgr_->deallocate(tid, sibling);
            recmgr_->deallocate(tid, new_internal);
            recmgr_->deallocate(tid, op);
            delete version;
            delete leaf_v;
        }
    }

    Value erase(int tid, Key k) {
        if (!valid_key(k)) return no_value_;
        for (;;) {
            SearchResult found = search(k, tid);
            auto& path = *paths_[tid];
            path.pop(); // leaf
            if (found.leaf->key != k) {
                propagate(tid);
                return no_value_;
            }
            path.pop(); // parent is removed by this delete
            if (found.gpupdate.state != State::CLEAN) {
                help(found.gpupdate);
                continue;
            }
            if (found.pupdate.state != State::CLEAN) {
                help(found.pupdate);
                continue;
            }
            auto* op = new DInfo<Key>(found.gp, found.p, found.leaf,
                               found.pupdate);
            Update<Key> expected = found.gpupdate;
            if (found.gp->update.compare_exchange_strong(
                    expected, Update<Key>{State::DFLAG, op})) {
                if (help_delete(op)) {
                    const Value removed = found.leaf->version->value;
                    propagate(tid);
                    return removed;
                }
            } else {
                help(expected);
                recmgr_->deallocate(tid, op);
            }
        }
    }

    int size(int) const {
        return root_->slot_ptr.load(std::memory_order_acquire)->sum;
    }
    std::int64_t keySum() const {
        return key_sum(root_->slot_ptr.load(std::memory_order_acquire));
    }
    bool validateStructure() const {
        VersionRef<Key>* root_snapshot = root_->slot_ptr.load(std::memory_order_acquire);
        return validate_snapshot(root_snapshot) &&
               root_snapshot->sum == live_count(root_);
    }
    Node<Key>* get_root() const { return root_; }
};

} // namespace bst_fatptr

#endif
