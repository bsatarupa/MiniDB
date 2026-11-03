#include <cassert>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

#include "minidb/storage/btree/btree_node.h"

using namespace minidb::storage;
using namespace minidb::storage::format;

namespace {

const std::string TEST_FILE = "build/btree_node_test.db";

//A. Leaf Node tests:-----------
// Test 1: Serialize and deserialize an empty leaf.
void test_empty_leaf_round_trip() {
    BTreeLeafNode original{};
    original.page_id = 1;
    original.next_leaf_page_id = INVALID_PAGE_ID;
    original.key_count = 0;

    RawPage page{};
    serialize_leaf_node(original, page);

    const auto restored = deserialize_leaf_node(page, 1);

    assert(restored.page_id == 1);
    assert(restored.key_count == 0);
    assert(restored.next_leaf_page_id == INVALID_PAGE_ID);

    std::cout << "PASS: empty leaf round trip\n";
}

// Test 2: Serialize and deserialize a populated leaf.
void test_populated_leaf_round_trip() {
    BTreeLeafNode original{};
    original.page_id = 1;
    original.next_leaf_page_id = 2;
    original.key_count = 3;

    original.entries[0] = {10, 100};
    original.entries[1] = {20, 200};
    original.entries[2] = {30, 300};

    RawPage page{};
    serialize_leaf_node(original, page);

    const auto restored = deserialize_leaf_node(page, 1);

    assert(restored.page_id == original.page_id);
    assert(restored.key_count == original.key_count);
    assert(restored.next_leaf_page_id == 2);

    for (std::uint16_t i = 0; i < original.key_count; ++i) {
        assert(restored.entries[i].key == original.entries[i].key);
        assert(restored.entries[i].value == original.entries[i].value);
    }

    std::cout << "PASS: populated leaf round trip\n";
}

// Test 3: Negative and positive keys are preserved.
void test_negative_keys() {
    BTreeLeafNode original{};
    original.page_id = 1;
    original.next_leaf_page_id = INVALID_PAGE_ID;
    original.key_count = 3;

    original.entries[0] = {-100, 1};
    original.entries[1] = {-1, 2};
    original.entries[2] = {5, 3};

    RawPage page{};
    serialize_leaf_node(original, page);

    const auto restored = deserialize_leaf_node(page, 1);

    assert(restored.entries[0].key == -100);
    assert(restored.entries[1].key == -1);
    assert(restored.entries[2].key == 5);

    std::cout << "PASS: negative keys\n";
}

// Test 4: The maximum supported number of entries round-trips
void test_leaf_capacity() {
    BTreeLeafNode original{};
    original.page_id = 1;
    original.next_leaf_page_id = INVALID_PAGE_ID;
    original.key_count = static_cast<std::uint16_t>(BTreeNodeFormat::LEAF_CAPACITY);

    for (std::uint16_t i = 0; i < original.key_count; ++i) {
        original.entries[i] = {static_cast<std::int64_t>(i), static_cast<std::uint64_t>(i) * 10};
    }

    RawPage page{};
    serialize_leaf_node(original, page);

    const auto restored = deserialize_leaf_node(page, 1);

    assert(restored.key_count == BTreeNodeFormat::LEAF_CAPACITY);

    for (std::uint16_t i = 0; i < original.key_count; ++i) {
        assert(restored.entries[i].key == original.entries[i].key);
        assert(restored.entries[i].value == original.entries[i].value);
    }

    std::cout << "PASS: leaf capacity\n";
}

// Test 5: Serialization rejects duplicate keys.
void test_duplicate_keys_rejected() {
    BTreeLeafNode node{};
    node.page_id = 1;
    node.next_leaf_page_id = INVALID_PAGE_ID;
    node.key_count = 2;

    node.entries[0] = {10, 100};
    node.entries[1] = {10, 200};

    RawPage page{};

    bool throw_exception = false;
    try {
        serialize_leaf_node(node, page);
    } catch (const std::invalid_argument &) {
        throw_exception = true;
    }
    assert(throw_exception);
    std::cout << "PASS: duplicate keys rejected\n";
}

// Test 6: Serialization rejects unsorted keys.
void test_unsorted_keys_rejected() {
    BTreeLeafNode node{};
    node.page_id = 1;
    node.next_leaf_page_id = INVALID_PAGE_ID;
    node.key_count = 2;

    node.entries[0] = {20, 200};
    node.entries[1] = {10, 100};

    RawPage page{};

    bool throw_exception = false;

    try {
        serialize_leaf_node(node, page);
    } catch (const std::invalid_argument &) {
        throw_exception = true;
    }

    assert(throw_exception);
    std::cout << "PASS: unsorted keys rejected\n";
}

// Test 7: Serialization rejects an excessive key count.
void test_excessive_key_count_rejected() {
    BTreeLeafNode node{};
    node.page_id = 1;
    node.next_leaf_page_id = INVALID_PAGE_ID;
    node.key_count = static_cast<std::uint16_t>(BTreeNodeFormat::LEAF_CAPACITY + 1);

    RawPage page{};

    bool throw_exception = false;

    try {
        serialize_leaf_node(node, page);
    } catch (const std::invalid_argument&) {
        throw_exception = true;
    }

    assert(throw_exception);
    std::cout << "PASS: excessive key count rejected\n";
}

// Test 8: Deserialization rejects a mismatched page ID.
void test_mismatched_page_id_rejected() {
    BTreeLeafNode node{};
    node.page_id = 1;
    node.next_leaf_page_id = INVALID_PAGE_ID;

    RawPage page{};
    serialize_leaf_node(node, page);

    bool throw_exception = false;

    try {
        (void)deserialize_leaf_node(page, 2);
    } catch (const std::exception &) {
        throw_exception = true;
    }

    assert(throw_exception);
    std::cout << "PASS: mismatched page ID rejected\n";
}

// Test 9: Deserialization rejects a non-leaf page.
void test_non_leaf_page_rejected() {

    RawPage page = make_data_page(1, PageType::FREE);

    bool throw_exception = false;
    try {
        (void)deserialize_leaf_node(page, 1);
    } catch (const std::invalid_argument &) {
        throw_exception = true;
    }

    assert(throw_exception);
    std::cout << "PASS: non-leaf page rejected\n";
}

// Test 10: Deserialization rejects an invalid next-leaf pointer.
void test_invalid_next_leaf_pointer_rejected() {
    BTreeLeafNode node{};
    node.page_id = 1;
    node.next_leaf_page_id = 0;

    RawPage page{};

    // Serialization itself must reject page zero as a next-leaf pointer.
    bool throw_exception = false;

    try {
        serialize_leaf_node(node, page);
    } catch (const std::invalid_argument &) {
        throw_exception = true;
    }

    assert(throw_exception);
    std::cout << "PASS: invalid next-leaf pointer rejected\n";
}

//B.Internal Node Tests:----------
// Test 1: Empty internal node round trip.
void test_empty_internal_node_round_trip() {
    BTreeInternalNode original{};
    original.page_id = 10;
    original.key_count = 0;
    original.first_child_page_id = 1;

    RawPage page{};
    serialize_internal_node(original, page);

    const auto restored = deserialize_internal_node(page, 10);

    assert(restored.page_id == 10);
    assert(restored.key_count == 0);
    assert(restored.first_child_page_id == 1);

    std::cout << "PASS: empty internal node round trip\n";
}

// Test 2: Populated internal node round trip.
void test_populated_internal_node_round_trip() {
    BTreeInternalNode original{};
    original.page_id = 10;
    original.key_count = 2;
    original.first_child_page_id = 1;

    original.entries[0] = {30, 2};
    original.entries[1] = {60, 3};

    RawPage page{};
    serialize_internal_node(original, page);

    const auto restored = deserialize_internal_node(page, 10);

    assert(restored.page_id == 10);
    assert(restored.key_count == 2);
    assert(restored.first_child_page_id == 1);

    assert(restored.entries[0].separator_key == 30);
    assert(restored.entries[0].right_child_page_id == 2);

    assert(restored.entries[1].separator_key == 60);
    assert(restored.entries[1].right_child_page_id == 3);

    std::cout << "PASS: populated internal node round trip\n";
}

// Test 3: Negative separator keys round trip.
void test_negative_separator_keys() {
    BTreeInternalNode original{};
    original.page_id = 10;
    original.key_count = 3;
    original.first_child_page_id = 1;

    original.entries[0] = {-100, 2};
    original.entries[1] = {-1, 3};
    original.entries[2] = {50, 4};

    RawPage page{};
    serialize_internal_node(original, page);

    const auto restored = deserialize_internal_node(page, 10);

    assert(restored.entries[0].separator_key == -100);
    assert(restored.entries[1].separator_key == -1);
    assert(restored.entries[2].separator_key == 50);

    std::cout << "PASS: negative separator keys\n";
}

// Test 4: Maximum internal-node capacity round trip.
void test_internal_node_capacity() {
    BTreeInternalNode original{};
    original.page_id = 10;
    original.first_child_page_id = 1;
    original.key_count = static_cast<std::uint16_t>(
        BTreeNodeFormat::INTERNAL_CAPACITY);

    for (std::uint16_t i = 0; i < original.key_count; ++i) {
        original.entries[i] = {
            static_cast<std::int64_t>(i) * 10,
            static_cast<std::uint64_t>(i) + 20
        };
    }

    RawPage page{};
    serialize_internal_node(original, page);

    const auto restored = deserialize_internal_node(page, 10);

    assert(restored.key_count == BTreeNodeFormat::INTERNAL_CAPACITY);

    for (std::uint16_t i = 0; i < original.key_count; ++i) {
        assert(restored.entries[i].separator_key == original.entries[i].separator_key);
        assert(restored.entries[i].right_child_page_id == original.entries[i].right_child_page_id);
    }

    std::cout << "PASS: internal node capacity\n";
}

// Test 5: Reject duplicate separator keys.
void test_duplicate_separator_keys_rejected() {
    BTreeInternalNode node{};
    node.page_id = 10;
    node.key_count = 2;
    node.first_child_page_id = 1;

    node.entries[0] = {30, 2};
    node.entries[1] = {30, 3};

    RawPage page{};

    bool threw = false;
    try {
        serialize_internal_node(node, page);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);
    std::cout << "PASS: duplicate separator keys rejected\n";
}

// Test 6: Reject unsorted separator keys.
void test_unsorted_separator_keys_rejected() {
    BTreeInternalNode node{};
    node.page_id = 10;
    node.key_count = 2;
    node.first_child_page_id = 1;

    node.entries[0] = {60, 2};
    node.entries[1] = {30, 3};

    RawPage page{};
    bool threw = false;

    try {
        serialize_internal_node(node, page);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);
    std::cout << "PASS: unsorted separator keys rejected\n";
}

// Test 7: Reject excessive key count.
void test_excessive_internal_key_count_rejected() {
    BTreeInternalNode node{};
    node.page_id = 10;
    node.first_child_page_id = 1;
    node.key_count = static_cast<std::uint16_t>(
        BTreeNodeFormat::INTERNAL_CAPACITY + 1
    );

    RawPage page{};
    bool threw = false;

    try {
        serialize_internal_node(node, page);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);
    std::cout << "PASS: excessive internal key count rejected\n";
}

// Test 8: Reject mismatched page ID during deserialization.
void test_internal_mismatched_page_id_rejected() {
    BTreeInternalNode node{};
    node.page_id = 10;
    node.key_count = 0;
    node.first_child_page_id = 1;

    RawPage page{};
    serialize_internal_node(node, page);

    bool threw = false;

    try {
        (void)deserialize_internal_node(page, 11);
    } catch (const std::exception &) {
        threw = true;
    }
    assert(threw);
    std::cout << "PASS: internal mismatched page ID rejected\n";
}

// Test 9: Reject a non-internal page.
void test_non_internal_page_rejected() {
    RawPage page = make_data_page(10, PageType::FREE);

    bool threw = false;

    try {
        (void)deserialize_internal_node(page, 10);
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    assert(threw);
    std::cout << "PASS: non-internal page rejected\n";
}

// Test 10: Reject invalid first-child page ID.
void test_invalid_first_child_rejected() {
    BTreeInternalNode node{};
    node.page_id = 10;
    node.key_count = 0;
    node.first_child_page_id = INVALID_PAGE_ID;

    RawPage page{};
    bool threw = false;

    try {
        serialize_internal_node(node, page);
    } catch (const std::invalid_argument &) {
        threw = true;
    }

    assert(threw);
    std::cout << "PASS: invalid first child rejected\n";
}

// Test 11: Reject a right child that points to the node itself.
void test_self_referencing_child_rejected() {
    BTreeInternalNode node{};
    node.page_id = 10;
    node.key_count = 1;
    node.first_child_page_id = 1;

    node.entries[0] = {30, 10};

    RawPage page{};
    bool threw = false;

    try {
        serialize_internal_node(node, page);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);
    std::cout << "PASS: self-referencing child rejected\n";
}

//C. Key Search Routing:-----
void test_find_child_page_id() {
    BTreeInternalNode node{};
    node.page_id = 10;
    node.key_count = 2;
    node.first_child_page_id = 1;

    node.entries[0] = {30, 2};
    node.entries[1] = {60, 3};

    assert(find_child_page_id(node, 10) == 1);
    assert(find_child_page_id(node, 29) == 1);
    assert(find_child_page_id(node, 30) == 2);
    assert(find_child_page_id(node, 45) == 2);
    assert(find_child_page_id(node, 60) == 3);
    assert(find_child_page_id(node, 100) == 3);

    std::cout << "PASS: internal-node child routing\n";
}

void test_find_child_page_id_empty_node() {
    BTreeInternalNode node{};
    node.page_id = 10;
    node.key_count = 0;
    node.first_child_page_id = 1;

    assert(find_child_page_id(node, 42) == 1);

    std::cout << "PASS: empty internal-node routing\n";
}

} // namespace

int main() {

    test_empty_leaf_round_trip();
    test_populated_leaf_round_trip();
    test_negative_keys();
    test_leaf_capacity();
    test_duplicate_keys_rejected();
    test_unsorted_keys_rejected();
    test_excessive_key_count_rejected();
    test_mismatched_page_id_rejected();
    test_non_leaf_page_rejected();
    test_invalid_next_leaf_pointer_rejected();
    std::cout << "All B+Tree leaf-node tests passed.\n";

    test_empty_internal_node_round_trip();
    test_populated_internal_node_round_trip();
    test_negative_separator_keys();
    test_internal_node_capacity();
    test_duplicate_separator_keys_rejected();
    test_unsorted_separator_keys_rejected();
    test_excessive_internal_key_count_rejected();
    test_internal_mismatched_page_id_rejected();
    test_non_internal_page_rejected();
    test_invalid_first_child_rejected();
    test_self_referencing_child_rejected();
    std::cout << "All B+Tree internal-node tests passed.\n";

    test_find_child_page_id();
    test_find_child_page_id_empty_node();

    return 0;
}