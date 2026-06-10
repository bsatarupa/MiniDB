# MiniDB: Complete Implementation Roadmap

**Project:** MiniDB — A C++20 Educational Database Engine  
**Primary Goal:** Build a persistent database engine demonstrating practical knowledge of storage systems, database internals, indexing, transactions, search, and distributed systems.

## Project Objectives

- Implement a persistent page-based storage engine.
- Understand and implement buffer management and B+Tree indexing.
- Implement write-ahead logging, crash recovery, and checkpointing.
- Build transaction management, locking, and MVCC.
- Implement an LSM-Tree storage architecture.
- Explore replication, consensus, and sharding.
- Extend the engine with secondary indexes, vector search, full-text search, and hybrid search.
- Develop benchmarks, tests, documentation, and interview-ready explanations.

## Time Estimation Assumptions

- Focused implementation: 6–8 hours per day.
- Theoretical knowledge of database internals is already covered.
- Estimates include implementation, basic testing, debugging, and integration.
- Advanced production-grade reliability is outside the initial scope.
- Completed modules: DiskManager (1 day) and PageManager (1.5 days).

---

# PHASE 1: Core Storage Engine

**Goal:** Establish the foundation for persistent database storage.

| # | Module | Status | Estimated Time |
|---|---|---|---|
| 1 | DiskManager — raw page reads and writes | Completed | 1 day |
| 2 | PageManager — page allocation and management | Completed | 1.5 days |
| 3 | Page Format and Serialization | Completed | Included in initial storage work |
| 4 | Page Header and Metadata Management | Pending / Verify implementation | 1–2 days |
| 5 | Buffer Pool Manager | Pending | 3–5 days |
| 6 | LRU Page Replacement Policy | Pending | 1–2 days |
| 7 | Page Pinning and Unpinning | Pending | 1–2 days |
| 8 | Dirty Page Tracking and Eviction | Pending | 2–3 days |
| 9 | Buffer Pool Tests and Integration | Pending | 1–2 days |

**Deliverable:** A persistent page-based storage engine with a buffer pool, page replacement, and dirty-page management.

**Estimated remaining time:** Approximately 11–19 days, depending on how much page-format and metadata work is already complete.

---

# PHASE 2: B+Tree Index

**Goal:** Implement a disk-backed ordered index.

| # | Module | Status | Estimated Time |
|---|---|---|---|
| 10 | B+Tree Node Representation | Pending | 1–2 days |
| 11 | Leaf Node Structure | Pending | 1 day |
| 12 | Internal Node Structure | Pending | 1–2 days |
| 13 | Search and Traversal | Pending | 1–2 days |
| 14 | Insertion | Pending | 1–2 days |
| 15 | Leaf Node Splits | Pending | 1–2 days |
| 16 | Internal Node Splits | Pending | 2–3 days |
| 17 | Root Splits and Tree Height Changes | Pending | 1–2 days |
| 18 | Range Queries and Ordered Scans | Pending | 1–2 days |
| 19 | Persistent B+Tree Reopening | Pending | 1–2 days |
| 20 | B+Tree Integration and Regression Tests | Pending | 1–2 days |
| 21 | B+Tree Deletion and Rebalancing | Optional / Advanced | 3–5 days |

**Deliverable:** A persistent B+Tree supporting point lookups, insertions, and range scans.

**Estimated time:** Approximately 13–22 days without deletion, or 16–27 days with deletion.

---

# PHASE 3: Write-Ahead Logging and Crash Recovery

**Goal:** Protect database changes against crashes and interrupted writes.

| # | Module | Status | Estimated Time |
|---|---|---|---|
| 22 | WAL Record Format | Pending | 1–2 days |
| 23 | WAL Append and Read Operations | Pending | 1–2 days |
| 24 | Log Sequence Numbers (LSNs) | Pending | 1 day |
| 25 | WAL Flush and Durability Rules | Pending | 1–2 days |
| 26 | Recovery Record Processing | Pending | 1–2 days |
| 27 | Redo Recovery | Pending | 1–2 days |
| 28 | Undo Recovery, if supported by the chosen design | Pending | 2–3 days |
| 29 | Crash Simulation and Recovery Tests | Pending | 2–3 days |
| 30 | Checkpointing | Pending | 2–4 days |

**Deliverable:** A database with a defined WAL protocol, restart recovery, and checkpoint support.

**Estimated time:** Approximately 10–18 days for a carefully scoped educational implementation.

**Important:** Define the recovery model before implementation. Redo-only recovery and an ARIES-style undo/redo system have substantially different complexity.

---

# PHASE 4: Transactions and Concurrency

**Goal:** Support concurrent operations and transactional data access.

| # | Module | Status | Estimated Time |
|---|---|---|---|
| 31 | Transaction Representation | Pending | 1–2 days |
| 32 | Transaction Manager | Pending | 2–3 days |
| 33 | Lock Manager | Pending | 3–5 days |
| 34 | Shared and Exclusive Locks | Pending | 1–2 days |
| 35 | Lock Compatibility and Waiting | Pending | 1–2 days |
| 36 | Deadlock Detection or Prevention | Pending | 2–3 days |
| 37 | Transaction Commit and Abort | Pending | 2–3 days |
| 38 | MVCC Version Representation | Pending | 2–3 days |
| 39 | Snapshot Visibility Rules | Pending | 2–3 days |
| 40 | Version Cleanup and Garbage Collection | Pending | 2–4 days |
| 41 | Transaction and Concurrency Tests | Pending | 2–3 days |

**Deliverable:** An educational transaction layer with concurrency control and a defined visibility model.

**Estimated time:** Approximately 15–25 days, depending on the isolation level and recovery guarantees.

---

# PHASE 5: LSM-Tree Storage Engine

**Goal:** Implement an alternative storage architecture optimized for write-heavy workloads.

| # | Module | Status | Estimated Time |
|---|---|---|---|
| 42 | MemTable | Pending | 2–3 days |
| 43 | Sorted MemTable Iteration | Pending | 1 day |
| 44 | SSTable File Format | Pending | 2–3 days |
| 45 | SSTable Lookup and Iteration | Pending | 1–2 days |
| 46 | Sorted-Run Merging | Pending | 1–2 days |
| 47 | Bloom Filter | Pending | 1–2 days |
| 48 | Compaction | Pending | 3–5 days |
| 49 | Tombstones and Deleted Records | Pending | 1–2 days |
| 50 | LSM Recovery and Persistence Tests | Pending | 2–3 days |

**Deliverable:** A basic LSM-Tree key-value storage engine with MemTable, SSTables, Bloom filters, and compaction.

**Estimated time:** Approximately 14–23 days.

---

# PHASE 6: Distributed Database Fundamentals

**Goal:** Explore how database engines coordinate data across multiple nodes.

| # | Module | Status | Estimated Time |
|---|---|---|---|
| 51 | Replication Architecture | Pending | 1–2 days |
| 52 | WAL Shipping and Replica Application | Pending | 2–3 days |
| 53 | Replica State and Lag Tracking | Pending | 1–2 days |
| 54 | Raft Node and Message Representation | Pending | 1–2 days |
| 55 | Leader Election | Pending | 2–3 days |
| 56 | Log Replication | Pending | 2–4 days |
| 57 | Commit Index and Log Consistency | Pending | 2–3 days |
| 58 | Restart and Failure Tests | Pending | 2–4 days |
| 59 | Consistent Hashing | Pending | 1–2 days |
| 60 | Sharding and Key Routing | Pending | 2–3 days |
| 61 | Tablet Placement and Basic Rebalancing | Pending | 3–5 days |

**Deliverable:** An educational distributed-storage prototype with replication, a basic Raft implementation, and sharding.

**Estimated time:** Approximately 20–33 days.

**Important:** Network failures, membership changes, snapshots, and production-grade consensus require additional work. Treat this phase as a separate advanced milestone.

---

# PHASE 7: Core Engine Integration and Finalization

**Goal:** Make the core storage engine coherent, testable, and demonstrable.

| # | Task | Status | Estimated Time |
|---|---|---|---|
| 62 | End-to-End Core Engine Tests | Pending | 2–3 days |
| 63 | Regression and Error-Handling Tests | Pending | 1–2 days |
| 64 | Persistence and Restart Tests | Pending | 1–2 days |
| 65 | Core Storage Benchmarks | Pending | 1–2 days |
| 66 | Architecture Documentation | Pending | 1–2 days |
| 67 | Storage Internals Interview Notes | Pending | 1–2 days |

**Deliverable:** A documented, tested core storage engine with reproducible demonstrations.

**Estimated time:** Approximately 7–13 days.

---

# PHASE 8: Query and Record Access Foundation

**Goal:** Provide a clean interface between database records, indexes, and the storage layer before adding advanced search.

| # | Module | Status | Estimated Time |
|---|---|---|---|
| 68 | Record Representation | Pending | 1–2 days |
| 69 | Record Identifiers and Record Lookup | Pending | 1–2 days |
| 70 | Basic Insert, Read, Update, and Delete Operations | Pending | 2–4 days |
| 71 | Basic Query and Filter Interface | Pending | 2–3 days |
| 72 | Record and Index Integration Tests | Pending | 1–2 days |

**Deliverable:** A basic record-access layer that advanced indexes and search modules can reuse.

**Estimated time:** Approximately 7–13 days.

**Scope note:** Keep this interface minimal. A complete SQL parser and sophisticated query optimizer are not required at this stage.

---

# PHASE 9: Secondary Indexes

**Goal:** Support efficient lookups using fields other than the primary key.

| # | Module | Status | Estimated Time |
|---|---|---|---|
| 73 | Secondary B+Tree Index | Pending | 2–3 days |
| 74 | Hash Index | Pending | 2–3 days |
| 75 | Index Maintenance on Record Updates | Pending | 2–3 days |
| 76 | Index Consistency and Persistence Tests | Pending | 1–2 days |

**Deliverable:** Multiple indexes over stored records, with consistent updates and persistent index data.

**Estimated time:** Approximately 7–11 days.

---

# PHASE 10: Vector Storage and Exact Search

**Goal:** Store numerical embeddings and perform exact nearest-neighbor searches.

| # | Module | Status | Estimated Time |
|---|---|---|---|
| 77 | Vector Data Type and Storage Format | Pending | 1–2 days |
| 78 | Vector Distance Metrics: L2, Cosine, and Dot Product | Pending | 1 day |
| 79 | Brute-Force Vector Search | Pending | 1–2 days |
| 80 | Top-K Nearest-Neighbor Results | Pending | 1 day |
| 81 | Vector Search Tests and Benchmarks | Pending | 1–2 days |

**Deliverable:** A working vector search implementation that serves as the correctness baseline for approximate search.

**Estimated time:** Approximately 4–8 days.

---

# PHASE 11: Approximate Vector Search — HNSW

**Goal:** Accelerate vector search using a graph-based approximate nearest-neighbor index.

| # | Module | Status | Estimated Time |
|---|---|---|---|
| 82 | HNSW Graph Data Structures | Pending | 1–2 days |
| 83 | Multi-Layer Graph Construction | Pending | 2–3 days |
| 84 | Neighbor Selection and Graph Insertion | Pending | 2–4 days |
| 85 | Approximate Nearest-Neighbor Search | Pending | 2–3 days |
| 86 | Search Parameters and Tuning | Pending | 1–2 days |
| 87 | Recall, Latency, and Throughput Benchmarks | Pending | 2–4 days |

**Deliverable:** A working HNSW index with configurable search parameters and benchmarks comparing its results against exact search.

**Estimated time:** Approximately 10–18 days.

---

# PHASE 12: Full-Text Search

**Goal:** Search text documents efficiently and rank results by relevance.

| # | Module | Status | Estimated Time |
|---|---|---|---|
| 88 | Text Tokenization and Normalization | Pending | 1–2 days |
| 89 | Inverted Index | Pending | 2–3 days |
| 90 | Posting Lists and Term Frequencies | Pending | 1–2 days |
| 91 | TF-IDF and BM25 Ranking | Pending | 2–3 days |
| 92 | Query Processing and Result Ranking | Pending | 1–2 days |
| 93 | Full-Text Search Tests and Benchmarks | Pending | 1–2 days |

**Deliverable:** A searchable document collection with ranked results using BM25.

**Estimated time:** Approximately 8–14 days.

---

# PHASE 13: Hybrid Search

**Goal:** Combine keyword-based search with semantic vector search.

| # | Module | Status | Estimated Time |
|---|---|---|---|
| 94 | Unified Search Result Representation | Pending | 1 day |
| 95 | Combining Keyword and Vector Results | Pending | 1–2 days |
| 96 | Reciprocal Rank Fusion (RRF) | Pending | 1–2 days |
| 97 | Hybrid Search Evaluation and Tests | Pending | 1–2 days |

**Deliverable:** Search results combining full-text relevance and vector similarity.

**Estimated time:** Approximately 4–7 days.

---

# PHASE 14: Persistent Vector Index

**Goal:** Integrate vector indexing with MiniDB's persistent storage engine.

| # | Module | Status | Estimated Time |
|---|---|---|---|
| 98 | Persistent Vector Records | Pending | 1–2 days |
| 99 | Persisting HNSW Graph Data | Pending | 2–3 days |
| 100 | Index Loading and Reopening | Pending | 1 day |
| 101 | Vector Index Recovery and Consistency | Pending | 1–2 days |
| 102 | Persistence and Crash-Recovery Tests | Pending | 1–2 days |

**Deliverable:** A vector index that survives database restarts, with recovery and consistency tested against the supported design.

**Estimated time:** Approximately 6–10 days.

**Design note:** Reuse MiniDB's page and WAL infrastructure where appropriate. Define the persistence and recovery strategy before implementing durable HNSW updates.

---

# PHASE 15: Query Execution and Search Integration

**Goal:** Integrate storage, indexing, and search behind a coherent query interface.

| # | Module | Status | Estimated Time |
|---|---|---|---|
| 103 | Query and Search API | Pending | 1–2 days |
| 104 | Index Scan and Filter Execution | Pending | 2–3 days |
| 105 | Top-K Query Execution | Pending | 1–2 days |
| 106 | Basic Query Planning | Pending | 2–3 days |
| 107 | End-to-End Integration Tests | Pending | 2–4 days |

**Deliverable:** A unified interface for record lookups, keyword search, vector search, and hybrid search.

**Estimated time:** Approximately 8–14 days.

---

# PHASE 16: Search Performance and Finalization

**Goal:** Demonstrate the correctness, performance, and engineering quality of MiniDB.

| # | Module | Status | Estimated Time |
|---|---|---|---|
| 108 | Search Benchmark Suite | Pending | 1–2 days |
| 109 | Exact vs. Approximate Search Comparison | Pending | 1–2 days |
| 110 | Recall-Latency Trade-Off Analysis | Pending | 1–2 days |
| 111 | Memory and Storage Usage Analysis | Pending | 1–2 days |
| 112 | Architecture Documentation and Diagrams | Pending | 1–2 days |
| 113 | Search Internals Interview Notes | Pending | 1–2 days |

**Deliverable:** A documented, benchmarked database project demonstrating storage internals, indexing, and search.

**Estimated time:** Approximately 6–12 days.

---

# Consolidated Timeline

The estimates below are approximate planning ranges. Some work can be performed in parallel conceptually, but dependencies mean that the full roadmap is not simply a sum of independent tasks.

| Phase | Scope | Estimated Time |
|---|---|---:|
| 1 | Core Storage Engine | 11–19 days remaining |
| 2 | B+Tree Index | 13–22 days |
| 3 | WAL, Recovery, and Checkpointing | 10–18 days |
| 4 | Transactions and Concurrency | 15–25 days |
| 5 | LSM-Tree Storage Engine | 14–23 days |
| 6 | Distributed Database Fundamentals | 20–33 days |
| 7 | Core Integration and Finalization | 7–13 days |
| 8 | Query and Record Access Foundation | 7–13 days |
| 9 | Secondary Indexes | 7–11 days |
| 10 | Vector Storage and Exact Search | 4–8 days |
| 11 | HNSW Vector Search | 10–18 days |
| 12 | Full-Text Search | 8–14 days |
| 13 | Hybrid Search | 4–7 days |
| 14 | Persistent Vector Index | 6–10 days |
| 15 | Query Execution and Integration | 8–14 days |
| 16 | Search Performance and Finalization | 6–12 days |

**Estimated remaining time for the complete roadmap: approximately 140–250 focused working days**, depending on scope, implementation choices, and integration complexity.

This is a broad estimate for the entire roadmap, including advanced distributed systems. It is not a requirement to finish every phase before the project is useful.

---

# Recommended Milestones

## Milestone 1: Persistent Storage Engine

Complete:
- DiskManager
- PageManager
- Page format
- Buffer Pool
- LRU replacement

**Outcome:** A reliable foundation for storing and managing database pages.

## Milestone 2: Indexed Storage

Complete:
- B+Tree
- Point lookups
- Insertions and node splits
- Range scans
- Persistence tests

**Outcome:** A persistent, indexed storage engine.

## Milestone 3: Durable Database

Complete:
- WAL
- Crash recovery
- Checkpointing
- Recovery tests

**Outcome:** A storage engine with a defined durability and recovery model.

## Milestone 4: Transactional Database

Complete:
- Transaction Manager
- Lock Manager
- MVCC
- Concurrency and visibility tests

**Outcome:** An educational transactional storage engine.

## Milestone 5: Alternative Storage Architecture

Complete:
- MemTable
- SSTable
- Bloom Filter
- Compaction

**Outcome:** An LSM-Tree implementation demonstrating a different storage-engine design.

## Milestone 6: Search-Capable Database

Complete:
- Secondary indexes
- Vector storage
- Exact vector search
- HNSW
- Full-text search
- Hybrid search

**Outcome:** A database engine that supports structured indexing, semantic retrieval, and keyword search.

## Milestone 7: Distributed Database Prototype

Complete:
- Replication
- Raft
- Sharding
- Basic data placement

**Outcome:** An educational distributed-storage prototype.

---

# Career-Focused Priorities

For C++ database internals and storage-engine engineering roles, prioritize:

1. Persistent page storage and buffer management
2. B+Tree indexing
3. WAL and crash recovery
4. Correctness and regression testing
5. Transactions and MVCC
6. LSM-Tree internals
7. Secondary indexes
8. HNSW vector search and benchmarking
9. Full-text and hybrid search
10. Replication, Raft, and sharding

You do not need to complete all 16 phases before applying for jobs.

A thoroughly tested persistent B+Tree with WAL and crash recovery is already a meaningful portfolio milestone. Adding vector and full-text search afterward can broaden the project's relevance to search infrastructure and vector database teams.

---

# Engineering Standards for Every Module

For each module, aim to complete the following before marking it done:

- [ ] Public interface and responsibilities are clear.
- [ ] Implementation is complete for the agreed scope.
- [ ] Unit tests cover normal and boundary cases.
- [ ] Invalid inputs and error conditions are handled.
- [ ] Integration tests pass.
- [ ] Persistence and restart behavior are tested where relevant.
- [ ] Code builds cleanly under C++20 with compiler warnings enabled.
- [ ] Design decisions and limitations are documented.
- [ ] You can explain the implementation without relying on the code alone.

**Definition of done:** A module is complete when its agreed functionality works, its tests pass, and you can explain its design and limitations.

**Final principle:** Build a smaller number of correct, well-tested components before expanding the scope. MiniDB's value comes from demonstrating engineering depth, not merely the number of features listed in the roadmap.