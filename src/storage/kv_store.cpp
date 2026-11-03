#include "minidb/storage/kv_store.h"
#include "minidb/storage/btree/bplus_tree.h"
#include "minidb/storage/index_page_manager.h"

namespace minidb::storage {

    KVStore::KVStore(IndexPageManager &index_page_manager) : bplus_tree(index_page_manager) {}

    void KVStore::put(std::int64_t key, std::uint64_t value) {
        bplus_tree.insert(key, value);
    }

    std::optional<std::uint64_t> KVStore::get(std::int64_t key) {
        return bplus_tree.search(key);
    }

    std::vector<BTreeLeafEntry> KVStore::scan(std::int64_t start_key, std::int64_t end_key) {
        return bplus_tree.range_scan(start_key, end_key);
    }

} //namespace