#include "minidb/storage/btree/btree_node.h"
#include "minidb/storage/page_format.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace minidb::storage {

    void serialize_leaf_node(const BTreeLeafNode &node, format::RawPage &page) {
        using namespace format;

        if (node.page_id == META_PAGE_ID) {
            throw std::invalid_argument("A B+Tree leaf cannot use metadata pageID : 0");
        }

        if (node.key_count > BTreeNodeFormat::LEAF_CAPACITY) {
            throw std::invalid_argument("Leaf node key count exceeds capacity");
        }

        if (node.next_leaf_page_id != INVALID_PAGE_ID && 
            (node.next_leaf_page_id == META_PAGE_ID || node.next_leaf_page_id == node.page_id)) {
                throw std::invalid_argument("Invalid next leaf pageID");
        }

        for (std::uint16_t i = 1; i < node.key_count; i++) {
          if (node.entries[i - 1].key >= node.entries[i].key) {
            throw std::invalid_argument("Leaf keys must be strictly increasing");
          }
        }

        page = make_data_page(node.page_id, PageType::BTREE_LEAF);

        write_le<std::uint16_t>(page.data() + BTreeNodeFormat::KEY_COUNT_OFFSET, node.key_count);
        write_le<std::uint64_t>(page.data() + BTreeNodeFormat::LEAF_NEXT_PAGE_ID_OFFSET, node.next_leaf_page_id);

        constexpr std::size_t entries_offset = PAGE_HEADER_SIZE + BTreeNodeFormat::LEAF_HEADER_SIZE;
        for (std::uint16_t i = 0; i < node.key_count; i++) {

            const std::size_t offset = entries_offset + static_cast<std::size_t>(i) * BTreeNodeFormat::LEAF_ENTRY_SIZE;

            write_le<std::int64_t>(page.data() + offset, node.entries[i].key);
            write_le<std::uint64_t>(page.data() + offset + sizeof(std::int64_t), node.entries[i].value);
        }
    }

    BTreeLeafNode deserialize_leaf_node(const format::RawPage &page, std::uint64_t expected_page_id) {
        using namespace format;

        validate_data_page(page, expected_page_id);
        BTreeLeafNode node{};

        const auto page_type = read_le<uint16_t>(page.data() + PAGE_TYPE_OFFSET);
        if (page_type != static_cast<std::uint16_t>(PageType::BTREE_LEAF)) {
            throw std::invalid_argument("Page is not a B+Tree Leaf");
        }

        node.page_id = expected_page_id;

        node.key_count = read_le<std::uint16_t>(page.data() + BTreeNodeFormat::KEY_COUNT_OFFSET);
        if (node.key_count > BTreeNodeFormat::LEAF_CAPACITY) {
            throw std::invalid_argument("Leaf node's key count exceeds capacity");
        }

        node.next_leaf_page_id = read_le<std::uint64_t>(page.data() + BTreeNodeFormat::LEAF_NEXT_PAGE_ID_OFFSET);
        if (node.next_leaf_page_id != INVALID_PAGE_ID && 
            (node.next_leaf_page_id == META_PAGE_ID || node.next_leaf_page_id == expected_page_id)) {
            throw std::invalid_argument("Invalid next Leaf page ID");
        }

        constexpr std::size_t entries_offset = PAGE_HEADER_SIZE + BTreeNodeFormat::LEAF_HEADER_SIZE;
        for (std::uint16_t i = 0; i < node.key_count; i++) {

            const std::size_t offset = entries_offset + static_cast<std::size_t>(i) * BTreeNodeFormat::LEAF_ENTRY_SIZE;

            node.entries[i].key = read_le<std::int64_t>(page.data() + offset);
            node.entries[i].value = read_le<std::uint64_t>(page.data() + offset + sizeof(std::int64_t));

            if (i > 0 && (node.entries[i - 1].key >= node.entries[i].key)) {
                throw std::invalid_argument("Leaf keys must be strictly Increasing");
            }
        }
        
        return node;
    }

    void serialize_internal_node(const BTreeInternalNode &node, format::RawPage &page) {
        using namespace format;

        if (node.page_id == META_PAGE_ID) {
            throw std::invalid_argument("A B+Tree internal node cannot use metadata pageID : 0");
        }

        if (node.key_count > BTreeNodeFormat::INTERNAL_CAPACITY) {
            throw std::invalid_argument("Internal node key count exceeds capacity");
        }

        if (node.first_child_page_id == META_PAGE_ID || node.first_child_page_id == INVALID_PAGE_ID) {
                throw std::invalid_argument("Invalid first child pageID");
        }

        for (std::uint16_t i = 0; i < node.key_count; i++) {

            const auto child_page_id = node.entries[i].right_child_page_id;

            if (child_page_id == META_PAGE_ID || child_page_id == INVALID_PAGE_ID ||
                child_page_id == node.page_id || child_page_id == node.first_child_page_id) {
                    throw std::invalid_argument("Invalid right child pageID");
            }

            if (i > 0 && node.entries[i - 1].separator_key >= node.entries[i].separator_key) {
                throw std::invalid_argument("Internal separator keys must be strictly increasing");
            }
        }

        page = make_data_page(node.page_id, PageType::BTREE_INTERNAL);

        write_le<std::uint16_t>(page.data() + BTreeNodeFormat::KEY_COUNT_OFFSET, node.key_count);

        constexpr std::size_t first_child_offset = format::PAGE_HEADER_SIZE + BTreeNodeFormat::INTERNAL_HEADER_SIZE;
        write_le<std::uint64_t>(page.data() + first_child_offset, node.first_child_page_id);

        constexpr std::size_t entries_offset = first_child_offset + sizeof(std::uint64_t);
        for (std::uint16_t i = 0; i < node.key_count; i++) {

            const std::size_t offset = entries_offset + static_cast<std::size_t>(i) * BTreeNodeFormat::INTERNAL_ENTRY_SIZE;

            write_le<std::int64_t>(page.data() + offset, node.entries[i].separator_key);
            write_le<std::uint64_t>(page.data() + offset + sizeof(std::int64_t), node.entries[i].right_child_page_id);
        }
    }

    BTreeInternalNode deserialize_internal_node(const format::RawPage &page, std::uint64_t expected_page_id) {
        using namespace format;

        validate_data_page(page, expected_page_id);
        BTreeInternalNode node{};

        const auto page_type = read_le<uint16_t>(page.data() + PAGE_TYPE_OFFSET);
        if (page_type != static_cast<std::uint16_t>(PageType::BTREE_INTERNAL)) {
            throw std::invalid_argument("Page is not a B+Tree Internal node");
        }

        node.page_id = expected_page_id;

        node.key_count = read_le<std::uint16_t>(page.data() + BTreeNodeFormat::KEY_COUNT_OFFSET);
        if (node.key_count > BTreeNodeFormat::INTERNAL_CAPACITY) {
            throw std::invalid_argument("Internal node's key count exceeds capacity");
        }

        constexpr std::size_t first_child_offset = format::PAGE_HEADER_SIZE + BTreeNodeFormat::INTERNAL_HEADER_SIZE; 
        node.first_child_page_id = read_le<std::uint64_t>(page.data() + first_child_offset);
        if (node.first_child_page_id == INVALID_PAGE_ID || node.first_child_page_id == META_PAGE_ID) {
            throw std::invalid_argument("Invalid first child pageID");
        }

        constexpr std::size_t entries_offset = first_child_offset + sizeof(std::uint64_t);
        for (std::uint16_t i = 0; i < node.key_count; i++) {

            const std::size_t offset = entries_offset + static_cast<std::size_t>(i) * BTreeNodeFormat::INTERNAL_ENTRY_SIZE;

            node.entries[i].separator_key = read_le<std::int64_t>(page.data() + offset);
            node.entries[i].right_child_page_id = read_le<std::uint64_t>(page.data() + offset + sizeof(std::int64_t));

            const auto child_page_id = node.entries[i].right_child_page_id;
            if (child_page_id == META_PAGE_ID || child_page_id == INVALID_PAGE_ID ||
                child_page_id == expected_page_id || child_page_id == node.first_child_page_id) {
                    throw std::invalid_argument("Invalid right child page ID");
            }

            if (i > 0 && (node.entries[i - 1].separator_key >= node.entries[i].separator_key)) {
                throw std::invalid_argument("Internal separator keys must be strictly Increasing");
            }
        }
        
        return node;
    }

    //determines which child pageID to visit next based on the search key
    std::uint64_t find_child_page_id(const BTreeInternalNode &node, std::int64_t key) {
    
        if (node.key_count > BTreeNodeFormat::INTERNAL_CAPACITY) {
            throw std::invalid_argument("Internal node's key count exceeds capacity");
        }

        const auto it = std::upper_bound(node.entries, node.entries + node.key_count, key,
                                        [&](std::int64_t key, const BTreeInternalEntry &entry) {
                                            return key < entry.separator_key;
        });
        const std::size_t child_index = static_cast<std::size_t>(it - node.entries);

        if (child_index == 0)
            return node.first_child_page_id;
        else
            return node.entries[child_index - 1].right_child_page_id;
    }

}   //namespace