# MiniDB Write-Ahead Logging (WAL) — Architectural Design Document
  
**Storage model:** Append-only persistent log

## Overview

Write-Ahead Logging (WAL) is the durability mechanism responsible for recording database modifications before the corresponding modified pages are written to persistent storage.

MiniDB currently uses a page-oriented storage architecture. Database pages are read and written through the storage layer, while B+Tree operations may modify multiple pages during insertion, splitting, and root changes.

Writing modified pages directly to disk is insufficient to guarantee recovery from a process or system crash. A crash may occur while multiple related pages are being updated, leaving persistent data structures in an inconsistent state.

WAL addresses this problem by recording modifications in a sequential log before allowing the corresponding data pages to reach persistent storage.

The central WAL invariant is:

**A log record describing a page modification must be durable before the modified page is written to persistent storage.**

This is known as the Write-Ahead Logging rule.

MiniDB's WAL subsystem will provide:

- Persistent logging of page modifications.
- Monotonically increasing Log Sequence Numbers (LSNs).
- Association between log records and modified pages.
- Enforcement of write-ahead ordering.
- Recovery by replaying logged modifications.
- Checkpointing to limit recovery work.
- A foundation for future transaction management and MVCC.

The initial implementation will focus on page-level durability and redo recovery. Transaction commit, rollback, undo logging, and full transactional recovery will be introduced in later milestones.

## Goals

### Functional Goals

1. Define a persistent binary log-record format.
2. Append log records to a WAL file.
3. Assign a unique, monotonically increasing LSN to each log record.
4. Persist log records through an explicit flush operation.
5. Associate modified database pages with their corresponding LSNs.
6. Track the latest applied log record on each page.
7. Enforce WAL ordering before writing dirty pages to disk.
8. Read and scan log records during recovery.
9. Redo logged modifications when recovering from a crash.
10. Support checkpoints to reduce recovery time.
11. Detect incomplete or corrupted log records during log scanning.
12. Provide tests that simulate crashes and verify recovery correctness.

### Engineering Goals

- Keep WAL independent of B+Tree-specific algorithms.
- Use explicit binary serialization rather than writing C++ structures directly to disk.
- Use fixed-width integer fields.
- Maintain monotonically increasing LSNs.
- Separate log generation, log persistence, and log recovery.
- Make recovery deterministic and repeatable.
- Avoid unnecessary random I/O in the logging path.
- Integrate with the existing storage infrastructure.
- Keep the initial implementation single-threaded.
- Introduce concurrency and group commit incrementally.

### Initial Scope

| Property | Initial Design |
|---|---|
| Log organization | Append-only WAL file |
| Record representation | Binary serialized records |
| LSN | Monotonically increasing |
| LSN assignment | LogManager |
| Log persistence | Explicit flush |
| Recovery strategy | Redo-oriented |
| Page tracking | PageLSN |
| Execution model | Single-threaded |
| Checkpointing | Initial synchronous checkpoint |
| Transaction management | Deferred |
| Undo logging | Deferred |
| Group commit | Deferred optimization |
| Concurrent log writers | Deferred |
| Log truncation and archival | Deferred |

The first implementation is intended to establish the durability foundation for MiniDB's storage engine.

It is not intended to implement a complete transactional recovery system.

## WAL Architecture

### Component Responsibilities

The WAL subsystem will initially contain the following components.

**LogRecord**

Represents an individual modification record.

Responsibilities:

- Store record metadata.
- Store the affected page ID, when applicable.
- Store the LSN.
- Store the record type.
- Store the modification payload.
- Serialize and deserialize records.

**LogManager**

Owns the WAL file and manages log persistence.

Responsibilities:

- Open and initialize the WAL file.
- Assign LSNs.
- Append records.
- Maintain the in-memory log buffer.
- Flush buffered records.
- Read records during recovery.
- Track the durable log position.

**LogSequenceNumber**

Represents the position of a log record in the WAL stream.

Responsibilities:

- Identify log records.
- Support ordering comparisons.
- Track the relationship between page modifications and log persistence.

**RecoveryManager**

Responsible for reconstructing database pages using the WAL.

Responsibilities:

- Scan log records.
- Identify records requiring redo.
- Apply modifications to pages.
- Skip modifications already reflected on disk.
- Coordinate recovery with checkpoint metadata.

### Storage Architecture

The intended dependency structure is:

```text
             Storage Engine
                   |
                   v
              LogManager
                   |
                   v
                WAL File
```

The page-write path must also coordinate with WAL:

```text
                 B+Tree
                   |
                   v
             BufferPoolManager
                   |
             Dirty Page Write
                   |
                   v
             WAL Enforcement
                   |
          Flush WAL up to PageLSN
                   |
                   v
              DiskManager
                   |
                   v
              Database File
```

The RecoveryManager reads the WAL and applies the required modifications through the storage layer.

WAL should not depend on B+Tree node layouts. It should operate on generic page identifiers and serialized modification records.

### File Organization

The initial implementation will use a single append-only WAL file.

Conceptually:

```text
MiniDB/
|
+-- minidb.db
|
+-- minidb.wal
```

The database file stores data pages.

The WAL file stores the ordered history of logged modifications.

The WAL file will initially contain a sequential stream of records. Segment rotation, log archival, and truncation can be introduced later.

The WAL file must be recoverable after an unexpected process termination.

## Log Record Format

### Overview

Every modification recorded in the WAL is represented by a binary log record.

The log format must be:

- Explicitly serialized.
- Versioned.
- Self-describing enough to scan sequentially.
- Validatable during recovery.
- Independent of C++ object layout.

A log record contains metadata and a variable-length payload.

### Proposed Record Layout

The initial record format will use the following logical structure:

| Field | Size | Description |
|---|---:|---|
| Record length | 4 bytes | Total serialized record length |
| Record type | 2 bytes | Identifies the operation |
| Format version | 2 bytes | Log-record format version |
| LSN | 8 bytes | Log Sequence Number |
| Page ID | 8 bytes | Affected page identifier |
| Payload length | 4 bytes | Number of payload bytes |
| Payload | Variable | Operation-specific data |
| Checksum | 4 bytes | Detects record corruption |

The fixed header and trailer occupy 32 bytes, excluding the payload.

All fields will use fixed-width integer types and explicit little-endian serialization.

The exact representation of special values, such as records that do not target a particular page, will be defined before implementation.

### Record Types

The initial record-type enumeration should distinguish different kinds of log records.

| Record type | Purpose |
|---|---|
| PAGE_UPDATE | Describes a modification to a page |
| PAGE_IMAGE | Contains a complete page image |
| CHECKPOINT_BEGIN | Marks the beginning of a checkpoint |
| CHECKPOINT_END | Marks the completion of a checkpoint |
| METADATA_UPDATE | Describes a metadata modification |

The initial implementation does not need to support every record type immediately.

Transaction-related records can be introduced later:

| Future record type | Purpose |
|---|---|
| TRANSACTION_BEGIN | Starts a transaction |
| TRANSACTION_COMMIT | Records transaction commit |
| TRANSACTION_ABORT | Records transaction abort |
| COMPENSATION_LOG | Describes an undo operation |

### Record Payload

The payload contains the information required to redo a modification.

Possible representations include:

**Physical page update**

Contains the page identifier and the affected byte range or operation details.

**Full page image**

Contains the complete serialized page after modification.

**Physiological update**

Contains the page identifier and a logical modification within that page.

The initial implementation must choose a representation that supports deterministic redo.

A full-page image is simpler to replay, but can increase WAL volume. More compact page-oriented records require more sophisticated redo logic.

The choice of initial record payload format will be finalized before implementing LogRecord serialization.

### Record Serialization

Serialization must:

1. Calculate the total record length.
2. Encode the record header.
3. Encode the LSN.
4. Encode the page ID.
5. Encode the payload length.
6. Encode the payload.
7. Calculate and append the checksum.

Deserialization must:

1. Read the record length.
2. Validate that the record is large enough for its fixed fields.
3. Validate that the record fits within the available WAL data.
4. Decode the header.
5. Validate the record type and format version.
6. Validate the payload length.
7. Verify the checksum.
8. Return the reconstructed record.

Malformed or incomplete records must not be applied during recovery.

## Log Sequence Number (LSN)

### Purpose

A Log Sequence Number uniquely identifies a record's position in the WAL stream.

LSNs establish the ordering of modifications and allow database pages to identify the latest log record reflected in their contents.

Each newly appended log record receives an LSN greater than the preceding record's LSN.

### LSN Representation

The initial implementation will use a fixed-width unsigned integer representation:

```cpp
using LSN = std::uint64_t;
```

A special invalid LSN can be defined for pages that have not yet received any logged modification.

The precise sentinel representation will be defined in the implementation.

### LSN Generation

LogManager is responsible for assigning LSNs.

The following properties must hold:

- LSNs are monotonically increasing.
- Every appended record receives a unique LSN.
- LSN ordering matches log-record ordering.
- LSN assignment remains consistent across process restarts.

The implementation must define whether an LSN represents a logical record sequence or a byte offset within the WAL file.

A byte-offset-based LSN is a useful initial option because it directly identifies the location of a record in the log.

### Page LSN

Every logged database page must track the latest LSN reflected in its contents.

This value is called the PageLSN.

Conceptually:

```text
Database Page
+--------------------------------+
| Generic Page Header            |
+--------------------------------+
| Page-specific Metadata         |
+--------------------------------+
| PageLSN                        |
+--------------------------------+
| Page Data                      |
+--------------------------------+
```

The exact location of PageLSN must be defined in the page-format design.

When a modification described by log record `L` is applied to a page:

```text
PageLSN = LSN(L)
```

PageLSN provides the information required to determine whether a logged modification has already been applied.

### PageLSN and Recovery

During redo, the RecoveryManager compares the log record's LSN with the page's PageLSN.

Conceptually:

```cpp
if (page.page_lsn < record.lsn) {
    redo(record, page);
    page.page_lsn = record.lsn;
}
```

If the page already contains a PageLSN greater than or equal to the record's LSN, that modification does not need to be applied again.

This makes redo recovery repeatable, provided the logged operation and PageLSN update are applied consistently.

## Write Path

### Overview

Every page modification that participates in WAL must follow the correct ordering.

The general sequence is:

1. Read the page.
2. Construct the modification record.
3. Append the record to the WAL.
4. Apply the modification to the in-memory page.
5. Update the PageLSN.
6. Mark the page dirty.
7. Persist the page only after the corresponding WAL record is durable.

### Detailed Write Path

Conceptually:

```text
          Modification Request
                   |
                   v
              Read Page
                   |
                   v
          Construct LogRecord
                   |
                   v
          Append to LogManager
                   |
                   v
             Assign LSN
                   |
                   v
         Apply Page Modification
                   |
                   v
              Update PageLSN
                   |
                   v
             Mark Page Dirty
                   |
                   v
           Page Flush Requested
                   |
                   v
        Is WAL Durable Through LSN?
                   |
             +-----+-----+
             |           |
            No          Yes
             |           |
             v           v
         Flush WAL    Write Page
             |
             v
          Write Page
```

### Write-Ahead Rule

Before a dirty page is written to persistent storage, LogManager must guarantee that the WAL is durable through the page's PageLSN.

The invariant is:

```text
durable_lsn >= page.page_lsn
```

If this condition does not hold, the page flush must wait until the required log records have been persisted.

### Modification Atomicity

A page modification must not expose a state in which the page contains an update but its PageLSN does not describe that update.

The modification and PageLSN update must be applied as one consistent in-memory operation.

The implementation must also ensure that the WAL record contains enough information to reproduce the intended modification.

### Multi-Page Operations

Some B+Tree operations modify multiple pages.

For example, a leaf split may modify:

- The original leaf.
- The new right leaf.
- The parent internal node.
- The root or metapage, when root creation is required.

WAL must record the necessary changes to all affected pages.

The initial page-oriented redo implementation must define how recovery handles an interrupted multi-page operation.

In particular, redo logging alone does not automatically provide transaction atomicity or guarantee that a multi-page structural operation is all-or-nothing.

The recovery protocol must be designed to leave the B+Tree structurally valid after an interrupted operation.

## Flush Policy

### Overview

Log records may initially reside in an in-memory log buffer before being written to the WAL file.

LogManager must distinguish between records that have been appended and records that have become durable.

### Log Buffer

The log buffer temporarily stores serialized log records.

Its responsibilities include:

- Accumulating records.
- Reducing the number of small write operations.
- Maintaining append order.
- Providing data to flush operations.

The initial implementation can use a simple in-memory byte buffer.

A bounded log buffer and background flushing can be introduced later.

### Flush Operations

LogManager should provide an explicit flush operation.

Conceptually:

```cpp
void flush();

void flush_up_to(LSN target_lsn);
```

`flush()` persists all currently buffered records.

`flush_up_to(target_lsn)` ensures that the WAL is durable through the requested LSN.

The exact method signatures will be finalized during implementation.

### Durability Boundary

Three states must be distinguished:

| State | Meaning |
|---|---|
| Appended | Record exists in the LogManager's in-memory buffer |
| Written | Record bytes have been passed to the operating system |
| Durable | The required synchronization operation has completed successfully |

A successful append does not imply durable persistence.

The durable LSN represents the highest contiguous log position guaranteed to have been persisted.

### Synchronization

The initial implementation must use an operating-system synchronization mechanism to establish the required durability guarantee.

On POSIX systems, this will generally involve `fsync()` or an appropriately selected alternative.

The implementation must handle write failures and synchronization failures explicitly.

It must not advance the durable LSN when the required persistence operation fails.

### Flush Triggers

Initially, flushing can be triggered by:

- An explicit flush request.
- A page-flush request requiring a higher durable LSN.
- A checkpoint request.
- A future transaction commit request.

Automatic periodic flushing and background flush workers are future improvements.

## Durability Guarantees

### Page Durability

A dirty page cannot be written to persistent storage before its corresponding WAL records are durable.

This is the fundamental WAL guarantee.

### Log Durability

A log record is durable only after the required synchronization operation completes successfully.

Appending a record to an in-memory buffer is not sufficient.

### Recovery Durability

Once WAL is durable, recovery must be able to reconstruct the corresponding logged modifications, subject to the supported record format and recovery protocol.

### Transaction Durability

Transaction commit durability is not part of the initial implementation.

It will be introduced when TransactionManager is implemented.

A successful WAL append or flush alone must not be advertised as a committed transaction.

### Atomicity

WAL provides a mechanism for recovering modifications, but logging alone does not guarantee atomicity of multi-page operations or transactions.

Transaction atomicity requires additional mechanisms such as undo logging, transaction states, and commit/abort processing.

## Group Commit

### Overview

Group commit allows multiple log records or transaction commit requests to share a single WAL synchronization operation.

Instead of synchronizing the log independently for every request, LogManager can persist a batch of records together.

### Conceptual Flow

```text
Request A ----+
              |
Request B ----+----> Log Buffer
              |
Request C ----+
                    |
                    v
                WAL Flush
                    |
                    v
              Durable Records
                    |
          +---------+---------+
          |         |         |
          v         v         v
        A Done    B Done    C Done
```

### Benefits

- Reduces synchronization overhead.
- Improves write throughput.
- Amortizes expensive disk synchronization.
- Supports higher transaction commit throughput.

### Initial Scope

Group commit is not required for the initial single-threaded implementation.

The initial LogManager will provide the flush primitives required to introduce group commit later.

Concurrent requests, batching policies, and waiter coordination will be designed when MiniDB supports concurrent transactions.

## Recovery Architecture

### Overview

RecoveryManager reconstructs database state using the persistent WAL.

The initial recovery strategy is redo-oriented.

It scans the WAL and reapplies modifications that are present in the log but not yet reflected in the corresponding database pages.

### Recovery Responsibilities

RecoveryManager is responsible for:

- Reading the WAL.
- Validating log records.
- Determining the recovery starting position.
- Identifying records that require redo.
- Reading affected database pages.
- Comparing PageLSN with record LSN.
- Applying necessary modifications.
- Persisting recovered pages.

Recovery must be repeatable.

Running recovery more than once should not apply the same modification repeatedly when PageLSN indicates that it has already been applied.

### Redo Algorithm

The initial conceptual algorithm is:

1. Open the database and WAL files.
2. Locate the most recent valid checkpoint, if available.
3. Determine the starting LSN for recovery.
4. Scan WAL records in ascending LSN order.
5. Validate each record.
6. Identify records that modify database pages.
7. Read the corresponding page.
8. Compare the record LSN with the page's PageLSN.
9. If the record is newer, apply the modification.
10. Update PageLSN.
11. Mark the page dirty.
12. Persist recovered pages after the required WAL is durable.

Conceptually:

```cpp
for (const LogRecord& record : records) {
    if (!record.modifies_page()) {
        continue;
    }

    Page page = read_page(record.page_id);

    if (page.page_lsn < record.lsn) {
        redo(record, page);
        page.page_lsn = record.lsn;
        write_page(page);
    }
}
```

This is conceptual pseudocode. The actual implementation must coordinate recovery writes with the storage layer and handle invalid or incomplete records.

### Idempotent Redo

Redo must be safe to repeat.

PageLSN provides a mechanism for skipping modifications that are already reflected on disk.

For a record with LSN `L`:

```text
If PageLSN >= L:
    Skip redo.

If PageLSN < L:
    Apply redo and update PageLSN.
```

The record format must contain enough information to make this operation deterministic.

### Recovery Limitations

The initial redo-oriented recovery design does not provide:

- Transaction rollback.
- Undo of uncommitted changes.
- Full transaction atomicity.
- Multi-version transaction recovery.
- Complete ARIES recovery semantics.

These capabilities require transaction identifiers, commit/abort records, undo information, and additional recovery machinery.

They will be introduced alongside TransactionManager and MVCC.

## Checkpointing

### Purpose

As the WAL grows, recovery may require scanning an increasingly large log.

Checkpointing establishes a recovery boundary and reduces the amount of WAL that needs to be examined during subsequent recovery.

A checkpoint records a consistent recovery position.

### Initial Checkpoint Strategy

The first checkpoint implementation will use a synchronous, single-threaded approach.

Conceptually:

1. Stop accepting new modifications temporarily.
2. Ensure all preceding WAL records are durable.
3. Flush all dirty database pages covered by those records.
4. Persist checkpoint metadata.
5. Resume normal operations.

This is a deliberately simple initial strategy.

A concurrent checkpoint mechanism with a dirty page table and active transaction tracking can be introduced later.

### Checkpoint Records

The WAL may contain:

- CHECKPOINT_BEGIN
- CHECKPOINT_END

Checkpoint metadata must identify the recovery boundary.

The exact record contents and ordering will be finalized before implementation.

### Recovery from Checkpoint

During recovery:

1. Locate the latest valid completed checkpoint.
2. Determine the corresponding recovery position.
3. Begin scanning from the appropriate LSN.
4. Redo modifications after that position as required.

An incomplete checkpoint must not be treated as a completed recovery boundary.

### WAL Retention

The initial implementation will not aggressively truncate or recycle WAL files.

Log truncation requires a well-defined guarantee that all changes needed for recovery are safely represented in persistent database pages and checkpoint metadata.

Log reclamation will be introduced only after the recovery protocol is established.

## Crash Scenarios

WAL correctness must be evaluated against different crash points.

### Crash Before WAL Flush

Scenario:

1. A modification is appended to the in-memory log buffer.
2. The database page is modified in memory.
3. The process crashes before the WAL record becomes durable.

Expected behavior:

- The modified page must not have reached persistent storage.
- The modification is not guaranteed to survive.
- Recovery must not assume that the in-memory record exists.

### Crash After WAL Flush but Before Page Flush

Scenario:

1. A modification is appended.
2. The WAL is flushed successfully.
3. The modified database page remains dirty in memory.
4. The process crashes.

Expected behavior:

- The log record is available after restart.
- Recovery identifies the modification.
- Redo applies it if the page's PageLSN indicates that it is missing.

### Crash After Page Flush

Scenario:

1. The WAL is made durable.
2. The modified page is written to disk.
3. The process crashes.

Expected behavior:

- The page contains the modification.
- PageLSN reflects the corresponding record.
- Recovery skips the modification if it is already reflected on disk.

### Crash During Page Write

Scenario:

1. The WAL is durable.
2. A database page write begins.
3. The process or system crashes during the write.

Expected behavior:

- The page may be incomplete or corrupted depending on the storage guarantees.
- Recovery must detect and handle the affected page.
- WAL must contain sufficient information to reconstruct a valid page.

Torn-page protection may require full-page images, double-write buffering, or another page-write protection mechanism.

This must be considered before claiming protection against arbitrary partial page writes.

### Crash During Checkpoint

Scenario:

1. Checkpoint processing begins.
2. Some pages are flushed.
3. The process crashes before checkpoint completion.

Expected behavior:

- The incomplete checkpoint must not be treated as a valid completed checkpoint.
- Recovery must use the preceding valid recovery boundary.
- WAL required by that boundary must remain available.

## Design Decisions

| Decision | Initial choice | Reason |
|---|---|---|
| Log organization | Append-only file | Simple sequential logging |
| Record format | Explicit binary serialization | Independent of C++ layout |
| Integer representation | Fixed-width integers | Predictable disk representation |
| Byte order | Little-endian | Consistent serialized format |
| LSN | 64-bit unsigned integer | Large sequence space |
| LSN generation | LogManager | Centralized ordering |
| Page tracking | PageLSN | Supports redo decisions |
| Persistence | Explicit flush and synchronization | Clear durability boundary |
| Recovery | Redo-oriented | Establishes initial recovery mechanism |
| Checkpointing | Synchronous | Simplifies initial recovery boundary |
| Group commit | Deferred | Requires concurrent request handling |
| Undo logging | Deferred | Requires transaction machinery |
| Transaction management | Deferred | Separate implementation phase |
| WAL truncation | Deferred | Requires established checkpoint guarantees |
| Concurrent log writers | Deferred | Initial implementation is single-threaded |

## Testing Strategy

Testing must verify both individual components and end-to-end durability behavior.

### LogRecord Tests

- Serialize a valid record.
- Deserialize a valid record.
- Verify round-trip correctness.
- Verify record length.
- Verify payload length.
- Verify record type.
- Verify format version.
- Verify checksum.
- Reject truncated records.
- Reject invalid record types.
- Reject invalid payload lengths.
- Reject corrupted records.

### LogManager Tests

- Create a WAL file.
- Append one record.
- Append multiple records.
- Verify LSN assignment.
- Verify LSN monotonicity.
- Read records back in order.
- Flush buffered records.
- Flush up to a specified LSN.
- Reopen an existing WAL file.
- Verify persisted records after reopening.
- Handle incomplete trailing records.
- Handle write and flush failures.

### PageLSN Tests

- Initialize a page with an invalid PageLSN.
- Update PageLSN after a modification.
- Verify PageLSN persistence.
- Verify LSN ordering.
- Verify that recovery skips already-applied modifications.

### WAL Ordering Tests

- Verify that WAL is flushed before a dirty page is written.
- Verify that a page cannot be flushed when its PageLSN exceeds the durable LSN.
- Verify that a successful flush advances the durable LSN.
- Verify that failed WAL synchronization does not advance the durable LSN.

### Recovery Tests

- Recover a page after a logged modification.
- Recover multiple modifications to the same page.
- Skip modifications already reflected in PageLSN.
- Recover multiple pages.
- Repeat recovery and verify idempotence.
- Handle invalid or incomplete trailing records.
- Recover from a valid checkpoint.
- Ignore an incomplete checkpoint.

### Crash Simulation Tests

Simulate crashes at different points:

- Before WAL flush.
- After WAL flush.
- Before page flush.
- After page flush.
- During a multi-page B+Tree operation.
- During checkpoint processing.

Verify that the recovered database satisfies the invariants supported by the recovery protocol.

### Integration Tests

Integrate WAL with the existing storage engine and verify:

- B+Tree insertions are logged.
- Leaf splits are recoverable.
- Internal splits are recoverable.
- Root changes are recoverable.
- Range scans work after recovery.
- KVStore operations work after reopening and recovery.

## Implementation Milestones

The implementation will proceed incrementally.

1. **LogRecord**
   - Define record types.
   - Define the binary record header.
   - Implement serialization.
   - Implement deserialization.
   - Add checksum validation.
   - Add unit tests.

2. **LogSequenceNumber**
   - Define the LSN type.
   - Define invalid LSN representation.
   - Implement ordering comparisons.
   - Decide between byte-offset and logical sequence-number representation.
   - Add LSN tests.

3. **LogManager**
   - Implement WAL file creation and opening.
   - Implement append.
   - Assign LSNs.
   - Implement the log buffer.
   - Implement flush.
   - Implement flush-up-to-LSN.
   - Implement sequential log reading.
   - Add persistence tests.

4. **PageLSN Integration**
   - Define the PageLSN field in the page format.
   - Update page serialization and deserialization.
   - Track the latest applied LSN.
   - Ensure page modifications update PageLSN.
   - Add page-level tests.

5. **WAL Enforcement**
   - Integrate LogManager with the page-write path.
   - Ensure WAL is durable before dirty pages are written.
   - Track the durable LSN.
   - Enforce the WAL invariant.
   - Add ordering tests.

6. **RecoveryManager**
   - Implement sequential log scanning.
   - Validate records.
   - Identify records requiring redo.
   - Compare record LSN with PageLSN.
   - Apply modifications.
   - Persist recovered pages.
   - Add recovery and idempotence tests.

7. **Checkpointing**
   - Define checkpoint record formats.
   - Implement synchronous checkpoint processing.
   - Persist checkpoint metadata.
   - Identify the recovery starting point.
   - Add checkpoint recovery tests.

8. **B+Tree Integration**
   - Log B+Tree page modifications.
   - Log leaf splits.
   - Log internal splits.
   - Log root changes.
   - Verify recovery of multi-page operations.
   - Add end-to-end crash-recovery tests.

9. **Future Extensions**
   - Transaction begin, commit, and abort records.
   - Undo logging.
   - Transaction recovery.
   - Group commit.
   - Concurrent log writers.
   - Background log flushing.
   - WAL segmentation and rotation.
   - Log truncation and archival.
   - Full ARIES-style recovery, if required.

## Open Questions

### 1. LSN Representation

Should LSN represent:

- A logical monotonically increasing record sequence number?
- A byte offset in the WAL file?

A byte-offset-based LSN simplifies locating records, while a logical sequence number provides a more abstract identifier.

The final representation must support monotonicity, persistence, and recovery.

### 2. Log Record Payload

Should the initial WAL use:

- Full page images?
- Physical byte-range updates?
- Physiological page modifications?

The choice affects log size, redo complexity, and integration with B+Tree operations.

### 3. PageLSN Placement

Should PageLSN be part of the generic page header or stored in page-specific metadata?

A generic location simplifies WAL integration across different page types, while a page-specific location can preserve the existing generic header format.

### 4. Torn-Page Protection

How will MiniDB handle a crash during a database page write?

Possible approaches include:

- Full-page images.
- Double-write buffering.
- Atomic page-write assumptions, if the underlying storage guarantees them.

The recovery design must not assume arbitrary page writes are atomic without justification.

### 5. Multi-Page Operation Recovery

How will recovery handle an interrupted B+Tree split?

The initial protocol must guarantee that replaying logged modifications leaves the tree structurally valid, even if the process crashes between individual page writes.

### 6. Checkpoint Metadata

Should checkpoint information live:

- Only in the WAL?
- In a separate metadata file?
- In the database metapage?

The design must ensure that checkpoint metadata itself is recoverable and that WAL required by the preceding valid checkpoint is not discarded prematurely.

### 7. Log Buffer Management

Should the initial LogManager use:

- An unbounded in-memory buffer?
- A fixed-capacity buffer?
- A configurable buffer with explicit flush thresholds?

The initial implementation should favor simplicity while avoiding uncontrolled memory growth.

### 8. WAL and BufferPool Integration

Where should WAL enforcement occur?

Potential locations include:

- BufferPoolManager's dirty-page flush path.
- A dedicated storage write coordinator.
- A page-write interface that receives the page's PageLSN.

The selected design must ensure that no storage path can bypass the WAL ordering rule.

### 9. Transaction Semantics

How will WAL evolve when transactions are introduced?

Future requirements include:

- Transaction IDs.
- Commit and abort records.
- Undo information.
- Recovery of incomplete transactions.
- Commit durability.
- Interaction with MVCC.

These requirements are intentionally outside the first implementation scope.

## Conclusion

WAL establishes the durability and recovery foundation of MiniDB.

It introduces an ordered persistent history of database modifications and ensures that the log reaches durable storage before modified database pages are written.

The initial implementation will focus on:

- Binary log records.
- LSN management.
- LogManager.
- PageLSN tracking.
- WAL enforcement.
- Redo recovery.
- Synchronous checkpointing.
- Crash-recovery testing.

This work builds on the existing DiskManager, IndexPageManager, B+Tree, and KVStore implementations.

Once WAL and recovery are established, MiniDB can extend the same infrastructure toward transaction management, concurrency control, MVCC, and more advanced recovery mechanisms.

The objective is not merely to write a log file, but to establish a well-defined durability protocol that future storage-engine components can rely on.