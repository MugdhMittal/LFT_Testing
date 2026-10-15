#ifndef TRIE_Baseline_ADAPTER_H
#define TRIE_Baseline_ADAPTER_H

#include <atomic>
#include <csignal>
#include <iostream>

#include "Trie_Baseline.h"
#include "errors.h"

#ifdef USE_TREE_STATS
#define TREE_STATS_BYTES_AT_DEPTH
#include "tree_stats.h"
#endif

extern int MAXKEY;

template <typename K, typename V>
using node_t = Node<K, V>;

#define RECORD_MANAGER_T record_manager<Reclaim, Alloc, Pool, node_t<K, V>>
#define DATA_STRUCTURE_T Trie_Baseline<K, V, RECORD_MANAGER_T>

template <typename K, typename V, class Reclaim = reclaimer_debra<K>,
          class Alloc = allocator_new<K>, class Pool = pool_none<K>>
class ds_adapter {
   private:
    const V NO_VALUE;
    DATA_STRUCTURE_T* ds;
    RECORD_MANAGER_T* recmgr;
    const size_t N;
    K KEY_MIN, KEY_MAX;

   public:
    ds_adapter(const int NUM_THREADS, const K& _KEY_MIN, const K& _KEY_MAX,
               const V& VALUE_RESERVED, Random64* const /*unused*/)
        : NO_VALUE(VALUE_RESERVED),
          N((size_t)((_KEY_MAX > (K)MAXKEY * 2 ? (K)MAXKEY : _KEY_MAX) -
                     _KEY_MIN + 1)),
          KEY_MIN(_KEY_MIN),
          KEY_MAX(_KEY_MAX > (K)MAXKEY * 2 ? (K)MAXKEY : _KEY_MAX) {
        recmgr = new RECORD_MANAGER_T(NUM_THREADS);
        ds = new DATA_STRUCTURE_T(recmgr, NUM_THREADS, N, KEY_MIN, KEY_MAX,
                                  NO_VALUE);
    }

    ~ds_adapter() {
        delete ds;
        delete recmgr;
    }

    void initThread(const int tid) { ds->initThread(tid); }
    void deinitThread(const int tid) { ds->deinitThread(tid); }

    V getNoValue() { return NO_VALUE; }

    V insert(const int tid, const K& key, const V& val) {
        return insertIfAbsent(tid, key, val);
    }

    V insertIfAbsent(const int tid, const K& key, const V& val) {
        return ds->insertIfAbsent(tid, key, val);
    }

    V erase(const int tid, const K& key) { return ds->erase(tid, key); }

    V find(const int tid, const K& key) { return ds->find(tid, key); }

    bool contains(const int tid, const K& key) {
        return find(tid, key) != getNoValue();
    }

    int64_t size(const int tid) { return ds->size(tid); }

    int64_t keySum() { return ds->keySum(); }

    int rangeQuery(const int tid, const K& lo, const K& hi, K* const resultKeys,
                   V* const resultValues) {
        return 0;  // range queries not supported
    }

    void printSummary() {
        recmgr->printStatus();

#ifdef MEASURE_VERSIONS
        printf("Total versions created = %lld\n",
               GSTATS_GET_STAT_METRICS(versions_created, TOTAL)[0].sum);
#endif

#ifdef USE_TREE_STATS
        auto stats = createTreeStats(KEY_MIN, KEY_MAX);
        std::cout << stats->toString() << std::endl;
        delete stats;
#endif
    }

    bool validateStructure() { return ds->validateStructure(); }

    void printObjectSizes() {
        std::cout << "sizes: node=" << sizeof(node_t<K, V>)
                  << " version=" << sizeof(Version<K, V>) << std::endl;
    }

#ifdef USE_TREE_STATS
    class NodeHandler {
       public:
        typedef node_t<K, V>* NodePtrType;
        K minKey;
        K maxKey;

        NodeHandler(const K& _minKey, const K& _maxKey)
            : minKey(_minKey), maxKey(_maxKey) {}

        class ChildIterator {
           private:
            int childIndex;
            NodePtrType node;

           public:
            explicit ChildIterator(NodePtrType _node)
                : childIndex(0), node(_node) {
                if (!node ||
                    (node->left == nullptr && node->right == nullptr)) {
                    childIndex = 2;
                }
            }
            bool hasNext() { return childIndex < 2; }
            NodePtrType next() {
                if (childIndex == 0) {
                    ++childIndex;
                    return node->left;
                }
                if (childIndex == 1) {
                    ++childIndex;
                    return node->right;
                }
                return nullptr;
            }
        };

        static bool isLeaf(NodePtrType node) {
            return node && node->left == nullptr && node->right == nullptr;
        }

        static size_t getNumChildren(NodePtrType node) {
            if (isLeaf(node)) return 0;
            return (node->left ? 1 : 0) + (node->right ? 1 : 0);
        }

        // v1 has no vcounter or slot array: a leaf is logically present iff
        // its current Version has sum == 1.
        static bool isLogicallyPresent(NodePtrType node) {
            if (!node) return false;
            const Version<K, V>* v =
                node->version.load(std::memory_order_acquire);
            return v && v->sum == 1;
        }

        static size_t getNumKeys(NodePtrType node) {
            if (!isLeaf(node)) return 0;
            return isLogicallyPresent(node) ? 1 : 0;
        }

        static size_t getSumOfKeys(NodePtrType node) {
            if (!isLeaf(node)) return 0;
            return isLogicallyPresent(node) ? (size_t)node->key : 0;
        }

        static ChildIterator getChildIterator(NodePtrType node) {
            return ChildIterator(node);
        }

        static size_t getSizeInBytes(NodePtrType node) { return sizeof(*node); }
    };

    TreeStats<NodeHandler>* createTreeStats(const K& _minKey,
                                            const K& _maxKey) {
        return new TreeStats<NodeHandler>(new NodeHandler(_minKey, _maxKey),
                                          ds->get_root(), true);
    }
#endif  // USE_TREE_STATS
};

#undef RECORD_MANAGER_T
#undef DATA_STRUCTURE_T

#endif  // TRIE_Baseline_ADAPTER_H