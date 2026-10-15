#ifndef TRIE_FatNode_PadFull_H
#define TRIE_FatNode_PadFull_H

#include <stdlib.h>

#include <atomic>
#include <cmath>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>

#include "errors.h"
#include "record_manager.h"

static const int ARRAY_SIZE = 100;  // slots per Version node

// ─── ArraySlot
// ──────────────────────────────────────────────────────────────── Occupies
// exactly one 64-byte cache line. Layout:
//   [0-3]   int  sum        (4B)
//   [4-7]   pad0            (4B) — aligns vcounter to 8-byte boundary
//   [8-15]  uint64_t vcounter (8B)
//   [16-63] pad1            (48B) — fills to 64B; prevents false sharing
//                                    between adjacent heap-allocated slots
template <typename Key, typename Value>
struct ArraySlot {
    int sum;                     // 4B
    char _pad0[4];               // 4B
    uint64_t vcounter;           // 8B
    char _pad1[64 - 4 - 4 - 8];  // 48B  →  total = 64B

    ArraySlot() : sum(0), vcounter(0) {}
    ArraySlot(int s, uint64_t vc) : sum(s), vcounter(vc) {}
};
static_assert(sizeof(ArraySlot<int, void*>) == 64,
              "ArraySlot must be exactly 64 bytes");

template <typename Key, typename Value>
static inline uintptr_t encode_invalid(ArraySlot<Key, Value>* slot) {
    return reinterpret_cast<uintptr_t>(slot) & ~1ULL;  // LSB = 0
}

template <typename Key, typename Value>
static inline uintptr_t encode_valid(ArraySlot<Key, Value>* slot) {
    return reinterpret_cast<uintptr_t>(slot) | 1ULL;  // LSB = 1
}

template <typename Key, typename Value>
static inline ArraySlot<Key, Value>* decode_ptr(uintptr_t tagged) {
    return reinterpret_cast<ArraySlot<Key, Value>*>(tagged & ~1ULL);
}

static inline bool is_valid(uintptr_t tagged) { return (tagged & 1ULL) != 0; }

// ─── num_versions_created
// ───────────────────────────────────────────────────── Wrapped in a struct so
// the 64-byte isolation is part of the type, not a compiler hint.  Every access
// site uses num_versions_created.value.

// ─── Version
// ────────────────────────────────────────────────────────────────── Cache-line
// layout (64B boundary per group):
//
//   Line 0  [0-23]   left, right, previous  (3 × 8B = 24B)
//            [24-63]  _pad0                  (40B)
//   Lines 1-13        array[100]             (100 × 8B = 800B)
//   [after array ends at byte 824]
//   800 % 64 = 32  →  array tail shares 32B of a line.
//   _pad1 = 64-32 = 32B pushes next_empty_slot to a fresh line.
//   Lines 14+         next_empty_slot (4B) + _pad2 (60B)
//
// Effect: next_empty_slot (hit by FAA on every update) never shares a
// cache line with the array tail — eliminates false sharing on the hot path.
template <typename Key, typename Value>
struct Version {
    std::atomic<Version<Key, Value>*> left;      // 8B
    std::atomic<Version<Key, Value>*> right;     // 8B
    std::atomic<Version<Key, Value>*> previous;  // 8B
    char _pad0[64 - 24];                         // 40B  →  line 0 complete

    std::atomic<uintptr_t> array[ARRAY_SIZE];  // 800B (lines 1-13 + 32B)
    char _pad1[64 - (ARRAY_SIZE * 8) % 64];    // 32B  →  flush to line boundary
    std::atomic<int> next_empty_slot;          // 4B
    char _pad2[64 - 4];                        // 60B  →  line complete

    Version()
        : left(nullptr), right(nullptr), previous(nullptr), next_empty_slot(0) {
        for (int i = 0; i < ARRAY_SIZE; ++i) {
            auto* slot = new ArraySlot<Key, Value>();  // sum=0, vcounter=0
            array[i].store(encode_invalid<Key, Value>(slot),
                           std::memory_order_relaxed);
        }
    }

    explicit Version(Version<Key, Value>* prev)
        : left(nullptr), right(nullptr), previous(prev), next_empty_slot(0) {
        for (int i = 0; i < ARRAY_SIZE; ++i) {
            auto* slot = new ArraySlot<Key, Value>();
            array[i].store(encode_invalid<Key, Value>(slot),
                           std::memory_order_relaxed);
        }
    }

    ~Version() {
        for (int i = 0; i < ARRAY_SIZE; ++i) {
            uintptr_t tagged = array[i].load(std::memory_order_relaxed);
            delete decode_ptr<Key, Value>(tagged);
        }
    }
};

// ─── Node
// ─────────────────────────────────────────────────────────────────────
// Occupies exactly one 64-byte cache line.
// Layout:
//   [0-3]   Key   key        (4B, assuming int)
//   [4-7]   _pad0            (4B) — aligns Value (void*) to 8B boundary
//   [8-15]  Value value      (8B)
//   [16-23] Node* left       (8B)
//   [24-31] Node* right      (8B)
//   [32-39] Node* parent     (8B)
//   [40-47] atomic<Version*> version  (8B)
//   [48-55] atomic<uint64_t> vcounter (8B)
//   [56-63] _pad1            (8B) — tail pad to reach 64B
template <typename Key, typename Value>
struct Node {
    Key key;                                         // 4B
    char _pad0[4];                                   // 4B
    Value value;                                     // 8B
    Node<Key, Value>* left;                          // 8B
    Node<Key, Value>* right;                         // 8B
    Node<Key, Value>* parent;                        // 8B
    std::atomic<Version<Key, Value>*> version;       // 8B
    std::atomic<uint64_t> vcounter;                  // 8B
    char _pad1[64 - 4 - 4 - 8 - 8 - 8 - 8 - 8 - 8];  // 8B  →  total = 64B

    explicit Node(Key k)
        : key(k),
          value(Value{}),
          left(nullptr),
          right(nullptr),
          parent(nullptr),
          version(nullptr),
          vcounter(0) {}
};
static_assert(sizeof(Node<int, void*>) == 64, "Node must be exactly 64 bytes");

// ─── Trie_FatNode_PadFull
// ────────────────────────────────────────────────────────────────── Algorithm:
// identical to Trie_FatNode.
//   - Updates use slot-array / vcounter architecture.
//   - Reads (find) traverse the full path from root to child, reading every
//     node's version in between (v2 behaviour; contrast with v3/v5 which jump
//     directly to the leaf after reading the root timestamp).
// Padding: all alignas() directives replaced by explicit char _padN members.

#ifdef MEASURE_VERSIONS
#define COUNT_VERSION(tid) GSTATS_ADD((tid), versions_created, 1)
#else
#define COUNT_VERSION(tid) ((void)(tid))
#endif

#ifdef MEASURE_PREV_TRAVERSAL
#define FTLV_LOOKUP(tid, v, tvc) \
    find_the_latest_version_counted((tid), (v), (tvc), true)
#define FTLV_UPDATE(tid, v, tvc) \
    find_the_latest_version_counted((tid), (v), (tvc), false)
#else
#define FTLV_LOOKUP(tid, v, tvc) find_the_latest_version((v), (tvc))
#define FTLV_UPDATE(tid, v, tvc) find_the_latest_version((v), (tvc))
#endif

template <typename Key, typename Value, class RecMgr>
class Trie_FatNode_PadFull {
   public:
    Trie_FatNode_PadFull(RecMgr* recmgr_, int num_threads, size_t N_,
                         Key key_min, Key key_max, Value no_val)
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

    ~Trie_FatNode_PadFull() {
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

    // ── find: v2 algorithm ─────────────────────────────────────────────────
    // Reads the root timestamp, then traverses every node on the path from
    // root to the target leaf, finding the version at each node that is ≤ the
    // snapshot bound established at the root.
    Value find(const int tid, Key k) {
        if (k < KEY_MIN || k > KEY_MAX) return NO_VALUE;

        Version<Key, Value>* Vr = Root->version.load(std::memory_order_acquire);
        uint64_t root_vc = Root->vcounter.load(std::memory_order_acquire);
        SlotInfo root_info = FTLV_LOOKUP(tid, Vr, root_vc);
        if (!root_info.found) return NO_VALUE;
        uint64_t bound_vc = root_info.vcounter;

        Version<Key, Value>* Vcur = Vr;
        size_t idx = (size_t)(k - KEY_MIN);  // target leaf index [0, N-1]
        size_t l = 0, r = N - 1;

        while (l < r) {
            size_t mid = l + (r - l) / 2;

            Version<Key, Value>* Vchild;
            if (idx <= mid) {
                // go left
                Vchild = Vcur->left.load(std::memory_order_acquire);
                r = mid;
            } else {
                // go right
                Vchild = Vcur->right.load(std::memory_order_acquire);
                l = mid + 1;
            }

            if (!Vchild)
                return NO_VALUE;  // Sanity check: child version should never be
                                  // null if root's version is valid

            SlotInfo child_info = FTLV_LOOKUP(tid, Vchild, bound_vc);
            if (!child_info.found) return NO_VALUE;

            // Tighten the bound if the child's committed vcounter is earlier
            if (child_info.vcounter < bound_vc) {
                bound_vc = child_info.vcounter;
            }

            Vcur = Vchild;
        }
        SlotInfo leaf_info = FTLV_LOOKUP(tid, Vcur, bound_vc);
        return (leaf_info.found && leaf_info.sum == 1) ? Leaf[idx]->value
                                                       : NO_VALUE;
    }

    Value insertIfAbsent(const int tid, Key k, Value val) {
        if (k < KEY_MIN || k > KEY_MAX) return NO_VALUE;
        size_t idx = k - KEY_MIN;
        Node<Key, Value>* leaf = Leaf[idx];

        Version<Key, Value>* oldV =
            leaf->version.load(std::memory_order_acquire);
        uint64_t snap = leaf->vcounter.load(std::memory_order_acquire);

        SlotInfo latest = FTLV_UPDATE(tid, oldV, snap);
        bool result = !latest.found || latest.sum == 0;

        if (result) {
            leaf->value = val;

            int mySlotAtVx =
                oldV->next_empty_slot.fetch_add(1, std::memory_order_acq_rel);

            if (mySlotAtVx < ARRAY_SIZE) {
                uintptr_t tagged =
                    oldV->array[mySlotAtVx].load(std::memory_order_relaxed);
                ArraySlot<Key, Value>* slot = decode_ptr<Key, Value>(tagged);
                slot->sum = 1;
                slot->vcounter = snap + 1;

                uint64_t expected_vc = snap;
                if (leaf->vcounter.compare_exchange_strong(
                        expected_vc, snap + 1, std::memory_order_acq_rel,
                        std::memory_order_relaxed)) {
                    oldV->array[mySlotAtVx].store(
                        encode_valid<Key, Value>(slot),
                        std::memory_order_release);
                    result = true;
                } else {
                    result = false;
                }

            } else {
                Version<Key, Value>* newV = new Version<Key, Value>(oldV);
                COUNT_VERSION(tid);
                uintptr_t tagged =
                    newV->array[0].load(std::memory_order_relaxed);
                ArraySlot<Key, Value>* slot = decode_ptr<Key, Value>(tagged);
                slot->sum = 1;
                slot->vcounter = snap + 1;
                newV->next_empty_slot.store(1, std::memory_order_relaxed);

                if (leaf->version.compare_exchange_strong(
                        oldV, newV, std::memory_order_acq_rel,
                        std::memory_order_relaxed)) {
                    leaf->vcounter.fetch_add(1, std::memory_order_release);
                    slot->vcounter =
                        leaf->vcounter.load(std::memory_order_acquire);
                    newV->array[0].store(encode_valid<Key, Value>(slot),
                                         std::memory_order_release);
                    result = true;
                } else {
                    delete newV;
                    result = false;
                }
            }
        }
        propagate(tid, leaf->parent);
        return result ? NO_VALUE : (Value)k;
    }

    Value erase(const int tid, Key k) {
        if (k < KEY_MIN || k > KEY_MAX) return NO_VALUE;
        size_t idx = k - KEY_MIN;
        Node<Key, Value>* leaf = Leaf[idx];

        Version<Key, Value>* oldV =
            leaf->version.load(std::memory_order_acquire);
        uint64_t snap = leaf->vcounter.load(std::memory_order_acquire);

        SlotInfo latest = FTLV_UPDATE(tid, oldV, snap);
        bool result = latest.found && latest.sum == 1;

        if (result) {
            int mySlotAtVx =
                oldV->next_empty_slot.fetch_add(1, std::memory_order_acq_rel);

            if (mySlotAtVx < ARRAY_SIZE) {
                uintptr_t tagged =
                    oldV->array[mySlotAtVx].load(std::memory_order_relaxed);
                ArraySlot<Key, Value>* slot = decode_ptr<Key, Value>(tagged);
                slot->sum = 0;
                slot->vcounter = snap + 1;

                uint64_t expected_vc = snap;
                if (leaf->vcounter.compare_exchange_strong(
                        expected_vc, snap + 1, std::memory_order_acq_rel,
                        std::memory_order_relaxed)) {
                    oldV->array[mySlotAtVx].store(
                        encode_valid<Key, Value>(slot),
                        std::memory_order_release);
                    result = true;
                } else {
                    result = false;
                }

            } else {
                Version<Key, Value>* newV = new Version<Key, Value>(oldV);
                COUNT_VERSION(tid);
                uintptr_t tagged =
                    newV->array[0].load(std::memory_order_relaxed);
                ArraySlot<Key, Value>* slot = decode_ptr<Key, Value>(tagged);
                slot->sum = 0;
                slot->vcounter = snap + 1;
                newV->next_empty_slot.store(1, std::memory_order_relaxed);

                if (leaf->version.compare_exchange_strong(
                        oldV, newV, std::memory_order_acq_rel,
                        std::memory_order_relaxed)) {
                    leaf->vcounter.fetch_add(1, std::memory_order_release);
                    slot->vcounter =
                        leaf->vcounter.load(std::memory_order_acquire);
                    newV->array[0].store(encode_valid<Key, Value>(slot),
                                         std::memory_order_release);
                    result = true;
                } else {
                    delete newV;
                    result = false;
                }
            }
        }
        propagate(tid, leaf->parent);
        return result ? (Value)k : NO_VALUE;
    }

    int size(const int tid) {
        int keysum = 0;
        for (size_t i = 0; i < N; ++i) {
            Node<Key, Value>* leaf = Leaf[i];
            Version<Key, Value>* v =
                leaf->version.load(std::memory_order_acquire);
            uint64_t snap = leaf->vcounter.load(std::memory_order_acquire);
            SlotInfo info = find_the_latest_version(v, snap);
            if (info.found && info.sum == 1) {
                keysum += (int)leaf->key;
            }
        }
        return keysum;
    }

    int64_t keySum() {
        int64_t total = 0;
        for (size_t i = 0; i < N; ++i) {
            Node<Key, Value>* leaf = Leaf[i];
            uint64_t snap = leaf->vcounter.load(std::memory_order_acquire);
            Version<Key, Value>* v =
                leaf->version.load(std::memory_order_acquire);
            SlotInfo info = find_the_latest_version(v, snap);
            if (info.found && info.sum == 1) {
                total += (int64_t)leaf->key;
            }
        }
        return total;
    }

    bool validateStructure() { return validate_node(Root); }

    RecMgr* debugGetRecMgr() { return recmgr; }
    Node<Key, Value>* get_root() { return Root; }
    const Key& get_key_min() { return KEY_MIN; }
    const Key& get_key_max() { return KEY_MAX; }

   private:
    struct SlotInfo {
        bool found;
        int sum;
        uint64_t vcounter;
        SlotInfo() : found(false), sum(0), vcounter(0) {}
    };

    RecMgr* recmgr;
    Node<Key, Value>* Root;
    std::vector<bool> init;
    const Key KEY_MIN;
    const Key KEY_MAX;
    const size_t N;
    Node<Key, Value>** Leaf;
    const Value NO_VALUE;

    Node<Key, Value>* build_tree(size_t l, size_t r, Node<Key, Value>* parent) {
        auto* node = new Node<Key, Value>(-1);
        node->parent = parent;

        if (l == r) {
            node->key = (Key)(l + KEY_MIN);
            Leaf[l] = node;
            return node;
        }

        size_t mid = l + (r - l) / 2;
        node->left = build_tree(l, mid, node);
        node->right = build_tree(mid + 1, r, node);
        return node;
    }

    void init_versions(Node<Key, Value>* node) {
        if (!node) return;
        if (node->left) init_versions(node->left);
        if (node->right) init_versions(node->right);

        auto* v = new Version<Key, Value>();

        if (node->left && node->right) {
            v->left.store(node->left->version.load(std::memory_order_relaxed),
                          std::memory_order_relaxed);
            v->right.store(node->right->version.load(std::memory_order_relaxed),
                           std::memory_order_relaxed);
        }

        uintptr_t tagged = v->array[0].load(std::memory_order_relaxed);
        ArraySlot<Key, Value>* slot = decode_ptr<Key, Value>(tagged);
        slot->sum = 0;
        slot->vcounter = 0;
        v->next_empty_slot.store(1, std::memory_order_relaxed);
        v->array[0].store(encode_valid<Key, Value>(slot),
                          std::memory_order_relaxed);

        node->vcounter.store(0, std::memory_order_relaxed);
        node->version.store(v, std::memory_order_release);
    }

    void destroy_tree(Node<Key, Value>* node) {
        if (!node) return;
        destroy_tree(node->left);
        destroy_tree(node->right);

        Version<Key, Value>* v = node->version.load(std::memory_order_relaxed);
        while (v != nullptr) {
            Version<Key, Value>* prev =
                v->previous.load(std::memory_order_relaxed);
            delete v;
            v = prev;
        }
        delete node;
    }

    SlotInfo find_the_latest_version(Version<Key, Value>* start_v,
                                     uint64_t target_vc) {
        SlotInfo latest;
        Version<Key, Value>* v = start_v;

        while (v != nullptr) {
            int limit = v->next_empty_slot.load(std::memory_order_acquire);
            if (limit > ARRAY_SIZE) limit = ARRAY_SIZE;

            for (int i = 0; i < limit; ++i) {
                uintptr_t tagged = v->array[i].load(std::memory_order_acquire);
                if (!is_valid(tagged)) continue;

                ArraySlot<Key, Value>* slot = decode_ptr<Key, Value>(tagged);
                uint64_t vc = slot->vcounter;

                if (vc <= target_vc) {
                    if (!latest.found || vc > latest.vcounter) {
                        latest.found = true;
                        latest.sum = slot->sum;
                        latest.vcounter = vc;
                    }
                }
            }

            if (latest.found && latest.vcounter == target_vc) return latest;
            v = v->previous.load(std::memory_order_acquire);
        }

        return latest;
    }

#ifdef MEASURE_PREV_TRAVERSAL
    SlotInfo find_the_latest_version_counted(const int tid,
                                             Version<Key, Value>* start_v,
                                             uint64_t target_vc, bool lookup) {
        if (lookup)
            GSTATS_ADD(tid, find_ftlv_calls, 1);
        else
            GSTATS_ADD(tid, upd_ftlv_calls, 1);
        SlotInfo latest;
        Version<Key, Value>* v = start_v;

        while (v != nullptr) {
            int limit = v->next_empty_slot.load(std::memory_order_acquire);
            if (limit > ARRAY_SIZE) limit = ARRAY_SIZE;

            for (int i = 0; i < limit; ++i) {
                uintptr_t tagged = v->array[i].load(std::memory_order_acquire);
                if (!is_valid(tagged)) continue;

                ArraySlot<Key, Value>* slot = decode_ptr<Key, Value>(tagged);
                uint64_t vc = slot->vcounter;

                if (vc <= target_vc) {
                    if (!latest.found || vc > latest.vcounter) {
                        latest.found = true;
                        latest.sum = slot->sum;
                        latest.vcounter = vc;
                    }
                }
            }

            if (latest.found && latest.vcounter == target_vc) return latest;

            v = v->previous.load(std::memory_order_acquire);
            if (v != nullptr) {
                if (lookup)
                    GSTATS_ADD(tid, find_prev_entries, 1);
                else
                    GSTATS_ADD(tid, upd_prev_entries, 1);
            }
        }
        return latest;
    }
#endif

    bool refresh(const int tid, Node<Key, Value>* x, int mySlotAtVx,
                 Version<Key, Value>* Vx) {
        if (!x || !x->left || !x->right) return true;
        uint64_t vcounter1 = x->vcounter.load(std::memory_order_acquire);

        if (mySlotAtVx < ARRAY_SIZE) {
            uint64_t vcl = x->left->vcounter.load(std::memory_order_acquire);
            uint64_t vcr = x->right->vcounter.load(std::memory_order_acquire);

            Version<Key, Value>* curLeft =
                Vx->left.load(std::memory_order_acquire);
            Version<Key, Value>* freshLeft =
                x->left->version.load(std::memory_order_acquire);
            if (curLeft != freshLeft) {
                Vx->left.store(freshLeft, std::memory_order_release);
            }
            Version<Key, Value>* curRight =
                Vx->right.load(std::memory_order_acquire);
            Version<Key, Value>* freshRight =
                x->right->version.load(std::memory_order_acquire);
            if (curRight != freshRight) {
                Vx->right.store(freshRight, std::memory_order_release);
            }

            SlotInfo sl = FTLV_UPDATE(tid,
                x->left->version.load(std::memory_order_acquire), vcl);
            SlotInfo sr = FTLV_UPDATE(tid,
                x->right->version.load(std::memory_order_acquire), vcr);

            int new_sum = (sl.found ? sl.sum : 0) + (sr.found ? sr.sum : 0);

            uintptr_t tagged =
                Vx->array[mySlotAtVx].load(std::memory_order_relaxed);
            ArraySlot<Key, Value>* slot = decode_ptr<Key, Value>(tagged);
            slot->sum = new_sum;
            slot->vcounter = vcounter1 + 1;

            uint64_t expected_vc = vcounter1;
            if (x->vcounter.compare_exchange_strong(
                    expected_vc, vcounter1 + 1, std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                Vx->array[mySlotAtVx].store(encode_valid<Key, Value>(slot),
                                            std::memory_order_release);
                return true;
            }
            return false;
        }

        // ── Overflow path
        uint64_t vcl = x->left->vcounter.load(std::memory_order_acquire);
        uint64_t vcr = x->right->vcounter.load(std::memory_order_acquire);
        SlotInfo sl = FTLV_UPDATE(tid,
            x->left->version.load(std::memory_order_acquire), vcl);
        SlotInfo sr = FTLV_UPDATE(tid,
            x->right->version.load(std::memory_order_acquire), vcr);

        int new_sum = (sl.found ? sl.sum : 0) + (sr.found ? sr.sum : 0);

        auto* newV = new Version<Key, Value>(Vx);
        COUNT_VERSION(tid);
        newV->left.store(x->left->version.load(std::memory_order_acquire),
                         std::memory_order_relaxed);
        newV->right.store(x->right->version.load(std::memory_order_acquire),
                          std::memory_order_relaxed);
        uintptr_t tagged = newV->array[0].load(std::memory_order_relaxed);
        ArraySlot<Key, Value>* slot = decode_ptr<Key, Value>(tagged);
        slot->sum = new_sum;
        slot->vcounter = vcounter1 + 1;
        newV->next_empty_slot.fetch_add(1, std::memory_order_relaxed);

        Version<Key, Value>* expected_v = Vx;
        if (x->version.compare_exchange_strong(expected_v, newV,
                                               std::memory_order_acq_rel,
                                               std::memory_order_relaxed)) {
            if (x->vcounter.compare_exchange_strong(
                    vcounter1, vcounter1 + 1, std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                newV->array[0].store(encode_valid<Key, Value>(slot),
                                     std::memory_order_release);
                return true;
            } else {
                return false;
            }
        } else {
            delete newV;
            return false;
        }
    }

    void propagate(const int tid, Node<Key, Value>* x) {
        while (x != nullptr) {
            Version<Key, Value>* Vx =
                x->version.load(std::memory_order_acquire);
            int mySlotAtVx =
                Vx->next_empty_slot.fetch_add(1, std::memory_order_acq_rel);

            if (!refresh(tid, x, mySlotAtVx, Vx)) {
                Vx = x->version.load(std::memory_order_acquire);
                mySlotAtVx =
                    Vx->next_empty_slot.fetch_add(1, std::memory_order_acq_rel);
                refresh(tid, x, mySlotAtVx, Vx);
            }
            x = x->parent;
        }
    }

    bool validate_node(Node<Key, Value>* node) {
        if (!node) return true;

        Version<Key, Value>* v = node->version.load(std::memory_order_acquire);

        while (v != nullptr) {
            int limit = v->next_empty_slot.load(std::memory_order_acquire);
            if (limit > ARRAY_SIZE) limit = ARRAY_SIZE;

            for (int i = 0; i < limit; ++i) {
                uintptr_t ti = v->array[i].load(std::memory_order_acquire);
                if (!is_valid(ti)) continue;
                ArraySlot<Key, Value>* si = decode_ptr<Key, Value>(ti);
                for (int j = i + 1; j < limit; ++j) {
                    uintptr_t tj = v->array[j].load(std::memory_order_acquire);
                    if (!is_valid(tj)) continue;
                    ArraySlot<Key, Value>* sj = decode_ptr<Key, Value>(tj);
                    if (sj->vcounter == si->vcounter && sj->sum != si->sum) {
                        return false;
                    }
                }
            }
            v = v->previous.load(std::memory_order_acquire);
        }

        return validate_node(node->left) && validate_node(node->right);
    }
};

#ifdef MEASURE_PREV_TRAVERSAL
#undef FTLV_LOOKUP
#undef FTLV_UPDATE
#endif

#undef COUNT_VERSION
#endif  // TRIE_FatNode_PadFull_H