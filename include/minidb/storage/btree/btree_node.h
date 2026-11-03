#ifndef MINIDB_STORAGE_BTREE_NODE_H
#define MINIDB_STORAGE_BTREE_NODE_H

#include "minidb/storage/page_format.h"

#include <cstddef>
#include <cstdint>

/* B+Tree structure:---------
Leaf nodes: sorted (key, value) entries, with a next-leaf page ID for range scans
Internal nodes: sorted separator keys and child page IDs
Page size: 4096 bytes
Page header: 16 bytes
Leaf header: 10 additional bytes (2 bytes for KeyCount and 8 bytes next leaf node reference)
Internal header: 2 additional bytes (2 bytes for KeyCount)
Capacity: derive from format rather than hard-coding
*/

namespace minidb::storage {

    enum class BTreeNodeType : std::uint16_t {  //distinguishes leaf nodes from internal nodes
        LEAF = 1,
        INTERNAL = 2
    };

    struct BTreeNodeFormat {
        
        //Generic page Header: 16 Bytes
        //Bytes [0, 8) : PageID (uint64_t)
        //Bytes [8, 10) : PageType (uint16_t)
        //Bytes [10, 12) : Format version (uint16_t)
        //Bytes [12, 16) : Flags (uint32_t)
        static constexpr std::size_t NODE_TYPE_OFFSET = format::PAGE_TYPE_OFFSET;   //Node type: BTREE_LEAF or BTREE_INTERNAL (2 bytes at offset 8)

        //For both INTERNAL and LEAF nodes 
        static constexpr std::size_t KEY_COUNT_OFFSET = format::PAGE_HEADER_SIZE;   //Number of keys (uint16_t at offset 16)

        //only for LEAF nodes
        static constexpr std::size_t LEAF_NEXT_PAGE_ID_OFFSET = KEY_COUNT_OFFSET + sizeof(std::uint16_t); //Next leaf page ID (uint64_t at offset 18)

        //Leaf-specific Header size = 10 bytes (contains 2 bytes of Node's KeyCount and 8 bytes of next Leaf pageID on right)
        static constexpr std::size_t LEAF_HEADER_SIZE = sizeof(std::uint16_t) + sizeof(std::uint64_t);

        //Internal-node-specific Header size = 2 bytes(contains Key count). First child pageID will be subtracted separately from PAGE_SIZE
        static constexpr std::size_t INTERNAL_HEADER_SIZE = sizeof(std::uint16_t);

        //Leaf entry: int64_t key + uint64_t value
        static constexpr std::size_t LEAF_ENTRY_SIZE = sizeof(std::int64_t) + sizeof(std::uint64_t);

        //Internal entry: int64_t separator key + uint64_t right child page ID
        static constexpr std::size_t INTERNAL_ENTRY_SIZE = sizeof(std::int64_t) + sizeof(std::uint64_t);

        //Leaf capacity = 254 entries
        static constexpr std::size_t LEAF_CAPACITY = (format::PAGE_SIZE - format::PAGE_HEADER_SIZE - LEAF_HEADER_SIZE) / LEAF_ENTRY_SIZE;

        //Internal capacity = 254 separator keys and 255 child pointers(child pageIDs), STD::uint64_t is for the first child pageID
        //An Internal B+Tree node with N separator keys needs (N + 1) children pointers/pageIDs
        static constexpr std::size_t INTERNAL_CAPACITY = (format::PAGE_SIZE - format::PAGE_HEADER_SIZE - INTERNAL_HEADER_SIZE - sizeof(std::uint64_t)) / INTERNAL_ENTRY_SIZE; 
    };
    
    struct BTreeLeafEntry {
        std::int64_t key;
        std::uint64_t value;
    };

    struct BTreeLeafNode {
        static constexpr std::uint64_t INVALID_NEXT_PAGE_ID = format::INVALID_PAGE_ID;  //Initialization

        std::uint64_t page_id;
        std::uint16_t key_count = 0;    //records #valid entries
        std::uint64_t next_leaf_page_id = INVALID_NEXT_PAGE_ID;     //links leaf nodes together

        BTreeLeafEntry entries[BTreeNodeFormat::LEAF_CAPACITY];     //holds sorted key-value pairs
    };

    //writes leaf header, key count, next leaf_page_id and entires into a raw page
    void serialize_leaf_node(const BTreeLeafNode &node, format::RawPage &page);

    //validates a raw page, checks if pageID matches with the expected ID and reconstructs leaf node
    BTreeLeafNode deserialize_leaf_node(const format::RawPage &page, std::uint64_t expected_page_id);


    struct BTreeInternalEntry {
        std::int64_t separator_key;
        std::uint64_t right_child_page_id;
    };

    struct BTreeInternalNode {
        std::uint64_t page_id;
        std::uint16_t key_count = 0;    //records #valid entries

        //An Internal B+Tree node with N separator keys needs (N + 1) children pointers/pageIDs
        std::uint64_t first_child_page_id = 0;

        BTreeInternalEntry entries[BTreeNodeFormat::INTERNAL_CAPACITY];     //holds sorted <separator_key-right_child_page_id> pairs
    };

    //writes node header, key count, first_child_page_id and entires into a raw page
    void serialize_internal_node(const BTreeInternalNode &node, format::RawPage &page);

    //validates a raw page, checks if pageID matches with the expected ID and reconstructs internal node
    BTreeInternalNode deserialize_internal_node(const format::RawPage &page, std::uint64_t expected_page_id);

    //Search routing: for a given key, return the correct child of an internal node
    std::uint64_t find_child_page_id(const BTreeInternalNode &node, std::int64_t key);

}   //namespace
#endif