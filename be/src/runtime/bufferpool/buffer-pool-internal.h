// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

// This file includes definitions of classes used internally in the buffer pool.
//
/// +========================+
/// | IMPLEMENTATION NOTES   |
/// +========================+
///
/// Lock Ordering
/// =============
/// The lock acquisition order is:
/// 1. Client::lock_
/// 2. FreeBufferArena::lock_. If multiple arena locks are acquired, must be acquired in
///    ascending order.
/// 3. Page::lock
///
/// If a reference to a Page is acquired through a page list, the Page* reference only
/// remains valid so long as list's lock is held.
///
/// Page States
/// ===========
/// Each Page object is owned by at most one InternalList<Page> at any given point.
/// Each page is either pinned or unpinned. Unpinned has a number of sub-states, which
/// is determined by which list in Client/BufferPool contains the page.
/// * Pinned: Always in this state when 'pin_count' > 0. The page has a buffer and is in
///     Client::pinned_pages_. 'pin_in_flight' determines which sub-state the page is in:
///   -> When pin_in_flight=false, the buffer contains the page's data and the client can
///      read and write to the buffer.
///   -> When pin_in_flight=true, the page's data is in the process of being read from
///      scratch disk into the buffer. Clients will block on the read I/O if they attempt
///      to access the buffer.
/// * Unpinned - Dirty: When no write to scratch has been started for an unpinned page.
///     The page is in Client::dirty_unpinned_pages_.
/// * Unpinned - Write in flight: When the write to scratch has been started but not
///     completed for a dirty unpinned page. The page is in
///     Client::write_in_flight_pages_. For accounting purposes this is considered a
///     dirty page.
/// * Unpinned - Clean: When the write to scratch has completed but the page was not
///     evicted. The page is in a clean pages list in a BufferAllocator arena.
/// * Unpinned - Evicted: After a clean page's buffer has been reclaimed. The page is
///     not in any list.
///
/// Page Eviction Policy
/// ====================
/// The page eviction policy is designed so that clients that run only in-memory (i.e.
/// don't unpin pages) never block on I/O. To achieve this, we must be able to
/// fulfil reservations by either allocating buffers or evicting clean pages. Assuming
/// reservations are not overcommitted (they shouldn't be), this global invariant can be
/// maintained by enforcing a local invariant for every client:
///
///   reservation >= BufferHandles returned to client
//                   + pinned pages + dirty pages (dirty unpinned or write in flight)
///
/// The local invariant is maintained by writing pages to disk as the first step of any
/// operation that allocates a new buffer or reclaims buffers from clean pages. I.e.
/// "dirty pages" must be decreased before one of the other values on the R.H.S. of the
/// invariant can be increased. Operations block waiting for enough writes to complete
/// to satisfy the invariant.

#pragma once

#include <iosfwd>
#include <memory>
#include <mutex>

#include "runtime/bufferpool/buffer-pool-counters.h"
#include "runtime/bufferpool/buffer-pool.h"
#include "runtime/bufferpool/reservation-tracker.h"
#include "util/condition-variable.h"
#include "util/internal-queue.h"
#include "util/spinlock.h"

// Ensure that DCheckConsistency() function calls get removed in release builds.
#ifndef NDEBUG
#define DCHECK_CONSISTENCY() DCheckConsistency()
#else
#define DCHECK_CONSISTENCY()
#endif

namespace impala {

class TmpFileGroup;
class TmpWriteHandle;

/// The internal representation of a page, which can be pinned or unpinned. See the
/// class comment for explanation of the different page states.
struct BufferPool::Page : public InternalList<Page>::Node {
  // Define constructor and destructor out-of-line to avoid include of TmpWriteHandle
  // body in header.
  Page(Client* client, int64_t len);
  ~Page();

  std::string DebugString();

  // Helper for BufferPool::DebugString().
  static bool DebugStringCallback(std::stringstream* ss, BufferPool::Page* page);

  /// The client that the page belongs to.
  Client* const client;

  /// The length of the page in bytes.
  const int64_t len;

  /// The pin count of the page. Only accessed in contexts that are passed the associated
  /// PageHandle, so it cannot be accessed by multiple threads concurrently.
  int pin_count;

  /// True if the read I/O to pin the page was started but not completed. Only accessed
  /// in contexts that are passed the associated PageHandle, so can only be accessed
  /// by multiple threads concurrently via PageHandle::GetBuffer(), since other page
  /// handle operators are not thread-safe. This is atomic so that GetBuffer() can do
  /// optimistic checks to avoid acquiring 'buffer_lock'.
  AtomicBool pin_in_flight;

  /// Non-null if there is a write in flight, the page is clean, or the page is evicted.
  std::unique_ptr<TmpWriteHandle> write_handle;

  /// Condition variable signalled when a write for this page completes. Protected by
  /// client->lock_.
  ConditionVariable write_complete_cv_;

  /// This lock must be held when accessing 'buffer' if the page is unpinned and not
  /// evicted (i.e. it is safe to access 'buffer' if the page is pinned or evicted).
  SpinLock buffer_lock;

  /// Buffer with the page's contents. Closed only iff page is evicted. Open otherwise.
  BufferHandle buffer;
};

/// Wrapper around InternalList<Page> that tracks the # of bytes in the list.
class BufferPool::PageList {
 public:
  PageList() : bytes_(0) {}
  ~PageList() {
    // Clients always empty out their list before destruction.
    DCHECK(list_.empty());
    DCHECK_EQ(0, bytes_);
  }

  void Enqueue(Page* page) {
    list_.Enqueue(page);
    bytes_ += page->len;
  }

  bool Remove(Page* page) {
    if (list_.Remove(page)) {
      bytes_ -= page->len;
      return true;
    }
    return false;
  }

  Page* Dequeue() {
    Page* page = list_.Dequeue();
    if (page != nullptr) {
      bytes_ -= page->len;
    }
    return page;
  }

  Page* PopBack() {
    Page* page = list_.PopBack();
    if (page != nullptr) {
      bytes_ -= page->len;
    }
    return page;
  }

  void Iterate(const boost::function<bool(Page*)>& fn) { list_.Iterate(fn); }
  void IterateFirstN(const boost::function<bool(Page*)>& fn, int n) {
    list_.IterateFirstN(fn, n);
  }
  bool Contains(Page* page) { return list_.Contains(page); }
  Page* tail() { return list_.tail(); }
  bool empty() const { return list_.empty(); }
  int size() const { return list_.size(); }
  int64_t bytes() const { return bytes_; }

  void DCheckConsistency() {
    DCHECK_GE(bytes_, 0);
    DCHECK_EQ(list_.empty(), bytes_ == 0);
  }

 private:
  InternalList<Page> list_;
  int64_t bytes_;
};

/// The internal state for the client.
class BufferPool::Client {
 public:
  Client(BufferPool* pool, TmpFileGroup* file_group, const string& name,
      ReservationTracker* parent_reservation, MemTracker* mem_tracker,
      MemLimit mem_limit_mode, int64_t reservation_limit, RuntimeProfile* profile);

  ~Client() {
    DCHECK_EQ(0, num_pages_);
    DCHECK_EQ(0, buffers_allocated_bytes_);
  }

  /// Release reservation for this client.
  void Close() { reservation_.Close(); }

  /// Create a pinned page using 'buffer', which was allocated using AllocateBuffer().
  /// No client or page locks should be held by the caller.
  Page* CreatePinnedPage(BufferHandle&& buffer);

  /// Reset 'handle', clean up references to handle->page and release any resources
  /// associated with handle->page. If the page is pinned, 'out_buffer' can be passed in
  /// and the page's buffer will be returned.
  /// Neither the client's lock nor handle->page_->buffer_lock should be held by the
  /// caller.
  void DestroyPageInternal(PageHandle* handle, BufferHandle* out_buffer = NULL);

  /// Updates client state to reflect that 'page' is now a dirty unpinned page. May
  /// initiate writes for this or other dirty unpinned pages.
  /// Neither the client's lock nor page->buffer_lock should be held by the caller.
  void MoveToDirtyUnpinned(Page* page);

  /// Move an unpinned page to the pinned state, moving between data structures and
  /// reading from disk if necessary. Ensures the page has a buffer. If the data is
  /// already in memory, ensures the data is in the page's buffer. If the data is on
  /// disk, starts an async read of the data and sets 'pin_in_flight' on the page to
  /// true. Neither the client's lock nor page->buffer_lock should be held by the caller.
  Status StartMoveToPinned(ClientHandle* client, Page* page) WARN_UNUSED_RESULT;

  /// Moves a page that has a pin in flight back to the evicted state, undoing
  /// StartMoveToPinned(). Neither the client's lock nor page->buffer_lock should be held
  /// by the caller.
  void UndoMoveEvictedToPinned(Page* page);

  /// Finish the work of bring the data of an evicted page to memory if
  /// page->pin_in_flight was set to true by StartMoveToPinned().
  Status FinishMoveEvictedToPinned(Page* page) WARN_UNUSED_RESULT;

  /// Must be called once before allocating a buffer of 'len' via the AllocateBuffer() or
  /// AllocateUnreservedBuffer() APIs. Deducts from the client's reservation and updates
  /// internal accounting. Cleans dirty pages if needed to satisfy the buffer pool's
  /// internal invariants. No page or client locks should be held by the caller.
  /// If 'reserved' is true, we assume that the memory is already reserved. If it is
  /// false, tries to increase the reservation if needed.
  ///
  /// On success, returns OK and sets 'success' to true if non-NULL. If an error is
  /// encountered, e.g. while cleaning pages, returns an error status. If the reservation
  /// could not be increased for an unreserved allocation, returns OK and sets 'success'
  /// to false (for unreserved allocations, 'success' must be non-NULL).
  Status PrepareToAllocateBuffer(
      int64_t len, bool reserved, bool* success) WARN_UNUSED_RESULT;

  /// Implementation of ClientHandle::DecreaseReservationTo().
  Status DecreaseReservationTo(int64_t max_decrease, int64_t target_bytes) WARN_UNUSED_RESULT;

  /// Implementations of ClientHandle::TransferReservationTo().
  Status TransferReservationTo(ReservationTracker* dst, int64_t bytes, bool* transferred);

  /// Called after a buffer of 'len' is freed via the FreeBuffer() API to update
  /// internal accounting and release the buffer to the client's reservation. No page or
  /// client locks should be held by the caller.
  void FreedBuffer(int64_t len) {
    std::lock_guard<std::mutex> cl(lock_);
    reservation_.ReleaseTo(len);
    buffers_allocated_bytes_ -= len;
    DCHECK_CONSISTENCY();
  }

  /// Wait for the in-flight write for 'page' to complete.
  /// 'lock_' must be held by the caller via 'client_lock'. page->buffer_lock should
  /// not be held.
  void WaitForWrite(std::unique_lock<std::mutex>* client_lock, Page* page);

  /// Test helper: wait for all in-flight writes to complete.
  /// 'lock_' must not be held by the caller.
  void WaitForAllWrites();

  /// Asserts that 'client_lock' is holding 'lock_'.
  void DCheckHoldsLock(const std::unique_lock<std::mutex>& client_lock) {
    DCHECK(client_lock.mutex() == &lock_ && client_lock.owns_lock());
  }

  int64_t min_buffer_len() const { return pool_->min_buffer_len(); }
  ReservationTracker* reservation() { return &reservation_; }
  const BufferPoolClientCounters& counters() const { return counters_; }
  bool spilling_enabled() const { return file_group_ != NULL; }
  void set_debug_write_delay_ms(int val) { debug_write_delay_ms_ = val; }
  bool has_unpinned_pages() const {
    // Safe to read without lock since other threads should not be calling BufferPool
    // functions that create, destroy or unpin pages.
    return pinned_pages_.size() < num_pages_;
  }

  /// Print debugging info about the state of the client. Caller must not hold 'lock_'.
  std::string DebugString();

 private:
  // Check consistency of client, DCHECK if inconsistent. 'lock_' must be held.
  void DCheckConsistency() {
    DCHECK_GE(buffers_allocated_bytes_, 0) << DebugStringLocked();
    pinned_pages_.DCheckConsistency();
    dirty_unpinned_pages_.DCheckConsistency();
    in_flight_write_pages_.DCheckConsistency();
    DCHECK_LE(pinned_pages_.size() + dirty_unpinned_pages_.size()
            + in_flight_write_pages_.size(),
        num_pages_) << DebugStringLocked();
    // Check that we flushed enough pages to disk given our eviction policy.
    DCHECK_GE(reservation_.GetReservation(), buffers_allocated_bytes_
            + pinned_pages_.bytes() + dirty_unpinned_pages_.bytes()
            + in_flight_write_pages_.bytes()) << DebugStringLocked();
  }

  /// Must be called once before allocating or reclaiming a buffer of 'len'. Ensures that
  /// enough dirty pages are flushed to disk to satisfy the buffer pool's internal
  /// invariants after the allocation. 'lock_' should be held by the caller via
  /// 'client_lock'. If 'lazy_flush' is true, only write out pages if needed to reclaim
  /// 'len', and do not return a write error if the error prevents flushing enough pages.
  Status CleanPages(std::unique_lock<std::mutex>* client_lock, int64_t len,
      bool lazy_flush = false);

  /// Initiates asynchronous writes of dirty unpinned pages to disk. Ensures that at
  /// least 'min_bytes_to_write' bytes of writes will be written asynchronously. May
  /// start writes more aggressively so that I/O and compute can be overlapped. If
  /// any errors are encountered, 'write_status_' is set. 'write_status_' must therefore
  /// be checked before reading back any pages. 'lock_' must be held by the caller.
  void WriteDirtyPagesAsync(int64_t min_bytes_to_write = 0);

  /// Called when a write for 'page' completes.
  void WriteCompleteCallback(Page* page, const Status& write_status);

  /// Move an evicted page to the pinned state by allocating a new buffer, starting an
  /// async read from disk and moving the page to 'pinned_pages_'. client->impl must be
  /// locked by the caller via 'client_lock' and handle->page must be unlocked.
  /// 'client_lock' is released then reacquired.
  Status StartMoveEvictedToPinned(
      std::unique_lock<std::mutex>* client_lock, ClientHandle* client, Page* page);

  /// Same as DebugString() except the caller must already hold 'lock_'.
  std::string DebugStringLocked();

  /// The buffer pool that owns the client.
  BufferPool* const pool_;

  /// The file group that should be used for allocating scratch space. If NULL, spilling
  /// is disabled.
  TmpFileGroup* const file_group_;

  /// A name identifying the client.
  const std::string name_;

  /// The reservation tracker for the client. All pages pinned by the client count as
  /// usage against 'reservation_'.
  ReservationTracker reservation_;

  /// The RuntimeProfile counters for this client, owned by the client's RuntimeProfile.
  /// All non-NULL.
  BufferPoolClientCounters counters_;

  /// Debug option to delay write completion.
  int debug_write_delay_ms_;

  /// Lock to protect the below member variables;
  std::mutex lock_;

  /// Condition variable signalled when a write for this client completes.
  ConditionVariable write_complete_cv_;

  /// Used to ensure that only one thread at a time is active in CleanPages().
  bool cleaning_pages_ = false;
  ConditionVariable clean_pages_done_cv_;

  /// All non-OK statuses returned by write operations are merged into this status.
  /// All operations that depend on pages being written to disk successfully (e.g.
  /// reading pages back from disk) must check 'write_status_' before proceeding, so
  /// that write errors that occurred asynchronously are correctly propagated. The
  /// write error is global to the client so can be propagated to any Status-returning
  /// operation for the client (even for operations on different Pages or Buffers).
  /// Write errors are not recoverable so it is best to propagate them as quickly
  /// as possible, instead of waiting to propagate them in a specific way.
  Status write_status_;

  /// Total number of pages for this client. Used for debugging and enforcing that all
  /// pages are destroyed before the client.
  int64_t num_pages_;

  /// Total bytes of buffers in BufferHandles returned to clients (i.e. obtained from
  /// AllocateBuffer() or ExtractBuffer()).
  int64_t buffers_allocated_bytes_;

  /// All pinned pages for this client.
  PageList pinned_pages_;

  /// Dirty unpinned pages for this client for which writes are not in flight. Page
  /// writes are started in LIFO order, because operators typically have sequential access
  /// patterns where the most recently evicted page will be last to be read.
  PageList dirty_unpinned_pages_;

  /// Dirty unpinned pages for this client for which writes are in flight.
  PageList in_flight_write_pages_;
};
}
