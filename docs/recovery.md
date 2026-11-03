# Recovery

## Overview

Recovery is responsible for bringing MiniDB back to a consistent, transactionally valid state after an unexpected failure.

A database process can terminate at any point during a write operation. Pages may be partially written, dirty pages may remain in memory, transactions may have committed without all their modified pages reaching disk, and uncommitted changes may have reached persistent storage.

Recovery uses the Write-Ahead Log (WAL) to reconstruct the database state after a crash.

MiniDB follows a WAL-based recovery architecture in which log records provide the information required to redo committed changes and undo incomplete transactions.

The recovery subsystem operates across the following components:

- **WAL Manager:** Persists log records and provides sequential log access.
- **Recovery Manager:** Coordinates startup recovery, analysis, REDO, and UNDO.
- **Transaction Manager:** Tracks transaction states and determines which transactions committed or remained incomplete.
- **Buffer Pool Manager:** Loads and modifies database pages during recovery.
- **Disk Manager:** Provides persistent page reads and writes.
- **Checkpoint Manager:** Limits the amount of WAL that must be processed during startup.
- **Storage Engine:** Applies recovered changes to database pages and indexes.

The recovery design must distinguish between the durable log and the database pages. A committed transaction may have durable log records even when its modified pages have not yet been flushed to disk.

### Recovery Architecture

    Database Startup
          |
          v
    Recovery Manager
          |
          v
       Analysis
          |
          +----------------------+
          |                      |
          v                      v
    Committed Transactions   Incomplete Transactions
          |                      |
          v                      v
         REDO                   UNDO
          |                      |
          +-----------+----------+
                      |
                      v
              Recovery Complete
                      |
                      v
              Database Available

### Core Concepts

- **WAL:** An append-only log containing database modification records.
- **LSN:** Log Sequence Number identifying the position of a record in the WAL.
- **Page LSN:** The LSN of the latest log record whose changes are reflected in a page.
- **Commit Record:** Indicates that a transaction has committed.
- **Checkpoint:** A recovery boundary that records sufficient state to reduce startup work.
- **REDO:** Reapplies logged changes that may not have reached their database pages.
- **UNDO:** Reverses changes belonging to incomplete transactions.

### Recovery and Durability

MiniDB must enforce the Write-Ahead Logging rule:

A database page containing changes described by a WAL record must not be written to persistent storage until the corresponding WAL record is durable.

For a transaction to be acknowledged as committed, its commit record must be durable.

These rules establish the foundation for crash recovery.

## Goals

The recovery subsystem has the following goals.

### Atomicity

A transaction must either be fully committed or have its incomplete effects removed.

If a transaction crashes before its commit record becomes durable, recovery must treat it as incomplete.

### Durability

Once a transaction has been acknowledged as committed, its effects must survive a process or machine crash, subject to the configured storage durability guarantees.

### Consistency

Recovery must restore database pages to a structurally valid state.

This includes:

- Page format and checksum validity.
- Correct page allocation metadata.
- Valid B+Tree structures.
- Consistent index and data references.
- Correct transaction visibility metadata.
- Consistent WAL and checkpoint metadata.

### Idempotent REDO

Recovery may encounter the same log record more than once.

Reapplying a record must not corrupt the database or apply the same logical change multiple times.

Page LSNs provide the mechanism for identifying changes that have already been applied.

### Bounded Startup Time

Checkpointing should reduce the amount of WAL that must be scanned during recovery.

Recovery should start from the latest valid checkpoint rather than always scanning the entire WAL.

### Extensibility

The recovery architecture should support:

- Page-oriented redo.
- Transaction-oriented undo.
- B+Tree recovery.
- Transactional updates.
- MVCC version recovery.
- LSM-based storage.
- Replication and distributed recovery.

## Startup Recovery

Startup recovery runs before MiniDB makes the database available for normal operations.

Its responsibility is to inspect persistent database state, identify incomplete transactions, replay necessary changes, undo incomplete work, and establish a valid recovery boundary.

### Startup Sequence

    Database Process Starts
             |
             v
    Acquire Database Lock
             |
             v
    Validate Database Metadata
             |
             v
    Open WAL
             |
             v
    Locate Latest Valid Checkpoint
             |
             v
    Analysis Phase
             |
             v
    REDO Phase
             |
             v
    UNDO Phase
             |
             v
    Flush Recovery Changes
             |
             v
    Persist Recovery Completion
             |
             v
    Database Ready

### 1. Database Initialization

The recovery manager first initializes the components required for recovery.

Responsibilities:

- Acquire exclusive startup access to the database.
- Validate database metadata.
- Open the WAL.
- Read the latest valid checkpoint metadata.
- Determine the WAL range required for recovery.
- Initialize transaction and recovery state.

Recovery must not begin normal transaction processing while the database is in an inconsistent intermediate state.

### 2. Checkpoint Discovery

The recovery manager locates the latest valid checkpoint.

The checkpoint identifies a known recovery boundary and contains metadata needed to determine which transactions and pages require additional processing.

A checkpoint must itself be crash-safe. If a crash occurs while a checkpoint is being written, startup must be able to fall back to an earlier valid checkpoint.

### 3. Analysis

The analysis phase scans WAL records from the selected recovery starting point.

It reconstructs the state required for REDO and UNDO.

The analysis phase identifies:

- Transactions that started.
- Transactions that committed.
- Transactions that aborted.
- Transactions that were still active at the time of the crash.
- The latest relevant LSN for each transaction.
- Pages modified by logged operations.
- The earliest WAL record potentially required for recovery.

The result is a recovery transaction table and, when required by the recovery algorithm, a dirty page table.

#### Transaction Table

Conceptual structure:

    TransactionEntry {
        transaction_id
        status
        last_lsn
    }

Possible transaction states:

- ACTIVE
- COMMITTED
- ABORTED

Transactions without a durable commit record are treated as incomplete unless a durable abort record establishes their completed rollback.

#### Dirty Page Table

Conceptual structure:

    DirtyPageEntry {
        page_id
        rec_lsn
    }

`rec_lsn` identifies the earliest WAL record that may need to be replayed for a particular dirty page.

The dirty page table helps avoid replaying log records that cannot affect the current persistent database state.

### 4. REDO

The REDO phase reapplies logged changes that may not have reached persistent database pages.

It restores the effects of committed transactions and any other operations required by the selected recovery protocol.

### 5. UNDO

The UNDO phase reverses operations belonging to incomplete transactions.

Recovery must ensure that partially executed transactions do not remain visible after startup.

### 6. Recovery Completion

After REDO and UNDO:

- Recovery-generated page modifications must be persisted safely.
- Required WAL records must be durable.
- Transaction state must be reconstructed.
- Database metadata must be consistent.
- The database can transition to normal operation.

Recovery completion must be recorded in a crash-safe manner so that another crash during startup does not invalidate the recovery process.

## REDO

REDO reapplies database modifications recorded in the WAL.

Its purpose is to recover changes that were logged durably but may not have been flushed to their corresponding database pages before the crash.

### Why REDO Is Required

Consider the following sequence:

    Transaction T1:
        BEGIN
        UPDATE page 10
        COMMIT

    WAL:
        LSN 100: BEGIN T1
        LSN 110: UPDATE page 10
        LSN 120: COMMIT T1

    Persistent State:
        WAL records 100-120: durable
        Page 10: old version

A crash occurs after the commit record is persisted but before page 10 is flushed.

After restart, the database page does not contain the committed update.

REDO reapplies the modification using the WAL record at LSN 110.

### REDO Eligibility

Not every WAL record needs to be replayed.

The recovery manager considers:

- Whether the record represents a redoable operation.
- Whether the record falls within the recovery range.
- Whether the target page exists.
- Whether the page has already incorporated the change.
- Whether the operation is valid under the recovery protocol.

### Page LSN

Every database page should contain a Page LSN.

The Page LSN represents the latest WAL record whose changes have been applied to that page.

Conceptual page metadata:

    PageHeader {
        page_id
        page_type
        page_lsn
        checksum
    }

During REDO:

    if (page.page_lsn < log_record.lsn) {
        apply(log_record, page);
        page.page_lsn = log_record.lsn;
    }

    otherwise:
        skip(log_record);

This makes page-oriented REDO idempotent.

A record whose LSN is less than or equal to the page's Page LSN does not need to be applied again, provided the recovery protocol guarantees that page LSNs accurately describe the applied log history.

### REDO Algorithm

Conceptual pseudocode:

    for each record in redo_log_range:

        if (!record.is_redoable()) {
            continue;
        }

        page = buffer_pool.fetch_page(record.page_id);

        if (record.lsn <= page.page_lsn) {
            continue;
        }

        apply_redo(record, page);

        page.page_lsn = record.lsn;

        buffer_pool.mark_dirty(page);

    flush_recovery_pages();

### Physiological REDO

MiniDB should use page-oriented or physiological logging for storage-engine modifications.

A physiological log record identifies:

- The page being modified.
- The operation being performed.
- The relevant page offset or logical location.
- The data needed to reproduce the modification.

For example, a B+Tree insertion record may identify the index page, slot or key position, and inserted key/value data.

This provides more flexibility than logging an entire page for every modification.

The exact logging format must be defined for each storage-engine operation.

### REDO Ordering

Recovery must preserve the ordering constraints of the original log.

For page modifications, records must be applied in WAL order unless the recovery algorithm explicitly establishes that reordering is safe.

Operations involving multiple pages require particular care.

Examples:

- B+Tree leaf split.
- Internal node split.
- Root page replacement.
- Page allocation.
- Page deallocation.
- Index metadata updates.

A multi-page operation must have enough logging information to recover from a crash between its individual page writes.

### Structural REDO

B+Tree structural modifications require special attention.

Consider a leaf split:

    Before:

        [10 | 20 | 30 | 40]

    After:

        Left Leaf:  [10 | 20]
        Right Leaf: [30 | 40]

A crash may occur after the right page has been allocated but before the parent page is updated.

Recovery must restore a valid tree structure.

The logging protocol must define how to recover:

- New page allocation.
- Left-page modification.
- Right-page initialization.
- Parent separator insertion.
- Root replacement.
- Page-link updates.

The implementation must not assume that all pages belonging to a structural operation are written atomically.

### REDO and Page Allocation

Page allocation and deallocation must also be recoverable.

For example:

1. A transaction allocates a new page.
2. The allocation metadata is persisted.
3. The transaction crashes before the page is initialized.

Recovery must not leave an allocated page in an invalid state.

Allocation and deallocation operations must be represented in the recovery protocol, with enough information to restore page-manager metadata.

## UNDO

UNDO reverses changes made by transactions that did not commit before the crash.

Its purpose is to restore atomicity.

### Why UNDO Is Required

Consider the following sequence:

    Transaction T1:
        BEGIN
        UPDATE page 10
        UPDATE page 20

    WAL:
        LSN 100: BEGIN T1
        LSN 110: UPDATE page 10
        LSN 120: UPDATE page 20

    Crash:
        Before COMMIT T1

The transaction has modified two pages, but it never committed.

Some modifications may already have reached persistent storage.

Recovery must remove the effects of T1.

### Undo Information

A WAL record must contain sufficient information to reverse an operation.

For an update, this may include:

- Transaction ID.
- Page ID.
- Record or slot identifier.
- Previous value.
- New value.
- LSN.
- Previous transaction LSN.

Conceptual record:

    UpdateRecord {
        transaction_id
        page_id
        record_id
        before_image
        after_image
        lsn
        previous_lsn
    }

The before-image allows recovery to restore the previous state.

The after-image supports REDO.

The exact record format depends on whether MiniDB uses physical, physiological, or logical logging for a particular operation.

### Previous LSN

Each transaction maintains a pointer to its most recent WAL record.

Each subsequent record references the transaction's previous LSN.

Example:

    T1:
        BEGIN        LSN 100
        UPDATE       LSN 110  prev_lsn = 100
        UPDATE       LSN 120  prev_lsn = 110
        UPDATE       LSN 130  prev_lsn = 120

The transaction's log records form a backward-linked chain.

UNDO can traverse this chain in reverse order without scanning the entire WAL for every transaction.

### UNDO Algorithm

Conceptual pseudocode:

    for each incomplete_transaction:

        lsn = incomplete_transaction.last_lsn

        while (lsn is valid):

            record = wal.read(lsn)

            if (record.is_undoable()) {
                apply_undo(record);
            }

            lsn = record.previous_lsn;

        write_abort_record(incomplete_transaction);

The implementation must ensure that undo operations themselves are recoverable.

### Compensation Log Records

A Compensation Log Record (CLR) records an undo operation.

CLRs are important when recovery itself crashes.

Without CLRs, a second crash during UNDO could cause the same operation to be undone repeatedly without a reliable way to determine which undo operations have already completed.

A CLR records:

- The transaction ID.
- The page affected.
- The compensation operation.
- The LSN of the CLR.
- The next LSN to undo.

CLRs are redoable but are not undone again.

Conceptual structure:

    CompensationLogRecord {
        transaction_id
        page_id
        compensation_data
        lsn
        undo_next_lsn
    }

`undo_next_lsn` identifies the next original transaction record that recovery must process.

This allows recovery to resume an interrupted rollback.

### UNDO Ordering

UNDO generally processes a transaction's records in reverse order.

For example:

    Original Operations:

        UPDATE page 10
        UPDATE page 20
        UPDATE page 30

    Undo Order:

        UNDO page 30
        UNDO page 20
        UNDO page 10

Reverse ordering preserves the logical dependency order of operations.

### REDO and UNDO Together

A common recovery architecture uses three phases:

1. Analysis.
2. REDO.
3. UNDO.

The analysis phase identifies transaction and page state.

REDO repeats history from the recovery starting point, including operations belonging to transactions that may later be undone.

UNDO then removes the effects of incomplete transactions.

This approach is associated with ARIES-style recovery.

MiniDB can adopt this architecture incrementally, beginning with simpler redo-only recovery and adding transaction-aware undo and compensation records as transaction processing matures.

## Checkpointing

Checkpointing reduces startup recovery time by recording a consistent recovery boundary and sufficient metadata about the database's state.

Without checkpointing, recovery may need to scan the WAL from the beginning.

As the database grows, this becomes increasingly expensive.

### Goals

- Reduce WAL scanning during startup.
- Record active transaction state.
- Record dirty page state.
- Establish a safe recovery starting point.
- Allow WAL segments before the safe recovery boundary to be reclaimed when no longer needed.

### Checkpoint Metadata

A checkpoint may contain:

    CheckpointMetadata {
        checkpoint_lsn
        active_transactions
        dirty_pages
        transaction_metadata
        database_metadata_version
    }

The exact fields depend on the selected checkpoint protocol.

The checkpoint must contain enough information to reconstruct the recovery state without losing any modifications that might exist only in WAL.

### Fuzzy Checkpoint

A fuzzy checkpoint allows normal transactions and page modifications to continue while checkpointing is in progress.

It does not require flushing every dirty page or stopping all database activity.

The checkpoint records transaction and dirty-page information needed to resume recovery correctly.

This is more scalable than requiring a complete database flush at every checkpoint.

### Checkpoint Workflow

    Begin Checkpoint
          |
          v
    Write Checkpoint Begin Record
          |
          v
    Capture Active Transaction State
          |
          v
    Capture Dirty Page Information
          |
          v
    Write Checkpoint Metadata
          |
          v
    Flush Required WAL
          |
          v
    Write Checkpoint Completion Record
          |
          v
    Persist Checkpoint Reference
          |
          v
    Checkpoint Complete

The exact ordering of metadata writes and WAL flushes must be defined so that a crash cannot cause recovery to trust an incomplete checkpoint.

### Checkpoint and WAL Retention

WAL segments cannot be deleted merely because a checkpoint has completed.

A segment is reclaimable only when it is no longer required by:

- Crash recovery.
- Active transactions.
- In-progress checkpoints.
- Replication.
- Backup or archival requirements.
- Any other component retaining a WAL position.

The earliest required LSN determines the WAL retention boundary.

### Checkpoint Frequency

Checkpoint frequency involves a trade-off.

Frequent checkpoints:

- Reduce recovery time.
- Increase checkpoint I/O.
- May increase background write pressure.

Infrequent checkpoints:

- Reduce checkpoint overhead.
- Increase the amount of WAL scanned during recovery.
- Increase potential startup time.

MiniDB should initially support configurable checkpoint intervals and later consider triggering checkpoints based on WAL volume, dirty-page volume, or recovery-time objectives.

### Checkpoint Crash Safety

Consider a crash during checkpoint creation.

Recovery must be able to distinguish:

- A fully completed checkpoint.
- A partially written checkpoint.
- A checkpoint whose metadata is durable but whose completion record is not.
- An older checkpoint that remains valid.

Checkpoint metadata should be versioned and validated, and checkpoint publication must be atomic or otherwise recoverable.

## Recovery Workflow

This section describes the complete recovery process from a database crash to normal operation.

### Normal Transaction Execution

A transaction modifies database pages through the storage engine.

The WAL must be written before the corresponding dirty pages are allowed to reach persistent storage.

    Client
      |
      v
    Transaction Manager
      |
      v
    Storage Engine
      |
      +-------> WAL Manager
      |
      v
    Buffer Pool Manager
      |
      v
    Dirty Database Pages

### Commit Workflow

    BEGIN
      |
      v
    Generate WAL Records
      |
      v
    Modify Buffer Pool Pages
      |
      v
    Append COMMIT Record
      |
      v
    Flush WAL Through Commit LSN
      |
      v
    Acknowledge Commit
      |
      v
    Pages Flushed Later

The database does not need to flush every modified page before acknowledging commit.

The durable WAL provides the information needed to recover those modifications.

### Crash Recovery Workflow

    Crash Detected
          |
          v
    Restart Database
          |
          v
    Open WAL and Database Files
          |
          v
    Locate Latest Valid Checkpoint
          |
          v
    ANALYSIS
          |
          +-----------------------------+
          |                             |
          v                             v
    Identify Dirty Pages        Identify Transactions
          |                             |
          +--------------+--------------+
                         |
                         v
                       REDO
                         |
                         v
                Restore Logged Changes
                         |
                         v
                       UNDO
                         |
                         v
             Roll Back Incomplete Work
                         |
                         v
              Persist Recovery Results
                         |
                         v
                Database Available

### Recovery Record Types

The WAL should support record types that allow recovery to reconstruct and restore database state.

| Record Type | Purpose |
|---|---|
| BEGIN | Marks transaction start |
| UPDATE | Records a database modification |
| COMMIT | Marks transaction commit |
| ABORT | Marks transaction rollback completion |
| CLR | Records a compensation operation |
| CHECKPOINT_BEGIN | Starts checkpoint metadata capture |
| CHECKPOINT_END | Marks checkpoint completion |
| PAGE_ALLOC | Records page allocation |
| PAGE_FREE | Records page deallocation |
| ROOT_UPDATE | Records index root metadata changes |

Additional record types may be introduced for B+Tree structural operations, MVCC version changes, and LSM storage operations.

### Recovery Invariants

The following invariants must hold:

1. A database page must never be persisted ahead of its corresponding WAL records.
2. A committed transaction must have a durable commit record.
3. REDO must be safe to repeat.
4. UNDO must be restartable after a recovery crash.
5. An incomplete checkpoint must never be treated as a valid recovery boundary.
6. WAL required for recovery must not be reclaimed prematurely.
7. Recovery must not expose partially recovered database state to normal clients.
8. Page LSNs must accurately reflect the log history applied to their pages.
9. Structural storage-engine operations must be recoverable across partial page writes.

## Failure Scenarios

Recovery must be tested against failures occurring at different stages of transaction execution, page persistence, checkpointing, and recovery itself.

### 1. Crash Before WAL Record Is Written

Scenario:

- Transaction begins.
- No modification record becomes durable.
- Process crashes.

Expected behavior:

- No persistent database change should be attributed to the transaction.
- Transaction is treated as incomplete or absent, depending on whether its BEGIN record was durable.
- No page modification should require recovery.

### 2. Crash After WAL Record but Before Page Write

Scenario:

- Transaction modifies page 10.
- WAL record is persisted.
- Database page remains unchanged on disk.
- Process crashes.

Expected behavior:

- REDO reapplies the logged modification if required.
- The recovered page reflects the logged operation.

### 3. Crash After Page Write but Before Commit

Scenario:

- Transaction modifies page 10.
- WAL record is persisted.
- Dirty page reaches disk.
- Transaction crashes before commit.

Expected behavior:

- REDO may repeat the logged operation.
- UNDO reverses the incomplete transaction's modification.
- The uncommitted change must not remain visible.

### 4. Crash After Commit Record but Before Page Flush

Scenario:

- Transaction modifies page 10.
- Commit record is flushed.
- Commit is acknowledged.
- Page 10 remains dirty in memory.
- Process crashes.

Expected behavior:

- REDO restores the committed change.
- The transaction remains committed.

### 5. Crash During UNDO

Scenario:

- Recovery starts rolling back an incomplete transaction.
- Some undo operations complete.
- Process crashes before rollback finishes.

Expected behavior:

- The next recovery must determine which undo operations have already completed.
- CLRs and undo-next pointers allow recovery to resume rollback.
- Previously completed compensation operations must not be undone again.

### 6. Crash During Checkpoint

Scenario:

- Checkpoint begins.
- Some checkpoint metadata is written.
- Process crashes before checkpoint completion.

Expected behavior:

- The incomplete checkpoint must not be treated as valid.
- Recovery falls back to the previous valid checkpoint or another safe recovery boundary.
- Required WAL must remain available.

### 7. Torn Page Write

Scenario:

- A page write is interrupted.
- Persistent storage contains a partially written page.

Expected behavior:

- Page validation detects corruption where possible.
- Recovery restores the page using WAL or another valid recovery source.
- If WAL does not contain enough information to reconstruct the page, the system must report corruption rather than silently accepting invalid data.

Possible protections include:

- Page checksums.
- Full-page images.
- Double-write buffering.
- Atomic page-write guarantees where available.

A checksum detects corruption but does not, by itself, repair a torn page.

### 8. WAL Tail Corruption

Scenario:

- The process crashes while appending a WAL record.
- The final WAL record is incomplete.

Expected behavior:

- WAL scanning identifies the last valid record.
- An incomplete trailing record is rejected.
- Recovery does not interpret partially written bytes as a valid log record.
- Previously durable records remain usable.

WAL records should include sufficient framing and integrity information, such as record length, type, LSN, and checksum.

### 9. Disk Full During WAL Flush

Scenario:

- A transaction attempts to append or flush WAL.
- The storage device reports insufficient space.

Expected behavior:

- The transaction must not be acknowledged as committed unless its commit record is durable.
- The system must report the I/O failure.
- The database must preserve the distinction between committed and incomplete transactions.
- Further writes may need to be rejected until sufficient space is available.

### 10. Crash During B+Tree Split

Scenario:

- A new index page is allocated.
- Leaf contents are redistributed.
- Parent separator is inserted.
- Process crashes between these operations.

Expected behavior:

- Recovery restores a valid tree structure.
- Page allocation metadata remains consistent.
- No reachable index page references an uninitialized or invalid page.
- Root metadata and parent-child relationships are consistent.

### 11. Crash During Page Deallocation

Scenario:

- A page is removed from an index structure.
- The page allocator is updated.
- Process crashes between the two operations.

Expected behavior:

- Recovery must not expose references to a freed page.
- A page must not be reallocated while it is still reachable through a valid index structure.
- The structural modification and allocator metadata must be recoverable as one logical operation.

### 12. Crash During Recovery REDO

Scenario:

- Startup recovery applies some WAL records.
- Process crashes before REDO completes.

Expected behavior:

- The next recovery can safely repeat the required REDO work.
- Page LSNs prevent already-applied changes from being applied again.
- Recovery starts from a valid checkpoint and WAL boundary.

### 13. Crash During Recovery Completion

Scenario:

- REDO and UNDO finish.
- Recovery begins persisting completion metadata.
- Process crashes before startup completes.

Expected behavior:

- The next startup must determine whether recovery completed safely.
- Recovery must be repeatable without corrupting database state.
- Normal operation must not begin until recovery completion is established.

## Design Decisions

### 1. WAL-Based Recovery

MiniDB will use WAL as the source of truth for recovering database modifications.

Database pages are persisted independently from transaction commit records.

The WAL provides the ordering and durability information needed to reconstruct database state.

### 2. Separate WAL Manager and Recovery Manager

The WAL Manager owns log creation, append, flush, and sequential reading.

The Recovery Manager owns startup analysis, REDO, UNDO, and recovery coordination.

This separation keeps WAL storage mechanics independent from recovery policy.

### 3. Page LSN-Based REDO

Every persistent database page will contain a Page LSN.

Recovery compares the record LSN against the page LSN to determine whether a change must be replayed.

This supports idempotent page-oriented REDO.

### 4. Transaction-Linked WAL Records

Each transaction maintains a previous-LSN pointer.

This creates a transaction-local log chain that supports reverse traversal during UNDO.

### 5. ARIES-Inspired Recovery

MiniDB will adopt an ARIES-inspired recovery architecture:

- Analysis.
- REDO.
- UNDO.
- Compensation Log Records.
- Fuzzy checkpoints.
- Restartable rollback.

This is a target architecture rather than a claim that all these mechanisms are already implemented.

The implementation can evolve incrementally.

### 6. Recovery Must Support Structural Operations

Recovery cannot be limited to ordinary record updates.

B+Tree page splits, root changes, page allocation, and page deallocation must also be logged and recoverable.

Storage-engine operations must define their logging and recovery semantics explicitly.

### 7. Checkpointing Is Independent of Commit

A transaction commit does not require a checkpoint.

Commit durability is established through WAL persistence.

Checkpointing is a separate mechanism for limiting recovery work.

### 8. Buffer Pool Integration

Recovery should apply page changes through a controlled recovery interface.

The Buffer Pool Manager must support:

- Fetching pages during recovery.
- Marking pages dirty.
- Updating Page LSNs.
- Flushing recovery-modified pages.
- Preventing normal transaction access before recovery completes.

Recovery must preserve the WAL-before-data write ordering even when flushing pages during startup.

### 9. Initial Implementation Strategy

Recovery should be implemented incrementally.

**Stage 1 — WAL Foundations**

- Define stable WAL record format.
- Assign monotonically increasing LSNs.
- Implement WAL append and flush.
- Implement WAL scanning and record validation.
- Add Page LSN metadata.

**Stage 2 — REDO**

- Implement page-oriented redo records.
- Replay records in WAL order.
- Skip records already reflected in pages.
- Test repeated recovery.
- Add page allocation recovery.

**Stage 3 — Transaction Recovery**

- Add transaction IDs.
- Add BEGIN and COMMIT records.
- Reconstruct transaction states.
- Implement before-images or equivalent undo information.
- Implement rollback of incomplete transactions.

**Stage 4 — Restartable UNDO**

- Add previous-LSN chains.
- Add compensation log records.
- Add undo-next pointers.
- Test crashes during rollback.

**Stage 5 — Checkpointing**

- Add checkpoint records.
- Capture active transaction state.
- Capture dirty page state.
- Implement fuzzy checkpointing.
- Add safe WAL retention.

**Stage 6 — Advanced Recovery**

- Add full-page images or an equivalent torn-page recovery strategy.
- Add B+Tree structural recovery.
- Integrate MVCC recovery.
- Integrate LSM recovery.
- Integrate replication and recovery coordination.

### 10. Recovery Module Boundaries

A possible module organization:

    recovery/
        recovery_manager.h
        recovery_manager.cpp
        analysis.h
        analysis.cpp
        redo.h
        redo.cpp
        undo.h
        undo.cpp
        checkpoint_manager.h
        checkpoint_manager.cpp
        recovery_state.h
        recovery_state.cpp

    wal/
        log_manager.h
        log_manager.cpp
        log_record.h
        log_record.cpp
        log_reader.h
        log_reader.cpp
        log_serializer.h
        log_serializer.cpp

The exact directory structure can evolve without changing the logical boundaries.

### 11. Testing Strategy

Recovery testing must focus on crash points, not only successful transaction execution.

Tests should include:

- REDO of a committed transaction.
- UNDO of an incomplete transaction.
- Repeated REDO.
- Recovery after interrupted UNDO.
- Recovery after interrupted checkpoint.
- WAL tail truncation.
- Invalid WAL checksum.
- Torn page detection.
- B+Tree split recovery.
- Page allocation recovery.
- Root page replacement recovery.
- Recovery with multiple active transactions.
- Recovery with interleaved transactions.
- Recovery with dirty pages from both committed and incomplete transactions.

Crash injection should allow the test suite to terminate MiniDB after specific WAL appends, WAL flushes, page writes, checkpoint operations, and recovery steps.

The expected result should be checked by reopening the database and validating both logical contents and structural invariants.

## Open Questions

### WAL Design

- Will MiniDB use physical, physiological, or logical log records for each storage-engine operation?
- Should WAL records contain before-images and after-images, or should different record types use different strategies?
- Will WAL be segmented into fixed-size files?
- What is the WAL record framing and checksum format?
- How will LSNs be represented and persisted?
- Will WAL support group commit?
- What is the maximum WAL record size?
- How will incomplete trailing records be detected?

### REDO

- Will REDO operate exclusively at page level?
- Which operations require logical or physiological redo?
- How will multi-page B+Tree operations be recovered?
- How will page allocation and deallocation be represented?
- Should recovery use a dirty page table from the beginning?
- How will recovery handle pages whose Page LSN is inconsistent with the WAL?
- Will full-page images be required after checkpoints to protect against torn writes?

### UNDO

- Will MiniDB implement full ARIES-style UNDO?
- Will all update records contain before-images?
- How will logical operations such as B+Tree insertion be reversed?
- How will CLRs be serialized?
- How will undo-next pointers be validated?
- How will recovery handle transactions that were already in the process of aborting before the crash?
- Will savepoints and partial rollback be supported?

### Checkpointing

- Will MiniDB use fuzzy checkpoints?
- What information must be included in checkpoint metadata?
- How will checkpoint metadata be published atomically?
- How will the database identify the latest valid checkpoint?
- What should trigger a checkpoint?
- How frequently should checkpoints occur?
- How will WAL retention account for replication and backups?

### Buffer Pool and Storage Engine

- How will recovery interact with pinned or dirty buffer pool pages?
- Will recovery use the existing Buffer Pool Manager or a dedicated recovery page interface?
- How will page checksums and torn-page repair work?
- How will B+Tree structural modifications be logged?
- How will page allocator metadata be recovered?
- How will recovery coordinate with future PageGuard and concurrent buffer pool operations?

### Transactions and MVCC

- How will transaction status be reconstructed after restart?
- How will aborted transaction IDs be represented?
- How will MVCC version chains be recovered?
- How will transaction visibility metadata be reconciled with recovered pages?
- How will vacuum avoid removing versions required by active transactions?
- How will transaction ID wraparound be handled?

### LSM Storage

- How will recovery handle MemTable state?
- Will WAL records be replayed into the MemTable?
- How will flush operations be made crash-safe?
- How will SSTable creation and manifest updates be recovered?
- How will recovery coordinate with compaction?
- How will sequence numbers relate to transaction commit order without being confused with transaction IDs?

### Replication and Distributed Recovery

- How will local crash recovery interact with Raft log recovery?
- Will the Raft log and database WAL be separate?
- How will committed Raft entries be reconciled with database state?
- How will follower recovery work after an extended outage?
- When is a snapshot required instead of WAL replay?
- How will recovery coordinate with leader election and failover?

### Operational Requirements

- How will recovery progress be exposed through monitoring?
- Which metrics should be reported: WAL bytes scanned, records replayed, pages recovered, transactions undone, and recovery duration?
- How will operators inspect recovery failures?
- Should recovery support a read-only diagnostic mode?
- How will corruption be reported without silently discarding data?
- What recovery-time objective should MiniDB target for large databases?
- Should point-in-time recovery from archived WAL be supported?

### Final Architectural Principle

Recovery is not an isolated startup utility. It is a correctness contract shared by the WAL Manager, Buffer Pool Manager, Disk Manager, Transaction Manager, and every storage-engine component.

Every persistent modification must have a defined recovery strategy.

A storage-engine feature is not complete merely because it works during normal execution. It is complete only when its behavior remains correct across crashes at every intermediate persistence boundary.