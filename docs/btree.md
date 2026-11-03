# MiniDB B+Tree — Architectural Design Document

**Storage model:** Page-oriented, persistent index  

## Overview

MiniDB's B+Tree is a persistent, page-oriented index that supports efficient key lookup, insertion, and range scans.

Internal nodes direct searches toward the appropriate child pages. Leaf nodes store sorted keys and their associated values.

The implementation will integrate with MiniDB's existing storage infrastructure:

- `DiskManager` — reads and writes fixed-size pages.
- `PageManager` — allocates pages and maintains allocation metadata.
- `BufferPoolManager` — caches pages in memory and manages their residency.

Each B+Tree index will have its own index file. Page 0 of that file will be reserved for index-specific metadata, including the root page ID and tree level. Pages 1 onward will contain internal and leaf nodes.

This follows PostgreSQL's general organization of storing B-tree metadata separately from tree pages, while keeping MiniDB's initial implementation simpler.

The first version will focus on correctness, persistence, and clear separation of responsibilities. Concurrency, deletion, and advanced recovery mechanisms will be introduced in later milestones.

## Goals

### Functional Goals

1. Search for a key and retrieve its associated value.
2. Insert a new key-value pair while maintaining B+Tree invariants.
3. Split leaf nodes when they exceed their capacity.
4. Split internal nodes when separator keys no longer fit.
5. Propagate splits toward the root when necessary.
6. Create a new root when the existing root splits.
7. Persist tree pages and root metadata across process restarts.
8. Support efficient range scans using linked leaf pages.

### Engineering Goals

- Reuse MiniDB's existing page-I/O and buffer-pool infrastructure.
- Store page references as page IDs rather than memory pointers.
- Define the on-disk representation explicitly.
- Use explicit integer serialization rather than relying on C++ struct layout.
- Keep the implementation testable through small, independent operations.
- Introduce complexity incrementally.

### Initial Scope

| Property | Initial Design |
|---|---|
| Key type | `std::int64_t` |
| Value type | `std::uint64_t`, initially representing a page ID |
| Duplicate keys | Rejected |
| Page size | 4096 bytes |
| Node capacity | Calculated from the serialized page layout |
| Execution | Single-threaded |
| Root discovery | Persistent index metapage |
| Concurrency | Deferred |
| Deletion and merging | Deferred until insertion is stable |

The initial value type is a simplification. It does not yet implement PostgreSQL's table-row addressing or full index semantics.

## Node Structure

### Common Page Representation

Every node occupies one `DiskManager::Page`, exactly 4096 bytes.

The physical representation is a byte array, not a C++ object containing vectors or pointers. Node fields are serialized into and decoded from the page using the helpers in `page_format.h`.

The existing generic page header occupies 16 bytes:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 bytes | Page ID |
| 8 | 2 bytes | Page type |
| 10 | 2 bytes | Format version |
| 12 | 4 bytes | Page flags |

The remaining 4080 bytes form the payload.

The page type distinguishes `BTREE_LEAF` from `BTREE_INTERNAL`, using the existing `PageType` enumeration.

Node-specific metadata and entries will be encoded within the payload. Their exact offsets and capacities must be defined before implementation.

### Internal Nodes

Internal nodes direct searches toward child pages.

They store:

- Sorted separator keys.
- Child page IDs.
- Node-specific metadata, including the number of separator keys.

For a node containing `n` separator keys, the initial design uses `n + 1` child references.

Conceptually:

```text
Internal Node
+--------------------------------+
| Generic page header            |
+--------------------------------+
| Node metadata                  |
+--------------------------------+
| Child 0                        |
| Separator key 0                |
| Child 1                        |
| Separator key 1                |
| ...                            |
| Child n                        |
+--------------------------------+
```

Child references are page IDs, not pointers to in-memory objects.

The search algorithm compares the target key against separator keys to determine which child to visit.

### Internal Page Layout

Each internal page uses the existing 16-byte generic data page header,
followed by an internal-specific header containing:

- Separator count: unsigned 16-bit integer.

The internal page stores:
- Sorted separator keys, each a signed 64-bit integer.
- Child page IDs, each an unsigned 64-bit integer.

For `n` separator keys, the page contains exactly `n + 1` child page IDs.

Separator keys follow the convention defined in the Design Decisions
section: each separator is the smallest key in its right child subtree.

All fields are serialized explicitly into the page's byte array.
No C++ struct is written directly to disk.

### Separator Key Convention

Each separator key is the smallest key in its right child subtree.

For a separator key `S`:
- Keys smaller than `S` belong to the left child.
- Keys greater than or equal to `S` belong to the right child.

During a leaf split, the first key in the new right leaf is copied
into the parent as the separator.

### Internal Node Capacity

Each separator key occupies 8 bytes.
Each child page ID occupies 8 bytes.

For `n` separator keys, the page stores `n + 1` child page IDs.

The generic page header occupies 16 bytes.
The internal-specific header occupies 2 bytes.

Total space required: 16 + 2 + 8n + 8(n + 1) bytes.

Maximum separator count:
floor((4096 - 26) / 16) = 254.

Therefore, an internal page can hold at most 254 separator keys
and 255 child page IDs.

The implementation must derive this capacity from the page layout
rather than hardcoding the value 254.

### Parent Page IDs

B+Tree nodes do not store parent page IDs.

During insertion, the implementation maintains a traversal path
containing the page IDs of internal nodes visited while descending
to the target leaf.

This path is used to propagate separator keys upward when nodes split.

This avoids maintaining persistent parent pointers when children move
during internal-node splits.

### Leaf Nodes

Leaf nodes contain sorted key-value entries.

Each initial entry consists of:

- An `std::int64_t` key.
- An `std::uint64_t` value.

Each entry occupies 16 bytes under the proposed fixed-width serialized representation.

Conceptually:

```text
Leaf Node
+--------------------------------+
| Generic page header            |
+--------------------------------+
| Node metadata                  |
+--------------------------------+
| Key 0 -> Value 0               |
| Key 1 -> Value 1               |
| Key 2 -> Value 2               |
| ...                            |
+--------------------------------+
| Next leaf page ID              |
+--------------------------------+
```

The next-leaf reference supports sequential range scans without repeatedly traversing the internal levels.

We will use a defined invalid-page sentinel for absent references rather than treating page 0 as an ordinary node.

**Capacity calculation:** Leaf capacity will be calculated from the 4096-byte page size, generic header, node metadata, entry sizes, and sibling-reference fields. Internal-node capacity will be calculated separately.

### Leaf Page Header

Each leaf page uses the existing 16-byte generic data page header,
followed by a leaf-specific header containing:

- Entry count: unsigned 16-bit integer.
- Next leaf page ID: unsigned 64-bit integer.

The leaf-specific header is followed by sorted key-value entries.

Each entry contains:
- Key: signed 64-bit integer.
- Value: unsigned 64-bit integer.

The next leaf page ID uses `UINT64_MAX` as the sentinel indicating
that there is no next leaf.

All fields are serialized explicitly into the page's byte array.
No C++ struct is written directly to disk.

The number of entries is determined by the serialized page layout
and must never exceed the page's physical capacity.

### Leaf Capacity

Each leaf entry occupies 16 bytes:
- Key: 8 bytes.
- Value: 8 bytes.

The generic page header occupies 16 bytes.
The leaf-specific header occupies 10 bytes.

Available space for entries: 4096 - 16 - 10 = 4070 bytes.

Maximum leaf capacity: floor(4070 / 16) = 254 entries.

The implementation must derive this capacity from the page layout
rather than hardcoding the value 254.

## Search

The search operation accepts a key and returns its associated value if present.

### Algorithm

1. Read the root page ID from the index metapage.
2. If the tree is empty, return not found.
3. Fetch the root page.
4. Inspect its page type.
5. If it is an internal node, use separator keys to choose the appropriate child page.
6. Repeat until reaching a leaf.
7. Search the leaf's sorted entries.
8. Return the associated value if the key exists; otherwise, return not found.

### Complexity

For a balanced B+Tree of height `h`, search requires `O(h)` node visits, assuming bounded or logarithmic work within each node.

With sufficient fan-out, the tree's height remains relatively small.

### Initial Tests

- Searching an empty tree.
- Searching for an existing key.
- Searching for a missing key.
- Searching keys at the lower and upper ends of a leaf.
- Searching after reopening an index file, once persistence is implemented.

## Insert

The insert operation adds a new key-value entry while preserving the tree's ordering and balance.

Duplicate keys will initially be rejected.

### Algorithm

1. Find the leaf that should contain the new key.
2. Search the leaf for a duplicate.
3. If the key already exists, reject the insertion.
4. If the leaf has enough space, insert the entry in sorted order.
5. If the leaf is full, split it.
6. Propagate the resulting separator to the parent.
7. If the parent overflows, split the internal node and continue upward.
8. If the root splits, allocate a new root and update the metapage.

### Root Changes

The root page ID must not exist solely in the B+Tree object's memory.

When the root changes, the metapage must be updated so the tree can be rediscovered after reopening the index file.

Crash consistency will be addressed separately. Updating the metapage and writing the new root are not automatically atomic.

## Delete

Deletion is outside the initial implementation scope.

Eventually, deletion will remove a key-value entry while preserving the tree's ordering and occupancy requirements.

When a node becomes underfull, the implementation may need to:

- Borrow an entry or child from a sibling.
- Update separator keys in the parent.
- Merge nodes when redistribution is not possible.
- Propagate structural changes upward.
- Shrink the tree if the root becomes unnecessary.

Deletion will be introduced after insertion, splitting, persistence, and structural validation are reliable.

## Split

Splitting handles a node that cannot accommodate another entry.

### Leaf Split

When a leaf overflows:

1. Allocate a new leaf page.
2. Redistribute entries between the original and new leaves.
3. Update their sibling links.
4. Select the separator key that divides the resulting key ranges.
5. Insert the separator and new child reference into the parent.
6. Split the parent if necessary.

The separator convention must be consistent across search, insertion, and splitting.

### Internal-Node Split

When an internal node overflows:

1. Allocate a new internal page.
2. Divide separator keys and child references between the two pages.
3. Promote the appropriate separator to the parent.
4. Update parent references if parent links are implemented.
5. Continue propagating splits when necessary.

### Root Split

If the root overflows, the tree needs a new root.

The new root references the two resulting children and contains the separator that divides their key ranges.

The metapage is then updated with the new root page ID and tree level.

### Correctness Requirements

After each split:

- Keys remain correctly ordered.
- Child ranges remain correctly routed.
- All leaf nodes remain at the same depth.
- Leaf sibling links remain consistent.
- Every allocated node has a valid page representation.
- The metapage identifies the current root.

## Merge

Merging is deferred until deletion is implemented.

When two neighboring nodes cannot maintain the required occupancy after deletion, the tree may combine them into one node.

For leaf nodes, merging also requires updating sibling links.

For internal nodes, merging requires reconciling separator keys and child references with the parent.

The merge may cause the parent to become underfull, so the process can propagate upward. If the root is reduced to a single child, the tree may shrink by one level.

Minimum occupancy, redistribution rules, and root-shrink behavior will be defined when deletion becomes an active milestone.

## Range Scan

A range scan retrieves entries whose keys fall within a specified interval.

For example, `scan(20, 50)` could return keys `20`, `30`, `40`, and `50`, assuming those keys exist and the interval is inclusive.

### Algorithm

1. Search for the leaf containing the lower bound.
2. Find the first qualifying key in that leaf.
3. Return qualifying entries in sorted order.
4. Follow the next-leaf page reference when the current leaf is exhausted.
5. Stop when the upper bound is exceeded or there are no more leaves.

### Complexity

Finding the initial leaf costs `O(log N)` for a balanced tree.

Scanning `K` qualifying entries then costs approximately `O(K)`, excluding page-I/O costs and overhead for visiting pages containing no qualifying entries.

Leaf links make range scans more efficient than restarting a root-to-leaf search for every key.

## Complexity Analysis

Let:

- `N` be the number of indexed entries.
- `h` be the tree height.
- `K` be the number of entries returned by a range scan.

| Operation | Expected Complexity | Notes |
|---|---|---|
| Search | `O(log N)` | Balanced tree; includes navigation and leaf search |
| Insert without split | `O(log N)` | Locates the leaf, then inserts the entry |
| Insert with split | `O(log N)` | May propagate splits up the tree |
| Range scan | `O(log N + K)` | Locates the starting leaf, then scans entries |
| Delete | Not implemented initially | Rebalancing may propagate toward the root |
| Merge | Not implemented initially | Depends on sibling and parent updates |

These bounds describe algorithmic work in a balanced tree. Actual performance also depends on buffer-pool hit rates, page reads and writes, entry movement, and serialization costs.

## Design Decisions

| Decision | Choice | Reason |
|---|---|---|
| Storage organization | Separate index file per B+Tree | Supports independent index metadata |
| Page size | 4096 bytes | Reuses MiniDB's existing page size |
| Metapage | Page 0 of each index file | Stores index-specific metadata |
| Tree nodes | Pages 1 onward | Separates metadata from node contents |
| Page representation | Serialized byte arrays | Avoids dependence on C++ object layout |
| Node types | Existing `BTREE_LEAF` and `BTREE_INTERNAL` page types | Reuses `page_format.h` |
| Page references | Page IDs | Remain valid across reloads |
| Key type | `std::int64_t` | Simplifies the initial implementation |
| Value type | `std::uint64_t` | Initially represents a page ID |
| Duplicate keys | Rejected | Simplifies initial index semantics |
| Node capacity | Derived from the page format | Avoids arbitrary fixed limits |
| Concurrency | Deferred | Keeps initial algorithms single-threaded |
| PageGuard | Deferred until after indexing | Avoids coupling initial tree logic to unfinished guard code |
| Deletion | Deferred | Search and insertion are the initial priorities |
| Crash consistency | Deferred beyond basic persistence | Requires a separate recovery design |

### Index File and Storage Architecture

Each B+Tree index has its own index file.

Page 0 is reserved for index metadata, including the root page ID
and the next available page ID.

Pages 1 and above are allocated for B+Tree nodes.

The existing database PageManager is not reused unchanged because
it interprets page 0 as database metadata.

An index-specific IndexPageManager is responsible for:
- Managing index metadata.
- Allocating index pages.
- Reading and writing index pages.
- Persisting index metadata.

The intended storage architecture is:

BPlusTree -> BufferPoolManager -> IndexPageManager -> DiskManager

The index metadata must persist the root page ID so that the tree
can be reopened after the process restarts.

### Index Metapage Layout

Page 0 stores the index metadata

The metadata contains:

| Field | Size | Purpose |
|---|---:|---|
| Magic number | 4 bytes | Identifies a valid MiniDB index file |
| Format version | 2 bytes | Identifies the metadata format version |
| Root page ID | 8 bytes | Identifies the root or INVALID_PAGE_ID |
| Next page ID | 8 bytes | Identifies the next page to allocate |

The metadata fields occupy 22 bytes.

All fields are serialized explicitly using little-endian encoding.
No C++ struct is written directly to disk.

The remaining bytes in the 4096-byte page are reserved.

When the index is reopened, the metadata is read to restore the
root page ID and the next available page ID.

### Index Page Allocation

Page 0 is reserved for index metadata.

The first allocatable page ID is 1.

The index metapage stores `next_page_id`, which identifies the
next page ID available for allocation.

When a page is allocated:
1. Assign the current `next_page_id` to the new page.
2. Initialize the page.
3. Increment `next_page_id`.
4. Persist the updated index metadata.

The implementation must ensure that page ID 0 is never allocated
as a tree node.

For the initial implementation, allocated page IDs are never reused.
Page reclamation and free-page management are deferred.

### Empty Tree and Root Page

`INVALID_PAGE_ID` is defined as `UINT64_MAX`.

An empty B+Tree has `root_page_id = INVALID_PAGE_ID`.

When the first key is inserted:
1. Allocate a leaf page.
2. Insert the key-value entry into the leaf.
3. Set the leaf page as the root.
4. Persist the root page ID in the index metapage.

When the root splits:
1. Allocate a new internal page.
2. Populate it with the separator key and child page IDs.
3. Update the root page ID in the index metapage.

Page ID 0 is reserved for the index metapage and is never used
as a tree node.

### Storage Integration

The intended dependency structure is:

```text
             B+Tree
                |
                v
       BufferPoolManager
                |
                v
           PageManager
                |
                v
           DiskManager
                |
                v
          Index file
```

This is the intended integration, not yet an implemented interface.

**Important unresolved detail:** The existing `PageManager` treats page 0 as database allocation metadata and tracks `next_page_id` there. An index file needs a storage context that can allocate index pages while preserving its own metapage. This conflict must be resolved before implementation. - Resolved by including node_type_specific header_extensions

## Open Questions

### 1. Index-Page Allocation

**Question:** How will MiniDB allocate pages in an index file without overwriting the metapage?

The current `PageManager` assumes page 0 stores allocation metadata. The index metapage needs page 0 for the root page ID and tree level.

**Proposed direction:** Define an index-specific storage context and decide whether the index metapage will also track the next page ID.

### 2. Empty-Tree Representation

**Question:** How will we represent a tree with no root?

**Proposed direction:** Use a dedicated invalid-page sentinel, such as `std::numeric_limits<std::uint64_t>::max()`, and define the initial tree level consistently.

### 3. Node-Specific Metadata

**Question:** Which fields must appear in every node?

Candidates include:

- Key count.
- Parent page ID.
- Next-leaf page ID for leaf nodes.

Only fields required by the initial algorithms should be included, with explicitly defined serialized offsets.

### 4. Parent References

**Question:** Should each node store its parent page ID?

Parent references can simplify upward split propagation, but they require careful updates whenever the tree structure changes.

This will be decided before implementing insertion and splitting.

### 5. Separator-Key Convention

**Question:** What exactly does a separator key mean?

We need one consistent convention for child selection and split propagation. For example, an internal separator might represent the smallest key in its right-hand child.

This will be defined before implementing search.

### 6. Minimum Node Occupancy

**Question:** What minimum occupancy rules should deletion and redistribution follow?

This can wait until deletion is implemented.

### 7. Persistence and Crash Consistency

**Question:** What guarantees should MiniDB provide if the process crashes during a split?

Basic persistence and crash consistency are different requirements. A crash during a split may leave a partially updated tree unless a recovery protocol protects the operation.

Initially, we will establish persistence across clean shutdowns and reopenings, then design crash recovery separately.

### 8. PageGuard and Concurrency

**Question:** How should page pins and latches be managed once tree operations become concurrent?

The existing `ReadPageGuard` scaffolding will remain untouched for now. We will return to its ownership, cleanup, and concurrency guarantees after the initial indexing implementation.

## Implementation Milestones

1. Define the index-specific storage and metapage format.
2. Define node metadata and calculate leaf/internal capacities.
3. Implement basic node-page encoding and decoding.
4. Implement B+Tree search.
5. Implement insertion into a leaf that has space.
6. Implement leaf splitting.
7. Implement internal-node splitting and split propagation.
8. Implement root creation and metapage updates.
9. Integrate with the buffer pool and test persistence.
10. Add range scans.
11. Implement PageGuard.
12. Add concurrency.
13. Implement deletion, redistribution, and merging.
14. Investigate crash consistency and recovery.