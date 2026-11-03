#ifndef MINIDB_STORAGE_BPLUS_TREE_H
#define MINIDB_STORAGE_BPLUS_TREE_H

#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

#include "minidb/storage/index_page_manager.h"
#include "minidb/storage/btree/btree_node.h"

namespace minidb::storage {

    class BPlusTree {
        private:
            IndexPageManager &index_page_manager;
            mutable std::mutex mtx;

            struct SplitResult {    //Represents the new right node's separator_key and page_id, in case of a PageSplit
                std::int64_t separator_key;     //right.entries[0].key, smallest key in the right node
                std::uint64_t right_page_id;    //right.page_id
            };

            void create_initial_root(std::int64_t key, std::uint64_t value);

            std::uint64_t find_leaf(std::int64_t key, std::vector<BTreeInternalNode> &parent_path);

            //Returns new leaf node's content to its parent node for necessary modification, in case of a PageSplit
            std::optional<SplitResult> insert_into_leaf(std::uint64_t leaf_page_id, std::int64_t key, std::uint64_t value);

            void propagate_split(std::vector<BTreeInternalNode> &parent_path, SplitResult split);

            std::optional<SplitResult> insert_into_internal(BTreeInternalNode &node, const SplitResult &split);

            SplitResult split_internal_node(BTreeInternalNode &node, const SplitResult &split);

            void create_new_root(std::uint64_t left_page_id, const SplitResult &split);

        public:
            explicit BPlusTree(IndexPageManager &index_page_manager);

            BPlusTree(const BPlusTree &) = delete;
            BPlusTree &operator=(const BPlusTree &) = delete;

            std::optional<std::uint64_t> search(std::int64_t key); //may return std::nullopt

            void insert(std::int64_t key, std::uint64_t value); //generic insert, calls insert_into_leaf()

            std::vector<BTreeLeafEntry> range_scan(std::int64_t start_key, std::int64_t end_key);
    };

}   //namespace

#endif