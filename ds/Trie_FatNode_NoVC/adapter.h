#ifndef SETBENCH_TRIE_FATNODE_NOVC_ADAPTER_H
#define SETBENCH_TRIE_FATNODE_NOVC_ADAPTER_H

#include <cstdint>
#include <iostream>

#include "Trie_FatNode_NoVC.h"
#include "record_manager.h"

#ifdef USE_TREE_STATS
#define TREE_STATS_BYTES_AT_DEPTH
#include "tree_stats.h"
#endif

extern int MAXKEY;

namespace trie_novc {

template <typename K, typename V, class Reclaim, class Alloc, class Pool>
class Adapter {
    using TreeNode = Node<K, V>;
    using RecordManager = record_manager<Reclaim, Alloc, Pool, TreeNode>;
    using Structure = Trie<K, V, RecordManager>;

    const V no_value_;
    K key_min_;
    K key_max_;
    RecordManager* recmgr_;
    Structure* ds_;

    static K effective_max(K requested) {
        return requested > static_cast<K>(static_cast<std::int64_t>(MAXKEY) * 2)
                   ? static_cast<K>(MAXKEY) : requested;
    }

public:
    Adapter(int num_threads, const K& key_min, const K& key_max,
            const V& no_value, Random64* const)
        : no_value_(no_value), key_min_(key_min),
          key_max_(effective_max(key_max)),
          recmgr_(new RecordManager(num_threads)),
          ds_(new Structure(recmgr_, num_threads, key_min_, key_max_,
                            no_value_)) {}

    ~Adapter() {
        delete ds_;
        delete recmgr_;
    }
    void initThread(int tid) { ds_->initThread(tid); }
    void deinitThread(int tid) { ds_->deinitThread(tid); }
    V getNoValue() { return no_value_; }
    V insert(int tid, const K& k, const V& v) {
        return insertIfAbsent(tid, k, v);
    }
    V insertIfAbsent(int tid, const K& k, const V& v) {
        return ds_->insertIfAbsent(tid, k, v);
    }
    V erase(int tid, const K& k) { return ds_->erase(tid, k); }
    V find(int tid, const K& k) { return ds_->find(tid, k); }
    bool contains(int tid, const K& k) { return find(tid, k) != no_value_; }
    std::int64_t size(int tid) { return ds_->size(tid); }
    std::int64_t keySum() { return ds_->keySum(); }
    int rangeQuery(int, const K&, const K&, K* const, V* const) { return 0; }
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
        std::cout << "sizes: node=" << sizeof(TreeNode)
                  << " version=" << sizeof(Version<V>)
                  << " arrayslot=" << sizeof(ArraySlot<V>) << std::endl;
    }

#ifdef USE_TREE_STATS
    class NodeHandler {
    public:
        using NodePtrType = TreeNode*;
        K minKey, maxKey;
        NodeHandler(const K& lo, const K& hi) : minKey(lo), maxKey(hi) {}
        class ChildIterator {
            NodePtrType node_;
            unsigned next_ = 0;
        public:
            explicit ChildIterator(NodePtrType n) : node_(n) {
                if (!n || !n->left) next_ = 2;
            }
            bool hasNext() { return next_ < 2; }
            NodePtrType next() {
                return next_++ == 0 ? node_->left : node_->right;
            }
        };
        static bool isLeaf(NodePtrType n) { return n && !n->left; }
        static std::size_t getNumChildren(NodePtrType n) {
            return isLeaf(n) ? 0 : 2;
        }
        static std::size_t getNumKeys(NodePtrType n) {
            return isLeaf(n) && n->slot_ptr.load(std::memory_order_acquire)->sum
                       ? 1 : 0;
        }
        static std::size_t getSumOfKeys(NodePtrType n) {
            return getNumKeys(n) ? static_cast<std::size_t>(n->key) : 0;
        }
        static ChildIterator getChildIterator(NodePtrType n) {
            return ChildIterator(n);
        }
        static std::size_t getSizeInBytes(NodePtrType) {
            return sizeof(TreeNode);
        }
    };
    TreeStats<NodeHandler>* createTreeStats(const K& lo, const K& hi) {
        return new TreeStats<NodeHandler>(new NodeHandler(lo, hi),
                                          ds_->get_root(), true);
    }
#endif
};

} // namespace trie_novc

template <typename K, typename V>
using node_t = trie_novc::Node<K, V>;

template <typename K, typename V, class Reclaim = reclaimer_debra<K>,
          class Alloc = allocator_new<K>, class Pool = pool_none<K>>
using ds_adapter = trie_novc::Adapter<K, V, Reclaim, Alloc, Pool>;

#endif
