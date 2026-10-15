#ifndef LOCK_FREE_TREE_AUG_H
#define LOCK_FREE_TREE_AUG_H

#include <stdlib.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stack>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <vector>

#include "errors.h"
#include "record_manager.h"

#ifdef MEASURE_VERSIONS
#define COUNT_VERSION(tid) GSTATS_ADD((tid), versions_created, 1)
#else
#define COUNT_VERSION(tid) ((void)(tid))
#endif

template <typename Key, typename Value>
struct Node;
template <typename Key, typename Value>
struct Leaf;
template <typename Key, typename Value>
struct Internal;
template <typename Key, typename Value>
struct Info;

enum state { CLEAN, IFLAG, DFLAG, MARK };

template <typename Key, typename Value>
struct Version {
    Key key;
    Version* left;
    Version* right;
    const int sum;
    const Value value;  // only meaningful at leaf versions
    // Leaf version: no children.
    Version(Key k, int s, Value v)
        : key(k), left(nullptr), right(nullptr), sum(s), value(v) {}
    // Internal version: snapshot of both children.
    Version(Key k, Version* l, Version* r, int s)
        : key(k), left(l), right(r), sum(s), value(Value{}) {}
};

template <typename Key, typename Value>
struct Update {
    state st;
    Info<Key, Value>* info;

    bool operator==(const Update& other) const {
        return st == other.st && info == other.info;
    }
};

template <typename Key, typename Value>
struct Info {
    virtual ~Info() = default;
};

template <typename Key, typename Value>
struct Node {
    virtual ~Node() = default;
    std::atomic<Version<Key, Value>*> version;
};

template <typename Key, typename Value>
struct Internal : public Node<Key, Value> {
    Key key;
    std::atomic<Update<Key, Value>> update;
    Value value;
    std::atomic<Node<Key, Value>*> left;
    std::atomic<Node<Key, Value>*> right;
    Internal(Key k, Node<Key, Value>* l, Node<Key, Value>* r)
        : key(k), left(l), right(r) {
        update.store(Update<Key, Value>{CLEAN, nullptr});
        this->version.store(nullptr, std::memory_order_relaxed);
    }
    Internal(Key k, Node<Key, Value>* l, Node<Key, Value>* r,
             Version<Key, Value>* v)
        : key(k), left(l), right(r) {
        this->version.store(v, std::memory_order_relaxed);
        update.store(Update<Key, Value>{CLEAN, nullptr});
    }
};

template <typename Key, typename Value>
struct Leaf : public Node<Key, Value> {
    Key key;
    Value value;
    Leaf(Key k, Value v) : key(k), value(v) {
        this->version.store(nullptr, std::memory_order_relaxed);
    }
};

template <typename Key, typename Value>
struct IInfo : public Info<Key, Value> {
    Internal<Key, Value>* p;
    Internal<Key, Value>* newInternal;
    Leaf<Key, Value>* l;
    IInfo(Internal<Key, Value>* parent, Internal<Key, Value>* newInternalNode,
          Leaf<Key, Value>* leaf)
        : p(parent), newInternal(newInternalNode), l(leaf) {}
};

template <typename Key, typename Value>
struct DInfo : public Info<Key, Value> {
    Internal<Key, Value>* gp;
    Internal<Key, Value>* p;
    Leaf<Key, Value>* l;
    Update<Key, Value> pupdate;
    DInfo(Internal<Key, Value>* grandParent, Internal<Key, Value>* parent,
          Leaf<Key, Value>* leaf, Update<Key, Value> pu)
        : gp(grandParent), p(parent), l(leaf), pupdate(pu) {}
};

template <typename Key, typename Value, class RecMgr>
class Lock_Free_Tree {
   private:
    const int NUM_THREADS;
    const Value NO_VALUE;
    RecMgr* const recmgr;
    Node<Key, Value>* Root;
    std::vector<std::atomic<bool>> init;
    const Key KEY_MIN;
    const Key KEY_MAX;
    // Sentinel keys: larger than any key that can be inserted.
    const Key INFINITY1;
    const Key INFINITY2;
    std::vector<std::stack<Node<Key, Value>*>*> operation_stacks;

    bool valid_key(Key k) const {
        return k >= KEY_MIN && k <= KEY_MAX && k < INFINITY1;
    }

   public:
    Lock_Free_Tree(const int _NUM_THREADS, const Key& _KEY_MIN,
                   const Key& _KEY_MAX, const Value& _NO_VALUE,
                   int /* unused */)
        : NUM_THREADS(_NUM_THREADS),
          NO_VALUE(_NO_VALUE),
          recmgr(new RecMgr(NUM_THREADS)),
          init(_NUM_THREADS),
          KEY_MIN(_KEY_MIN),
          KEY_MAX(_KEY_MAX),
          INFINITY1(std::numeric_limits<Key>::max() - 1),
          INFINITY2(std::numeric_limits<Key>::max()) {
        if (_KEY_MIN > _KEY_MAX || _KEY_MAX >= INFINITY1)
            throw std::invalid_argument("BST key range overlaps sentinel");
        for (int i = 0; i < _NUM_THREADS; ++i) {
            init[i] = false;
        }
        Leaf<Key, Value>* leftLeaf = new Leaf<Key, Value>(INFINITY1, NO_VALUE);
        Leaf<Key, Value>* rightLeaf = new Leaf<Key, Value>(INFINITY2, NO_VALUE);

        leftLeaf->version.store(new Version<Key, Value>(INFINITY1, 0, NO_VALUE),
                                std::memory_order_relaxed);
        rightLeaf->version.store(
            new Version<Key, Value>(INFINITY2, 0, NO_VALUE),
            std::memory_order_relaxed);

        Root = new Internal<Key, Value>(INFINITY2, leftLeaf, rightLeaf);

        Root->version.store(
            new Version<Key, Value>(
                INFINITY2, leftLeaf->version.load(std::memory_order_relaxed),
                rightLeaf->version.load(std::memory_order_relaxed), 0),
            std::memory_order_relaxed);
        operation_stacks.resize(NUM_THREADS);
        for (int i = 0; i < NUM_THREADS; ++i) {
            operation_stacks[i] = new std::stack<Node<Key, Value>*>();
        }
    }

    ~Lock_Free_Tree() {
        delete recmgr;
        for (int i = 0; i < NUM_THREADS; ++i) {
            delete operation_stacks[i];
        }
    }

    void printTree(Node<Key, Value>* node, int depth) {
        if (node == nullptr) return;

        if (Internal<Key, Value>* internalNode =
                dynamic_cast<Internal<Key, Value>*>(node)) {
            std::cout << "depth=" << depth
                      << " internal_key=" << internalNode->key << std::endl;
            printTree(internalNode->left.load(), depth + 1);
            printTree(internalNode->right.load(), depth + 1);
        } else if (Leaf<Key, Value>* leafNode =
                       dynamic_cast<Leaf<Key, Value>*>(node)) {
            std::cout << "depth=" << depth << " leaf_key=" << leafNode->key
                      << std::endl;
        }
    }
    void printTree() { printTree(Root, 0); }

    void initThread(const int tid) {
        if (init[tid])
            return;
        else
            init[tid] = true;
        recmgr->initThread(tid);
    }

    void deinitThread(const int tid) {
        if (!init[tid])
            return;
        else
            init[tid] = false;
        recmgr->deinitThread(tid);
    }

    std::tuple<Internal<Key, Value>*, Internal<Key, Value>*, Leaf<Key, Value>*,
               Update<Key, Value>, Update<Key, Value>>
    SearchAndGetPath(Key k, int tid) {
        std::stack<Node<Key, Value>*>& path = *operation_stacks[tid];
        while (!path.empty()) path.pop();

        Internal<Key, Value>*gp = nullptr, *p = nullptr;
        Node<Key, Value>* l = Root;
        Update<Key, Value> gpupdate{CLEAN, nullptr}, pupdate{CLEAN, nullptr};

        path.push(l);

        while (Internal<Key, Value>* internalNode =
                   dynamic_cast<Internal<Key, Value>*>(l)) {
            gp = p;
            p = internalNode;
            gpupdate = pupdate;
            pupdate = p->update.load();
            if (k < p->key)
                l = p->left.load(std::memory_order_acquire);
            else
                l = p->right.load(std::memory_order_acquire);

            path.push(l);
        }

        return {gp, p, dynamic_cast<Leaf<Key, Value>*>(l), pupdate, gpupdate};
    }

    // Lookups traverse the root's version snapshot, not the live nodes.
    Value find(const int tid, Key k) {
        if (!valid_key(k)) return NO_VALUE;
        Version<Key, Value>* v = Root->version.load(std::memory_order_acquire);
        if (v == nullptr || v->sum == 0) return NO_VALUE;
        while (v->left != nullptr) {
            v = (k < v->key) ? v->left : v->right;
            if (v == nullptr || v->sum == 0) return NO_VALUE;
        }
        return (v->key == k) ? v->value : NO_VALUE;
    }

    Value insertIfAbsent(const int tid, Key k, Value v) {
        if (!valid_key(k)) return v;
        while (true) {
            auto searchResult = SearchAndGetPath(k, tid);
            std::stack<Node<Key, Value>*>& path = *operation_stacks[tid];

            Leaf<Key, Value>* leafNode = std::get<2>(searchResult);
            if (path.top() == leafNode) path.pop();

            if (leafNode != nullptr) {
                if (leafNode->key == k) {
                    Propagate(path, tid);
                    return leafNode->value;
                }

                Internal<Key, Value>* p = std::get<1>(searchResult);
                Update<Key, Value> pupdate = std::get<3>(searchResult);

                if (pupdate.st != CLEAN) {
                    Help(pupdate);
                } else {
                    Leaf<Key, Value>* newLeaf = new Leaf<Key, Value>(k, v);
                    Version<Key, Value>* newLeafVersion =
                        new Version<Key, Value>(k, 1, v);
                    COUNT_VERSION(tid);
                    newLeaf->version.store(newLeafVersion,
                                           std::memory_order_relaxed);
                    Leaf<Key, Value>* newSibling =
                        new Leaf<Key, Value>(leafNode->key, leafNode->value);
                    newSibling->version.store(
                        leafNode->version.load(std::memory_order_acquire),
                        std::memory_order_relaxed);

                    Internal<Key, Value>* newInternal =
                        (k < leafNode->key)
                            ? new Internal<Key, Value>(leafNode->key, newLeaf,
                                                       newSibling)
                            : new Internal<Key, Value>(newLeaf->key, newSibling,
                                                       newLeaf);
                    Version<Key, Value>* leftVersion =
                        static_cast<Leaf<Key, Value>*>(
                            newInternal->left.load(std::memory_order_relaxed))
                            ->version.load(std::memory_order_relaxed);
                    Version<Key, Value>* rightVersion =
                        static_cast<Leaf<Key, Value>*>(
                            newInternal->right.load(std::memory_order_relaxed))
                            ->version.load(std::memory_order_relaxed);
                    Version<Key, Value>* newInternalVersion =
                        new Version<Key, Value>(
                            newInternal->key, leftVersion, rightVersion,
                            leftVersion->sum + rightVersion->sum);
                    COUNT_VERSION(tid);
                    newInternal->version.store(newInternalVersion,
                                               std::memory_order_relaxed);

                    IInfo<Key, Value>* op =
                        new IInfo<Key, Value>(p, newInternal, leafNode);

                    Update<Key, Value> desired = {IFLAG, op};

                    if (p->update.compare_exchange_strong(pupdate, desired)) {
                        HelpInsert(op);
                        Propagate(path, tid);
                        return NO_VALUE;
                    } else {
                        Help(pupdate);
                        recmgr->deallocate(tid, newLeaf);
                        recmgr->deallocate(tid, newSibling);
                        recmgr->deallocate(tid, newInternal);
                        recmgr->deallocate(tid, op);
                        delete newInternalVersion;
                        delete newLeafVersion;
                    }
                }
            } else {
                Propagate(path, tid);
                continue;
            }
        }
    }

    void HelpInsert(IInfo<Key, Value>* op) {
        if (op == nullptr) return;
        CAS_CHILD(op->p, op->l, op->newInternal);
        Update<Key, Value> expected = {IFLAG, op};
        op->p->update.compare_exchange_strong(expected,
                                              Update<Key, Value>{CLEAN, op});
    }

    Value erase(const int tid, Key k) {
        if (!valid_key(k)) return NO_VALUE;
        while (true) {
            auto searchResult = SearchAndGetPath(k, tid);
            std::stack<Node<Key, Value>*>& path = *operation_stacks[tid];

            Leaf<Key, Value>* leafNode = std::get<2>(searchResult);
            if (path.top() == leafNode) path.pop();

            if (leafNode != nullptr) {
                if (leafNode->key != k) {
                    Propagate(path, tid);
                    return NO_VALUE;
                }

                Internal<Key, Value>* p = std::get<1>(searchResult);
                Internal<Key, Value>* gp = std::get<0>(searchResult);
                Update<Key, Value> pupdate = std::get<3>(searchResult);
                Update<Key, Value> gpupdate = std::get<4>(searchResult);

                if (path.top() == p) path.pop();
                if (gpupdate.st != CLEAN) {
                    Help(gpupdate);
                } else if (pupdate.st != CLEAN) {
                    Help(pupdate);
                } else {
                    DInfo<Key, Value>* op =
                        new DInfo<Key, Value>(gp, p, leafNode, pupdate);
                    Update<Key, Value> desired = {DFLAG, op};

                    if (gp->update.compare_exchange_strong(gpupdate, desired)) {
                        if (HelpDelete(op)) {
                            Propagate(path, tid);
                            return leafNode->value;
                        }

                    } else {
                        Help(gp->update.load());
                        recmgr->deallocate(tid, op);
                    }
                }
            } else {
                Propagate(path, tid);
                continue;
            }
        }
    }

    bool HelpDelete(DInfo<Key, Value>* op) {
        if (op == nullptr) return false;

        Update<Key, Value> expected = op->pupdate;
        Update<Key, Value> desired = {MARK, op};

        if (op->p->update.compare_exchange_strong(expected, desired) ||
            expected == desired) {
            HelpMarked(op);
            return true;
        } else {
            Help(expected);
            Update<Key, Value> expected_dflag = {DFLAG, op};
            op->gp->update.compare_exchange_strong(
                expected_dflag, Update<Key, Value>{CLEAN, op});
            return false;
        };
    }

    void HelpMarked(DInfo<Key, Value>* op) {
        if (op == nullptr) return;
        Node<Key, Value>* other;
        if (op->p->left.load(std::memory_order_acquire) == op->l)
            other = op->p->right.load(std::memory_order_acquire);
        else
            other = op->p->left.load(std::memory_order_acquire);
        CAS_CHILD(op->gp, op->p, other);
        Update<Key, Value> expected_dflag = {DFLAG, op};
        op->gp->update.compare_exchange_strong(expected_dflag,
                                               Update<Key, Value>{CLEAN, op});
    }

    void Help(Update<Key, Value> u) {
        if (u.st == IFLAG) {
            HelpInsert(static_cast<IInfo<Key, Value>*>(u.info));
        } else if (u.st == MARK) {
            HelpMarked(static_cast<DInfo<Key, Value>*>(u.info));
        } else if (u.st == DFLAG) {
            HelpDelete(static_cast<DInfo<Key, Value>*>(u.info));
        }
    }

    void CAS_CHILD(Internal<Key, Value>* parent, Node<Key, Value>* old,
                   Node<Key, Value>* newchild) {
        if (parent->left.load(std::memory_order_acquire) == old) {
            parent->left.compare_exchange_strong(old, newchild,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire);
        } else {
            parent->right.compare_exchange_strong(old, newchild,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire);
        }
    }

    bool Refresh(Node<Key, Value>*& x, const int tid) {
        Internal<Key, Value>* ix = static_cast<Internal<Key, Value>*>(x);
        Version<Key, Value>* old_version =
            ix->version.load(std::memory_order_acquire);

        Node<Key, Value>* xL;
        Version<Key, Value>* vL;
        do {
            xL = ix->left.load(std::memory_order_acquire);
            vL = (xL ? xL->version.load(std::memory_order_acquire) : nullptr);
        } while (ix->left.load(std::memory_order_acquire) != xL);

        Node<Key, Value>* xR;
        Version<Key, Value>* vR;
        do {
            xR = ix->right.load(std::memory_order_acquire);
            vR = (xR ? xR->version.load(std::memory_order_acquire) : nullptr);
        } while (ix->right.load(std::memory_order_acquire) != xR);

        int leftSum = (vL ? vL->sum : 0);
        int rightSum = (vR ? vR->sum : 0);
        int newSum = 0 + leftSum + rightSum;

        Version<Key, Value>* newV =
            new Version<Key, Value>(ix->key, vL, vR, newSum);
        COUNT_VERSION(tid);

        if (ix->version.compare_exchange_strong(old_version, newV,
                                                std::memory_order_release,
                                                std::memory_order_acquire)) {
            return true;
        } else {
            delete newV;
            return false;
        }
    }

    void Propagate(std::stack<Node<Key, Value>*>& path, const int tid) {
        while (!path.empty()) {
            Node<Key, Value>* x = path.top();
            path.pop();

            if (x == nullptr) continue;

            if (!Refresh(x, tid)) {
                Refresh(x, tid);
            }
        }
    }

   private:
    static int64_t key_sum(Version<Key, Value>* v) {
        if (v == nullptr || v->sum == 0) return 0;
        if (v->left == nullptr) return static_cast<int64_t>(v->key);
        return key_sum(v->left) + key_sum(v->right);
    }

    static bool validate_version(Version<Key, Value>* v) {
        if (v == nullptr || v->sum < 0) return false;
        if (v->left == nullptr && v->right == nullptr) return v->sum <= 1;
        return v->left && v->right &&
               v->sum == v->left->sum + v->right->sum &&
               validate_version(v->left) && validate_version(v->right);
    }

    static int live_count(Node<Key, Value>* node) {
        if (auto* leaf = dynamic_cast<Leaf<Key, Value>*>(node))
            return leaf->version.load(std::memory_order_acquire)->sum;
        auto* x = static_cast<Internal<Key, Value>*>(node);
        return live_count(x->left.load(std::memory_order_acquire)) +
               live_count(x->right.load(std::memory_order_acquire));
    }

   public:
    int size(const int tid) {
        return Root->version.load(std::memory_order_acquire)->sum;
    }
    int64_t keySum() {
        return key_sum(Root->version.load(std::memory_order_acquire));
    }
    bool validateStructure() {
        Version<Key, Value>* root_version =
            Root->version.load(std::memory_order_acquire);
        return validate_version(root_version) &&
               root_version->sum == live_count(Root);
    }
    RecMgr* debugGetRecMgr() { return recmgr; }
    Node<Key, Value>* get_root() { return Root; }

    const Key& get_key_min() { return KEY_MIN; }
    const Key& get_key_max() { return KEY_MAX; }
};

#undef COUNT_VERSION
#endif  // LOCK_FREE_TREE_H
