# Transactions

## Overview

A transaction is a logical unit of database operations that must execute according to the ACID (Atomicity, Consistency, Isolation, Durability) properties.

Transactions allow multiple database operations to be grouped into a single unit of work. Either all modifications become committed, or the transaction's effects are rolled back.

MiniDB uses transaction management to coordinate concurrent operations, maintain database consistency, and ensure durability through Write-Ahead Logging (WAL).

The transaction subsystem coordinates the following components:

- Transaction Manager
- Transaction Context
- Lock Manager
- MVCC Manager
- WAL Manager
- Recovery Manager
- Storage Engine
- Buffer Pool Manager

### ACID Properties

| Property | Description |
|---|---|
| Atomicity | All transaction operations succeed or none of their effects become visible |
| Consistency | Transactions preserve database invariants |
| Isolation | Concurrent transactions execute without exposing invalid intermediate states |
| Durability | Committed transactions survive system crashes |

### Architecture

    Database API
         |
         v
    Transaction Manager
         |
    +----+-------------+-------------+
    |                  |             |
    v                  v             v
    Transaction     Lock          Snapshot
    Context         Manager       Manager
    |                  |             |
    +------------------+-------------+
                       |
                       v
                    MVCC Layer
                       |
                       v
                  Storage Engine
                       |
             +---------+---------+
             |                   |
             v                   v
         Buffer Pool          WAL Manager
             |                   |
             v                   v
        Disk Manager         Log Storage
                                 |
                                 v
                          Recovery Manager

### Transaction Context

Every active transaction maintains a context containing its identity, lifecycle state, isolation level, snapshot, and acquired locks.

Conceptual representation:

    using TransactionId = uint64_t;

    enum class TransactionState {
        ACTIVE,
        COMMITTED,
        ABORTED
    };

    enum class IsolationLevel {
        READ_COMMITTED,
        REPEATABLE_READ,
        SNAPSHOT_ISOLATION
    };

    struct TransactionContext {
        TransactionId transaction_id;
        TransactionState state;
        IsolationLevel isolation_level;

        // Snapshot information for MVCC.
        Snapshot snapshot;

        // Locks held by this transaction.
        std::vector<LockId> held_locks;
    };

The actual implementation should use appropriate ownership and synchronization mechanisms rather than assuming that a transaction context can be safely shared without coordination.

---

## Goals

1. Provide explicit transaction boundaries.
2. Guarantee atomicity through WAL and recovery.
3. Guarantee durability for committed transactions.
4. Coordinate concurrent transactions.
5. Prevent conflicting operations from violating consistency.
6. Integrate with MVCC for snapshot-based reads.
7. Implement shared and exclusive locking.
8. Detect and resolve deadlocks.
9. Support transaction commit and abort.
10. Maintain transaction state across crash recovery.
11. Provide a foundation for future Serializable isolation.
12. Support transactional operations across B+Tree and LSM storage.

### Non-Goals for Initial Implementation

- Distributed transactions.
- Two-Phase Commit (2PC).
- Distributed deadlock detection.
- Multi-node transaction coordination.
- Serializable Snapshot Isolation.
- Predicate locking.
- Nested transactions.
- Savepoints.

---

## Transaction Lifecycle

A transaction progresses through a defined lifecycle.

    +---------+
    |  BEGIN  |
    +---------+
         |
         v
    +---------+
    | ACTIVE  |
    +---------+
         |
    +----+----+
    |         |
    v         v
    +---------+    +---------+
    | COMMIT  |    | ABORT   |
    +---------+    +---------+
         |              |
         v              v
    +-----------+  +-----------+
    | COMMITTED |  | ABORTED   |
    +-----------+  +-----------+

A transaction must reach exactly one terminal state.

Once committed or aborted, it cannot return to the ACTIVE state.

### Begin

`BEGIN` creates a new transaction and initializes its execution context.

Responsibilities:

1. Allocate a unique transaction ID.
2. Create a transaction context.
3. Initialize transaction state as ACTIVE.
4. Record the isolation level.
5. Initialize snapshot information when required.
6. Register the transaction with the Transaction Manager.
7. Initialize transaction-specific WAL state.

Conceptual API:

    TransactionId begin(IsolationLevel isolation_level);

Example:

    BEGIN;

    INSERT INTO accounts VALUES (100, 500);
    UPDATE accounts SET balance = 700 WHERE id = 100;

    COMMIT;

Transaction Manager responsibilities:

    BEGIN
      |
      v
    Allocate Transaction ID
      |
      v
    Create Transaction Context
      |
      v
    Initialize Isolation Level
      |
      v
    Create or Defer Snapshot
      |
      v
    Register Active Transaction
      |
      v
    Return Transaction Handle

Snapshot creation depends on the isolation level:

- Read Committed: create a new snapshot for each statement.
- Repeatable Read: create and retain a transaction snapshot.
- Snapshot Isolation: create and retain a transaction snapshot.

The exact snapshot lifecycle must be coordinated with the MVCC implementation.

### Commit

`COMMIT` makes all transaction modifications durable and visible to subsequent transactions.

Responsibilities:

1. Verify that the transaction is ACTIVE.
2. Validate transaction constraints and write conflicts.
3. Generate the appropriate WAL commit record.
4. Ensure WAL durability according to the configured durability policy.
5. Mark the transaction COMMITTED.
6. Publish the commit state to visibility and transaction-state management.
7. Release acquired locks.
8. Release snapshot resources.
9. Remove the transaction from the active transaction registry.

Conceptual API:

    void commit(TransactionId transaction_id);

#### Commit Sequence

    COMMIT
       |
       v
    Validate Transaction
       |
       v
    Check Write Conflicts
       |
       v
    Append COMMIT Record
       |
       v
    Flush WAL
       |
       v
    Mark Transaction COMMITTED
       |
       v
    Publish Commit Visibility
       |
       v
    Release Locks
       |
       v
    Release Snapshot
       |
       v
    COMPLETE

#### WAL Durability

MiniDB follows the Write-Ahead Logging rule:

**The WAL record describing a data-page modification must become durable before the corresponding dirty data page is written to persistent storage.**

For transaction commit, the commit record must be durable before a successful commit is acknowledged to the client.

Conceptually:

    Transaction T1:

        BEGIN
          |
          v
        UPDATE
          |
          v
        Append WAL Records
          |
          v
        COMMIT
          |
          v
        Flush WAL Through Commit LSN
          |
          v
        Acknowledge COMMIT

The commit LSN represents the log position associated with the transaction's commit record.

The WAL manager must ensure that the log is durable through this position before the commit is acknowledged.

#### Commit Visibility

A transaction must not become visible to other transactions before its commit is durable under the configured durability policy.

The transaction-state transition and publication of commit visibility must be coordinated so that readers cannot observe partially committed changes.

### Abort

`ABORT` terminates a transaction without exposing its modifications as committed changes.

An abort may be triggered by:

- Explicit client request.
- Constraint violation.
- Write-write conflict.
- Deadlock victim selection.
- Execution error.
- Resource exhaustion.
- Recovery of an incomplete transaction.

Conceptual API:

    void abort(TransactionId transaction_id);

#### Abort Sequence

    ABORT
       |
       v
    Mark Transaction Aborting
       |
       v
    Identify Transaction Changes
       |
       v
    Perform Required Undo
       |
       v
    Append ABORT Record
       |
       v
    Mark Transaction ABORTED
       |
       v
    Release Locks
       |
       v
    Release Snapshot
       |
       v
    COMPLETE

#### Undo Processing

The initial WAL implementation supports page-oriented redo.

Transactional abort requires an extension to support undo or an equivalent rollback mechanism.

Possible approaches:

**Physical Undo**

Restore the previous page contents using before-images or undo information.

**Logical Undo**

Reverse the logical operation using transaction-aware records.

**MVCC-Based Rollback**

Mark versions created by the aborted transaction as aborted or otherwise invisible. Restore the validity of previous versions when the aborted transaction had invalidated them.

The implementation must coordinate undo with WAL and recovery so that rollback itself is recoverable.

#### Abort Invariants

- An aborted transaction must never expose its writes as committed data.
- Locks must not be released before rollback reaches a safe state.
- Previous committed versions must remain accessible.
- Recovery must be able to identify incomplete transactions.
- Repeated abort requests must not cause duplicate or inconsistent rollback.

---

## Lock Manager

The Lock Manager coordinates conflicting operations between concurrent transactions.

It provides synchronization for resources such as:

- Logical records.
- Index entries.
- B+Tree pages.
- Metadata structures.
- Other shared database resources.

The initial implementation uses shared and exclusive locks.

### Lock Manager Architecture

    Transaction Manager
            |
            v
       Lock Manager
            |
       +----+----+
       |         |
       v         v
    Lock Table  Wait Queues
       |         |
       v         v
    Resource   Blocked
    -> Lock    Transactions
       |
       v
    Lock Compatibility
       |
       v
    Deadlock Detection

### Lock Representation

Conceptual representation:

    using LockId = uint64_t;
    using ResourceId = uint64_t;

    enum class LockMode {
        SHARED,
        EXCLUSIVE
    };

    struct LockRequest {
        TransactionId transaction_id;
        ResourceId resource_id;
        LockMode mode;
    };

    struct LockEntry {
        ResourceId resource_id;
        std::vector<LockRequest> granted_locks;
        std::vector<LockRequest> waiting_locks;
    };

The actual implementation must protect the lock table and wait queues against concurrent access.

### Lock Compatibility Matrix

| Requested / Held | Shared | Exclusive |
|---|---:|---:|
| Shared | Compatible | Conflict |
| Exclusive | Conflict | Conflict |

Multiple transactions may hold shared locks on the same resource.

Only one transaction may hold an exclusive lock on a resource, and no other transaction may hold a conflicting lock simultaneously.

### Shared Locks

A shared lock (S-lock) allows a transaction to read a resource while preventing conflicting exclusive modifications.

Multiple transactions can hold shared locks on the same resource concurrently.

Example:

    T1: Acquire S-lock on key=100
    T2: Acquire S-lock on key=100

    Both transactions can read key=100.

    T3: Request X-lock on key=100
        BLOCKED

Shared lock behavior:

- Multiple shared locks are allowed.
- Shared locks conflict with exclusive locks.
- Shared locks prevent conflicting modifications while held.
- A transaction may release a shared lock according to the selected locking protocol.

Conceptual API:

    bool acquire_shared_lock(
        TransactionId transaction_id,
        ResourceId resource_id
    );

### Exclusive Locks

An exclusive lock (X-lock) grants a transaction exclusive access to a resource.

Only one transaction can hold an exclusive lock at a time.

Example:

    T1: Acquire X-lock on key=100

    T2: Request S-lock on key=100
        BLOCKED

    T3: Request X-lock on key=100
        BLOCKED

    T1: Release X-lock

    T2 or T3 may proceed according to wait-queue policy.

Exclusive lock behavior:

- Conflicts with shared locks.
- Conflicts with other exclusive locks.
- Allows the holder to modify the protected resource.
- Prevents concurrent conflicting modifications.

Conceptual API:

    bool acquire_exclusive_lock(
        TransactionId transaction_id,
        ResourceId resource_id
    );

### Lock Acquisition

When a transaction requests a lock:

1. Locate or create the resource's lock-table entry.
2. Check compatibility with granted locks.
3. Grant the lock if compatible.
4. Otherwise, enqueue the request.
5. Register the transaction as waiting.
6. Wake the transaction when the lock becomes available.

### Lock Release

Locks must be released according to the transaction's locking protocol.

For the initial strict two-phase locking design:

- Locks are acquired during transaction execution.
- Locks are not released before commit or abort.
- All transaction locks are released after successful commit or completed rollback.

This prevents other transactions from reading or overwriting uncommitted writes protected by those locks.

### Lock Upgrade

A lock upgrade occurs when a transaction holding a shared lock requests an exclusive lock on the same resource.

Example:

    T1: Acquire S-lock on key=100
    T2: Acquire S-lock on key=100

    T1: Request upgrade to X-lock
        BLOCKED because T2 holds S-lock

    T2: Release S-lock

    T1: Upgrade to X-lock
        GRANTED

Lock upgrades must be handled carefully because multiple transactions attempting simultaneous upgrades can create deadlocks.

The initial implementation should support at most one pending upgrade per resource or use a deterministic upgrade queue.

---

## Deadlock Detection

A deadlock occurs when two or more transactions wait indefinitely for resources held by one another.

Deadlocks are possible when transactions acquire multiple locks in different orders.

### Example

    T1:
        Acquire X-lock on Resource A
        Request X-lock on Resource B
        WAIT

    T2:
        Acquire X-lock on Resource B
        Request X-lock on Resource A
        WAIT

Neither transaction can proceed.

### Wait-For Graph

MiniDB uses a Wait-For Graph (WFG) to represent transaction dependencies.

- Each node represents an active transaction.
- An edge `T1 -> T2` means T1 is waiting for a resource held by T2.
- A cycle indicates a deadlock.

Example:

              +---------+
              |   T1    |
              +---------+
                   |
                   | waits for
                   v
              +---------+
              |   T2    |
              +---------+
                   |
                   | waits for
                   v
              +---------+
              |   T1    |
              +---------+

                 DEADLOCK

### Deadlock Detection Algorithm

1. Identify transactions currently waiting for locks.
2. Construct the Wait-For Graph.
3. Add edges from waiting transactions to transactions holding conflicting locks.
4. Run cycle detection.
5. If a cycle exists, select a victim transaction.
6. Abort the victim.
7. Release its locks after rollback is safe.
8. Allow waiting transactions to proceed.

### Cycle Detection

Cycle detection can use Depth-First Search (DFS).

Conceptual pseudocode:

    DetectDeadlock(graph):

        visited = {}
        recursion_stack = {}

        for each transaction in graph:

            if transaction not in visited:

                if DFS(transaction):
                    return DEADLOCK_FOUND

        return NO_DEADLOCK

A transaction encountered again while it is on the recursion stack indicates a cycle.

### Victim Selection

Possible victim-selection policies:

- Youngest transaction.
- Transaction holding the fewest locks.
- Transaction with the lowest rollback cost.
- Transaction that has performed the least work.

For the initial implementation, selecting the transaction with the lowest rollback cost is a reasonable policy.

The victim-selection algorithm must avoid repeatedly aborting the same transaction under similar contention patterns.

### Deadlock Prevention

An alternative to detection is preventing deadlocks by controlling lock acquisition.

**Resource Ordering**

Require transactions to acquire locks in a globally consistent order.

**Wait-Die**

An older transaction may wait for a younger transaction, while a younger transaction requesting a resource held by an older transaction aborts.

**Wound-Wait**

An older transaction may abort a younger transaction holding a conflicting resource, while a younger transaction waits for an older transaction.

For MiniDB, Wait-For Graph-based detection is the initial design.

### Deadlock Detection Frequency

Deadlock detection can run:

- Whenever a transaction becomes blocked.
- Periodically in a background worker.
- After a configurable number of waiting transactions.

The initial implementation can trigger detection when a new wait dependency is introduced.

---

## Isolation Levels

Isolation levels define how concurrent transactions observe each other's modifications.

MiniDB uses MVCC for snapshot-based reads and locks for coordinating conflicting writes and protected resources.

### Read Committed

Read Committed guarantees that each statement sees committed data.

- A new snapshot is created for every statement.
- Uncommitted changes from other transactions are invisible.
- Repeated reads may return different results.
- Non-repeatable reads are possible.
- Write-write conflicts must be handled.

Example:

    T1: BEGIN
        READ key=100 -> A

    T2: BEGIN
        UPDATE key=100 -> B
        COMMIT

    T1:
        READ key=100 -> B
        COMMIT

### Repeatable Read

Repeatable Read maintains a stable snapshot throughout the transaction.

- Snapshot is established at the transaction's read-view boundary.
- Subsequent statements reuse the snapshot.
- Repeated reads observe the same committed version.
- Non-repeatable reads are prevented.
- Write conflicts must be detected.

Example:

    T1: BEGIN
        Snapshot S1
        READ key=100 -> A

    T2: BEGIN
        UPDATE key=100 -> B
        COMMIT

    T1:
        READ key=100 -> A
        COMMIT

### Snapshot Isolation

Snapshot Isolation provides a consistent transaction snapshot and detects conflicting concurrent writes.

- Each transaction has a stable snapshot.
- Reads do not normally block writes.
- Writes must detect conflicting committed updates.
- First-committer-wins can prevent lost updates.
- Write skew remains possible.

### Serializable

Serializable isolation guarantees that the result of concurrent transactions is equivalent to some serial execution order.

It is stronger than Snapshot Isolation.

Possible implementation approaches:

- Strict Two-Phase Locking (Strict 2PL).
- Predicate locking.
- Serializable Snapshot Isolation (SSI).

The initial implementation does not provide full Serializable Snapshot Isolation.

If Strict 2PL is used, the locking protocol and MVCC visibility rules must be coordinated carefully to provide the advertised serializable guarantees.

### Isolation Level Comparison

| Property | Read Committed | Repeatable Read | Snapshot Isolation | Serializable |
|---|---|---|---|---|
| Snapshot lifetime | Per statement | Per transaction | Per transaction | Depends on implementation |
| Dirty reads | Prevented | Prevented | Prevented | Prevented |
| Non-repeatable reads | Possible | Prevented | Prevented | Prevented |
| Write-write conflict detection | Required | Required | Required | Required |
| Write skew | Possible | Possible under snapshot-based semantics | Possible | Prevented |
| Full serializability | No | Not necessarily | No | Yes |

---

## Design Decisions

| Area | Initial Decision |
|---|---|
| Transaction architecture | Centralized Transaction Manager |
| Transaction identifiers | Monotonically allocated 64-bit IDs |
| Transaction states | ACTIVE, COMMITTED, ABORTED |
| Transaction lifecycle | Explicit BEGIN, COMMIT, ABORT |
| Atomicity | WAL-backed rollback and recovery |
| Durability | Flush WAL through commit LSN before acknowledging commit |
| Read visibility | MVCC |
| Write coordination | Lock Manager and write-conflict detection |
| Lock modes | Shared and Exclusive |
| Lock protocol | Strict Two-Phase Locking for lock-managed resources |
| Lock granularity | Initially record-level where supported |
| Lock table | In-memory lock table |
| Wait queues | Per-resource waiting queues |
| Deadlock detection | Wait-For Graph |
| Cycle detection | DFS |
| Victim selection | Lowest estimated rollback cost |
| Read Committed | Statement-level snapshots |
| Repeatable Read | Transaction-level snapshots |
| Snapshot Isolation | Stable snapshot with first-committer-wins |
| Serializable | Deferred |
| Distributed transactions | Deferred |
| Savepoints | Deferred |
| Nested transactions | Deferred |

### Locking and MVCC Integration

MVCC and locking serve different purposes.

MVCC provides version visibility and consistent reads.

Locking coordinates conflicting operations and protects resources that cannot safely be modified concurrently.

    Transaction
         |
         v
    Transaction Manager
         |
    +----+----+
    |         |
    v         v
    MVCC      Lock Manager
    Manager       |
    |             v
    v        Lock Conflicts
    Snapshot      |
    Visibility    |
    |             |
    +------+------+
           |
           v
      Storage Engine

The initial implementation should use MVCC for read visibility and locking for write coordination.

The exact locking policy must be specified for each operation. In particular, MVCC reads should not automatically acquire shared locks unless the selected isolation level requires them.

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

---

## Open Questions

### Transaction Lifecycle

1. Should transactions be created through explicit handles or transaction IDs?
2. Should the Transaction Manager own all transaction contexts?
3. How should transaction ID allocation behave after restart?
4. Should read-only transactions receive transaction IDs?
5. How should transaction timeout and cancellation work?
6. Should transaction state be persisted or reconstructed during recovery?

### Commit and Abort

1. Should commit use synchronous WAL flush for every transaction?
2. Should group commit be introduced to improve throughput?
3. Should abort use physical undo, logical undo, or MVCC-based rollback?
4. How should rollback progress be logged?
5. How should commit visibility be synchronized with transaction-state publication?
6. How should commit behave if WAL flushing fails?
7. How should a client distinguish an aborted transaction from an uncertain commit outcome after a connection failure?

### Lock Manager

1. Should locks initially be record-level or page-level?
2. Should lock entries be stored in a hash table?
3. Should the lock table support lock upgrades?
4. How should lock escalation be implemented?
5. Should shared and exclusive locks be extended with intention locks?
6. Should lock acquisition support timeouts?
7. How should locks interact with MVCC snapshot reads?
8. Should index range scans require predicate or range locks?

### Deadlock Detection

1. Should deadlock detection run on every blocked lock request?
2. Should the Wait-For Graph be maintained incrementally?
3. Should DFS be sufficient for the initial implementation?
4. Which victim-selection policy should be used?
5. How should repeated deadlocks be handled?
6. Should lock acquisition ordering be introduced as an additional prevention mechanism?

### Isolation Levels

1. Should Read Committed be the default isolation level?
2. Should Repeatable Read and Snapshot Isolation share the same implementation?
3. Should Serializable be implemented using Strict 2PL or SSI?
4. How should write skew be prevented?
5. How should phantom reads be handled?
6. Should predicate locking be supported?
7. How should isolation semantics apply to range scans?

### WAL and Recovery

1. How should transaction-aware WAL records extend the existing page-oriented WAL?
2. Should the WAL use logical or physical undo information?
3. Should Compensation Log Records be implemented?
4. How should recovery reconstruct active transaction state?
5. How should checkpoints interact with active transactions?
6. Should group commit be part of the initial transactional implementation?
7. How should recovery handle partially completed index updates?

### Storage Engine Integration

1. How should transaction metadata be stored in B+Tree records?
2. How should MVCC versions be represented in LSM SSTables?
3. How should transaction visibility work across multiple SSTables?
4. How should vacuum coordinate with compaction?
5. How should index entries be maintained when a record is updated?
6. Should the storage engine expose transaction-aware read and write interfaces?

### Future Extensions

1. Should MiniDB support Serializable Snapshot Isolation?
2. Should it support savepoints and nested transactions?
3. Should it support two-phase commit?
4. Should it support distributed transactions?
5. Should it implement group commit and asynchronous commit?
6. How should transaction management integrate with replication and Raft?
7. Should the system eventually support transaction-level resource quotas and workload management?