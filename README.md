# MiniDB

MiniDB is a database systems project written in modern C++ that incrementally builds the core components behind modern storage engines and distributed databases.

The project starts with page-based storage, indexing, caching, logging, and recovery, and gradually evolves into a transactional storage engine, an LSM-based database, a replicated datastore, and eventually a distributed database inspired by systems such as PostgreSQL, Aurora, AlloyDB, Bigtable, DynamoDB, CockroachDB, YugabyteDB, Spanner, GFS, and Colossus.

---

# Vision

Most modern databases are built from a small set of fundamental building blocks:

* Storage Engines
* Indexes
* Buffer Pools
* Write-Ahead Logging
* Recovery
* Transactions
* MVCC
* Replication
* Consensus
* Distributed Storage

MiniDB aims to implement these building blocks from first principles to develop a deeper understanding of database internals and distributed systems.

---

# Current Scope

MiniDB is implemented incrementally in phases.

The initial focus is on building a single-node storage engine with:

* Durable Storage
* B+Tree Indexing
* Buffer Pool Management
* Write-Ahead Logging
* Crash Recovery
* Transactions
* MVCC

Distributed features such as replication, consensus, sharding, and distributed transactions are planned as future phases.

---

# System Evolution

```text
Storage Engine
      ↓
Transactions
      ↓
LSM Storage
      ↓
Replication
      ↓
Consensus
      ↓
Distributed Storage
      ↓
Distributed Database
```

---

# Core Components

```text
MiniDB
│
├── Common
│   ├── Disk Manager
│   ├── Page Manager
│   ├── Buffer Pool
│   ├── WAL
│   └── Recovery
│
├── BTree Engine
│   ├── B+Tree
│   └── Checkpointing
│
├── LSM Engine
│   ├── MemTable
│   ├── SSTable
│   ├── Bloom Filter
│   └── Compaction
│
├── Transaction Layer
│   ├── Transaction Manager
│   ├── MVCC
│   └── Lock Manager
│
├── Replication
│   └── WAL Shipping
│
├── Consensus
│   └── Raft
│
└── Distributed Storage
    ├── Sharding
    ├── Partitioning
    └── Replica Placement
```

---

# Architecture

```text
                           Client
                              |
                              v
                        KV Store API
                              |
      +-----------------------+-----------------------+
      |                                               |
      v                                               v

  BTree Engine                                  LSM Engine
      |                                               |
      v                                               v

  Buffer Pool                            MemTable / SSTables
      |                                               |
      +-----------------------+-----------------------+
                              |
                              v

                      Storage Layer
                              |
              +---------------+---------------+
              |                               |
              v                               v

             WAL                         Data Files
              |
              v

          Recovery
              |
              v

        Replication
              |
              v

            Raft
```

---

# Planned Components

## Common Infrastructure

* Disk Manager
* Page Manager
* Buffer Pool
* Write-Ahead Logging (WAL)
* Recovery Manager
* Checkpointing

Inspired by:

* PostgreSQL
* InnoDB
* Aurora

---

## BTree Storage Engine

* B+Tree Index
* Page Splits
* Root Splits
* Range Scans

Inspired by:

* PostgreSQL
* InnoDB
* Aurora

---

## Transaction Layer

* Lock Manager
* MVCC
* Transaction Manager
* Snapshot Isolation

Inspired by:

* PostgreSQL
* Spanner
* CockroachDB

---

## LSM Storage Engine

* MemTable
* SSTables
* Bloom Filters
* Compaction

Inspired by:

* Bigtable
* RocksDB
* Cassandra
* DynamoDB

---

## Replication Layer

* WAL Shipping
* Leader-Follower Replication
* Replica Recovery
* Failover

Inspired by:

* PostgreSQL Streaming Replication
* Aurora

---

## Consensus Layer

* Leader Election
* Heartbeats
* Log Replication
* Membership Changes

Inspired by:

* Raft
* etcd
* CockroachDB
* YugabyteDB

---

## Distributed Storage

* Sharding
* Partition Management
* Consistent Hashing
* Replica Placement

Inspired by:

* Bigtable
* DynamoDB
* Colossus

---

## Distributed Database

* Distributed Transactions
* Tablet Management
* Multi-Node Query Routing
* Fault Tolerance

Inspired by:

* Spanner
* CockroachDB
* YugabyteDB

---

# Development Roadmap

## Phase 1 – Storage Engine Foundations

* Disk Manager
* Page Manager
* Buffer Pool
* B+Tree Index
* KV Store API

## Phase 2 – Durability & Recovery

* WAL
* Recovery
* Checkpointing

## Phase 3 – Transactions & Concurrency

* Lock Manager
* MVCC
* Transaction Manager

## Phase 4 – LSM Storage Engine

* MemTable
* SSTables
* Bloom Filters
* Compaction

## Phase 5 – Replication

* WAL Shipping
* Replica Recovery
* Failover

## Phase 6 – Consensus

* Raft Leader Election
* Log Replication

## Phase 7 – Distributed Storage

* Sharding
* Partitioning
* Replica Placement

## Phase 8 – Distributed Database

* Distributed Transactions
* Tablet Management
* Query Routing

---

# Repository Structure

```text
MiniDB

├── README.md
│
├── docs
│   ├── architecture.md
│   ├── storage.md
│   ├── bufferpool.md
│   ├── btree.md
│   ├── wal.md
│   ├── recovery.md
│   ├── mvcc.md
│   ├── transactions.md
│   ├── lsm.md
│   ├── replication.md
│   ├── raft.md
│   └── distributed.md
│
├── data
│
└── src
    ├── api
    ├── common
    ├── storage
    │   ├── btree
    │   └── lsm
    ├── transaction
    ├── replication
    ├── consensus
    └── distributed
```

---

# Learning Goals

This project is intended to provide hands-on understanding of:

* Database Internals
* Storage Engine Design
* Recovery Mechanisms
* Concurrency Control
* Distributed Systems
* Consensus Algorithms
* Replication Protocols
* Large Scale Data Storage

---

🚧 Project under active development.

