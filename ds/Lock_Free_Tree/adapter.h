#ifndef LOCK_FREE_TREE_ADAPTER_H
#define LOCK_FREE_TREE_ADAPTER_H

#include <csignal>
#include <iostream>

#include "Lock_Free_Tree.h"
#include "errors.h"
#ifdef USE_TREE_STATS
#define TREE_STATS_BYTES_AT_DEPTH
#include "tree_stats.h"
#endif

// Typedefs must come BEFORE the macro definitions that use them
template <typename K, typename V>
using node_t = Node<K, V>;
template <typename K, typename V>
using info_t = Info<K, V>;

#define RECORD_MANAGER_T \
    record_manager<Reclaim, Alloc, Pool, node_t<K, V>, info_t<K, V>>
#define DATA_STRUCTURE_T Lock_Free_Tree<K, V, RECORD_MANAGER_T>

template <typename K, typename V, class Reclaim = reclaimer_debra<K>,
          class Alloc = allocator_new<K>, class Pool = pool_none<K>>
class ds_adapter {
   private:
    const V NO_VALUE;
    DATA_STRUCTURE_T* const ds;
    const K KEY_MIN;
    const K KEY_MAX;

   public:
    ds_adapter(const int NUM_THREADS, const K& _KEY_MIN, const K& _KEY_MAX,
               const V& VALUE_RESERVED, Random64* const unused2)
        : NO_VALUE(VALUE_RESERVED),
          ds(new DATA_STRUCTURE_T(NUM_THREADS, _KEY_MIN, _KEY_MAX, NO_VALUE,
                                  0 /* unused */)),
          KEY_MIN(_KEY_MIN),
          KEY_MAX(_KEY_MAX) {}
    ~ds_adapter() { delete ds; }

    V getNoValue() { return NO_VALUE; }

    void initThread(const int tid) { ds->initThread(tid); }
    void deinitThread(const int tid) { ds->deinitThread(tid); }

    V insert(const int tid, const K& key, const V& val) {
        setbench_error(
            "insert-replace functionality not implemented for this data "
            "structure");
        return NO_VALUE;  // Added return
    }

    V insertIfAbsent(const int tid, const K& key, const V& val) {
        return ds->insertIfAbsent(tid, key, val);
    }

    V erase(const int tid, const K& key) { return ds->erase(tid, key); }

    V find(const int tid, const K& key) { return ds->find(tid, key); }

    bool contains(const int tid, const K& key) {
        return find(tid, key) != getNoValue();
    }

    int size(const int tid) { return ds->size(tid); }

    int64_t keySum() { return ds->keySum(); }

    int rangeQuery(const int tid, const K& lo, const K& hi, K* const resultKeys,
                   V* const resultValues) {
        setbench_error("not implemented");
        return 0;  // Added return
    }

    void printSummary() {
        auto recmgr = ds->debugGetRecMgr();
        recmgr->printStatus();

#ifdef USE_TREE_STATS
        auto stats = createTreeStats(KEY_MIN, KEY_MAX);
        std::cout << stats->toString() << std::endl;
        delete stats;
#endif
    }

    bool validateStructure() { return true; }

    void printObjectSizes() {
        std::cout << "sizes: node=" << (sizeof(node_t<K, V>))
                  << " internal_node=" << (sizeof(Internal<K, V>))
                  << " leaf_node=" << (sizeof(Leaf<K, V>))
                  << " descriptor=" << (sizeof(info_t<K, V>))
                  << " iinfo=" << (sizeof(IInfo<K, V>))
                  << " dinfo=" << (sizeof(DInfo<K, V>)) << std::endl;
    }

#ifdef USE_TREE_STATS
    class NodeHandler {
       public:
        typedef node_t<K, V>* NodePtrType;
        K minKey;
        K maxKey;

        NodeHandler(const K& _minKey, const K& _maxKey) {
            minKey = _minKey;
            maxKey = _maxKey;
        }

        class ChildIterator {
           private:
            bool leftDone;
            bool rightDone;
            Internal<K, V>* node;  // node being iterated over
           public:
            ChildIterator(NodePtrType _node) {
                node = dynamic_cast<Internal<K, V>*>(_node);
                if (node == nullptr) {
                    leftDone = true;
                    rightDone = true;
                } else {
                    leftDone = (node->left.load() == NULL);
                    rightDone = (node->right.load() == NULL);
                }
            }
            bool hasNext() { return !(leftDone && rightDone); }
            NodePtrType next() {
                if (!leftDone) {
                    leftDone = true;
                    return node->left.load();
                }
                if (!rightDone) {
                    rightDone = true;
                    return node->right.load();
                }
                setbench_error(
                    "ERROR: it is suspected that you are calling "
                    "ChildIterator::next() without first verifying that it "
                    "hasNext()");
                return NULL;  // Added return
            }
        };

        bool isLeaf(NodePtrType node) {
            return dynamic_cast<Leaf<K, V>*>(node) != nullptr;
        }

        size_t getNumChildren(NodePtrType node) {
            if (isLeaf(node)) return 0;
            auto internalNode = static_cast<Internal<K, V>*>(node);
            return (internalNode->left.load() != NULL) +
                   (internalNode->right.load() != NULL);
        }

        size_t getNumKeys(NodePtrType node) {
            if (!isLeaf(node)) return 0;
            auto leaf = static_cast<Leaf<K, V>*>(node);

            // **FIX:** Check against the actual sentinel values defined in
            // Lock_Free_Tree.h instead of minKey/maxKey from the benchmark
            // parameters.
            if (leaf->key == INFINITY1 || leaf->key == INFINITY2) return 0;

            return 1;
        }

        size_t getSumOfKeys(NodePtrType node) {
            if (getNumKeys(node) == 0) return 0;
            auto leaf = static_cast<Leaf<K, V>*>(node);
            return (size_t)leaf->key;
        }

        ChildIterator getChildIterator(NodePtrType node) {
            return ChildIterator(node);
        }

        static size_t getSizeInBytes(NodePtrType node) {
            if (dynamic_cast<Internal<K, V>*>(node)) {
                return sizeof(Internal<K, V>);
            } else if (dynamic_cast<Leaf<K, V>*>(node)) {
                return sizeof(Leaf<K, V>);
            }
            return sizeof(*node);
        }
    };
    TreeStats<NodeHandler>* createTreeStats(const K& _minKey,
                                            const K& _maxKey) {
        return new TreeStats<NodeHandler>(new NodeHandler(_minKey, _maxKey),
                                          ds->get_root(), true);
    }
#endif
};

#endif  // LOCK_FREE_TREE_ADAPTER_H