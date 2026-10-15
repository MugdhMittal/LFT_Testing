#ifndef LOCK_FREE_TREE_NoCAS_H
#define LOCK_FREE_TREE_NoCAS_H

#include <stdlib.h>

#include <atomic>
#include <cmath>
#include <iostream>
#include <stack>
#include <thread>
#include <tuple>
#include <vector>

#include "errors.h"
#include "record_manager.h"

inline std::atomic<size_t> num_versions_created = 0;

using Key = int;
const Key INFINITY1 = 100000000;
const Key INFINITY2 = 1000000000;

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
    Version(Key k) : key(k), left(nullptr), right(nullptr), sum(0) {
        num_versions_created++;
    }
    Version(Key k, Version* l, Version* r, int s)
        : key(k), left(l), right(r), sum(s) {
        num_versions_created++;
    }
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
        this->version.store(nullptr);
    }
    Internal(Key k, Node<Key, Value>* l, Node<Key, Value>* r,
             Version<Key, Value>* v)
        : key(k), left(l), right(r) {
        this->version.store(v);
        update.store(Update<Key, Value>{CLEAN, nullptr});
    }
};

template <typename Key, typename Value>
struct Leaf : public Node<Key, Value> {
    Key key;
    Value value;
    Leaf(Key k, Value v) : key(k), value(v) { this->version.store(nullptr); }
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
    std::vector<std::stack<Node<Key, Value>*>*> operation_stacks;

   public:
    Lock_Free_Tree(const int _NUM_THREADS, const Key& _KEY_MIN,
                   const Key& _KEY_MAX, const Value& _NO_VALUE,
                   int /* unused */)
        : NUM_THREADS(_NUM_THREADS),
          NO_VALUE(_NO_VALUE),
          recmgr(new RecMgr(NUM_THREADS)),
          init(_NUM_THREADS),
          KEY_MIN(_KEY_MIN),
          KEY_MAX(_KEY_MAX) {
        for (int i = 0; i < _NUM_THREADS; ++i) {
            init[i] = false;
        }
        Leaf<Key, Value>* leftLeaf = new Leaf<Key, Value>(INFINITY1, NO_VALUE);
        Leaf<Key, Value>* rightLeaf = new Leaf<Key, Value>(INFINITY2, NO_VALUE);

        leftLeaf->version.store(
            new Version<Key, Value>(INFINITY1, nullptr, nullptr, 0));
        rightLeaf->version.store(
            new Version<Key, Value>(INFINITY2, nullptr, nullptr, 0));

        Root = new Internal<Key, Value>(INFINITY2, leftLeaf, rightLeaf);

        Root->version.store(new Version<Key, Value>(
            INFINITY2, leftLeaf->version.load(), rightLeaf->version.load(), 0));
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
                l = p->left.load();
            else
                l = p->right.load();

            path.push(l);
        }

        return {gp, p, dynamic_cast<Leaf<Key, Value>*>(l), pupdate, gpupdate};
    }

    Value find(const int tid, Key k) {
        auto result = SearchAndGetPath(k, tid);
        Leaf<Key, Value>* l = std::get<2>(result);
        if (l != nullptr && l->key == k) return l->value;
        return NO_VALUE;
    }

    Value insertIfAbsent(const int tid, Key k, Value v) {
        while (true) {
            auto searchResult = SearchAndGetPath(k, tid);
            std::stack<Node<Key, Value>*>& path = *operation_stacks[tid];

            Leaf<Key, Value>* leafNode = std::get<2>(searchResult);
            if (path.top() == leafNode) path.pop();

            if (leafNode != nullptr) {
                if (leafNode->key == k) {
                    Propagate(path);
                    return leafNode->value;
                }

                Internal<Key, Value>* p = std::get<1>(searchResult);
                Update<Key, Value> pupdate = std::get<3>(searchResult);

                if (pupdate.st != CLEAN) {
                    Help(pupdate);
                } else {
                    Leaf<Key, Value>* newLeaf = new Leaf<Key, Value>(k, v);
                    newLeaf->version.store(
                        new Version<Key, Value>(k, nullptr, nullptr, 1));
                    Leaf<Key, Value>* newSibling =
                        new Leaf<Key, Value>(leafNode->key, leafNode->value);
                    newSibling->version.store(leafNode->version.load());

                    Internal<Key, Value>* newInternal =
                        (k < leafNode->key)
                            ? new Internal<Key, Value>(leafNode->key, newLeaf,
                                                       newSibling)
                            : new Internal<Key, Value>(newLeaf->key, newSibling,
                                                       newLeaf);
                    newInternal->version.store(new Version<Key, Value>(
                        newInternal->key, newLeaf->version.load(),
                        newSibling->version.load(),
                        (newSibling->version.load()->sum +
                         newLeaf->version.load()->sum)));

                    IInfo<Key, Value>* op =
                        new IInfo<Key, Value>(p, newInternal, leafNode);

                    Update<Key, Value> desired = {IFLAG, op};
                    p->update.store(desired);
                    HelpInsert(op);
                    Propagate(path);
                    return NO_VALUE;
                }
            } else {
                Propagate(path);
                continue;
            }
        }
    }

    void HelpInsert(IInfo<Key, Value>* op) {
        if (op == nullptr) return;
        CAS_CHILD(op->p, op->l, op->newInternal);
        Update<Key, Value> expected = {IFLAG, op};
        op->p->update.store(Update<Key, Value>{CLEAN, op});
    }

    Value erase(const int tid, Key k) {
        while (true) {
            auto searchResult = SearchAndGetPath(k, tid);
            std::stack<Node<Key, Value>*>& path = *operation_stacks[tid];

            Leaf<Key, Value>* leafNode = std::get<2>(searchResult);
            if (path.top() == leafNode) path.pop();

            if (leafNode != nullptr) {
                if (leafNode->key != k) {
                    Propagate(path);
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

                    gp->update.store(desired);
                    if (HelpDelete(op)) {
                        Propagate(path);
                        return leafNode->value;
                    }
                }
            } else {
                Propagate(path);
                continue;
            }
        }
    }

    bool HelpDelete(DInfo<Key, Value>* op) {
        if (op == nullptr) return false;

        Update<Key, Value> expected = op->pupdate;
        Update<Key, Value> desired = {MARK, op};
        op->p->update.store(desired);
        if (expected == desired) {
            HelpMarked(op);
            return true;
        } else {
            Help(expected);
            Update<Key, Value> expected_dflag = {DFLAG, op};
            op->gp->update.store(Update<Key, Value>{CLEAN, op});
            return false;
        };
    }

    void HelpMarked(DInfo<Key, Value>* op) {
        if (op == nullptr) return;
        Node<Key, Value>* other;
        if (op->p->left.load() == op->l)
            other = op->p->right.load();
        else
            other = op->p->left.load();
        CAS_CHILD(op->gp, op->p, other);
        Update<Key, Value> expected_dflag = {DFLAG, op};
        op->gp->update.store(Update<Key, Value>{CLEAN, op});
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
        if (parent->left.load() == old) {
            parent->left.store(newchild);
        } else {
            parent->right.store(newchild);
        }
    }

    bool Refresh(Node<Key, Value>*& x) {
        Internal<Key, Value>* ix = static_cast<Internal<Key, Value>*>(x);
        Version<Key, Value>* old_version = ix->version.load();

        Node<Key, Value>* xL;
        Version<Key, Value>* vL;
        do {
            xL = ix->left.load();
            vL = (xL ? xL->version.load() : nullptr);
        } while (ix->left.load() != xL);

        Node<Key, Value>* xR;
        Version<Key, Value>* vR;
        do {
            xR = ix->right.load();
            vR = (xR ? xR->version.load() : nullptr);
        } while (ix->right.load() != xR);

        int leftSum = (vL ? vL->sum : 0);
        int rightSum = (vR ? vR->sum : 0);
        int newSum = 0 + leftSum + rightSum;

        Version<Key, Value>* newV =
            new Version<Key, Value>(ix->key, vL, vR, newSum);

        ix->version.store(newV);
        return true;
    }

    void Propagate(std::stack<Node<Key, Value>*>& path) {
        while (!path.empty()) {
            Node<Key, Value>* x = path.top();
            path.pop();

            if (x == nullptr) continue;

            if (!Refresh(x)) {
                Refresh(x);
            }
        }
    }

   public:
    int size(const int tid) { return Root->version.load()->sum; }
    int64_t keySum() {
        setbench_error("keySum() not implemented");
        return 0;
    }
    RecMgr* debugGetRecMgr() { return recmgr; }
    Node<Key, Value>* get_root() { return Root; }

    const Key& get_key_min() { return KEY_MIN; }
    const Key& get_key_max() { return KEY_MAX; }
};
#endif  // LOCK_FREE_TREE_H