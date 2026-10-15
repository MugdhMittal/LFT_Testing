#ifndef LOCK_FREE_TREE_H
#define LOCK_FREE_TREE_H

#include <stdlib.h>

#include <atomic>
#include <cmath>
#include <iostream>
#include <thread>
#include <tuple>
#include <vector>

#include "errors.h"
#include "record_manager.h"

using Key = int;
const Key INFINITY1 = 100000000;
const Key INFINITY2 = 1000000000;

template <typename Key, typename Value>
struct Node;
template <typename Key, typename Value>
struct Leaf;
template <typename Key, typename Value>
struct Internal;

enum state { CLEAN, IFLAG, DFLAG, MARK };

template <typename Key, typename Value>
struct Info;

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
    }
};

template <typename Key, typename Value>
struct Leaf : public Node<Key, Value> {
    Key key;
    Value value;
    Leaf(Key k, Value v) : key(k), value(v) {}
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
        Root = new Internal<Key, Value>(INFINITY2, leftLeaf, rightLeaf);
    }

    ~Lock_Free_Tree() { delete recmgr; }

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
    Search(Key k) {
        Internal<Key, Value>*gp = nullptr, *p = nullptr;
        Node<Key, Value>* l = Root;
        Update<Key, Value> gpupdate{CLEAN, nullptr}, pupdate{CLEAN, nullptr};

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
        }
        return {gp, p, dynamic_cast<Leaf<Key, Value>*>(l), pupdate, gpupdate};
    }

    Value find(const int tid, Key k) {
        auto result = Search(k);
        Leaf<Key, Value>* l = std::get<2>(result);
        if (l != nullptr && l->key == k) return l->value;
        return NO_VALUE;
    }

    Value insertIfAbsent(const int tid, Key k, Value v) {
        Internal<Key, Value>*p, *newInternal;
        Leaf<Key, Value>*l, *newSibling;
        Leaf<Key, Value>* newLeaf = new Leaf<Key, Value>(k, v);
        Update<Key, Value> pupdate;
        IInfo<Key, Value>* op;

        while (true) {
            auto searchResult = Search(k);
            p = std::get<1>(searchResult);
            l = std::get<2>(searchResult);
            pupdate = std::get<3>(searchResult);

            if (l != nullptr && l->key == k) {
                delete newLeaf;
                return l->value;
            }
            if (pupdate.st != CLEAN) {
                Help(pupdate);
            } else {
                newSibling = new Leaf<Key, Value>(l->key, l->value);
                newInternal =
                    (k < l->key)
                        ? new Internal<Key, Value>(l->key, newLeaf, newSibling)
                        : new Internal<Key, Value>(newLeaf->key, newSibling,
                                                   newLeaf);
                op = new IInfo<Key, Value>(p, newInternal, l);

                Update<Key, Value> desired = {IFLAG, op};
                if (p->update.compare_exchange_strong(pupdate, desired)) {
                    HelpInsert(op);
                    return NO_VALUE;
                } else {
                    Help(pupdate);
                    delete newSibling;
                    delete newInternal;
                    delete op;
                }
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
        Internal<Key, Value>*gp, *p;
        Leaf<Key, Value>* l;
        Update<Key, Value> pupdate, gpupdate;
        DInfo<Key, Value>* op;

        while (true) {
            auto searchResult = Search(k);
            gp = std::get<0>(searchResult);
            p = std::get<1>(searchResult);
            l = std::get<2>(searchResult);
            pupdate = std::get<3>(searchResult);
            gpupdate = std::get<4>(searchResult);

            if (l == nullptr || l->key != k) {
                return NO_VALUE;
            }
            if (gpupdate.st != CLEAN) {
                Help(gpupdate);
            } else if (pupdate.st != CLEAN) {
                Help(pupdate);
            } else {
                op = new DInfo<Key, Value>(gp, p, l, pupdate);
                Update<Key, Value> desired = {DFLAG, op};
                if (gp->update.compare_exchange_strong(gpupdate, desired)) {
                    if (HelpDelete(op)) {
                        return l->value;
                    }
                } else {
                    Help(gpupdate);
                    delete op;
                }
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
        if (op->p->left.load() == op->l)
            other = op->p->right.load();
        else
            other = op->p->left.load();
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
        if (parent->left.load() == old) {
            parent->left.compare_exchange_strong(old, newchild);
        } else {
            parent->right.compare_exchange_strong(old, newchild);
        }
    }

   public:
    int size(const int tid) {
        setbench_error("size() not implemented");
        return 0;
    }
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