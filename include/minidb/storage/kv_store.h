#ifndef MINIDB_STORAGE_KV_STORE_H
#define MINIDB_STORAGE_KV_STORE_H

#include <cstdint>
#include <optional>
#include <vector>

#include "minidb/storage/btree/bplus_tree.h"
#include "minidb/storage/btree/btree_node.h"
#include "minidb/storage/index_page_manager.h"

namespace minidb::storage {

class KVStore {
 
    private:
    BPlusTree bplus_tree;

    public:
    explicit KVStore(IndexPageManager &index_page_manager);

    KVStore(const KVStore &) = delete;
    KVStore &operator=(const KVStore &) = delete;

    void put(std::int64_t key, std::uint64_t value);

    std::optional<std::uint64_t> get(std::int64_t key);

    std::vector<BTreeLeafEntry> scan(std::int64_t start_key, std::int64_t end_key); //range-scan
};

} // namespace minidb::storage

#endif