#include "minidb/storage/btree/bplus_tree.h"
#include "minidb/storage/disk_manager.h"
#include "minidb/storage/index_page_manager.h"
#include "minidb/storage/btree/btree_node.h"
#include "minidb/storage/page_format.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>
#include <array>

namespace minidb::storage {

    BPlusTree::BPlusTree(IndexPageManager &index_page_manager) : index_page_manager(index_page_manager) {}

    /*
    search(key)
    |
    +-- Get root_page_id
    |
    +-- Is tree empty?
    |      |
    |      +-- YES: return std::nullopt
    |
    +-- Read current page
    |
    +-- Is it an internal node?
    |      |
    |      +-- YES: Find child using separator keys
    |      |         |
    |      |         +-- Move to selected child
    |      |         +-- Repeat traversal
    |      |
    |      +-- NO: Leaf node
    |                |
    |                +-- Search for key using std::lower_bound
    |                +-- Key found? Return value
    |                +-- Otherwise return std::nullopt

    search() : Starts at the root and traverses internal nodes using separator keys to find the appropriate leaf. 
    Searches that leaf for the requested key and returns its value if found, otherwise std::nullopt.
    */
    std::optional<std::uint64_t> BPlusTree::search(std::int64_t key) {
        std::lock_guard<std::mutex> lock(mtx);

        std::uint64_t root_page_id = index_page_manager.get_root_page_id();
        if (root_page_id == format::INVALID_PAGE_ID) {
            return std::nullopt;
        }

        std::uint64_t curr_page_id = root_page_id;  //iterating downwards in B+Tree Index
        while (true) {
            DiskManager::Page page = index_page_manager.read_page(curr_page_id);

            const auto page_type = static_cast<format::PageType>(format::read_le<std::uint16_t>(page.data() + format::PAGE_TYPE_OFFSET));

            if (page_type == format::PageType::BTREE_INTERNAL) {

                const BTreeInternalNode node = deserialize_internal_node(page, curr_page_id);
                curr_page_id = find_child_page_id(node, key);
            }
            else if (page_type == format::PageType::BTREE_LEAF) {

                const BTreeLeafNode node = deserialize_leaf_node(page, curr_page_id);

                const auto it = std::lower_bound(node.entries, node.entries + node.key_count, key, 
                    [&](const BTreeLeafEntry &entry, std::int64_t key) {
                    return entry.key < key;
                });

                if (it != (node.entries + node.key_count) && it->key == key) {
                    return it->value;
                }
                return std::nullopt;
            }
            else {
            throw std::runtime_error("B+Tree contains an Invalid PageType node");
            }
        }
    }

    /*
    create_initial_root(key, value)
    |
    +-- Allocate a new page
    |
    +-- Initialize BTreeLeafNode, Set page_id, Set key_count = 1
    |
    +-- Insert initial key-value pair
    |
    +-- Serialize leaf, Persist leaf page, Set root_page_id to allocated page
    
    create_initial_root() : Handles insertion into an empty B+Tree. Allocates the first leaf page, 
    inserts the initial key-value pair, persists the leaf, and sets its page ID as the root.
    */
    void BPlusTree::create_initial_root(std::int64_t key, std::uint64_t value) {

        const std::uint64_t root_page_id = index_page_manager.allocate_page();

        BTreeLeafNode leaf{};
        leaf.page_id = root_page_id;
        leaf.key_count = 1;
        leaf.entries[0] = BTreeLeafEntry{key, value};
        leaf.next_leaf_page_id = BTreeLeafNode::INVALID_NEXT_PAGE_ID;

        format::RawPage page{};
        serialize_leaf_node(leaf, page);

        index_page_manager.write_page(root_page_id, page);
        index_page_manager.set_root_page_id(root_page_id);
    }

    std::uint64_t BPlusTree::find_leaf(std::int64_t key, std::vector<BTreeInternalNode> &parent_path) {

        std::uint64_t curr_page_id = index_page_manager.get_root_page_id();

        while (true) {
            const DiskManager::Page page = index_page_manager.read_page(curr_page_id);

            const auto page_type = static_cast<format::PageType>(format::read_le<std::uint16_t>(page.data() + format::PAGE_TYPE_OFFSET));

            if (page_type == format::PageType::BTREE_LEAF) {
                return curr_page_id;
            }

            if (page_type == format::PageType::BTREE_INTERNAL) {

                const BTreeInternalNode node = deserialize_internal_node(page, curr_page_id);

                parent_path.push_back(node);

                curr_page_id = find_child_page_id(node, key);

                continue;
            }

            //else
            throw std::runtime_error("B+Tree contains invalid node");
        }
    }

/*
insert_into_leaf()
    |
    +-- Read and deserialize leaf
    |
    +-- Find insertion position (std::lower_bound)
    |
    +-- Check duplicate key
    |
    +-- Is leaf full?
          |
          +-- YES: Split leaf
          |         |
          |         +-- Allocate right leaf
          |         +-- Merge incoming key with existing entries
          |         +-- Distribute entries (127 / 128)
          |         +-- Update leaf links
          |         +-- Serialize and persist both leaves
          |         +-- Return SplitResult
          |
          +-- NO: Normal insertion
                    |
                    +-- Shift entries
                    +-- Insert new entry
                    +-- Increment key_count
                    +-- Serialize and persist leaf
                    +-- Return std::nullopt

Leaf split: the first key of the right leaf is copied up to the parent.
*/
    std::optional<BPlusTree::SplitResult> BPlusTree::insert_into_leaf(std::uint64_t leaf_page_id, std::int64_t key, std::uint64_t value) {

    const DiskManager::Page page = index_page_manager.read_page(leaf_page_id);

    BTreeLeafNode leaf = deserialize_leaf_node(page, leaf_page_id);

    auto it = std::lower_bound(leaf.entries, leaf.entries + leaf.key_count, key, 
                [&](const BTreeLeafEntry &entry, std::int64_t key){
            return entry.key < key;
    });
    if (it != (leaf.entries + leaf.key_count) && it->key == key) {  //Duplicate key already exists
            throw std::runtime_error("Duplicate B+Tree key");
    }

    const std::size_t position = static_cast<std::size_t>(it - leaf.entries);

    //Case1: leaf node requires splitting before insert
    if (leaf.key_count >= BTreeNodeFormat::LEAF_CAPACITY) {
        
        BTreeLeafNode left{}, right{};

        left.page_id = leaf.page_id;
        right.page_id = index_page_manager.allocate_page(); //newly allocated page

        //preserve the next leaf pageIDs on right
        right.next_leaf_page_id = leaf.next_leaf_page_id;
        left.next_leaf_page_id = right.page_id;

        const std::size_t TOTAL_ENTRIES = BTreeNodeFormat::LEAF_CAPACITY + 1;   //1 added for the new key
        const std::size_t left_count = TOTAL_ENTRIES / 2;
        const std::size_t right_count = TOTAL_ENTRIES - left_count;
        if (left_count + right_count != TOTAL_ENTRIES) {
            throw std::logic_error("Invaid B+Tree Leaf split distribution");
        }

        left.key_count = static_cast<std::uint16_t>(left_count);
        right.key_count = static_cast<std::uint16_t>(right_count);

        //merge and redistribute existing entries
        std::size_t old_index = 0;
        for (std::size_t i = 0; i < TOTAL_ENTRIES; i++) {

            BTreeLeafEntry entry{};
            if (i == position) {
                entry = BTreeLeafEntry{key, value};
            } else {
                entry = leaf.entries[old_index++];
            }

            if (i < left_count)
                left.entries[i] = entry;
            else
                right.entries[i - left_count] = entry;
        }

        //serialize and persist both leaves
        format::RawPage left_page{}, right_page{};

        serialize_leaf_node(left, left_page);
        serialize_leaf_node(right, right_page);

        index_page_manager.write_page(right.page_id, right_page); //right page is written first, as left pages's next_leaf_page_id = right.page_id
        index_page_manager.write_page(left.page_id, left_page);
        
        return SplitResult{right.entries[0].key, right.page_id}; //for leaf splitting branch
    } else {
        //Case2: leaf node is not-full, requires NO splitting
        //right-shift all larger keys one position
        for (std::size_t i = leaf.key_count; i > position; i--) {
            leaf.entries[i] = leaf.entries[i - 1];
        }

        leaf.entries[position] = BTreeLeafEntry{key, value};

        leaf.key_count++;

        //serialize and persist the updated leaf node before leaving
        format::RawPage updated_page{};
        serialize_leaf_node(leaf, updated_page);

        index_page_manager.write_page(leaf_page_id, updated_page);

        return std::nullopt;    //for normal branch
    }
    }

/*
insert_into_internal()
    |
    +-- Find separator insertion position (std::lower_bound)
    |
    +-- Is internal node full?
          |
          +-- YES: Split internal node
          |         |
          |         +-- Merge incoming separator and right-child pointer
          |         +-- Promote middle separator
          |         +-- Distribute keys and child pointers
          |         +-- Serialize and persist both nodes
          |         +-- Return SplitResult
          |
          +-- NO: Normal insertion
                    |
                    +-- Shift entries
                    +-- Insert separator and right-child pointer
                    +-- Increment key_count
                    +-- Serialize and persist internal node
                    +-- Return std::nullopt

Difference:
Leaf split: the first key of the right leaf is copied up to the parent.
Internal split: the middle separator is promoted and removed from the two resulting internal nodes.
*/
    //insert_into_internal() inserts into an internal node only when it has available capacity
    std::optional<BPlusTree::SplitResult> BPlusTree::insert_into_internal(BTreeInternalNode &node, const SplitResult &split) {
        if (node.key_count >= BTreeNodeFormat::INTERNAL_CAPACITY) {
            throw std::logic_error("Can not insert into a full Internal node");
        }

        const auto it = std::lower_bound(node.entries, node.entries + node.key_count, split.separator_key,
                                        [&](const BTreeInternalEntry &entry, std::int64_t key) {
                            return entry.separator_key < key;
        });
        const std::size_t position = static_cast<std::size_t>(it - node.entries);

        //shift existing entries to make room
        for (std::size_t i = node.key_count; i > position; i--) {
            node.entries[i] = node.entries[i - 1];
        }
        node.entries[position] = BTreeInternalEntry{split.separator_key, split.right_page_id};

        node.key_count++;
        return std::nullopt;
    }

    /*
    create_new_root(left_page_id, split)
    |
    +-- Allocate new root page
    |
    +-- Initialize BTreeInternalNode, Set root.page_id, Set root.key_count = 1, Set root.first_child_page_id = left_page_id
    |
    +-- Set root.entries[0]
    |       |
    |       +-- separator_key = split.separator_key
    |       +-- right_page_id = split.right_page_id
    |
    +-- Serialize new root, Persist new root page, Update IndexPageManager root metadata
    
    create_new_root(): The old root is the internal node that was previously at the top of the B+Tree. 
    If it becomes full and splits, it is divided into two nodes:
        - The old root page is reused for the left half.
        - A newly allocated page stores the right half.
    The new root is a completely new internal node created above those two nodes. It stores the promoted separator and points to both halves.
    */
    void BPlusTree::create_new_root(std::uint64_t left_page_id, const SplitResult &split) {
        //left_page_id = index_page_manager.get_root_page_id(); the old root_page_id

        //allocate a new root page
        const std::uint64_t new_root_page_id = index_page_manager.allocate_page();

        BTreeInternalNode root{};
        root.page_id = new_root_page_id;
        root.key_count = 1;

        root.first_child_page_id = left_page_id;
        root.entries[0] = BTreeInternalEntry{split.separator_key, split.right_page_id};

        //persist the new root and update metadata
        format::RawPage root_page{};
        serialize_internal_node(root, root_page);
        index_page_manager.write_page(new_root_page_id, root_page);
        index_page_manager.set_root_page_id(new_root_page_id);
    }

    /*
    split_internal_node(node, split)
    |
    +-- Calculate total keys and split positions
    |
    +-- Reconstruct existing child pointers
    |
    +-- Find incoming separator insertion position
    |
    +-- Merge existing entries with incoming separator
    |       |
    |       +-- Include incoming right-child pointer
    |
    +-- Allocate new right internal page
    |
    +-- Select middle separator for promotion
    |
    +-- Distribute keys and child pointers
    |       |
    |       +-- Left node: 127 keys, 128 children
    |       +-- Right node: 127 keys, 128 children
    |
    +-- Serialize both nodes
    |
    +-- Persist both nodes
    |
    +-- Return SplitResult
            |
            +-- Promoted separator
            +-- New right page ID

    split_internal_node() — Splits a Full Internal node after receiving a new separator and its right-child pointer. 
    Promotes the middle separator to the parent, distributes the remaining separator keys and child pointers between the left and right nodes, 
    persists both nodes, and returns the promoted separator with the new right pageID so that propagate_split() can continue upwards.
    */
    BPlusTree::SplitResult BPlusTree::split_internal_node(BTreeInternalNode &node, const SplitResult &split) {

        constexpr std::size_t TOTAL_KEYS = BTreeNodeFormat::INTERNAL_CAPACITY + 1;  //extra 1 for the to-be-added key
        constexpr std::size_t LEFT_KEY_COUNT = TOTAL_KEYS / 2;
        constexpr std::size_t RIGHT_KEY_COUNT = TOTAL_KEYS - LEFT_KEY_COUNT - 1;
        constexpr std::size_t PROMOTED_INDEX = LEFT_KEY_COUNT;

        //Temporary arrays for n merged keys and (n + 1) child pointers
        std::array<BTreeInternalEntry, TOTAL_KEYS> merged_entries{};
        std::array<std::uint64_t, TOTAL_KEYS + 1> merged_children{};

        //1. Find the incoming separator's insertion position
        const auto it = std::lower_bound(node.entries, node.entries + node.key_count, split.separator_key,
                                        [&](const BTreeInternalEntry &entry, int64_t key){
                return entry.separator_key < key;
        });
        const std::size_t position = static_cast<std::size_t>(it - node.entries);

        //2. Merge existing separators with incoming separator and keep it sorted
        for (std::size_t i = 0, old_index = 0; i < TOTAL_KEYS; i++) {
            if (i == position) {
                merged_entries[i] = BTreeInternalEntry{split.separator_key, split.right_page_id};
            } else {
                merged_entries[i] = node.entries[old_index++];
            }
        }

        //3a. Reconstruct existing (n + 1) Child pointers
        merged_children[0] = node.first_child_page_id;
        for (std::size_t i = 0; i < node.key_count; i++) {
            merged_children[i + 1] = node.entries[i].right_child_page_id;
        }
        //3b. Insert the Incoming right-child pointer at (position + 1)
        for (std::size_t i = TOTAL_KEYS; i > position + 1; i--) {
            merged_children[i] = merged_children[i - 1];
        }
        merged_children[position + 1] = split.right_page_id;

        //4. Allocate new right Internal node
        BTreeInternalNode left = node;

        BTreeInternalNode right = {};
        right.page_id = index_page_manager.allocate_page();

        //5. Identify the separator to promote
        const std::int64_t promoted_key = merged_entries[PROMOTED_INDEX].separator_key;

        //6. Configure the left node
        left.key_count = static_cast<std::uint16_t>(LEFT_KEY_COUNT);
        left.first_child_page_id = merged_children[0];
        for (std::size_t i = 0; i < LEFT_KEY_COUNT; i++) {
            left.entries[i] = BTreeInternalEntry{merged_entries[i].separator_key, merged_children[i + 1]};
        }

        //7. Configure the right node
        right.key_count = static_cast<std::uint16_t>(RIGHT_KEY_COUNT);
        right.first_child_page_id = merged_children[PROMOTED_INDEX + 1];
        for (std::size_t i = 0; i < RIGHT_KEY_COUNT; i++) {
            const std::size_t merged_index = PROMOTED_INDEX + 1 + i;
            right.entries[i] = BTreeInternalEntry{merged_entries[merged_index].separator_key, merged_children[merged_index + 1]};
        }

        //8. Serialize and persist both nodes
        format::RawPage left_page{}, right_page{};
        serialize_internal_node(left, left_page);
        serialize_internal_node(right, right_page);

        index_page_manager.write_page(right.page_id, right_page);
        index_page_manager.write_page(left.page_id, left_page); 
        //Persist the newly allocated right page before modifying the existing left page

        //9. Return Promoted separator and right pageID
        return SplitResult{promoted_key, right.page_id};
    }

    /*
    propagate_split(parent_path, split)
    |
    +-- Is parent_path empty?
    |       |
    |       +-- YES: create_new_root()
    |       |
    |       +-- NO
    |            |
    |            +-- Get immediate parent (parent_path.back()), Remove it from parent_path (pop_back())
    |            |
    |            +-- Is parent full?
    |                  |
    |                  +-- NO
    |                  |    |
    |                  |    +-- insert_into_internal()
    |                  |    +-- Persist updated parent
    |                  |    +-- Return
    |                  |
    |                  +-- YES
    |                       |
    |                       +-- split_internal_node()
    |                       +-- Receive new SplitResult
    |                       +-- Continue with next ancestor
    |
    +-- All ancestors exhausted?
            |
            +-- YES: create_new_root()

    propagate_split(): Walks parent_path bottom-up. Inserts the separator into a parent if it has space; 
    otherwise splits the parent and propagates the new separator to the next ancestor. If all ancestors are exhausted, it creates a new root.
    */
    void BPlusTree::propagate_split(std::vector<BTreeInternalNode> &parent_path, SplitResult split) {

        while (!parent_path.empty()) {

            //Get the immediate parent
            BTreeInternalNode parent = parent_path.back();
            parent_path.pop_back();

            //Case1: Parent has available capacity
            if (parent.key_count < BTreeNodeFormat::INTERNAL_CAPACITY) {

                const auto result = insert_into_internal(parent, split);
                if (result.has_value()) {   //default return type: nullopt
                    throw std::logic_error("Unexpected Split occured while inserting into a non-Full Internal node");
                }

                format::RawPage parent_page{};
                serialize_internal_node(parent, parent_page);
                index_page_manager.write_page(parent.page_id, parent_page);

                return;
            }

            //Case2: If Parent is Full; split it and propagate upwards
            split = split_internal_node(parent, split);
        }

        //All ancestors have been processed, parent_path.empty() == true 
        //The old root has split(hence not returned from Case1), let's create a new root
        create_new_root(index_page_manager.get_root_page_id(), split);
    }

/*
BPlusTree::insert()
       |
       +---- Empty tree?
       |        |
       |        +---- create_initial_root()
       |
       +---- find_leaf()
       |        |
       |        +---- Record parent path
       |
       +---- insert_into_leaf()
                |
                +---- Normal insertion
                |
                +---- Leaf split
                         |
                         +---- propagate_split()
                                  |
                                  +---- insert_into_internal()
                                  |
                                  +---- split_internal_node()
                                  |
                                  +---- create_new_root()
*/
    void BPlusTree::insert(std::int64_t key, std::uint64_t value) {
        
        std::lock_guard<std::mutex> lock(mtx);

        const std::uint64_t root_page_id = index_page_manager.get_root_page_id();
        //Case1: Empty Tree
        if (root_page_id == format::INVALID_PAGE_ID) {
            //Allocate root page, initialize an empty leaf, insert the first entry, persist the leaf, update root metadata
            create_initial_root(key, value);
            return;
        }

        //Case2: Find target leaf and record its ancestors
        //parent nodes are needed to decide where to insert the separator key of the new right node after an existing node split
        std::vector<BTreeInternalNode> parent_path;
        const std::uint64_t leaf_page_id = find_leaf(key, parent_path);

        //Case3: Insert into the target leaf
        const auto split_result = insert_into_leaf(leaf_page_id, key, value);

        //If leaf was non-full before insertion, return
        if (!split_result.has_value()) {
            return;
        }

        //Case4: Leaf was full before Insertion, so leaf split happened already inside insert_into_leaf(), now propagate result upwards
        propagate_split(parent_path, *split_result);    //since return value was Optional<SplitResult>
    }

    /*
range_scan(start, end)
       |
       v
find_leaf(start)
       |
       v
Search from start key in leaf
       |
       v
Collect matching entries
       |
       v
End of current leaf?
       |
       +---- NO ----> Continue scanning
       |
       +---- YES ---> Follow next_leaf_page_id
                            |
                            v
                     Continue until end_key
    */
    std::vector<BTreeLeafEntry> BPlusTree::range_scan(std::int64_t start_key, std::int64_t end_key) {
        std::lock_guard<std::mutex> lock(mtx);

        std::vector<BTreeLeafEntry> result;

        if (start_key > end_key) {  //Invalid or empty range
            return result;
        }

        if (index_page_manager.get_root_page_id() == format::INVALID_PAGE_ID) {   //Empty tree
            return result;
        }

        //1. Find leaf containing start_key
        std::vector<BTreeInternalNode> parent_path;
        std::uint64_t leaf_page_id = find_leaf(start_key, parent_path);

        while (leaf_page_id != BTreeLeafNode::INVALID_NEXT_PAGE_ID) {   //move across leaf-nodes rightwards

            const DiskManager::Page page = index_page_manager.read_page(leaf_page_id);
            BTreeLeafNode leaf = deserialize_leaf_node(page, leaf_page_id);

            //Inside the initial leafNode, find the first key >= start_key
            auto it = std::lower_bound(leaf.entries, leaf.entries + leaf.key_count, start_key,
                                        [&](const BTreeLeafEntry &entry, int64_t key){
                    return entry.key < key;
            });

            //Collect entries inside the same leafNode (until the end_key)
            for (   ; it != leaf.entries + leaf.key_count; it++) {
                if (it->key > end_key) {
                    return result;  //reached beyond end_key
                }
                result.push_back(*it);
            }

            leaf_page_id = leaf.next_leaf_page_id;  //jump to the next leaf node on right
        }
        return result;
    }

} //namespace