# MVCC

## Overview

Multi-Version Concurrency Control (MVCC) is a concurrency-control mechanism that allows multiple transactions to access and modify database records concurrently while maintaining consistent views of the database.

Instead of overwriting a record in place, MVCC maintains multiple versions of a logical record. Each version contains transaction metadata that determines when it was created, when it was invalidated, and whether it is visible to a particular transaction.

A transaction reads the version visible to its snapshot rather than necessarily reading the latest physical version.

MVCC reduces reader-writer contention because readers can continue accessing previously committed versions while writers create new versions.

### Core Concepts

| Concept | Description |
|---|---|
| Transaction ID (`TXID`) | Unique identifier assigned to a transaction |
| Version | A historical representation of a logical record |
| `xmin` | Transaction ID that created a version |
| `xmax` | Transaction ID that invalidated or deleted a version |
| Snapshot | A consistent view of transaction visibility |
| Visibility | Determines whether a version can be read by a transaction |
| Version Chain | Collection of historical versions belonging to a logical record |
| Transaction State | Indicates whether a transaction is active, committed, or aborted |
| Vacuum | Process that reclaims obsolete versions |

### Architecture

    +---------------------------------------------+
    |              Database Operations            |
    +---------------------------------------------+
                         |
                         v
    +---------------------------------------------+
    |              Transaction Manager            |
    |                                             |
    | Transaction IDs | Transaction State | Commit|
    +---------------------------------------------+
                         |
                         v
    +---------------------------------------------+
    |                 MVCC Manager                |
    |                                             |
    | +---------------+  +----------------------+ |
    | | Snapshot      |  | Visibility           | |
    | | Manager       |  | Manager              | |
    | +---------------+  +----------------------+ |
    |                                             |
    | +---------------+  +----------------------+ |
    | | Version       |  | Version Cleanup      | |
    | | Manager       |  | / Vacuum             | |
    | +---------------+  +----------------------+ |
    +---------------------------------------------+
                         |
                         v
    +---------------------------------------------+
    |                Storage Engine               |
    |                                             |
    |       +----------------+----------------+   |
    |       |                |                |   |
    |       v                v                v   |
    |     B+Tree           LSM Tree         Heap  |
    +---------------------------------------------+
                         |
                         v
    +---------------------------------------------+
    |                WAL / Recovery               |
    +---------------------------------------------+

### Architectural Responsibilities

**Transaction Manager**
- Allocates transaction IDs.
- Maintains transaction lifecycle and state.
- Coordinates transaction commit and abort.
- Provides transaction information to the MVCC layer.

**Snapshot Manager**
- Creates transaction snapshots.
- Tracks active transactions.
- Maintains the oldest active snapshot.
- Prevents premature reclamation of historical versions.

**Visibility Manager**
- Determines whether a version is visible to a transaction.
- Evaluates `xmin` and `xmax`.
- Consults transaction states and snapshot information.

**Version Manager**
- Creates record versions.
- Maintains version chains.
- Locates previous versions.
- Coordinates version updates and deletion.

**Vacuum Manager**
- Identifies obsolete versions.
- Removes versions that are no longer visible to any active snapshot.
- Reclaims physical storage safely.

**Storage Engine**
- Persists record versions.
- Maintains indexes and version references.
- Supports version retrieval and physical reclamation.

**WAL and Recovery**
- Persists transactional modifications.
- Supports commit and abort recovery.
- Restores consistent version metadata after crashes.

---

## Goals

1. Support concurrent readers and writers without unnecessary blocking.
2. Maintain multiple versions of a logical record.
3. Provide consistent snapshots for transaction reads.
4. Implement transaction-aware version visibility.
5. Support insert, update, and delete operations.
6. Implement Read Committed and Repeatable Read.
7. Provide Snapshot Isolation with write-write conflict detection.
8. Integrate with WAL and crash recovery.
9. Reclaim obsolete versions safely.
10. Support both B+Tree-based and LSM-based storage.
11. Separate logical transaction semantics from physical storage.
12. Provide a foundation for Serializable isolation and distributed transactions.

### Non-Goals for Initial Implementation

- Serializable Snapshot Isolation (SSI).
- Predicate locking.
- Distributed transactions.
- Multi-node snapshot coordination.
- Cross-shard transaction management.
- Time-travel queries.
- Distributed timestamp management.
- Multi-version replication.

---

## Version Chains

MVCC maintains multiple versions of a logical record instead of overwriting its existing contents.

Each update creates a new version. The previous version remains available until it is safe to reclaim.

A version chain connects the different versions of the same logical record.

### Version Chain Structure

Consider a record with key `100` that has been updated twice.

    Logical Key: 100

              Newest Version
                     |
                     v
              +--------------+
              | Version V3   |
              | xmin = 300   |
              | xmax = 0     |
              +--------------+
                     |
                     v
              +--------------+
              | Version V2   |
              | xmin = 200   |
              | xmax = 300   |
              +--------------+
                     |
                     v
              +--------------+
              | Version V1   |
              | xmin = 100   |
              | xmax = 200   |
              +--------------+
                     |
                     v
                   NULL

In this example:

- Transaction 100 created V1.
- Transaction 200 updated the record and created V2.
- Transaction 300 updated the record and created V3.
- V1 and V2 remain available to transactions whose snapshots can still see them.

### Version Metadata

Conceptual representation:

    using TransactionId = uint64_t;
    using VersionId = uint64_t;

    struct VersionMetadata {
        TransactionId xmin;
        TransactionId xmax;
        VersionId previous_version;
    };

    struct RecordVersion {
        VersionMetadata metadata;
        std::vector<std::byte> value;
    };

The actual implementation may use page IDs, slot IDs, tuple identifiers, or other stable references instead of direct pointers.

On-disk metadata must use explicit serialization rather than writing raw C++ structures.

`xmax = 0` represents an unset invalidating transaction in this conceptual design.

### Insert

When a transaction inserts a new record:

1. Allocate a new version.
2. Set `xmin` to the inserting transaction ID.
3. Set `xmax` to the unset transaction ID.
4. Set the previous-version reference to null.
5. Insert the record into the appropriate index.
6. Record the modification in WAL.

Example:

    INSERT key=100, value=A

           +--------------+
           | Version V1   |
           | xmin = 100   |
           | xmax = 0     |
           +--------------+

The inserted version is visible to the inserting transaction but remains invisible to other transactions until the inserting transaction commits.

### Update

An update creates a new version instead of overwriting the existing version.

The operation involves:

1. Locate the current visible version.
2. Check for conflicting concurrent updates.
3. Mark the previous version with `xmax = current_transaction_id`.
4. Allocate a new version.
5. Set the new version's `xmin` to the current transaction ID.
6. Set the new version's `xmax` to the unset value.
7. Link the new version to the previous version.
8. Update the index or version-chain head.
9. Record the modifications in WAL.

Example:

    Before UPDATE:

           +--------------+
           | Version V1   |
           | xmin = 100   |
           | xmax = 0     |
           +--------------+

    After UPDATE by transaction 200:

           +--------------+
           | Version V2   |
           | xmin = 200   |
           | xmax = 0     |
           +--------------+
                     |
                     v
           +--------------+
           | Version V1   |
           | xmin = 100   |
           | xmax = 200   |
           +--------------+

The old version is not immediately removed because an active transaction may still need it.

### Delete

A delete is represented as a logical deletion rather than immediately removing the record.

The operation involves:

1. Locate the current visible version.
2. Check for conflicting concurrent updates.
3. Set its `xmax` to the deleting transaction ID.
4. Preserve the previous version until cleanup determines it is no longer visible to any active snapshot.
5. Record the modification in WAL.

Example:

    Before DELETE:

           +--------------+
           | Version V1   |
           | xmin = 100   |
           | xmax = 0     |
           +--------------+

    After DELETE by transaction 200:

           +--------------+
           | Version V1   |
           | xmin = 100   |
           | xmax = 200   |
           +--------------+

The record becomes logically deleted only when the deleting transaction commits.

If the deleting transaction aborts, the previous version remains valid.

### Aborted Transactions

An aborted transaction must not expose any of its modifications to other transactions.

Example:

    T1: BEGIN
        INSERT key=100, value=A

    T1: ABORT

    T2: BEGIN
        READ key=100

        Result: NOT FOUND

The version created by T1 must be treated as invisible.

Physical cleanup can happen later.

### Multiple Updates Within One Transaction

A transaction may update the same record multiple times.

Example:

    T1: BEGIN

        INSERT key=100, value=A
        UPDATE key=100, value=B
        UPDATE key=100, value=C

    T1: COMMIT

The transaction must observe its own latest modification.

The implementation must define how intermediate versions created by the same transaction are represented and whether unnecessary intermediate versions can be consolidated safely.

### Version Chain Invariants

- A logical record must have a well-defined version-chain head.
- Every version must retain sufficient metadata for visibility checks.
- Aborted versions must never become visible to other transactions.
- Updates must preserve older versions until cleanup is safe.
- A deleted record must not reappear because an older version was incorrectly selected.
- Version references must remain valid while active operations can access them.
- A version-chain update must be atomic or recoverable.

---

## Visibility Rules

Visibility rules determine whether a version can be read by a particular transaction.

A version is not visible merely because its `xmin` is numerically smaller than the reader's transaction ID.

Visibility depends on:

- The creating transaction's state.
- The invalidating transaction's state.
- The reader's snapshot.
- Whether the version belongs to the current transaction.

### Transaction States

    enum class TransactionState {
        ACTIVE,
        COMMITTED,
        ABORTED
    };

The Transaction Manager maintains transaction states and provides the information required by the Visibility Manager.

A transaction state lookup should be efficient because visibility checks are performed frequently during reads.

### Snapshot Representation

A snapshot captures the transaction state relevant to a reader.

Conceptual representation:

    struct Snapshot {
        TransactionId current_transaction_id;

        // Oldest transaction ID active at snapshot creation.
        TransactionId xmin;

        // Upper transaction boundary at snapshot creation.
        TransactionId xmax;

        // Transactions active when the snapshot was created.
        std::vector<TransactionId> active_transactions;
    };

The exact interpretation of `xmin` and `xmax` must be defined consistently by the Transaction Manager.

For the initial implementation:

- `xmin` represents the oldest transaction still active when the snapshot is created.
- `xmax` represents the transaction ID allocation boundary at snapshot creation.
- Transactions in `active_transactions` were active when the snapshot was created.

Transaction IDs and snapshot comparisons must eventually account for wraparound.

### Creating Transaction Visibility

The creating transaction (`xmin`) determines whether a version exists in the reader's snapshot.

| Creating Transaction State | Visibility |
|---|---|
| Current transaction | Potentially visible |
| Aborted | Invisible |
| Active in snapshot | Invisible |
| Committed before snapshot | Potentially visible |
| Committed after snapshot | Invisible |

### Invalidating Transaction Visibility

The invalidating transaction (`xmax`) determines whether the version has been superseded or deleted in the reader's snapshot.

| Invalidating Transaction State | Visibility of Previous Version |
|---|---|
| Unset | Potentially visible |
| Current transaction | Invisible to subsequent reads by that transaction |
| Aborted | Remains potentially visible |
| Active in snapshot | Remains visible |
| Committed after snapshot | Remains visible |
| Committed before snapshot | Invisible |

The actual visibility decision must account for the current transaction's own writes and the snapshot's transaction membership.

### Visibility Algorithm

Conceptually:

    IsVisible(version, snapshot):

        1. Check the creating transaction (xmin).

           - If xmin belongs to the current transaction:
                 Version may be visible.

           - If xmin was aborted:
                 Version is invisible.

           - If xmin was active in the snapshot:
                 Version is invisible.

           - If xmin is outside the snapshot's visible
             committed transaction range:
                 Version is invisible.

           - Otherwise:
                 Version passes the creation check.

        2. Check the invalidating transaction (xmax).

           - If xmax is unset:
                 Version remains potentially visible.

           - If xmax belongs to the current transaction:
                 Version is invisible.

           - If xmax was aborted:
                 Version remains potentially visible.

           - If xmax was active in the snapshot:
                 Version remains visible.

           - If xmax committed after the snapshot:
                 Version remains visible.

           - If xmax committed before the snapshot:
                 Version is invisible.

        3. Apply deletion semantics.

        4. Return the visibility result.

This is conceptual pseudocode. The actual implementation should use explicit snapshot-membership checks and transaction-state lookups rather than relying only on numeric comparisons.

### Important Visibility Invariants

1. Uncommitted changes from another transaction must never become visible.
2. An aborted insert must remain invisible.
3. An aborted update must not hide the previous committed version.
4. An uncommitted update must not hide the previous committed version from other transactions.
5. A committed deletion must not expose an older version.
6. A transaction must see its own writes according to its transaction semantics.
7. A version must not be reclaimed while an active snapshot can still see it.

### Transaction ID Management

Transaction IDs must be allocated monotonically within the supported identifier range.

The implementation must eventually address:

- Transaction ID wraparound.
- Transaction state retention.
- Transaction ID reuse, if supported.
- Snapshot boundary comparisons.
- Recovery of transaction state after restart.

Numeric comparisons alone are insufficient if transaction ID wraparound is possible.

---

## Snapshot Isolation

Snapshot Isolation provides each transaction with a consistent snapshot of the database.

A transaction sees:

- Changes committed before its snapshot was established.
- Its own modifications.
- No changes committed by other transactions after its snapshot was established.

The snapshot remains stable throughout the transaction.

### Example

    Time ---------------------------------------------------->

    T1: BEGIN
        Snapshot S1 created
        READ key=100 -> A

                     T2: BEGIN
                         UPDATE key=100 -> B
                         COMMIT

        READ key=100 -> A
        COMMIT

Even though T2 commits before T1's second read, T1 continues to see A because its snapshot remains unchanged.

### Write-Write Conflicts

Snapshot Isolation must detect conflicting concurrent updates.

Consider:

    T1: BEGIN
        Snapshot S1 created
        READ key=100 -> A

                     T2: BEGIN
                         READ key=100 -> A
                         UPDATE key=100 -> B
                         COMMIT

    T1:
        UPDATE key=100 -> C
        COMMIT

T1 must detect that another transaction committed a conflicting update after T1's snapshot was established.

T1 must abort rather than silently overwrite T2's committed change.

### First-Committer-Wins

A common Snapshot Isolation strategy is first-committer-wins.

When two transactions attempt to modify the same logical record concurrently:

1. Both transactions may initially read the same committed version.
2. One transaction commits its update.
3. The other transaction detects that the version it intended to modify has been superseded.
4. The conflicting transaction aborts.

This prevents lost updates.

### Write Skew

Snapshot Isolation does not guarantee full serializability.

Consider two doctors who must ensure that at least one doctor remains on call.

    Initial State:
        Doctor A: ON_CALL
        Doctor B: ON_CALL

    T1:
        READ A = ON_CALL
        READ B = ON_CALL
        UPDATE A = OFF_CALL

    T2:
        READ A = ON_CALL
        READ B = ON_CALL
        UPDATE B = OFF_CALL

    T1: COMMIT
    T2: COMMIT

Both transactions modify different records, so simple write-write conflict detection does not detect the conflict.

The resulting state violates the business invariant.

This is write skew.

Serializable isolation requires additional mechanisms such as predicate locking or Serializable Snapshot Isolation.

### Characteristics

- Consistent snapshot for each transaction.
- Readers can access older versions while writers create newer versions.
- Non-blocking reads in the common case.
- Write-write conflict detection is required.
- Write skew is possible.
- Full serializability is not guaranteed.

---

## Read Committed

Read Committed guarantees that each statement sees committed data.

A new snapshot is created for each statement rather than once for the entire transaction.

### Example

    Time ---------------------------------------------------->

    T1: BEGIN
        SELECT key=100 -> A

                     T2: BEGIN
                         UPDATE key=100 -> B
                         COMMIT

        SELECT key=100 -> B
        COMMIT

The second statement in T1 sees B because T2 committed before the second statement's snapshot was established.

### Characteristics

- Each statement receives a fresh snapshot.
- Uncommitted changes from other transactions are invisible.
- Repeated reads may return different values.
- Non-repeatable reads are possible.
- Write-write conflict detection is still required.

### Snapshot Lifecycle

    BEGIN
      |
      v
    Statement 1
      |
      +--> Create Snapshot S1
      |
      +--> Execute statement
      |
      v
    Statement 2
      |
      +--> Create Snapshot S2
      |
      +--> Execute statement
      |
      v
    COMMIT

The transaction context remains active across statements, but statement snapshots are refreshed.

### Non-Repeatable Read

    T1: BEGIN
        SELECT balance FROM accounts WHERE id=100;
        Result: 500

                     T2: BEGIN
                         UPDATE accounts
                         SET balance=700
                         WHERE id=100;
                         COMMIT

    T1:
        SELECT balance FROM accounts WHERE id=100;
        Result: 700

    T1: COMMIT

The same query returns a different value because the second statement receives a new snapshot.

### Implementation Requirements

- Create a snapshot at the beginning of each statement.
- Release the statement snapshot when the statement completes.
- Retain the transaction context until commit or abort.
- Ensure each statement sees a consistent committed state.
- Handle concurrent updates using appropriate conflict detection.

---

## Repeatable Read

Repeatable Read guarantees that repeated reads within a transaction observe a consistent snapshot.

The snapshot is established when the transaction's read view is created and remains unchanged for subsequent statements.

### Example

    Time ---------------------------------------------------->

    T1: BEGIN
        Snapshot S1 created
        SELECT key=100 -> A

                     T2: BEGIN
                         UPDATE key=100 -> B
                         COMMIT

        SELECT key=100 -> A
        COMMIT

T1 continues to observe A even after T2 commits B.

### Characteristics

- A stable snapshot across statements.
- Repeated reads return the same visible version unless modified by the current transaction.
- Non-repeatable reads are prevented.
- Write-write conflicts must be detected.
- Write skew may still occur under Snapshot Isolation.

### Snapshot Lifecycle

    BEGIN
      |
      v
    Create Transaction Snapshot S1
      |
      v
    Statement 1
      |
      v
    Statement 2
      |
      v
    Statement 3
      |
      v
    COMMIT
      |
      v
    Release Snapshot S1

### Implementation Requirements

- Create the snapshot at the appropriate transaction read-view boundary.
- Reuse the snapshot for subsequent statements.
- Allow the transaction to observe its own writes.
- Retain versions required by the snapshot.
- Release the snapshot after commit or abort.

### Isolation Level Comparison

| Property | Read Committed | Repeatable Read / Snapshot Isolation |
|---|---|---|
| Snapshot lifetime | Per statement | Per transaction |
| Sees committed data | At statement snapshot creation | At transaction snapshot creation |
| Non-repeatable reads | Possible | Prevented |
| Write-write conflicts | Must be handled | Must be handled |
| Write skew | Possible | Possible |
| Full serializability | No | No |

The exact guarantees of Repeatable Read depend on the database's isolation-level definition. MiniDB's initial design uses snapshot-based Repeatable Read.

---

## Version Cleanup

MVCC creates obsolete versions as records are updated and deleted.

These versions cannot be removed immediately because active transactions may still need to read them.

Version cleanup reclaims storage occupied by versions that are no longer visible to any active snapshot.

### Why Cleanup Is Necessary

Without cleanup:

- Version chains grow indefinitely.
- Storage consumption increases.
- Read operations may traverse increasingly long chains.
- Buffer-pool pressure increases.
- Index and page maintenance become more expensive.
- LSM compaction must process unnecessary historical versions.

### Cleanup Requirements

A version is eligible for cleanup only when:

1. Its creating transaction has committed or aborted.
2. Its invalidating transaction has committed, if applicable.
3. No active snapshot can see the version.
4. No active transaction requires the version for visibility.
5. Removing it will not expose an older version that should remain hidden.
6. No concurrent reader or storage operation still references it.

### Oldest Active Snapshot

The Transaction Manager tracks the oldest active snapshot.

Conceptually:

    Transaction Timeline:

    T1: BEGIN -------------------------------- COMMIT
           |
           | Snapshot S1
           |
    T2:       BEGIN -------- COMMIT
                        |
    T3:             BEGIN -------- COMMIT

Even after T2 and T3 commit, versions required by T1's snapshot may still need to be retained.

The oldest active snapshot acts as an important boundary for determining which obsolete versions can be reclaimed.

Long-running transactions can prevent cleanup and cause storage growth.

### Cleanup Process

                  Version Cleanup
                         |
                         v
              Find Oldest Active Snapshot
                         |
                         v
                 Scan Version Chains
                         |
                         v
               Identify Obsolete Versions
                         |
                         v
                Check Visibility Safety
                         |
                         v
                 Remove Safe Versions
                         |
                         v
                  Reclaim Storage

### Cleanup Strategies

#### Background Vacuum

A background process periodically scans storage and removes obsolete versions.

Responsibilities:

- Identify obsolete versions.
- Remove versions no longer required by active snapshots.
- Compact pages where possible.
- Reclaim unused storage.
- Maintain statistics about cleanup progress.

#### Opportunistic Cleanup

Cleanup is performed during updates, deletes, or page modifications when it is safe.

This can reduce the amount of work required by background vacuum.

#### Page-Level Reclamation

When obsolete versions are removed from a page:

- Reorganize the page's free space.
- Update slot metadata.
- Remove invalid version references.
- Return completely unused pages to the free-page manager when safe.

### Storage Engine Considerations

#### B+Tree Storage

For B+Tree storage:

- Leaf entries may reference the newest version.
- Version chains may be stored within data pages or separately.
- Cleanup must coordinate with page-level locking and buffer-pool pinning.
- Reclaimed space must not invalidate references held by active operations.
- Index entries must not point to reclaimed versions.

#### LSM Storage

For LSM storage:

- Versions may exist in multiple SSTables.
- Compaction must preserve versions required by active snapshots.
- Obsolete versions can be removed when they are no longer needed.
- Tombstones must not be dropped while older values can still exist in uncompacted SSTables.
- Version cleanup must account for the snapshot visibility boundary.

### Vacuum and Concurrency

Vacuum must coordinate with concurrent readers and writers.

A safe cleanup operation must ensure:

1. The candidate version is no longer visible to any active snapshot.
2. No active reader is traversing or referencing the version.
3. The version-chain head and neighboring references are updated safely.
4. The removal is recoverable if a crash occurs during cleanup.
5. Reclaimed storage cannot be reused while stale references remain.

### Important Invariant

Version cleanup must never cause a transaction to observe an older version that should have been hidden by a committed update or deletion.

---

## Design Decisions

| Area | Initial Decision |
|---|---|
| Concurrency model | MVCC-based version visibility |
| Transaction metadata | `xmin` and `xmax` |
| Transaction identifiers | Monotonically allocated 64-bit transaction IDs |
| Transaction states | ACTIVE, COMMITTED, ABORTED |
| Version representation | Linked version chains |
| Snapshot representation | Snapshot boundaries and active transaction set |
| Read Committed | New snapshot per statement |
| Repeatable Read | Stable snapshot across transaction |
| Snapshot Isolation | Stable snapshot with write-write conflict detection |
| Conflict handling | First-committer-wins |
| Deletes | Logical deletion using transaction metadata |
| Version cleanup | Background vacuum |
| Storage integration | Keep visibility logic separate from physical storage |
| Initial Serializable support | Deferred |
| Distributed transactions | Deferred |

### Architectural Boundaries

MVCC should not be tightly coupled to either B+Tree or LSM storage.

    +------------------------------------------------+
    |              SQL / KV Operations               |
    +------------------------------------------------+
                          |
                          v
    +------------------------------------------------+
    |                Transaction Manager             |
    +------------------------------------------------+
                          |
                          v
    +------------------------------------------------+
    |                    MVCC Layer                  |
    |                                                |
    | Snapshot Manager | Visibility Manager         |
    | Version Manager  | Vacuum Manager             |
    +------------------------------------------------+
                          |
              +-----------+-----------+
              |                       |
              v                       v
    +-------------------+   +-------------------+
    |   B+Tree Storage  |   |    LSM Storage    |
    |                   |   |                   |
    | Version Chains    |   | SSTable Versions  |
    | Index References  |   | Tombstones        |
    | Page Reclamation  |   | Compaction        |
    +-------------------+   +-------------------+
              |                       |
              v                       v
          Buffer Pool             File Storage
              |                       |
              +-----------+-----------+
                          |
                          v
                    WAL / Recovery

The MVCC layer owns:

- Transaction visibility.
- Snapshot management.
- Version metadata semantics.
- Version-chain traversal.
- Write-write conflict detection.
- Safe version reclamation decisions.

The storage engines own:

- Physical version placement.
- Index maintenance.
- Version retrieval.
- Persistent storage.
- Physical space reclamation.

The Transaction Manager owns:

- Transaction ID allocation.
- Transaction lifecycle.
- Commit and abort state.
- Snapshot creation.
- Active transaction tracking.

The WAL and Recovery layers own:

- Durable logging of transactional modifications.
- Commit records.
- Recovery of transaction state.
- Recovery of incomplete operations.
- Consistent restoration of version metadata.

### B+Tree Integration

The B+Tree storage engine must support retrieving the appropriate version of a logical record.

A possible design is:

    B+Tree Index
         |
         v
    Logical Key
         |
         v
    Version Chain Head
         |
         v
    Version V3
         |
         v
    Version V2
         |
         v
    Version V1

The B+Tree index may point to the newest version or to a stable record identifier whose version chain is maintained separately.

The implementation must determine:

- Whether versions are stored inline or separately.
- Whether version metadata resides in the leaf page or record storage.
- How index references are updated.
- How version-chain traversal interacts with buffer-pool pins.
- How vacuum safely removes obsolete versions.

### LSM Integration

The LSM storage engine must preserve enough historical information to satisfy active transaction snapshots.

A logical key may have multiple versions across different SSTables.

    MemTable
       |
       v
    +----------------------+
    | key=100, seq=30, C   |
    | key=100, seq=20, B   |
    +----------------------+
       |
       v
    Flush
       |
       v
    +----------------------+
    | SSTable L0           |
    | key=100, seq=30, C   |
    | key=100, seq=20, B   |
    +----------------------+
       |
       v
    Compaction
       |
       v
    Preserve versions required by active snapshots

LSM sequence numbers represent storage-operation ordering. They are not transaction IDs and do not independently determine transaction visibility.

The LSM engine must coordinate:

- Transaction commit visibility.
- Sequence-number assignment.
- Version retention.
- Tombstone handling.
- Compaction.
- Snapshot-aware reads.

### WAL Integration

The existing MiniDB WAL is initially page-oriented and supports redo-oriented recovery.

Full transactional support requires extending it with transaction-aware records.

Potential record types:

| Record Type | Purpose |
|---|---|
| TRANSACTION_BEGIN | Marks transaction start |
| TRANSACTION_COMMIT | Records successful commit |
| TRANSACTION_ABORT | Records transaction abort |
| PAGE_UPDATE | Records page modification |
| PAGE_IMAGE | Stores a page image |
| COMPENSATION_LOG | Records an operation that compensates for an earlier update |
| CHECKPOINT_BEGIN | Begins checkpoint |
| CHECKPOINT_END | Completes checkpoint |

The exact record format and recovery behavior must be specified before transactional writes are considered durable.

The WAL must support the following invariants:

1. Log records are appended before their corresponding data-page modifications become durable.
2. A successful commit acknowledgment requires a durable commit record.
3. Recovery must identify transactions that committed and transactions that did not.
4. Undo operations must themselves be recoverable.
5. Checkpointing must preserve sufficient information for correct recovery.

### Transaction Recovery

After a crash, the Recovery Manager must reconstruct transaction state and restore a consistent database state.

A future recovery design may use analysis, redo, and undo phases:

    Database Restart
           |
           v
        ANALYSIS
           |
           v
    Identify Transactions
           |
           v
          REDO
           |
           v
    Replay Required Changes
           |
           v
          UNDO
           |
           v
    Roll Back Incomplete Transactions
           |
           v
    Database Available

This is a planned recovery architecture. Its implementation depends on extending the existing WAL with the necessary transactional and undo information.

### Implementation Dependencies

The MVCC implementation depends on the following components:

1. Transaction ID allocator.
2. Transaction Manager.
3. Transaction state table.
4. Snapshot Manager.
5. Visibility Manager.
6. Version-chain representation.
7. Storage-engine integration.
8. WAL integration.
9. Recovery Manager.
10. Vacuum and reclamation.

The implementation should introduce these components incrementally while preserving clear boundaries between transaction semantics and physical storage.

---

## Open Questions

### Transaction Management

1. Should transaction IDs be allocated by a centralized Transaction Manager?
2. How should transaction ID wraparound be handled?
3. Should transaction state be maintained in memory, persisted, or reconstructed during recovery?
4. How should transaction states be represented after restart?
5. Should read-only transactions receive transaction IDs?
6. Should transaction IDs be allocated at transaction start or when the first write occurs?
7. How should transaction commit timestamps be represented?
8. Should transaction IDs and commit timestamps be separate identifiers?

### Snapshot Management

1. Should snapshots use `xmin`, `xmax`, and an active transaction set?
2. Should snapshot creation be centralized or coordinated with the Transaction Manager?
3. How should long-running transactions affect cleanup and storage growth?
4. Should the system support exported snapshots?
5. How should snapshots behave during transaction abort?
6. How should concurrent snapshot creation and transaction commit be synchronized?
7. Should snapshots be created lazily or eagerly?

### Version Storage

1. Should version chains be stored directly in data pages?
2. Should versions be stored separately and referenced using stable record identifiers?
3. Should B+Tree leaf entries point to the newest version or directly store version metadata?
4. How should version chains be organized for LSM storage?
5. Should updates always create new physical versions?
6. Should version metadata be stored inline or separately?
7. How should version-chain traversal be optimized?
8. Should intermediate versions created by the same transaction be consolidated?
9. How should version references remain valid across page reorganization?

### Visibility and Isolation

1. Should Read Committed and Repeatable Read be the initial supported isolation levels?
2. Should Snapshot Isolation be exposed as a separate isolation level?
3. How should transaction state lookups be optimized?
4. Should committed transaction status be cached?
5. How should transaction ID wraparound affect snapshot comparisons?
6. How should write-write conflicts be detected?
7. Should row-level locks be introduced for conflicting writers?
8. How should write skew be prevented if Serializable isolation is introduced?
9. How should concurrent inserts of the same key be handled?
10. How should range scans and phantom reads be handled?

### WAL and Recovery

1. How should transaction commit records integrate with the existing WAL?
2. Should transaction metadata updates use physiological or logical logging?
3. How should aborted transactions and incomplete version chains be recovered?
4. How should commit ordering and WAL durability be coordinated?
5. Should recovery reconstruct transaction states before enabling normal reads?
6. How should WAL records identify individual versions?
7. How should recovery handle partially completed version-chain updates?
8. Should undo records or Compensation Log Records be introduced?
9. How should vacuum operations be logged?
10. How should checkpoints interact with active snapshots?

### Version Cleanup

1. Should cleanup be implemented as a background vacuum worker?
2. How should the oldest active snapshot be tracked?
3. How should cleanup coordinate with buffer-pool pins and concurrent readers?
4. When can deleted records and their index entries be physically removed?
5. How should cleanup work across multiple LSM levels?
6. How should tombstone retention interact with active snapshots?
7. How should long-running transactions and abandoned sessions affect cleanup?
8. Should vacuum be incremental or full-scan initially?
9. Should vacuum use a separate worker thread?
10. How should reclaimed pages be returned to the Page Manager?

### Storage Engine Integration

1. Should B+Tree records maintain version chains directly?
2. Should LSM versions use transaction IDs, sequence numbers, or both?
3. How should transaction visibility work across multiple SSTables?
4. How should vacuum coordinate with compaction?
5. How should index entries be maintained when a record is updated?
6. Should the storage engine expose transaction-aware read and write interfaces?
7. Should the same version representation be used by both B+Tree and LSM storage?
8. How should version metadata be serialized and persisted?

### Future Extensions

1. Should the system support Serializable Snapshot Isolation?
2. Should it support multi-version indexes?
3. Should it support time-travel queries?
4. Should transaction snapshots be usable across distributed replicas?
5. How should MVCC integrate with replication and Raft?
6. Should the system eventually support distributed transaction timestamps?
7. How should MVCC integrate with query execution and transaction-aware scans?
8. Should historical versions support point-in-time recovery?