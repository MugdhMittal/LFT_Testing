#ifndef TRIE_Baseline_H
#define TRIE_Baseline_H

#include <stdlib.h>

#include <atomic>
#include <cmath>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>

#include "errors.h"
#include "record_manager.h"

// ── Version
// ─────────────────────────────────────────────────────────────────── Immutable
// path-copied node.  Left/right are plain (non-atomic) pointers because a
// Version is never mutated after publication. keyValue is only meaningful at
// leaves (sum == 1).
template <typename Key, typename Value>
struct Version {
    const Version<Key, Value>* left;
    const Version<Key, Value>* right;
    const int sum;       // aggregate key count in this subtree
    const Key keyValue;  // actual key stored at leaf (only valid when sum==1)

    // Leaf constructor
    Version(int leafBit, Key key = Key{})
        : left(nullptr), right(nullptr), sum(leafBit), keyValue(key) {}

    // Internal-node constructor
    Version(const Version<Key, Value>* L, const Version<Key, Value>* R)
        : left(L),
          right(R),
          sum((L ? L->sum : 0) + (R ? R->sum : 0)),
          keyValue(Key{}) {}
};

// ── Node
// ──────────────────────────────────────────────────────────────────────
template <typename Key, typename Value>
struct Node {
    Key key;
    Value value;
    Node<Key, Value>* left;
    Node<Key, Value>* right;
    Node<Key, Value>* parent;
    std::atomic<const Version<Key, Value>*> version;

    explicit Node(Key k)
        : key(k),
          value(Value{}),
          left(nullptr),
          right(nullptr),
          parent(nullptr),
          version(nullptr) {}
};

// ── Trie_Baseline
// ───────────────────────────────────────────────────────────────────

#ifdef MEASURE_VERSIONS
#define COUNT_VERSION(tid) GSTATS_ADD((tid), versions_created, 1)
#else
#define COUNT_VERSION(tid) ((void)(tid))
#endif
template <typename Key, typename Value, class RecMgr>
class Trie_Baseline {
   public:
    Trie_Baseline(RecMgr* recmgr_, int num_threads, size_t N_, Key key_min,
                  Key key_max, Value no_val)
        : recmgr(recmgr_),
          Root(nullptr),
          init(num_threads, false),
          KEY_MIN(key_min),
          KEY_MAX(key_max),
          N(N_),
          NO_VALUE(no_val) {
        Leaf = new Node<Key, Value>*[N];
        for (size_t i = 0; i < N; ++i) Leaf[i] = nullptr;
        Root = build_tree(0, N - 1, nullptr);
        init_versions(Root);
    }

    ~Trie_Baseline() {
        destroy_tree(Root);
        delete[] Leaf;
    }

    void initThread(const int tid) {
        if (init[tid]) return;
        init[tid] = true;
        recmgr->initThread(tid);
    }

    void deinitThread(const int tid) {
        if (!init[tid]) return;
        init[tid] = false;
        recmgr->deinitThread(tid);
    }

    // ── find ─────────────────────────────────────────────────────────────────
    // Walks the root Version's immutable left/right pointers, splitting the
    // leaf range [l, r] exactly as build_tree does.
    Value find(const int tid, Key k) {
        if (k < KEY_MIN || k > KEY_MAX) return NO_VALUE;
        size_t idx = k - KEY_MIN;

        const Version<Key, Value>* v =
            Root->version.load(std::memory_order_acquire);

        size_t l = 0, r = N - 1;
        while (l < r) {
            if (v == nullptr) return NO_VALUE;
            size_t mid = l + (r - l) / 2;
            if (idx <= mid) {
                v = v->left;
                r = mid;
            } else {
                v = v->right;
                l = mid + 1;
            }
        }

        return (v && v->sum > 0) ? Leaf[idx]->value : NO_VALUE;
    }

    // ── insertIfAbsent ───────────────────────────────────────────────────────
    Value insertIfAbsent(const int tid, Key k, Value val) {
        if (k < KEY_MIN || k > KEY_MAX) return NO_VALUE;
        size_t idx = k - KEY_MIN;
        Node<Key, Value>* leaf = Leaf[idx];

        const Version<Key, Value>* oldV =
            leaf->version.load(std::memory_order_acquire);
        bool result = (oldV->sum == 0);

        if (result) {
            leaf->value = val;
            auto* newV = new Version<Key, Value>(1, k);
            COUNT_VERSION(tid);
            if (leaf->version.compare_exchange_strong(
                    oldV, newV, std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                result = true;
            } else {
                delete newV;
                result = false;
            }
        }

        propagate(tid, leaf->parent);
        return result ? NO_VALUE : (Value)k;
    }

    // ── erase ────────────────────────────────────────────────────────────────
    Value erase(const int tid, Key k) {
        if (k < KEY_MIN || k > KEY_MAX) return NO_VALUE;
        size_t idx = k - KEY_MIN;
        Node<Key, Value>* leaf = Leaf[idx];

        const Version<Key, Value>* oldV =
            leaf->version.load(std::memory_order_acquire);
        bool result = (oldV->sum == 1);

        if (result) {
            auto* newV = new Version<Key, Value>(0, Key{});
            COUNT_VERSION(tid);
            if (leaf->version.compare_exchange_strong(
                    oldV, newV, std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                result = true;
            } else {
                delete newV;
                result = false;
            }
        }

        propagate(tid, leaf->parent);
        return result ? (Value)k : NO_VALUE;
    }

    // ── size ─────────────────────────────────────────────────────────────────
    // Matches v3 convention: returns the sum of present key values, not a
    // count.
    int64_t size(const int tid) {
        int64_t keysum = 0;
        for (size_t i = 0; i < N; ++i) {
            Node<Key, Value>* leaf = Leaf[i];
            const Version<Key, Value>* v =
                leaf->version.load(std::memory_order_acquire);
            if (v && v->sum == 1) {
                keysum += (int64_t)leaf->key;
            }
        }
        return keysum;
    }

    // ── keySum ───────────────────────────────────────────────────────────────
    int64_t keySum() {
        int64_t total = 0;
        for (size_t i = 0; i < N; ++i) {
            Node<Key, Value>* leaf = Leaf[i];
            const Version<Key, Value>* v =
                leaf->version.load(std::memory_order_acquire);
            if (v && v->sum == 1) {
                total += (int64_t)leaf->key;
            }
        }
        return total;
    }

    // ── validateStructure ────────────────────────────────────────────────────
    bool validateStructure() { return validate_node(Root); }

    // ── accessors ────────────────────────────────────────────────────────────
    RecMgr* debugGetRecMgr() { return recmgr; }
    Node<Key, Value>* get_root() { return Root; }
    const Key& get_key_min() { return KEY_MIN; }
    const Key& get_key_max() { return KEY_MAX; }

   private:
    RecMgr* recmgr;
    Node<Key, Value>* Root;
    std::vector<bool> init;
    const Key KEY_MIN;
    const Key KEY_MAX;
    const size_t N;
    Node<Key, Value>** Leaf;
    const Value NO_VALUE;

    // ── build_tree ───────────────────────────────────────────────────────────
    // Range-split identical to v3.  Internal nodes get key=-1 as a sentinel;
    // leaves get their actual key assigned here.
    Node<Key, Value>* build_tree(size_t l, size_t r, Node<Key, Value>* parent) {
        auto* node = new Node<Key, Value>(static_cast<Key>(-1));
        node->parent = parent;

        if (l == r) {
            node->key = static_cast<Key>(l + KEY_MIN);
            Leaf[l] = node;
            return node;
        }

        size_t mid = l + (r - l) / 2;
        node->left = build_tree(l, mid, node);
        node->right = build_tree(mid + 1, r, node);
        return node;
    }

    // ── init_versions ────────────────────────────────────────────────────────
    // Post-order pass: assign the initial Version to every node after the
    // structural tree is fully built.  Leaves get sum=0; internal nodes get
    // sum derived from children (all zero at init).
    void init_versions(Node<Key, Value>* node) {
        if (!node) return;

        if (node->left) init_versions(node->left);
        if (node->right) init_versions(node->right);

        Version<Key, Value>* v;
        if (!node->left && !node->right) {
            // Leaf: sum=0, no key stored yet.
            v = new Version<Key, Value>(0, node->key);

        } else {
            // Internal node: snapshot children's current versions.
            const Version<Key, Value>* vl =
                node->left ? node->left->version.load(std::memory_order_relaxed)
                           : nullptr;
            const Version<Key, Value>* vr =
                node->right
                    ? node->right->version.load(std::memory_order_relaxed)
                    : nullptr;
            v = new Version<Key, Value>(vl, vr);
        }

        node->version.store(v, std::memory_order_release);
    }

    // ── destroy_tree ─────────────────────────────────────────────────────────
    // v1 Versions are singly-owned (no chain); one delete per node suffices.
    // During normal operation Refresh leaks the old Version — that is the
    // existing v1 behaviour and is not changed here.
    void destroy_tree(Node<Key, Value>* node) {
        if (!node) return;
        destroy_tree(node->left);
        destroy_tree(node->right);

        // Delete the version currently installed on this node.
        const Version<Key, Value>* v =
            node->version.load(std::memory_order_relaxed);
        delete v;

        delete node;
    }

    // ── refresh ──────────────────────────────────────────────────────────────
    // Pure path-copying: read left/right child versions, CAS a new aggregate
    // Version onto node.  No slot arrays, no vcounters.
    bool refresh(const int tid, Node<Key, Value>* node) {
        if (!node || !node->left || !node->right) return true;

        const Version<Key, Value>* oldV =
            node->version.load(std::memory_order_acquire);
        const Version<Key, Value>* vl =
            node->left->version.load(std::memory_order_acquire);
        const Version<Key, Value>* vr =
            node->right->version.load(std::memory_order_acquire);

        auto* newV = new Version<Key, Value>(vl, vr);
        COUNT_VERSION(tid);

        if (node->version.compare_exchange_strong(oldV, newV,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_relaxed)) {
            return true;
        }
        delete newV;
        return false;
    }

    // ── propagate ────────────────────────────────────────────────────────────
    void propagate(const int tid, Node<Key, Value>* x) {
        while (x != nullptr) {
            if (!refresh(tid, x)) refresh(tid, x);
            x = x->parent;
        }
    }

    // ── validate_node ────────────────────────────────────────────────────────
    // Structural invariant for v1: every internal node's version->sum must
    // equal the sum of its children's version->sum values.
    bool validate_node(Node<Key, Value>* node) {
        if (!node) return true;

        // Leaf: sum must be 0 or 1.
        if (!node->left && !node->right) {
            const Version<Key, Value>* v =
                node->version.load(std::memory_order_acquire);
            if (!v) return false;
            return (v->sum == 0 || v->sum == 1);
        }

        // Internal node: sum == left->sum + right->sum.
        const Version<Key, Value>* v =
            node->version.load(std::memory_order_acquire);
        if (!v) return false;

        int left_sum = 0;
        int right_sum = 0;
        if (node->left) {
            const Version<Key, Value>* vl =
                node->left->version.load(std::memory_order_acquire);
            if (vl) left_sum = vl->sum;
        }
        if (node->right) {
            const Version<Key, Value>* vr =
                node->right->version.load(std::memory_order_acquire);
            if (vr) right_sum = vr->sum;
        }

        if (v->sum != left_sum + right_sum) return false;

        return validate_node(node->left) && validate_node(node->right);
    }
};
#undef COUNT_VERSION
#endif  // TRIE_Baseline_H