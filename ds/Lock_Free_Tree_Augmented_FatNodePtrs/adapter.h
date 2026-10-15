#ifndef SETBENCH_LOCK_FREE_TREE_AUGMENTED_FATNODEPTRS_ADAPTER_H
#define SETBENCH_LOCK_FREE_TREE_AUGMENTED_FATNODEPTRS_ADAPTER_H

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>

#include "Lock_Free_Tree_Augmented_FatNodePtrs.h"
#include "errors.h"
#include "record_manager.h"

#ifdef USE_TREE_STATS
#define TREE_STATS_BYTES_AT_DEPTH
#include "tree_stats.h"
#endif

template <typename K, typename V>
using node_t = bst_fatptr::Node<K>;

template <typename K, typename V, class Reclaim = reclaimer_debra<K>,
          class Alloc = allocator_new<K>, class Pool = pool_none<K>>
class ds_adapter {
    using RecordManager = record_manager<
        Reclaim, Alloc, Pool, bst_fatptr::Internal<K>, bst_fatptr::Leaf<K, V>,
        bst_fatptr::IInfo<K>, bst_fatptr::DInfo<K>>;
    using Structure = bst_fatptr::Tree<K, V, RecordManager>;

    const V no_value_;
    const K key_min_;
    const K key_max_;
    RecordManager* recmgr_;
    Structure* ds_;

public:
    ds_adapter(int num_threads, const K& key_min, const K& key_max,
               const V& no_value, Random64* const)
        : no_value_(no_value), key_min_(key_min),
          key_max_(std::min(
              key_max, static_cast<K>(std::numeric_limits<K>::max() - 2))),
          recmgr_(new RecordManager(num_threads)),
          ds_(new Structure(num_threads, key_min_, key_max_, no_value_,
                            recmgr_)) {}
    ~ds_adapter() {
        delete ds_;
        delete recmgr_;
    }

    V getNoValue() { return no_value_; }
    void initThread(int tid) { ds_->initThread(tid); }
    void deinitThread(int tid) { ds_->deinitThread(tid); }
    V insert(int, const K&, const V&) {
        setbench_error("insert-replace is not supported by this BST");
        return no_value_;
    }
    V insertIfAbsent(int tid, const K& k, const V& v) {
        return ds_->insertIfAbsent(tid, k, v);
    }
    V erase(int tid, const K& k) { return ds_->erase(tid, k); }
    V find(int tid, const K& k) { return ds_->find(tid, k); }
    bool contains(int tid, const K& k) { return find(tid, k) != no_value_; }
    int size(int tid) { return ds_->size(tid); }
    std::int64_t keySum() { return ds_->keySum(); }
    int rangeQuery(int, const K&, const K&, K* const, V* const) {
        setbench_error("range queries are not supported by this BST");
        return 0;
    }
    void printSummary() {
        recmgr_->printStatus();
#ifdef MEASURE_VERSIONS
        std::cout << "Total versions created = "
                  << GSTATS_GET_STAT_METRICS(versions_created, TOTAL)[0].sum
                  << std::endl;
#endif
#ifdef USE_TREE_STATS
        auto* stats = createTreeStats(key_min_, key_max_);
        std::cout << stats->toString() << std::endl;
        delete stats;
#endif
    }
    bool validateStructure() { return ds_->validateStructure(); }
    void printObjectSizes() {
        std::cout << "sizes: node=" << sizeof(bst_fatptr::Node<K>)
                  << " internal=" << sizeof(bst_fatptr::Internal<K>)
                  << " leaf=" << sizeof(bst_fatptr::Leaf<K, V>)
                  << " internal_version=" << sizeof(bst_fatptr::VersionInternal<K>)
                  << " leaf_version=" << sizeof(bst_fatptr::VersionLeaf<K, V>)
                  << " arrayslot=" << sizeof(bst_fatptr::ArraySlot<K>)
                  << " iinfo=" << sizeof(bst_fatptr::IInfo<K>)
                  << " dinfo=" << sizeof(bst_fatptr::DInfo<K>) << std::endl;
    }

#ifdef USE_TREE_STATS
    class NodeHandler {
    public:
        using NodePtrType = bst_fatptr::Node<K>*;
        K minKey, maxKey;
        NodeHandler(const K& lo, const K& hi) : minKey(lo), maxKey(hi) {}
        class ChildIterator {
            bst_fatptr::Internal<K>* node_;
            unsigned next_ = 0;
        public:
            explicit ChildIterator(NodePtrType n)
                : node_(dynamic_cast<bst_fatptr::Internal<K>*>(n)) {
                if (!node_) next_ = 2;
            }
            bool hasNext() { return next_ < 2; }
            NodePtrType next() {
                return next_++ == 0 ? node_->left.load() : node_->right.load();
            }
        };
        static bool isLeaf(NodePtrType n) {
            return dynamic_cast<bst_fatptr::Leaf<K, V>*>(n) != nullptr;
        }
        static std::size_t getNumChildren(NodePtrType n) {
            return isLeaf(n) ? 0 : 2;
        }
        static std::size_t getNumKeys(NodePtrType n) {
            return isLeaf(n) && static_cast<bst_fatptr::Leaf<K, V>*>(n)->version->sum ? 1 : 0;
        }
        static std::size_t getSumOfKeys(NodePtrType n) {
            return getNumKeys(n) ? static_cast<std::size_t>(n->key) : 0;
        }
        static ChildIterator getChildIterator(NodePtrType n) {
            return ChildIterator(n);
        }
        static std::size_t getSizeInBytes(NodePtrType n) {
            return isLeaf(n) ? sizeof(bst_fatptr::Leaf<K, V>) : sizeof(bst_fatptr::Internal<K>);
        }
    };
    TreeStats<NodeHandler>* createTreeStats(const K& lo, const K& hi) {
        return new TreeStats<NodeHandler>(new NodeHandler(lo, hi),
                                          ds_->get_root(), true);
    }
#endif
};

#endif
