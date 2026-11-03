
# LSM Storage Engine

**Storage model:** Log-structured, append-oriented storage

## Overview

The Log-Structured Merge Tree (LSM Tree) is a storage architecture optimized for write-intensive workloads.

Unlike B+Trees, which update existing pages in place, an LSM-based storage engine accumulates modifications in memory and periodically writes them to immutable, sorted files on disk.

Incoming writes are first recorded in the Write-Ahead Log (WAL) and applied to an in-memory data structure called the MemTable.

When the MemTable reaches its configured size, its contents are flushed to disk as an immutable Sorted String Table (SSTable).

As the number of SSTables grows, background compaction merges them into larger sorted files and removes obsolete versions of keys.

MiniDB's LSM Storage Engine will provide:

- Efficient sequential write operations.
- In-memory buffering through MemTable.
- Persistent sorted storage through SSTables.
- WAL-based durability and crash recovery.
- Efficient key lookup using sorted files.
- Bloom filters to reduce unnecessary disk reads.
- Compaction to control SSTable growth.
- A foundation for future distributed and replicated storage.

The initial implementation will focus on a single-node, persistent key-value storage engine.

Advanced features such as concurrent writes, background compaction, snapshots, MVCC integration, and distributed replication will be introduced incrementally.

---

## Goals

### Functional Goals

1. Support key-value insertion and updates.
2. Support key deletion through tombstones.
3. Maintain an in-memory MemTable.
4. Persist MemTable contents as immutable SSTables.
5. Recover unflushed writes using WAL.
6. Search across MemTable and SSTables.
7. Maintain sorted key order within SSTables.
8. Support efficient range scans.
9. Build Bloom filters for SSTable lookup.
10. Implement minor compaction.
11. Implement major compaction.
12. Remove obsolete key versions during compaction.
13. Recover the LSM database after process restarts.
14. Maintain consistent metadata describing available SSTables.

### Engineering Goals

- Separate the LSM Engine from B+Tree implementation details.
- Reuse MiniDB's DiskManager and storage infrastructure where appropriate.
- Use immutable SSTables after successful creation.
- Use sequential I/O for SSTable generation and compaction.
- Define explicit binary formats for persistent structures.
- Keep WAL and SSTable responsibilities separate.
- Make recovery deterministic.
- Support configurable MemTable and SSTable sizes.
- Introduce concurrency and background workers incrementally.

### Initial Scope

| Property | Initial Design |
|---|---|
| Storage architecture | LSM Tree |
| Data model | Key-value |
| Key representation | Byte sequence |
| Value representation | Byte sequence |
| In-memory structure | MemTable |
| MemTable organization | Skip List |
| Persistence format | SSTable |
| SSTable organization | Sorted key-value entries |
| WAL | Append-only log |
| Deletion | Tombstones |
| Bloom filter | Per-SSTable |
| Compaction | Minor and major |
| Execution model | Single-threaded |
| I/O | Synchronous |
| Concurrent writes | Deferred |
| Background compaction | Deferred |
| Compression | Deferred |
| MVCC integration | Deferred |

---

## Architecture

The intended architecture is:

```text
                  KVStore API
                       |
                       v
                  LSMEngine
                       |
          +------------+------------+
          |                         |
          v                         v
       MemTable                  SSTableReader
          |                         |
          v                         v
        SkipList                SSTableManager
          |                         |
          v                         v
       WAL Manager             SSTable Files
          |                         |
          v                         v
       WAL File                Bloom Filters
                                    |
                                    v
                               Compaction
```

A more detailed storage architecture:

```text
                       KVStore
                          |
                          v
                       LSMEngine
                          |
             +------------+------------+
             |                         |
             v                         v
          MemTable                 SSTableManager
             |                         |
             v                         v
          SkipList                 SSTable Files
             |                         |
             v                  +------+------+
          WAL Manager           |             |
             |                  v             v
             v               Bloom        Index
           WAL File           Filter     / Metadata
                                    |
                                    v
                                Compaction
```

### Component Responsibilities

**LSMEngine**

Coordinates the overall LSM storage engine.

Responsibilities:

- Accept key-value operations.
- Coordinate writes with WAL and MemTable.
- Manage MemTable rotation.
- Trigger SSTable creation.
- Coordinate reads across storage levels.
- Coordinate compaction.
- Maintain LSM metadata.

**MemTable**

Maintains the latest in-memory representation of key-value modifications.

Responsibilities:

- Insert keys.
- Update existing keys.
- Record tombstones.
- Support ordered iteration.
- Provide efficient key lookup.
- Supply sorted entries for SSTable generation.

**WAL Manager**

Provides durability for writes that have not yet been safely persisted into SSTables.

Responsibilities:

- Append write operations.
- Persist log records.
- Recover unflushed writes.
- Maintain log sequence information.
- Coordinate WAL truncation after successful flushes.

**SSTableManager**

Manages immutable SSTable files.

Responsibilities:

- Create SSTables.
- Open existing SSTables.
- Read SSTable metadata.
- Search for keys.
- Iterate over sorted entries.
- Maintain SSTable metadata.

**BloomFilter**

Provides probabilistic membership testing.

Responsibilities:

- Determine whether a key may exist in an SSTable.
- Avoid unnecessary disk reads.
- Reduce lookup overhead.

**CompactionManager**

Merges SSTables and controls storage growth.

Responsibilities:

- Select SSTables for compaction.
- Merge sorted entries.
- Resolve duplicate keys.
- Remove obsolete versions when safe.
- Remove obsolete SSTable files.
- Update LSM metadata.

---

## MemTable

### Overview

The MemTable is the in-memory component responsible for storing recent key-value modifications.

All writes are initially applied to the MemTable after being recorded in the WAL.

The MemTable maintains entries in sorted key order.

When its configured memory threshold is reached, it is converted into an immutable SSTable.

### Data Structure

The initial implementation will use a Skip List.

A Skip List maintains sorted elements using multiple levels of forward pointers.

Conceptually:

```text
Level 3:       10 -----------------> 40
               |                     |
Level 2:       10 --------> 25 -----> 40
               |            |        |
Level 1:       10 -> 15 -> 25 -> 30 -> 40
```

The Skip List provides:

- Expected O(log N) lookup.
- Expected O(log N) insertion.
- Ordered iteration.
- Efficient traversal during SSTable generation.

### Entry Structure

Each MemTable entry contains:

| Field | Type | Purpose |
|---|---|---|
| Key | Byte sequence | Identifies the record |
| Value | Byte sequence | Stores the record contents |
| Sequence number | `uint64_t` | Identifies write ordering |
| Entry type | Enum | PUT or DELETE |

Conceptually:

```cpp
enum class EntryType : std::uint8_t {
    PUT,
    DELETE
};

struct MemTableEntry {
    Key key;
    Value value;
    std::uint64_t sequence_number;
    EntryType type;
};
```

The actual implementation may use a serialized representation or an appropriate in-memory object representation.

### Insert

When a new key-value pair is inserted:

1. Assign a monotonically increasing sequence number.
2. Create a PUT entry.
3. Insert the entry into the Skip List.
4. Update MemTable memory accounting.

### Update

When an existing key is updated:

1. Assign a new sequence number.
2. Insert the new version.
3. Preserve sufficient ordering information to resolve the latest value.

The initial MemTable may retain only the latest value for a key, provided WAL recovery and snapshot requirements are handled consistently.

### Delete

Deletion is represented using a tombstone.

Instead of immediately removing a key, the MemTable inserts a DELETE entry.

Example:

```text
PUT    key=101, value=Alice
PUT    key=101, value=Bob
DELETE key=101
```

The latest operation determines the visible state.

Tombstones must be preserved during SSTable generation and compaction until it is safe to remove them.

### MemTable Rotation

A MemTable is rotated when its configured memory threshold is reached.

Conceptually:

```text
             Active MemTable
                    |
                    v
              Size Threshold
                    |
                    v
              Freeze MemTable
                    |
                    v
              Create SSTable
                    |
                    v
              New MemTable
```

The initial implementation will use synchronous MemTable flushing.

Concurrent active and immutable MemTables can be introduced later.

### MemTable Invariants

- Keys remain ordered.
- Every entry has a valid sequence number.
- The latest sequence number determines the current value.
- DELETE entries remain distinguishable from absent keys.
- A frozen MemTable is not modified.
- A MemTable must not be discarded until its contents are safely represented by persistent SSTables.

---

## WAL Integration

### Overview

The WAL provides durability for writes that have not yet been flushed into SSTables.

Without WAL, a process crash could result in the loss of all modifications currently stored only in memory.

Every write must therefore be recorded in the WAL before it is acknowledged as durable.

The LSM Engine will use WAL records representing logical key-value operations.

This extends the existing WAL design, which initially focuses on page-oriented modifications and redo recovery.

### WAL Record Types

The LSM Engine requires logical operation records.

| Record Type | Purpose |
|---|---|
| LSM_PUT | Records a key-value insertion or update |
| LSM_DELETE | Records a key deletion |
| MEMTABLE_FLUSH | Records successful MemTable flush metadata |
| SSTABLE_INSTALL | Records installation of an SSTable |
| SSTABLE_REMOVE | Records removal of obsolete SSTables |

These record types will be introduced through the WAL format extension.

### Write Ordering

The initial write path follows:

1. Construct the logical write operation.
2. Append the operation to WAL.
3. Flush WAL when required by the durability policy.
4. Apply the operation to MemTable.
5. Acknowledge the write according to the configured durability guarantee.

The WAL must be durable before a write is acknowledged as durable.

### Recovery

After a crash:

1. Open the WAL.
2. Identify the last completed MemTable flush boundary.
3. Read subsequent WAL records.
4. Replay PUT and DELETE operations.
5. Reconstruct the MemTable.
6. Restore the latest sequence number.
7. Resume normal operation.

Replay must preserve the original operation ordering.

### WAL Truncation

WAL records cannot be discarded merely because they have been written to an SSTable buffer.

The corresponding SSTable must be completely written, validated, and installed in persistent metadata before the WAL records required for recovery can be safely reclaimed.

WAL truncation must also account for checkpoint and recovery requirements.

### Durability Invariant

```text
A write acknowledged as durable
must be recoverable from either:

1. WAL
or
2. Successfully installed SSTable state.
```

The WAL and SSTable installation protocol must ensure there is no gap in which a write exists in neither recoverable representation.

---

## SSTables

### Overview

A Sorted String Table (SSTable) is an immutable, sorted file containing key-value entries.

When a MemTable reaches its configured size, its entries are written sequentially into a new SSTable.

Once created, an SSTable is never modified in place.

Updates and deletions generate newer entries in MemTables or newer SSTables.

SSTables provide the persistent storage layer of the LSM Engine.

### SSTable Organization

An SSTable contains sorted key-value entries.

Conceptually:

```text
SSTable
+----------------------------+
| Header                     |
+----------------------------+
| Data Block 1               |
|  Key 10 -> Value A         |
|  Key 20 -> Value B         |
|  Key 30 -> Value C         |
+----------------------------+
| Data Block 2               |
|  Key 40 -> Value D         |
|  Key 50 -> Value E         |
|  Key 60 -> Value F         |
+----------------------------+
| Index Block                |
+----------------------------+
| Bloom Filter               |
+----------------------------+
| Footer                     |
+----------------------------+
```

The initial implementation may store all entries in one data region.

Block-based SSTables can be introduced when range scans and large-file performance become important.

### SSTable Header

The proposed header contains:

| Field | Purpose |
|---|---|
| Magic number | Identifies a valid SSTable |
| Format version | Identifies the file format |
| SSTable ID | Unique file identifier |
| Entry count | Number of stored entries |
| Minimum key | Smallest key |
| Maximum key | Largest key |
| Minimum sequence number | Oldest sequence number |
| Maximum sequence number | Newest sequence number |

The exact binary layout will be defined before implementation.

All persistent fields must use explicit serialization.

### Data Entries

Each entry contains:

| Field | Purpose |
|---|---|
| Key length | Length of key |
| Value length | Length of value |
| Sequence number | Write ordering |
| Entry type | PUT or DELETE |
| Key | Serialized key |
| Value | Serialized value |

Variable-length keys and values allow MiniDB to support different key-value workloads.

Tombstones have no application value payload.

### Sorted Order

SSTable entries are sorted by:

1. Key in ascending order.
2. Sequence number in descending order for multiple versions of the same key.

This allows readers to identify the newest eligible entry efficiently.

The ordering convention must remain consistent across MemTable iteration, SSTable creation, lookup, and compaction.

### SSTable Metadata

Each SSTable maintains metadata describing:

- File identifier.
- File size.
- Entry count.
- Minimum and maximum key.
- Sequence number range.
- Data block locations.
- Index block location.
- Bloom filter location.

The LSM metadata manager maintains the set of installed SSTables.

### Immutability

Once an SSTable is successfully installed:

- Its contents cannot be modified.
- Its index cannot be modified.
- Its Bloom filter cannot be modified.
- New modifications must be written to a MemTable and later flushed into another SSTable.

Immutability simplifies recovery, concurrent reads, and compaction.

---

## Bloom Filters

### Overview

Bloom Filters are probabilistic data structures used to determine whether a key may exist in an SSTable.

They help avoid unnecessary disk reads during key lookup.

A Bloom Filter can produce:

- A negative result: the key definitely does not exist.
- A positive result: the key may exist.

False positives are possible.

False negatives are not permitted.

### Structure

A Bloom Filter consists of:

- A bit array.
- A configured number of hash functions.
- A configured number of expected inserted elements.

When inserting a key, multiple hash functions determine bit positions in the bit array.

All corresponding bits are set.

During lookup, the same hash functions are applied.

If any required bit is unset, the key definitely does not exist in that SSTable.

If all required bits are set, the SSTable must be checked.

### Example

```text
Bloom Filter Bit Array

Index:  0 1 2 3 4 5 6 7 8 9
Bits:   1 0 1 1 0 1 0 1 1 0

Key A:
    Hash positions = 0, 3, 7

Key B:
    Hash positions = 2, 4, 8
```

Key B would return a negative result because bit 4 is unset.

### False Positive Probability

The approximate false positive probability is:

\[
p = \left(1-e^{-kn/m}\right)^k
\]

Where:

- `m` = number of bits in the filter.
- `n` = number of inserted keys.
- `k` = number of hash functions.

The optimal number of hash functions is approximately:

\[
k = \frac{m}{n}\ln 2
\]

The initial implementation will use a configurable false positive probability.

### SSTable Integration

Every SSTable will maintain its own Bloom Filter.

During lookup:

1. Check the SSTable key range.
2. Consult the Bloom Filter.
3. If the result is negative, skip the SSTable.
4. If the result is positive, search its index and data blocks.

Bloom Filters are especially useful when the database contains many SSTables.

### Design Considerations

- Bloom Filters must be generated during SSTable creation.
- Bloom Filters must be persisted alongside SSTables.
- Bloom Filters must be validated when opening an SSTable.
- A missing or invalid Bloom Filter must not cause false negatives.
- Bloom Filters must be regenerated during compaction.

### Initial Scope

The first implementation will use a conventional Bloom Filter.

Advanced alternatives such as Ribbon Filters and cache-aware Bloom Filters are deferred.

---

## Compaction

### Overview

Compaction is the process of merging multiple SSTables into new SSTables.

As writes continue, the number of SSTables grows.

Without compaction:

- Reads must examine more files.
- Duplicate key versions accumulate.
- Tombstones remain indefinitely.
- Storage consumption increases.
- Read amplification increases.

Compaction merges sorted SSTables, resolves duplicate keys, and removes obsolete versions when safe.

### Compaction Goals

1. Reduce the number of SSTables.
2. Maintain sorted key order.
3. Resolve multiple versions of the same key.
4. Remove obsolete versions when safe.
5. Remove tombstones when no older value can become visible.
6. Maintain correct sequence ordering.
7. Reduce read amplification.
8. Rebuild SSTable indexes and Bloom Filters.
9. Maintain crash-safe SSTable installation.

### Minor Compaction

Minor compaction merges a small number of SSTables, typically from a smaller storage level into a larger level.

The initial implementation will use a simplified size-tiered strategy.

When the number or combined size of SSTables reaches a configured threshold:

1. Select SSTables for compaction.
2. Read their sorted entries.
3. Merge entries in key order.
4. Resolve duplicate keys.
5. Write a new SSTable.
6. Generate its index and Bloom Filter.
7. Install the new SSTable.
8. Remove the old SSTables after the new file is safely installed.

Conceptually:

```text
SSTable A ----+
              |
SSTable B ----+----> Minor Compaction
              |             |
SSTable C ----+             v
                       Merge Entries
                             |
                             v
                       New SSTable
```

Minor compaction limits the growth of small SSTables.

### Major Compaction

Major compaction merges a larger set of SSTables, potentially across multiple storage levels.

It performs a more comprehensive merge of existing data.

Responsibilities:

- Merge sorted key-value entries.
- Resolve duplicate keys.
- Remove obsolete versions when safe.
- Remove obsolete tombstones when safe.
- Rewrite SSTables into a more organized structure.
- Rebuild indexes and Bloom Filters.

Conceptually:

```text
Level 0:
    SSTable A
    SSTable B
    SSTable C

Level 1:
    SSTable D
    SSTable E

             |
             v

       Major Compaction

             |
             v

       New SSTables

             |
             v

Level 1:
    SSTable F
    SSTable G
```

Major compaction reduces the number of files and improves read performance.

### Duplicate Version Resolution

Consider:

```text
SSTable A:
    key=10, value=Alice, sequence=1

SSTable B:
    key=10, value=Bob, sequence=2

SSTable C:
    key=10, DELETE, sequence=3
```

The newest operation determines the current state.

The resulting logical state is:

```text
key=10 -> DELETED
```

Older versions may be discarded only when no supported snapshot or recovery operation requires them.

### Tombstone Removal

Tombstones prevent deleted values from reappearing when older SSTables are searched.

A tombstone cannot be removed simply because it is the newest entry in the SSTable currently being compacted.

It can be removed only when the compaction process guarantees that no older value can be exposed by a future lookup.

The initial implementation will use a conservative cleanup policy.

### Compaction Safety

Compaction must never make an older value visible after a newer update or deletion.

The replacement SSTables must be completely written and validated before metadata is changed to reference them.

Old SSTables must not be deleted until the replacement state is recoverable.

Crash consistency of the compaction installation protocol must be coordinated with WAL and persistent LSM metadata.

---

## Read Path

### Overview

A read operation searches the MemTable first and then examines persistent SSTables.

The newest visible version of a key determines the result.

### Lookup Algorithm

1. Search the active MemTable.
2. If the key is present, return its latest value or interpret its tombstone.
3. Search any immutable MemTables, if supported.
4. Identify candidate SSTables using their key ranges.
5. Consult Bloom Filters.
6. Skip SSTables that definitely do not contain the key.
7. Search candidate SSTables.
8. Compare sequence numbers across matching entries.
9. Select the newest eligible entry.
10. Return the value or not-found result.

### Read Architecture

```text
             Read Request
                  |
                  v
               MemTable
                  |
             Found?
              /    \
            Yes     No
             |       |
             v       v
          Return   SSTables
                     |
                     v
                Key Range Check
                     |
                     v
                Bloom Filter
                     |
                     v
                 Index Search
                     |
                     v
                  Data Block
                     |
                     v
               Compare Versions
                     |
                     v
                Return Result
```

### Read Amplification

Read amplification refers to the amount of storage work required to satisfy a read.

If a key is absent from the MemTable and may exist in multiple SSTables, several files may need to be examined.

Bloom Filters and compaction help reduce this overhead.

### Range Scan

Range scans return keys within a specified key interval.

The initial implementation will:

1. Collect matching entries from the MemTable.
2. Identify SSTables whose key ranges overlap the requested range.
3. Iterate through their sorted entries.
4. Merge the sorted streams.
5. Resolve duplicate keys using sequence numbers.
6. Ignore deleted entries.
7. Return results in sorted order.

Range scans should use sequential iteration rather than performing an independent point lookup for every key.

---

## Write Path

### Overview

Writes are first recorded in WAL and then applied to the active MemTable.

When the MemTable reaches its configured threshold, it is flushed to an immutable SSTable.

### Write Algorithm

1. Receive a PUT or DELETE request.
2. Assign a sequence number.
3. Construct the corresponding WAL record.
4. Append the record to WAL.
5. Persist WAL according to the configured durability policy.
6. Apply the operation to MemTable.
7. Update memory accounting.
8. Acknowledge the write when its durability requirements are satisfied.
9. Trigger MemTable rotation when its size threshold is reached.
10. Flush the immutable MemTable into a new SSTable.

### Write Architecture

```text
              Write Request
                    |
                    v
              Assign Sequence
                    |
                    v
                WAL Append
                    |
                    v
                WAL Flush
                    |
                    v
               MemTable
                    |
                    v
             Size Threshold
                    |
                    v
             Freeze MemTable
                    |
                    v
             Create SSTable
                    |
                    v
            Persist SSTable
                    |
                    v
             Update Metadata
                    |
                    v
             Reclaim Old WAL
```

### MemTable Flush

MemTable flushing converts an in-memory sorted structure into an immutable SSTable.

The operation must:

- Preserve sorted order.
- Preserve sequence numbers.
- Preserve tombstones.
- Generate SSTable metadata.
- Generate the Bloom Filter.
- Ensure the SSTable is complete before installation.
- Update persistent LSM metadata.
- Preserve sufficient WAL information for recovery until installation succeeds.

### Failure Handling

If SSTable creation fails:

- Do not discard the MemTable.
- Do not remove required WAL records.
- Do not install incomplete SSTable metadata.
- Preserve the ability to retry the flush.

If metadata installation fails, recovery must determine whether the SSTable was successfully installed or remains an orphaned file.

---

## Complexity Analysis

Let:

- `N` = number of entries in the MemTable.
- `K` = number of entries in an SSTable.
- `S` = number of SSTables examined.
- `R` = number of entries returned by a range scan.
- `B` = total number of entries across SSTables selected for compaction.

| Operation | Expected Complexity | Notes |
|---|---|---|
| MemTable lookup | O(log N) | Skip List |
| MemTable insertion | O(log N) | Expected Skip List complexity |
| MemTable deletion | O(log N) | Tombstone insertion |
| MemTable iteration | O(N) | Ordered traversal |
| SSTable lookup | O(log K) | Sorted index or binary search |
| Bloom Filter lookup | O(k) | k hash functions |
| Read across SSTables | Depends on S | Includes candidate file searches |
| Range scan | O(log K + R) per sorted stream | Requires merging multiple streams |
| MemTable flush | O(N) | Sequential SSTable generation |
| Minor compaction | O(B) | Merge of sorted inputs |
| Major compaction | O(B) | Merge of selected SSTables |

These bounds describe algorithmic work.

Actual performance also depends on disk I/O, cache hit rates, Bloom Filter false positives, block sizes, SSTable count, and compaction strategy.

---

## Design Decisions

| Topic | Initial Decision | Rationale |
|---|---|---|
| Storage architecture | LSM Tree | Optimized for write-intensive workloads |
| Data model | Key-value | Extends MiniDB's existing KV functionality |
| Key representation | Byte sequence | Supports variable-length keys |
| Value representation | Byte sequence | Supports variable-length values |
| MemTable | Skip List | Ordered operations and predictable implementation |
| Sequence numbers | `uint64_t` | Establishes write ordering |
| Deletion | Tombstones | Prevents older values from reappearing |
| WAL | Append-only log | Provides write durability |
| SSTable | Immutable sorted file | Simplifies persistence and recovery |
| SSTable ordering | Key ascending, sequence descending | Efficient version resolution |
| Bloom Filter | Per-SSTable | Reduces unnecessary disk reads |
| Minor compaction | Size-tiered initially | Simple first implementation |
| Major compaction | Multi-SSTable merge | Controls storage growth |
| MemTable flushing | Synchronous initially | Simplifies implementation |
| Compaction | Synchronous initially | Avoids concurrency complexity |
| Compression | Deferred | Introduces additional format complexity |
| Concurrent writes | Deferred | Establish correctness first |
| Background compaction | Deferred | Introduce after basic compaction works |
| MVCC integration | Deferred | Requires explicit snapshot visibility semantics |

### Storage Architecture Integration

The LSM Engine will reuse the existing low-level storage abstractions where appropriate.

The intended integration is:

```text
                    LSMEngine
                        |
              +---------+---------+
              |                   |
              v                   v
           MemTable          SSTableManager
              |                   |
              v                   v
          WAL Manager        SSTable Files
              |                   |
              v                   v
           WAL File          DiskManager
                                  |
                                  v
                              File System
```

The initial SSTable implementation may use direct sequential file I/O through a dedicated SSTable storage abstraction.

It does not need to route every SSTable operation through the existing page-oriented Buffer Pool.

The B+Tree and LSM Engine are separate storage organizations that can share lower-level utilities without sharing their internal data structures.

---

## Open Questions

### 1. MemTable Data Structure

**Question:** Should MiniDB use a Skip List or another ordered data structure?

Possible approaches:

- Skip List.
- Balanced search tree.
- B-Tree.
- Concurrent Skip List.

**Current Decision:**

Use a Skip List for the initial implementation.

Future consideration:

- Compare Skip List and balanced tree performance.
- Introduce concurrent Skip Lists when supporting concurrent writers.

### 2. MemTable Size

**Question:** What should the MemTable memory threshold be?

The threshold determines how frequently SSTables are generated.

**Proposed direction:**

Make the threshold configurable.

Start with a relatively small threshold for deterministic tests.

### 3. WAL Organization

**Question:** Should LSM operations use the existing WAL file or a dedicated LSM WAL?

Both approaches are possible.

**Proposed direction:**

Extend the WAL record format to support logical key-value operations.

Keep page-oriented and logical LSM record types distinguishable.

### 4. SSTable Format

**Question:** Should SSTables initially contain one contiguous data region or multiple blocks?

**Proposed direction:**

Start with a simple sorted data region and metadata.

Introduce block-based organization and block indexes as the file size grows.

### 5. SSTable Index

**Question:** How should keys be located within large SSTables?

Possible approaches:

- Binary search over the entire data region.
- Sparse block index.
- Full in-memory index.

**Proposed direction:**

Begin with binary search for the simple implementation.

Introduce a sparse block index for block-based SSTables.

### 6. Bloom Filter Configuration

**Question:** What false positive probability should MiniDB target?

**Proposed direction:**

Start with a configurable false positive probability, such as 1%.

Allow filter size and hash count to be calculated from the expected number of keys.

### 7. Compaction Strategy

**Question:** Should MiniDB use size-tiered or leveled compaction?

**Proposed direction:**

Start with size-tiered compaction.

Introduce leveled compaction later to explore the trade-offs between write amplification, read amplification, and space amplification.

### 8. Tombstone Removal

**Question:** When can a tombstone be safely removed?

Removing a tombstone too early can cause an older value to become visible again.

**Proposed direction:**

Use conservative tombstone removal during major compaction.

Require proof that no older value can remain in an uncompacted SSTable or be visible to a supported snapshot.

### 9. SSTable Metadata

**Question:** How should MiniDB track installed SSTables?

Possible approaches:

- Dedicated manifest file.
- WAL-backed metadata.
- Database metadata pages.

**Proposed direction:**

Introduce a persistent MANIFEST describing installed SSTables and storage-level metadata.

Use WAL to protect metadata transitions where required.

### 10. Crash Consistency During Flush

**Question:** What happens if the process crashes while creating an SSTable?

**Proposed direction:**

Use temporary SSTable files.

Write and validate the complete SSTable before atomically installing it through persistent metadata.

Do not discard required WAL records until installation is recoverable.

### 11. Compaction Scheduling

**Question:** When should compaction be triggered?

Possible approaches:

- SSTable count threshold.
- Total storage size threshold.
- Level-size threshold.
- Background scheduling.

**Proposed direction:**

Start with configurable SSTable count and size thresholds.

Introduce background scheduling later.

### 12. MVCC Integration

**Question:** How should LSM storage support multiple versions of a key?

The initial LSM Engine uses sequence numbers to resolve the latest write.

Full MVCC requires transaction-aware visibility and snapshot management.

**Proposed direction:**

Keep sequence numbers separate from transaction IDs.

Introduce transaction-aware versions when the MVCC subsystem is integrated.

### 13. Concurrency

**Question:** How should multiple writers access the MemTable?

Possible approaches:

- Global MemTable lock.
- Fine-grained Skip List synchronization.
- Concurrent Skip List.

**Proposed direction:**

Begin with single-threaded operations.

Introduce concurrency only after WAL, MemTable rotation, SSTable installation, and compaction are correct.

### 14. Compression

**Question:** Should SSTable data blocks be compressed?

Compression can reduce storage consumption and disk I/O but introduces additional CPU overhead.

**Proposed direction:**

Defer compression until the basic SSTable format and block organization are stable.

Future consideration:

- Snappy.
- LZ4.
- Zstandard.

### 15. Distributed Storage

**Question:** How will the LSM Engine evolve toward distributed storage?

Potential future extensions:

- Replicated WAL.
- SSTable transfer.
- Distributed compaction.
- Raft-based replication.
- Consistent partitioning.
- Distributed snapshots.

These features are outside the initial single-node LSM implementation.

---

## Conclusion

The LSM Storage Engine provides an alternative storage architecture to MiniDB's B+Tree.

It optimizes write-heavy workloads through in-memory buffering, sequential SSTable creation, and background merging.

The initial implementation will focus on:

- MemTable.
- Skip List.
- WAL integration.
- Immutable SSTables.
- SSTable metadata.
- Bloom Filters.
- Minor compaction.
- Major compaction.
- Read path.
- Write path.
- Crash recovery.
- Storage consistency.

Once these foundations are established, MiniDB can evolve toward concurrent writes, leveled compaction, MVCC integration, compression, distributed storage, and replication.

The objective is not merely to implement a Skip List and write sorted files, but to build a complete log-structured storage engine with well-defined write durability, read semantics, recovery, and compaction guarantees.
