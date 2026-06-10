# Buffer Pool
**Initial scope:** Single-threaded, synchronous I/O, bounded page cache\

## Overview

The Buffer Pool is MiniDB's bounded in-memory cache for database pages.
It reduces repeated reads by keeping recently used pages in memory and
provides a controlled place to track page usage, modifications, and
eviction.

Callers such as the B+Tree should request pages through the Buffer Pool
rather than directly reading the database file. The Buffer Pool uses
PageManager for allocation and validation; PageManager uses DiskManager
for raw page I/O.

The initial implementation is single-threaded. It will not provide
thread safety, background flushing, asynchronous I/O, or crash-safe
transactions.

## Goals

1.  Cache database pages in a bounded amount of memory.
2.  Return a cached page on a cache hit without reading it again from
    storage.
3.  Load and validate a page on a cache miss.
4.  Prevent eviction while a page is pinned.
5.  Track modifications using dirty flags.
6.  Write dirty pages back before their frames are reused.
7.  Keep frame management separate from the replacement-policy
    implementation.
8.  Use small, deterministic tests to validate behavior and failure
    handling.
9.  Preserve compatibility with the existing DiskManager, page format,
    and PageManager.

### Non-goals for the first version

-   Thread safety and concurrent page access.
-   Background or asynchronous flushing.
-   WAL integration and crash recovery.
-   Guaranteed durable writes.
-   Free-page reuse.
-   Advanced replacement algorithms beyond the selected initial policy.
-   Performance tuning before correctness tests pass.

## Architecture

``` text
               B+Tree / page consumers
                         |
                         v
                  BufferPoolManager
             +--------------------------+
             | Page table               |
             | Frames                   |
             | Pin counts               |
             | Dirty flags              |
             | Replacement metadata     |
             +--------------------------+
                         |
                         v
                    PageManager
             allocation and page validation
                         |
                         v
                    DiskManager
                     raw page I/O
                         |
                         v
                    Database file
```

### Component responsibilities

-   **DiskManager:** Reads and writes fixed-size page byte arrays. It
    does not cache pages or choose eviction victims.
-   **PageManager:** Allocates page IDs, persists allocation metadata,
    and validates page headers.
-   **BufferPoolManager:** Owns a bounded collection of frames, maps
    page IDs to resident frames, tracks pins and dirty state, and
    chooses frames for reuse.
-   **Page consumers:** Obtain and release pages through the Buffer
    Pool.

### Frame structure

A frame initially contains one page and its management metadata:

``` cpp
struct Frame {
    DiskManager::Page data{};
    std::uint32_t pin_count{0};
    bool is_dirty{false};
};
```

The implementation also needs to represent whether a frame is occupied
and which page it contains. That state may be stored in the frame or
maintained consistently through the page table and a clearly defined
invalid-page representation. An all-zero page must not be used as an
"unused" marker because all-zero bytes can be valid page content.

### Capacity

The constructor accepts a runtime capacity:

``` cpp
BufferPoolManager(
    DiskManager& disk,
    PageManager& page_manager,
    std::size_t pool_size);
```

The pool owns exactly `pool_size` frame slots. Page bytes alone require
`pool_size * 4096` bytes, excluding metadata and container overhead. A
zero-sized pool should be rejected with `std::invalid_argument`.

### Page table

The page table maps page IDs to frame indices:

``` cpp
std::unordered_map<std::uint64_t, std::size_t> page_table_;
```

For example:

``` text
Page ID -> Frame index
   4    -> 2
   9    -> 0
  12    -> 1
```

A page-table entry exists only while its page is resident. Removing or
replacing a page requires removing the old mapping. Installing a newly
loaded page must not leave a stale mapping if reading or validation
fails.

## Page Cache

The cache has two basic paths:

-   **Cache hit:** The requested page already has a frame. Return that
    frame and pin it.
-   **Cache miss:** Select an available frame, safely write back a dirty
    victim if needed, read and validate the requested page through
    PageManager, and install it in the frame.

The cache is bounded. If every frame is pinned and the requested page is
not already cached, the operation must fail explicitly rather than
evicting a pinned frame.

Core invariants:

1.  A page ID has at most one resident frame.
2.  Every page-table entry points to the correct occupied frame.
3.  Every occupied frame has a consistent page-table entry.
4.  The number of frames never exceeds the configured capacity.
5.  A failed page load does not leave a bogus page-table entry.
6.  Page-table and frame state remain consistent after errors.

## Pin Count

`pin_count` tracks the number of active Buffer Pool users holding a
page.

-   Fetching a page successfully increments its pin count.
-   A caller releases its pin using `unpin_page()`.
-   A frame with a nonzero pin count cannot be evicted.
-   Unpinning a page whose pin count is already zero is an error.
-   Pin-count overflow must be rejected rather than allowed to wrap
    around.

Pinning protects a frame from eviction; it is **not a lock** and does
not make the Buffer Pool thread-safe.

The first implementation may return a frame reference for simplicity,
but callers must obey the pin/unpin contract. Page guards can be
considered later to make release safer.

## Dirty Pages

A page is dirty when its in-memory bytes have been modified since the
last successful write-back.

-   Reading a page does not make it dirty.
-   Modifying a page must mark it dirty.
-   Unpinning with an `is_dirty` argument set to true marks it dirty.
-   A dirty flag is cleared only after the corresponding write-back
    succeeds.
-   A failed write-back must preserve the dirty state and the page
    contents.

The API must clearly define how callers modify page bytes and report
that modification. Returning a mutable frame reference makes this the
caller's responsibility in the initial version.

## Eviction Policy

Eviction chooses a resident frame to reuse when the pool has no unused
frames. A frame with `pin_count > 0` is never eligible.

Frame acquisition should follow this order:

1.  Use an unused frame if one exists.
2.  Otherwise choose an eligible victim according to the configured
    policy.
3.  If the victim is dirty, write it back successfully before
    overwriting it.
4.  Only after successful write-back, remove the old page-table mapping
    and reuse the frame.
5.  If all frames are pinned, report that no frame is available.

If write-back fails, preserve the victim's bytes, dirty flag, and
existing mapping.

### LRU

**Least Recently Used (LRU)** prefers evicting the eligible page that
has gone the longest without being accessed.

The implementation must define what counts as an access. Initially,
successful fetches can update recency metadata; a cache hit and a newly
loaded page should both count as recent use.

Possible implementations:

-   **List-based LRU:** Maintain recency order in a list and update it
    on access. Victim selection is straightforward, but accesses require
    list operations.
-   **Timestamp-based LRU:** Record a monotonically increasing logical
    timestamp for each frame and select the eligible frame with the
    oldest timestamp. This is simple to reason about, although victim
    selection scans frames and timestamp overflow needs consideration.

For the first MiniDB implementation, choose one simple strategy and test
it. A timestamp-based approach is reasonable for a small learning
project; a list-based approach is also acceptable.

### Clock

Clock is an alternative replacement policy that approximates LRU using a
circular scan and a reference bit per frame.

Conceptually:

1.  Maintain a circular clock hand.
2.  Skip pinned frames.
3.  If an eligible frame's reference bit is set, clear it and continue
    scanning.
4.  If an eligible frame's reference bit is clear, select it as the
    victim.
5.  Advance the clock hand as the algorithm proceeds.

On a successful page access, set the frame's reference bit. Clock avoids
maintaining exact recency order and can reduce replacement bookkeeping,
at the cost of only approximating LRU.

**Implementation decision:** document both policies, but implement only
LRU initially. Clock remains a future experiment so we can compare
behavior and complexity after the basic Buffer Pool works.

## Read Path

### Cache hit

1.  Look up the requested page ID in the page table.
2.  If found, verify the mapping is consistent.
3.  Check that the pin count can be incremented without overflow.
4.  Increment the pin count.
5.  Update replacement metadata.
6.  Return the frame.

No storage read is required on a cache hit.

### Cache miss

1.  Validate the page ID according to PageManager's allocation rules.
2.  Find an unused frame or choose an unpinned victim.
3.  If the victim is dirty, write it back and verify success.
4.  Read the requested page through PageManager, which validates the
    page.
5.  Install the bytes and page identity in the chosen frame.
6.  Set `pin_count` to one and `is_dirty` to false.
7.  Add the page-table entry and update replacement metadata.
8.  Return the frame.

The implementation must not destroy a dirty victim before its write-back
succeeds. It should also avoid installing a partial mapping if the
incoming page read fails.

## Write Path

The first version uses write-back caching rather than writing every
modification immediately.

1.  The caller fetches and pins the page.
2.  The caller modifies the page's bytes in memory.
3.  The caller marks the page dirty, either explicitly or when unpinning
    it.
4.  The modified bytes remain cached.
5.  A later explicit flush, flush-all operation, or eviction writes the
    dirty page through PageManager.

This means a successful in-memory modification is not necessarily
persisted to the database file immediately.

The API must make it difficult to forget dirty marking. Initially, tests
should verify that modifications are persisted after a successful flush
and that dirty state remains set after a failed flush.

## Flush Policy

A flush writes dirty cached data to the storage layer. Flushing and
eviction are related but distinct: a page can be flushed while remaining
resident in the cache.

### Initial policy

-   **`flush_page(page_id)`:** Write one resident dirty page. A clean
    page requires no write.
-   **`flush_all_pages()`:** Attempt to write all resident dirty pages.
-   **Eviction:** Write a dirty victim before reusing its frame.
-   **No background flusher:** The first version performs flushes
    synchronously.
-   **Error handling:** Do not clear a dirty flag when its write-back
    fails.

The implementation should define whether flushing a nonresident page is
a no-op or an error, and whether `flush_all_pages()` stops at the first
error or attempts all pages and reports failures. These semantics must
be documented and tested.

### Durability limitation

The current DiskManager's `std::fstream::flush()` does not guarantee
that data has reached stable storage. Strong durability requires
operating-system-level synchronization and, once WAL exists, correct
log-before-data ordering. The initial Buffer Pool must not claim
crash-safe durability.

## Design Decisions

  -----------------------------------------------------------------------
  Topic                   Initial decision        Rationale
  ----------------------- ----------------------- -----------------------
  Concurrency             Single-threaded         Avoid synchronization
                                                  complexity until
                                                  required

  Capacity                Runtime constructor     Makes tiny
                          parameter               deterministic tests
                                                  possible

  Page size               4096 bytes              Matches the existing
                                                  storage format

  Cache lookup            Page ID to frame-index  Efficient direct lookup
                          hash map                

  Pinning                 Explicit pin/unpin      Easy to understand and
                          contract                test initially

  Dirty-page handling     Write-back caching      Avoid writing every
                                                  modification
                                                  immediately

  Replacement policy      LRU first               Straightforward to
                                                  reason about and test

  Clock policy            Document now, implement Enables a later
                          later                   comparison without
                                                  expanding initial scope

  I/O                     Synchronous PageManager Fits the existing
                          calls                   storage stack

  Background flushing     Deferred                Keep first
                                                  implementation small

  Error behavior          Explicit errors;        Prevent silent data
                          preserve dirty data     loss and inconsistent
                                                  cache state

  Page handles            Frame references        Reduces initial
                          initially; consider     abstraction overhead
                          guards later            
  -----------------------------------------------------------------------

## Open Questions

These questions are recorded for deliberate future decisions; they do
not all block the initial implementation.

1.  **Page handles:** Should raw frame references eventually be replaced
    by page guards that automatically unpin?
2.  **LRU representation:** Should we use a list or timestamps? Choose
    one for the first implementation and benchmark only if needed.
3.  **Clock comparison:** When the basic implementation is correct,
    should we implement Clock and compare eviction behavior?
4.  **All frames pinned:** Which error/result type should represent an
    unavailable frame?
5.  **New-page semantics:** Should `new_page()` reserve a frame before
    allocating a page ID, and how should it report allocation success
    when frame acquisition fails?
6.  **Dirty initialization:** Should a newly allocated page start dirty,
    or should the caller mark it dirty after initialization?
7.  **Flush-all semantics:** Should it stop at the first failure or
    attempt all dirty pages and aggregate errors?
8.  **Metrics:** Should we add cache-hit, cache-miss, eviction, and
    write-back counters for debugging and performance experiments?
9.  **Background flushing:** When would a background flusher provide
    enough value to justify its complexity?
10. **Concurrency:** What locking design is appropriate when multiple
    threads begin sharing the Buffer Pool?
11. **Durability:** What guarantees will be required after WAL and
    recovery are implemented?
12. **Page guards and API safety:** How can we make it harder for
    callers to use a frame after unpinning it?

## Testing Plan

Use a small pool, such as three frames, so cache behavior is easy to
verify.

1.  Positive capacity constructs successfully; zero capacity is
    rejected.
2.  Fetching a valid page returns its bytes.
3.  Fetching a cached page reuses its frame and increments the pin
    count.
4.  The pool never exceeds its configured capacity.
5.  Pinned frames are never evicted.
6.  A cache miss fails cleanly when every frame is pinned.
7.  Dirty modifications are persisted after a successful flush.
8.  A failed write-back preserves the victim page and dirty flag.
9.  LRU evicts the least recently used eligible page.
10. A failed page read does not leave a bogus mapping.
11. Unpinning a page with a zero pin count is rejected.
12. Pin-count overflow is rejected.
13. `flush_all_pages()` follows its documented error behavior.
14. All existing DiskManager, page-format, and PageManager tests
    continue to pass.

## Implementation Sequence

Implement one step at a time and run tests after each meaningful change.

1.  Define `Frame` and `BufferPoolManager`.
2.  Implement constructor and capacity validation.
3.  Add page identity/occupancy state and the page table.
4.  Implement cache-hit fetch.
5.  Implement cache-miss fetch.
6.  Implement pin/unpin and error handling.
7.  Add dirty tracking and `flush_page()`.
8.  Add `flush_all_pages()`.
9.  Implement safe frame selection and LRU eviction.
10. Implement new-page allocation.
11. Add failure-path tests and review all invariants.

**Definition of done:** The Buffer Pool is bounded, caches pages
correctly, never evicts pinned pages, preserves dirty data when
write-back fails, validates loaded pages, passes its tests, and keeps
the existing MiniDB test suite green.
