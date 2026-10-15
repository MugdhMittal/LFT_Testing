#ifndef TRIE_FatNode_ChildVC_PadVerSlot_A64_H
#define TRIE_FatNode_ChildVC_PadVerSlot_A64_H

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

template <typename Key, typename Value>
struct alignas(64) ArraySlot {
    int sum;                             // 4B
    char _pad0[4];                       // 4B
    uint64_t vcounter;                   // 8B
    uint64_t left_vcounter;              // 8B
    uint64_t right_vcounter;             // 8B
    char _pad1[64 - 4 - 4 - 8 - 8 - 8];  // 32B  → total 64B
    ArraySlot() : sum(0), vcounter(0), left_vcounter(0), right_vcounter(0) {}
    ArraySlot(int s, uint64_t vc)
        : sum(s), vcounter(vc), left_vcounter(0), right_vcounter(0) {}
    ArraySlot(int s, uint64_t vc, uint64_t lvc, uint64_t rvc)
        : sum(s), vcounter(vc), left_vcounter(lvc), right_vcounter(rvc) {}
};
static_assert(sizeof(ArraySlot<int, int>) == 64, "ArraySlot must be 64 bytes");

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
template <typename Key, typename Value>
struct alignas(64) Version {
    std::atomic<Version<Key, Value>*> left;      // 8B
    std::atomic<Version<Key, Value>*> right;     // 8B
    std::atomic<Version<Key, Value>*> previous;  // 8B
    char _pad0[64 - 24];                         // 40B  → line 0 complete
    std::atomic<uintptr_t> array[ARRAY_SIZE];
    char _pad1[64 - (ARRAY_SIZE * 8) % 64];  // flush array tail to a line
    std::atomic<int> next_empty_slot;        // 4B
    char _pad2[64 - 4];                      // 60B  → own line

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

template <typename Key, typename Value>
struct Node {
    Key key;
    Value value;
    Node<Key, Value>* left;
    Node<Key, Value>* right;
    Node<Key, Value>* parent;
    std::atomic<Version<Key, Value>*> version;
    std::atomic<uint64_t> vcounter;

    explicit Node(Key k)
        : key(k),
          value(Value{}),
          left(nullptr),
          right(nullptr),
          parent(nullptr),
          version(nullptr),
          vcounter(0) {}
};

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
class Trie_FatNode_ChildVC_PadVerSlot_A64 {
   public:
    Trie_FatNode_ChildVC_PadVerSlot_A64(RecMgr* recmgr_, int num_threads,
                                        size_t N_, Key key_min, Key key_max,
                                        Value no_val)
        : recmgr(recmgr_),
          Root(nullptr),
          init(num_threads, false),
          KEY_MIN(key_min),
          KEY_MAX(key_max),
          N(N_),
          NO_VALUE(no_val) {
        LOG_N = static_cast<int>(std::ceil(std::log2(static_cast<double>(N))));
        Leaf = new Node<Key, Value>*[N];
        for (size_t i = 0; i < N; ++i) Leaf[i] = nullptr;
        Root = build_tree(0, N - 1, nullptr);
        init_versions(Root);
    }

    ~Trie_FatNode_ChildVC_PadVerSlot_A64() {
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

    Value find(const int tid, Key k) {
        if (k < KEY_MIN || k > KEY_MAX) return NO_VALUE;
        size_t idx = k - KEY_MIN;

        // Read the parent's (Root's) own version and its own timestamp.
        Version<Key, Value>* v = Root->version.load(std::memory_order_acquire);
        uint64_t own_ts = Root->vcounter.load(std::memory_order_acquire);

        SlotInfo parent_slot = FTLV_LOOKUP(tid, v, own_ts);
        if (!parent_slot.found) return NO_VALUE;
        if (parent_slot.sum == 0) return NO_VALUE;  // empty subtree, stop here

        for (int i = 0; i < LOG_N; ++i) {
            if (v == nullptr) return NO_VALUE;
            int bit = (idx >> (LOG_N - 1 - i)) & 1;

            // Go down a level, same as v1's descent.
            v = (bit == 0) ? v->left.load(std::memory_order_acquire)
                           : v->right.load(std::memory_order_acquire);
            if (v == nullptr) return NO_VALUE;

            // Use the SAME timestamp for this branch that the parent had
            // stored for it.
            uint64_t child_ts = (bit == 0) ? parent_slot.left_vcounter
                                           : parent_slot.right_vcounter;

            parent_slot = FTLV_LOOKUP(tid, v, child_ts);
            if (!parent_slot.found) return NO_VALUE;
            if (parent_slot.sum == 0)
                return NO_VALUE;  // empty subtree, stop here
        }

        return (parent_slot.sum > 0) ? Leaf[idx]->value : NO_VALUE;
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
        int count = 0;

        for (size_t i = 0; i < N; ++i) {
            Node<Key, Value>* leaf = Leaf[i];
            Version<Key, Value>* v =
                leaf->version.load(std::memory_order_acquire);
            uint64_t snap = leaf->vcounter.load(std::memory_order_acquire);
            SlotInfo info = find_the_latest_version(v, snap);
            if (info.found && info.sum == 1) {
                keysum += (int)leaf->key;
                ++count;
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
    // Result from find_the_latest_version.
    struct SlotInfo {
        bool found;
        int sum;
        uint64_t vcounter;
        uint64_t left_vcounter;
        uint64_t right_vcounter;
        SlotInfo()
            : found(false),
              sum(0),
              vcounter(0),
              left_vcounter(0),
              right_vcounter(0) {}
    };

    RecMgr* recmgr;
    Node<Key, Value>* Root;
    std::vector<bool> init;
    const Key KEY_MIN;
    const Key KEY_MAX;
    const size_t N;
    int LOG_N;
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

        // slot 0 as VALID with sum=0, vcounter=0.
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
                        latest.left_vcounter = slot->left_vcounter;
                        latest.right_vcounter = slot->right_vcounter;
                    }
                }
            }

            // vcounters are monotonically increasing toward the head of the
            // chain. Once we have found a slot matching target_vc exactly,
            // all older version nodes can only contain smaller vcounters —
            // they cannot improve our result. Stop early.
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
                        latest.left_vcounter = slot->left_vcounter;
                        latest.right_vcounter = slot->right_vcounter;
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

            SlotInfo sl = FTLV_UPDATE(
                tid, x->left->version.load(std::memory_order_acquire), vcl);
            SlotInfo sr = FTLV_UPDATE(
                tid, x->right->version.load(std::memory_order_acquire), vcr);

            int new_sum = (sl.found ? sl.sum : 0) + (sr.found ? sr.sum : 0);

            uintptr_t tagged =
                Vx->array[mySlotAtVx].load(std::memory_order_relaxed);
            ArraySlot<Key, Value>* slot = decode_ptr<Key, Value>(tagged);
            slot->sum = new_sum;
            slot->vcounter = vcounter1 + 1;
            slot->left_vcounter = sl.vcounter;
            slot->right_vcounter = sr.vcounter;

            uint64_t expected_vc = vcounter1;
            // The slot should be currently invalid.
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
        SlotInfo sl = FTLV_UPDATE(
            tid, x->left->version.load(std::memory_order_acquire), vcl);
        SlotInfo sr = FTLV_UPDATE(
            tid, x->right->version.load(std::memory_order_acquire), vcr);

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
        slot->left_vcounter = sl.vcounter;
        slot->right_vcounter = sr.vcounter;
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

    bool validate_aggregate_matches_leaves() {
        int leaf_count = 0;

        for (size_t i = 0; i < N; ++i) {
            Node<Key, Value>* leaf = Leaf[i];
            uint64_t snap = leaf->vcounter.load(std::memory_order_acquire);
            Version<Key, Value>* v =
                leaf->version.load(std::memory_order_acquire);
            SlotInfo info = find_the_latest_version(v, snap);
            if (info.found && info.sum == 1) {
                ++leaf_count;
            }
        }

        uint64_t root_snap = Root->vcounter.load(std::memory_order_acquire);
        Version<Key, Value>* root_v =
            Root->version.load(std::memory_order_acquire);
        SlotInfo root_info = find_the_latest_version(root_v, root_snap);
        int root_aggregate = root_info.found ? root_info.sum : 0;

        return root_aggregate == leaf_count;
    }
};

#ifdef MEASURE_PREV_TRAVERSAL
#undef FTLV_LOOKUP
#undef FTLV_UPDATE
#endif

#undef COUNT_VERSION
#endif  // TRIE_FatNode_ChildVC_PadVerSlot_A64_H