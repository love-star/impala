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

#ifndef IMPALA_RUNTIME_BUFFERED_TUPLE_STREAM_H
#define IMPALA_RUNTIME_BUFFERED_TUPLE_STREAM_H

#include <set>
#include <vector>
#include <boost/function.hpp>
#include <boost/scoped_ptr.hpp>

#include "common/atomic.h"
#include "common/compiler-util.h"
#include "common/global-types.h"
#include "common/status.h"
#include "gutil/macros.h"
#include "runtime/bufferpool/buffer-pool.h"
#include "runtime/row-batch.h"

namespace impala {

class MemTracker;
class RuntimeState;
class RowDescriptor;
class SlotDescriptor;
class Tuple;
class TupleRow;

/// Class that provides an abstraction for a stream of tuple rows backed by BufferPool
/// Pages. Rows can be added to the stream and read back. Rows are returned in the order
/// they are added.
///
/// The BufferedTupleStream is *not* thread safe from the caller's point of view.
/// Different threads should not concurrently call methods of the same BufferedTupleStream
/// object.
///
/// Reading and writing the stream:
/// The stream supports two modes of reading/writing, depending on whether
/// PrepareForWrite() is called to initialize a write iterator only or
/// PrepareForReadWrite() is called to initialize both read and write iterators to enable
/// interleaved reads and writes.
///
/// To use write-only mode, PrepareForWrite() is called once and AddRow()/AddRowCustom*()
/// are called repeatedly to initialize then advance a write iterator through the stream.
/// Once the stream is fully written, it can be read back by calling PrepareForRead()
/// then GetNext() repeatedly to advance a read iterator through the stream. If the
/// stream is pinned, external read iterators can be created with
/// PrepareForPinnedRead(). Multiple external read iterators can be active at the same
/// time, reading from different positions in the stream. External read iterators are
/// thread-safe, which allows safe concurrent access to the stream from different threads.
///
/// To use read/write mode, PrepareForReadWrite() is called once to initialize the read
/// and write iterators. AddRow()/AddRowCustom*() then advance a write iterator through
/// the stream, and GetNext() advances a trailing read iterator through the stream.
///
/// Buffer management:
/// The tuple stream is backed by a sequence of BufferPool Pages. The tuple stream uses
/// the client's reservation to pin pages in memory. It will automatically try to
/// increase the client's reservation whenever it needs to do so to make progress.
///
/// Normally pages are all of the same default page length, but larger pages up to the
/// max page length are used if needed to store rows that are too large for a
/// default-length page.
///
/// The stream has both pinned and unpinned modes. In the pinned mode all pages are
/// pinned for reading. The pinned mode avoids I/O by keeping all pages pinned in memory
/// and allows clients to save pointers to rows in the stream and randomly access them.
/// E.g. hash tables can be backed by a BufferedTupleStream. In the unpinned mode, only
/// pages currently being read and written are pinned and other pages are unpinned and
/// therefore do not use the client's reservation and can be spilled to disk. The stream
/// always holds onto a default page's worth of reservation for the read and write
/// iterators (i.e. two page's worth if the stream is in read/write mode), even if that
/// many pages are not currently pinned. This means that UnpinStream() always succeeds,
/// and moving to the next default-length write page or read page on an unpinned stream
/// does not require additional reservation. This is implemented by saving reservations
/// in SubReservations.
///
/// To read or write a row larger than the default page size to/from an unpinned stream,
/// the client must have max_page_len - default_page_len unused reservation. Writing a
/// large row to an unpinned stream only uses the reservation for the duration of the
/// AddRow()/AddRowCustom*() call. Reading a large row from an unpinned stream uses the
/// reservation until the next call to GetNext(). E.g. to partition a single unpinned
/// stream into n unpinned streams, the reservation needed is (n - 1) *
/// default_page_len + 2 * max_page_len: one large read buffer and one large write
/// buffer is needed to keep the row being processed in-memory, but only default-sized
/// buffers are needed for the other streams being written.
///
/// The tuple stream also supports a 'attach_on_read' mode, enabled by passing a flag
/// to PrepareForRead() which attaches the stream's pages to the output batch as it
/// does a final destructive read pass over the stream.
///
/// Page layout:
/// Rows are stored back to back starting at the first byte of each page's buffer, with
/// no interleaving of data from different rows. There is no padding or alignment
/// between rows. Rows larger than the default page length are stored on their own
/// page.
///
/// Tuple row layout:
/// If the stream's tuples are nullable (i.e. has_nullable_tuple_ is true), there is a
/// bitstring at the start of each row with null indicators for all tuples in each row
/// (including non-nullable tuples). The bitstring occupies ceil(num_tuples_per_row / 8)
/// bytes. A 1 indicates the tuple is null.
///
/// The fixed length parts of the row's tuples are stored first, followed by var len data
/// for inlined_string_slots_ and inlined_coll_slots_. Other "external" var len slots can
/// point to var len data outside the stream. When reading the stream, the length of each
/// row's var len data in the stream must be computed to find the next row's start.
///
/// The tuple stream supports reading from the stream into RowBatches without copying
/// out any data: the RowBatches' Tuple pointers will point directly into the stream's
/// pages' buffers. The fixed length parts follow Impala's internal tuple format, so for
/// the tuple to be valid, we only need to update pointers to point to the var len data
/// in the stream. These pointers need to be updated by the stream because a spilled
/// page's data may be relocated to a different buffer. The pointers are updated lazily
/// upon reading the stream via GetNext().
///
/// Example layout for a row with two non-nullable tuples ((1, "hello"), (2, "world"))
/// with all var len data stored in the stream:
///  <---- tuple 1 -----> <------ tuple 2 ------> <- var len -> <- next row ...
/// +--------+-----------+-----------+-----------+-------------+
/// | IntVal | StringVal | BigIntVal | StringVal |             | ...
/// +--------+-----------+-----------+-----------++------------+
/// | val: 1 | len: 5    | val: 2    | len: 5    | helloworld  | ...
/// |        | ptr: 0x.. |           | ptr: 0x.. |             | ...
/// +--------+-----------+-----------+-----------+-------------+
///  <--4b--> <---12b---> <----8b---> <---12b---> <----10b---->
///
/// Example layout for a row with the second tuple nullable ((1, "hello"), NULL)
/// with all var len data stored in the stream:
/// <- null tuple bitstring -> <---- tuple 1 -----> <- var len -> <- next row ...
/// +-------------------------+--------+-----------+------------+
/// |                         | IntVal | StringVal |            | ...
/// +-------------------------+--------+-----------+------------+
/// | 0000 0010               | val: 1 | len: 5    | hello      | ...
/// |                         |        | ptr: 0x.. |            | ...
/// +-------------------------+--------+-----------+------------+
///  <---------1b------------> <--4b--> <---12b---> <----5b---->
///
/// Example layout for a row with a single non-nullable tuple (("hello", "world")) with
/// the second string slot stored externally to the stream:
///  <------ tuple 1 ------> <- var len ->  <- next row ...
/// +-----------+-----------+-------------+
/// | StringVal | StringVal |             | ...
/// +-----------+-----------+-------------+
/// | len: 5    | len: 5    |  hello      | ...
/// | ptr: 0x.. | ptr: 0x.. |             | ...
/// +-----------+-----------+-------------+
///  <---12b---> <---12b---> <-----5b---->
///
/// The behavior of reads and writes is as follows:
/// Read:
///   1. Unpinned: Only a single read page is pinned at a time. This means that only
///     enough reservation to pin a single page is needed to read the stream, regardless
///     of the stream's size. Each page is attached or unpinned (if attach on read is true
///     or false respectively) before advancing to the next page.
///   2. Pinned: All pages in the stream are pinned so do not need to be pinned or
///     unpinned when reading from the stream. If attach on read is true, pages are
///     attached after being read. If the stream was previously unpinned, the page's data
///     may not yet be in memory - reading from the stream can block on I/O or fail with
///     an I/O error.
/// Write:
///   1. Unpinned: Unpin pages as they fill up. This means that only a enough reservation
///     to pin a single write page is required to write to the stream, regardless of the
///     stream's size.
///   2. Pinned: Pages are left pinned. If the next page in the stream cannot be pinned
///     because the client's reservation is insufficient (and could not be increased by
///     the stream), the read call will fail and the client can either unpin the stream
///     or free up other memory before retrying.
///
/// Memory lifetime of rows read from stream:
/// There are several cases.
/// 1. If the stream is pinned and attach on read is false, it is valid to access any
///    tuples returned via GetNext() until the stream is unpinned.
/// 2. If the stream is in attach on read mode, all buffers referenced by returned rows
///    are attached to the batches by that GetNext() call or a subsequent one. The
///    caller is responsible for managing the lifetime of those buffers.
/// 3. If the stream is unpinned and not in attach on read mode, then the batch returned
///    from GetNext() may have the FLUSH_RESOURCES flag set, which means that any tuple
///    memory returned so far from the stream may be freed on the next call to GetNext().
///    It is *not* safe to return references to rows returned in this mode outside of
///    the ExecNode.
///
/// Manual construction of rows with AddRowCustomBegin()/AddRowCustomEnd():
/// The BufferedTupleStream supports allocation of uninitialized rows with
/// AddRowCustom*(). AddRowCustomBegin() is called instead of AddRow() if the client wants
/// to manually construct a row. The caller of AddRowCustomBegin() is responsible for
/// writing the row with exactly the layout described above then calling
/// AddRowCustomEnd() when done.
///
/// If a caller constructs a tuple in this way, the caller can set the pointers and they
/// will not be modified until the stream is read via GetNext().
/// TODO: IMPALA-5007: try to remove AddRowCustom*() by unifying with AddRow().
///
/// TODO: prefetching for pages could speed up iteration over unpinned streams.
class BufferedTupleStream {
 public:
  class ReadIterator;
  /// A pointer to the start of a flattened TupleRow in the stream.
  typedef uint8_t* FlatRowPtr;

  /// row_desc: description of rows stored in the stream. This is the desc for rows
  /// that are added and the rows being returned.
  /// page_len: the size of pages to use in the stream
  /// ext_varlen_slots: set of varlen slots with data stored externally to the stream
  BufferedTupleStream(RuntimeState* state, const RowDescriptor* row_desc,
      BufferPool::ClientHandle* buffer_pool_client, int64_t default_page_len,
      int64_t max_page_len,
      const std::set<SlotId>& ext_varlen_slots = std::set<SlotId>());

  ~BufferedTupleStream() { DCHECK(closed_); }

  /// Initializes the tuple stream object on behalf of a caller uniquely identified by a
  /// 'caller_label'. Must be called once before any of the other APIs.
  /// If 'pinned' is true, the tuple stream starts off pinned, otherwise it is unpinned.
  /// 'caller_label' is only used for error reporting.
  Status Init(const std::string& caller_label, bool pinned);

  /// Prepares the stream for writing by saving enough reservation for a default-size
  /// write page. Tries to increase reservation if there is not enough unused reservation
  /// for a page. Called after Init() and before the first AddRow() or
  /// AddRowCustomBegin() call.
  /// 'got_reservation': set to true if there was enough reservation to initialize the
  ///     first write page and false if there was not enough reservation and no other
  ///     error was encountered. Undefined if an error status is returned.
  Status PrepareForWrite(bool* got_reservation);

  /// Prepares the stream for interleaved reads and writes by saving enough reservation
  /// for default-sized read and write pages. Called after Init() and before the first
  /// AddRow() or AddRowCustomBegin() call.
  /// 'attach_on_read': Pages are attached to the output batch after they are read.
  /// 'got_reservation': set to true if there was enough reservation to initialize the
  ///     read and write pages and false if there was not enough reservation and no other
  ///     error was encountered. Undefined if an error status is returned.
  Status PrepareForReadWrite(
      bool attach_on_read, bool* got_reservation);

  /// Explicitly finishes writing the stream, invalidating the write iterator (if
  /// there is one). Must be called after the last AddRow() or AddRowCustomEnd().
  void DoneWriting();

  /// Prepares the stream for reading, invalidating the write iterator (if there is one).
  /// Therefore must be called after the last AddRow() or AddRowCustomEnd() and before
  /// GetNext(). PrepareForRead() can be called multiple times to do multiple read passes
  /// over the stream, unless rows were read from the stream after PrepareForRead() or
  /// PrepareForReadWrite() was called with attach_on_read = true.
  /// 'attach_on_read': Pages are attached to the output batch after they are read.
  /// 'got_reservation': set to true if there was enough reservation to initialize the
  ///     first read page and false if there was not enough reservation and no other
  ///     error was encountered. Undefined if an error status is returned.
  Status PrepareForRead(bool attach_on_read, bool* got_reservation);

  /// Prepares 'iter' for reading a pinned stream. The stream must not have a write
  /// iterator and stream must be pinned. This does not attach pages on read.
  ///
  /// This method is safe to call concurrently from different threads, as long as the
  /// stream remains pinned and 'iter' is not shared between threads.
  Status PrepareForPinnedRead(ReadIterator* iter);

  /// Adds a single row to the stream. There are three possible outcomes:
  /// a) The append succeeds. True is returned.
  /// b) The append fails because the unused reservation was not sufficient to add
  ///   a new page to the stream large enough to fit 'row' and the stream could not
  ///   increase the reservation to get enough unused reservation. Returns false and
  ///   sets 'status' to OK. The append can be retried after freeing up memory or
  ///   unpinning the stream.
  /// c) The append fails with a runtime error. Returns false and sets 'status' to an
  ///   error.
  /// d) The append fails becase the row is too large to fit in a page of a stream.
  ///   Returns false and sets 'status' to an error.
  ///
  /// Unpinned streams can only encounter case b) when appending a row larger than
  /// the default page size and the reservation could not be increased sufficiently.
  /// Otherwise enough memory is automatically freed up by unpinning the current write
  /// page.
  ///
  /// BufferedTupleStream will do a deep copy of the memory in the row. After AddRow()
  /// returns an error, it should not be called again.
  bool AddRow(TupleRow* row, Status* status) noexcept;

  /// Allocates space to store a row of 'size' bytes (including fixed and variable length
  /// data). If successful, returns a pointer to the allocated row. The caller then must
  /// writes valid data to the row and call AddRowCustomEnd().
  ///
  /// If unsuccessful, returns nullptr. The failure modes are the same as described in the
  /// AddRow() comment.
  ALWAYS_INLINE uint8_t* AddRowCustomBegin(int64_t size, Status* status);

  /// Called after AddRowCustomBegin() when done writing the row. Only should be called
  /// if AddRowCustomBegin() succeeded. See the AddRowCustomBegin() comment for
  /// explanation.
  /// 'size': the size passed into AddRowCustomBegin().
  void AddRowCustomEnd(int64_t size);

  /// Unflattens 'flat_row' into a regular TupleRow 'row'. Only valid to call if the
  /// stream is pinned. The row must have been allocated with the stream's row desc.
  /// The returned 'row' is backed by memory from the stream so is only valid as long
  /// as the stream is pinned.
  void GetTupleRow(FlatRowPtr flat_row, TupleRow* row) const;

  /// Pins all pages in this stream and switches to pinned mode. Has no effect if the
  /// stream is already pinned.
  /// If the current unused reservation is not sufficient to pin the stream in memory,
  /// this will try to increase the reservation. If that fails, 'pinned' is set to false
  /// and the stream is left unpinned. Otherwise 'pinned' is set to true.
  Status PinStream(bool* pinned);

  /// Modes for UnpinStream().
  enum UnpinMode {
    /// All pages in the stream are unpinned and the read/write positions in the stream
    /// are reset. No more rows can be written to the stream after this. The stream can
    /// be re-read from the beginning by calling PrepareForRead(). It in invalid to call
    /// UnpinStream(UNPIN_ALL) if the stream is in 'attach_on_read' mode and >= 1 row has
    /// been read from the stream, because this would leave the stream in limbo where it
    /// still has unpinned pages but it cannot be read or written to.
    UNPIN_ALL,
    /// All pages are unpinned aside from the current read and write pages (if any),
    /// which is left in the same state. The unpinned stream can continue being read
    /// or written from the current read or write positions.
    UNPIN_ALL_EXCEPT_CURRENT,
  };

  /// Unpins stream with the given 'mode' as described above.
  Status UnpinStream(UnpinMode mode);

  /// Get the next batch of output rows, which are backed by the stream's memory.
  ///
  /// If the stream is in 'attach_on_read' mode then buffers are attached to 'batch'
  /// when the last row referencing the buffer is returned. The FLUSH_RESOURCES flag
  /// is always set when attaching such a buffer.
  /// TODO: always flushing for pinned streams is overkill since we may not need
  /// to reuse the reservation immediately. Changing this may require modifying
  /// callers of this class.
  ///
  /// If the stream is unpinned and not in 'attach_on_read' mode, the FLUSH_RESOURCES
  /// flag may be set on the batch to signal that memory will be freed on the next call
  /// to GetNext() and that the caller should copy out any data it needs from rows in
  /// 'batch' or in previous batches returned from GetNext().
  ///
  /// If the stream is pinned and 'attach_on_read' is false, the memory backing the
  /// rows will remain valid until the stream is unpinned, destroyed, etc.
  Status GetNext(RowBatch* batch, bool* eos);

  /// Get the next batch of output rows from 'read_iter', which are backed by the
  /// pinned stream's memory. The memory backing the rows is valid as long as the
  /// stream remains pinned.
  ///
  /// This method is safe to call concurrently from different threads, as long as the
  /// stream remains pinned and 'iter' is not shared between threads.
  Status GetNext(ReadIterator* read_iter, RowBatch* batch, bool* eos);

  /// Same as above, but populate 'flat_rows' with a pointer to the flat version of
  /// each returned row in the pinned stream. The pointers in 'flat_rows' are only
  /// valid as long as the stream remains pinned.
  Status GetNext(
      RowBatch* batch, bool* eos, std::vector<FlatRowPtr>* flat_rows);

  /// Saves extra reservation of the underlying buffer pool client for a large read page
  /// in this stream. The caller should make sure there are enough unused reservation in
  /// the buffer pool. The reservation of 'large_read_page_reservation_' will be
  /// 'max_page_len_' - 'default_page_len_' after calling this.
  void SaveLargeReadPageReservation();

  /// Restores the large read page reservation back to the underlying buffer pool client.
  void RestoreLargeReadPageReservation();

  /// Returns true if we have saved some space for a large read page.
  bool HasLargeReadPageReservation();

  /// Must be called once at the end to cleanup all resources. If 'batch' is non-NULL,
  /// attaches buffers from pinned pages that rows returned from GetNext() may reference.
  /// Otherwise deletes all pages. Does nothing if the stream was already closed. The
  /// 'flush' mode is forwarded to RowBatch::AddBuffer() when attaching buffers.
  void Close(RowBatch* batch, RowBatch::FlushMode flush);

  /// Number of rows in the stream.
  int64_t num_rows() const { return num_rows_; }

  /// Number of rows returned via GetNext().
  int64_t rows_returned() const { return read_it_.rows_returned_; }

  /// Returns the byte size necessary to store the entire stream in memory.
  int64_t byte_size() const { return total_byte_size_; }

  /// Returns the number of bytes currently pinned in memory by the stream.
  /// If ignore_current is true, the write_page_ memory is not included.
  int64_t BytesPinned(bool ignore_current) const {
    if (ignore_current && write_page_ != nullptr && write_page_->is_pinned()) {
      return bytes_pinned_ - write_page_->len();
    }
    return bytes_pinned_;
  }

  /// Returns the number of bytes currently unpinned by the stream.
  int64_t bytes_unpinned() const {
    return bytes_unpinned_;
  }

  bool is_closed() const { return closed_; }
  bool is_pinned() const { return pinned_; }
  // Returns true if there is an active embedded read iterator.
  bool has_read_iterator() const { return read_it_.is_valid(); }
  bool has_write_iterator() const { return has_write_iterator_; }

  std::string DebugString() const;

  static constexpr int64_t MAX_PAGE_ITER_DEBUG = 100;

 private:
  /// Wrapper around BufferPool::PageHandle that tracks additional info about the page.
  struct Page {
    /// Default constructor.
    Page() {}

    /// Move constructor so this can reside in a vector. Not thread-safe.
    Page(Page&& src)
      : handle(std::move(src.handle)),
        num_rows(src.num_rows),
        retrieved_buffer(src.retrieved_buffer.Load()),
        attached_to_output_batch(src.attached_to_output_batch) {}

    inline int len() const { return handle.len(); }
    inline bool is_pinned() const { return handle.is_pinned(); }
    inline int pin_count() const { return handle.pin_count(); }
    Status GetBuffer(const BufferPool::BufferHandle** buffer) {
      RETURN_IF_ERROR(handle.GetBuffer(buffer));
      retrieved_buffer.Store(true);
      return Status::OK();
    }

    /// Attach the buffer from this page to 'batch'. Only valid to call if the page is
    /// pinned and 'retrieved_buffer' is true. Decrements parent->bytes_pinned_.
    void AttachBufferToBatch(
        BufferedTupleStream* parent, RowBatch* batch, RowBatch::FlushMode flush);

    std::string DebugString() const;

    BufferPool::PageHandle handle;

    /// Number of rows written to the page. Its type is int64_t since when the row size
    /// is zero, all rows will be added to one page. And the number of rows in the whole
    /// stream can exceed INT_MAX.
    int64_t num_rows = 0;

    /// Whether we called GetBuffer() on the page since it was last pinned. This means
    /// that GetBuffer() and ExtractBuffer() cannot fail and that GetNext() may have
    /// returned rows referencing the page's buffer. This is atomic because multiple
    /// iterators may call GetBuffer() concurrently.
    AtomicBool retrieved_buffer{true};

    /// If the page was just attached to the output batch on the last GetNext() call while
    /// in attach_on_read mode. If true, then 'handle' is closed.
    bool attached_to_output_batch = false;
  };

 public:
  /// A read iterator for a tuple stream. This keeps track of the read position in the
  /// tuple stream and is advanced forward while reading rows from the stream.
  /// This can be used in two modes:
  /// * As the built-in iterator for the stream. This supports various advanced
  ///   functionality like reading unpinned streams, co-existing with a write iterator,
  ///   etc.
  /// * As an external iterator for a read-only pinned stream. In this case, multiple
  ///   iterators can be used simultaneously and reading from the stream via different
  ///   iterators is thread safe (as long as each iterator is not accessed concurrently
  //    from different threads).
  class ReadIterator {
   public:
    int64_t rows_returned() const { return rows_returned_; }
    bool is_valid() const { return valid_; }

    std::string DebugString(const std::list<Page>& pages) const;

   private:
    friend class BufferedTupleStream;
    friend class StreamStateTest;

    /// True if the read iterator is currently valid
    bool valid_ = false;

    /// If true, pages are deleted after they are read via this read iterator. This is
    /// only used for the embedded read iterator, i.e. BufferedTupleStream::read_it_.
    /// Once rows have been read from a stream with 'attach_on_read_' true, this is
    /// always true.
    bool attach_on_read_ = false;

    /// The current page being read. When no read iterator is active, equal to list.end().
    /// When a read iterator is active, either points to the current read page, or equals
    /// list.end() if no rows have yet been read.  GetNext() does not advance this past
    /// the end of the stream, so upon eos 'read_page_' points to the last page and
    /// rows_returned_ == num_rows_. Always pinned, unless a Pin() call failed and an
    /// error status was returned.
    std::list<Page>::iterator read_page_;

    /// Total number of rows returned via this read iterator since Init() was called.
    int64_t rows_returned_ = 0;

    /// Number of rows returned from the current 'read_page_'. The type needs to be
    /// compatible with 'Page::num_rows' for them to compare.
    int64_t read_page_rows_returned_ = -1;

    /// Pointer into 'read_page_' to the byte after the last row read.
    uint8_t* read_ptr_ = nullptr;

    /// Pointer to one byte past the end of 'read_page_'. Used to detect overruns.
    const uint8_t* read_end_ptr_ = nullptr;

    /// Initializes a read iterator to a valid state.
    void Init(bool attach_on_read);

    /// Invalidate the read iterator.
    void Reset(std::list<Page>* pages);

    /// Sets the current read page and clears state from any previous read page.
    /// 'read_page' may be pages.end() if there are no pages in the stream yet.
    /// The iterator must be valid.
    void SetReadPage(std::list<Page>::iterator read_page);

    /// Set the read page to the next read page. This iterator must be valid and
    /// 'read_page_' must point to a page, not pages_.end().
    void AdvanceReadPage(const std::list<Page>& pages);

    /// Prepares to read from pinned 'read_page_'. Called after SetReadPage()
    /// or AdvanceReadPage().  Waits for the pin to complete if the page was
    /// unpinned earlier.  Sets 'read_ptr_' and 'read_end_ptr_'.
    Status InitReadPtrs();

    /// Move forward 'read_ptr_' by 'bytes'. It is invalid to advance more than 1
    /// byte past the end of the buffer.
    void AdvanceReadPtr(int64_t bytes) {
      DCHECK_GE(bytes, 0);
      read_ptr_ += bytes;
      DCHECK_LE(read_ptr_, read_end_ptr_);
    }

    /// Increment the number of rows returned from this page and in total.
    void IncrRowsReturned(int64_t rows);

    /// Return the number of rows left to read in 'read_page_'. This iterator must be
    /// valid and 'read_page_' must point to a page, not pages_.end().
    int64_t GetRowsLeftInPage() const {
      DCHECK_GE(read_page_->num_rows, read_page_rows_returned_);
      return read_page_->num_rows - read_page_rows_returned_;
    }
  };

  /// Returns the total additional bytes that this row will consume in write_page_ if
  /// appended to the page. This includes the row's null indicators, the fixed length
  /// part of the row and the data for inlined_string_slots_ and inlined_coll_slots_.
  int64_t ComputeRowSize(TupleRow* row) const noexcept;

 private:
  DISALLOW_COPY_AND_ASSIGN(BufferedTupleStream);
  friend class StreamStateTest;

  /// Runtime state instance used to check for cancellation. Not owned.
  RuntimeState* const state_;

  /// Description of rows stored in the stream.
  const RowDescriptor* desc_;

  /// Caller ID, used for error reporting. A unique id identifying the class using the
  /// BufferedTupleStream. For ExecNodes this is the TPlanNodeId, however, it could be
  /// any unique string.
  std::string caller_label_;

  /// The size of the fixed length portion for each tuple in the row.
  std::vector<int> fixed_tuple_sizes_;

  /// Vectors of all the strings slots that have their varlen data stored in stream
  /// grouped by tuple_idx.
  std::vector<std::pair<int, std::vector<SlotDescriptor*>>> inlined_string_slots_;

  /// Vectors of all the collection slots that have their varlen data stored in the
  /// stream, grouped by tuple_idx.
  std::vector<std::pair<int, std::vector<SlotDescriptor*>>> inlined_coll_slots_;

  /// Buffer pool and client used to allocate, pin and release pages. Not owned.
  BufferPool* buffer_pool_;
  BufferPool::ClientHandle* buffer_pool_client_;

  /// List of pages in the stream.
  /// Empty iff one of two cases applies:
  /// * before the first row has been added with AddRow() or AddRowCustom().
  /// * after the stream has been destructively read in 'attach_on_read' mode
  std::list<Page> pages_;
  // IMPALA-5629: avoid O(n) list.size() call by explicitly tracking the number of pages.
  // TODO: remove when we switch to GCC5+, where list.size() is O(1). See GCC bug #49561.
  int64_t num_pages_ = 0;

  /// Total size of pages_, including any pages already deleted in 'attach_on_read'
  /// mode.
  int64_t total_byte_size_ = 0;

  /// The built-in read iterator for the stream.
  ReadIterator read_it_;

  /// Saved reservation for the embedded read iterator. 'default_page_len_' reservation
  /// is saved if there is a read iterator, no pinned read page, and the possibility
  /// that the read iterator will advance to a valid page.
  BufferPool::SubReservation read_page_reservation_;
  /// Extra saved reservation for reading a large page.
  /// 'max_page_len_' - 'default_page_len_' is saved if this is the only input stream of
  /// the operator.
  BufferPool::SubReservation large_read_page_reservation_;

  /// Pointer into write_page_ to the byte after the last row written.
  uint8_t* write_ptr_ = nullptr;

  /// Pointer to one byte past the end of write_page_. Cached to speed up computation
  const uint8_t* write_end_ptr_ = nullptr;

  /// True if there is currently an active write iterator into the stream.
  bool has_write_iterator_ = false;

  /// The current page for writing. NULL if there is no write iterator or no current
  /// write page. Always pinned. Size is 'default_page_len_', except temporarily while
  /// appending a larger row between AddRowCustomBegin() and AddRowCustomEnd().
  Page* write_page_ = nullptr;

  /// Saved reservation for write iterator. 'default_page_len_' reservation is saved if
  /// there is a write iterator, no page currently pinned for writing and the possibility
  /// that a pin count will be needed for the write iterator in future. Specifically if:
  /// * no rows have been appended to the stream and 'pages_' is empty, or
  /// * the stream is unpinned, 'write_page_' is null and and the last page in 'pages_'
  ///   is a large page that we advanced past, or
  /// * there is only one pinned page in the stream and it is already pinned for reading.
  BufferPool::SubReservation write_page_reservation_;

  /// Total bytes of pinned pages in pages_, stored to avoid iterating over the list
  /// to compute it.
  int64_t bytes_pinned_ = 0;

  /// Total bytes of pinned pages in pages_, stored to avoid iterating over the list
  /// to compute it.
  int64_t bytes_unpinned_ = 0;

  /// Number of rows stored in the stream. Includes rows that were already deleted during
  /// a destructive 'attach_on_read' pass over the stream.
  int64_t num_rows_ = 0;

  /// The default length in bytes of pages used to store the stream's rows. All rows that
  /// fit in a default-sized page are stored in default-sized page.
  const int64_t default_page_len_;

  /// The maximum length in bytes of pages used to store the stream's rows. This is a
  /// hard limit on the maximum size of row that can be stored in the stream and the
  /// amount of reservation required to read or write to an unpinned stream.
  const int64_t max_page_len_;

  /// Whether any tuple in the rows is nullable.
  const bool has_nullable_tuple_;

  bool closed_ = false; // Used for debugging.

  /// If true, this stream has been explicitly pinned by the caller and all pages are
  /// kept pinned until the caller calls UnpinStream().
  bool pinned_ = true;

  /// Populates 'inlined_string_slots_' and 'inlined_coll_slots_' under the index of
  /// 'tuple_idx' with the inlined string and collection slots from the slots of
  /// 'tuple_desc'. Excludes the slots in 'ext_varlen_slot'.
  void CollectInlinedSlots(const TupleDescriptor* tuple_desc,
      const std::set<SlotId>& ext_varlen_slots, int tuple_idx);

  /// Return true if 'page' is the current page for the embedded read iterator.
  bool is_read_page(const Page* page) const {
    return read_it_.read_page_ != pages_.end() && &*read_it_.read_page_ == page;
  }

  /// Return true if 'page' is the current page for the embedded write iterator.
  bool is_write_page(const Page* page) const { return write_page_ == page; }

  /// Return true if the read and write page are the same.
  bool has_read_write_page() const {
    return write_page_ != nullptr && is_read_page(write_page_);
  }

  /// The slow path for AddRow() that is called if there is not sufficient space in
  /// the current page.
  bool AddRowSlow(TupleRow* row, Status* status) noexcept;

  /// The slow path for AddRowCustomBegin() that is called if there is not sufficient space in
  /// the current page.
  uint8_t* AddRowCustomBeginSlow(int64_t size, Status* status) noexcept;

  /// The slow path for AddRowCustomEnd() that is called for large pages.
  void AddLargeRowCustomEnd(int64_t size) noexcept;

  /// Copies 'row' into the buffer starting at *data and ending at the byte before
  /// 'data_end'. On success, returns true and updates *data to point after the last
  /// byte written. Returns false if there is not enough space in the buffer provided.
  bool DeepCopy(TupleRow* row, uint8_t** data, const uint8_t* data_end) noexcept;

  /// Templated implementation of DeepCopy().
  template <bool HAS_NULLABLE_TUPLE>
  bool DeepCopyInternal(TupleRow* row, uint8_t** data, const uint8_t* data_end) noexcept;

  /// Helper function to copy strings in string_slots from tuple into *data.
  /// Updates *data to the end of the string data added. Returns false if the data
  /// does not fit in the buffer [*data, data_end).
  static bool CopyStrings(const Tuple* tuple,
      const std::vector<SlotDescriptor*>& string_slots, uint8_t** data,
      const uint8_t* data_end);

  /// Helper function to deep copy collections in collection_slots from tuple into
  /// the buffer [*data, data_end). Updates *data to the end of the collection data
  /// added. Returns false if the data does not fit in the buffer.
  static bool CopyCollections(const Tuple* tuple,
      const std::vector<SlotDescriptor*>& collection_slots, uint8_t** data,
      const uint8_t* data_end);

  /// Gets a new page of 'page_len' bytes from buffer_pool_, updating write_page_,
  /// write_ptr_ and write_end_ptr_. The caller must ensure there is 'page_len' unused
  /// reservation. The caller must reset the write page (if there is one) before calling.
  Status NewWritePage(int64_t page_len) noexcept;

  /// Determines what page size is needed to fit a row of 'row_size' bytes.
  /// Returns an error if the row cannot fit in a page.
  Status CalcPageLenForRow(int64_t row_size, int64_t* page_len);

  /// Wrapper around NewWritePage() that allocates a new write page that fits a row of
  /// 'row_size' bytes. Increases reservation if needed to allocate the next page.
  /// Returns OK and sets 'got_reservation' to true if the write page was successfully
  /// allocated. Returns an error if the row cannot fit in a page. Returns OK and sets
  /// 'got_reservation' to false if the reservation could not be increased and no other
  /// error was encountered.
  Status AdvanceWritePage(
      int64_t row_size, bool* got_reservation) noexcept;

  /// Reset the write page, if there is one, and unpin pages accordingly. If there
  /// is an active write iterator, the next row will be appended to a new page.
  void ResetWritePage();

  /// Invalidate the write iterator and release any resources associated with it. After
  /// calling this, no more rows can be appended to the stream.
  void InvalidateWriteIterator();

  /// Helper for PrepareForRead() and PrepareForPinnedRead(). Sets up 'read_iter' for
  /// reading from the start of the stream. Does not invalidate any existing iterators.
  /// The caller must ensure there is sufficient unused reservation.
  Status PrepareForReadInternal(bool attach_on_read, ReadIterator* read_iter);

  /// Pins the next read page for 'read_iter'. This blocks reading from disk if necessary
  /// to bring the page's data into memory.
  Status NextReadPage(ReadIterator* read_iter);

  /// Invalidate the read iterator, and release any resources associated with the active
  /// iterator. Invalid to call if 'read_it_.attach_on_read_' is true and >= 1 rows have
  /// been read, because that would leave the stream in limbo where it still has pages
  /// but it is invalid to read or write from in future.
  void InvalidateReadIterator();

  /// Pins page and updates tracking stats.
  Status PinPage(Page* page);

  /// Increment the page's pin count if this page needs a higher pin count given the
  /// current read and write iterator positions and whether the stream will be pinned
  /// ('stream_pinned'). Assumes that no scenarios occur when the pin count needs to
  /// be incremented multiple times. The caller is responsible for ensuring sufficient
  /// reservation is available.
  Status PinPageIfNeeded(Page* page, bool stream_pinned);

  /// Decrement the page's pin count if this page needs a lower pin count given the
  /// current read and write iterator positions and whether the stream will be pinned
  /// ('stream_pinned'). Assumes that no scenarios occur when the pin count needs to
  /// be decremented multiple times.
  void UnpinPageIfNeeded(Page* page, bool stream_pinned);

  /// Return the expected pin count for 'page' in the current stream based on the current
  /// read and write pages and whether the stream is pinned. Not valid to call if
  /// the page was just deleted, i.e. page->attached_to_output_batch == false.
  int ExpectedPinCount(bool stream_pinned, const Page* page) const;

  /// Return true if the stream in its current state needs to have a reservation for
  /// a write page stored in 'write_page_reservation_'.
  bool NeedWriteReservation() const;

  /// Same as above, except assume the stream's 'pinned_' state is 'stream_pinned'.
  bool NeedWriteReservation(bool stream_pinned) const;

  /// Same as above, except assume the stream has 'num_pages' pages and different
  /// iterator state.
  static bool NeedWriteReservation(bool stream_pinned, int64_t num_pages,
      bool has_write_iterator, bool has_write_page, bool has_read_write_page);

  /// Return true if the stream in its current state needs to have a reservation for
  /// a read page stored in 'read_page_reservation_'.
  bool NeedReadReservation() const;

  /// Same as above, except assume the stream's 'pinned_' state is 'stream_pinned'.
  bool NeedReadReservation(bool stream_pinned) const;

  /// Same as above, except assume the stream has 'num_pages' pages and a different
  /// read iterator state.
  bool NeedReadReservation(bool stream_pinned, int64_t num_pages, bool has_read_iterator,
      bool has_read_page) const;

  /// Same as above, except assume the stream has 'num_pages' pages and a different
  /// write iterator state.
  static bool NeedReadReservation(bool stream_pinned, int64_t num_pages,
      bool has_read_iterator, bool has_read_page, bool has_write_iterator,
      bool has_write_page);

  /// Templated GetNext implementations.
  template <bool FILL_FLAT_ROWS>
  Status GetNextInternal(ReadIterator* RESTRICT read_iter, RowBatch* batch, bool* eos,
      std::vector<FlatRowPtr>* flat_rows);
  template <bool FILL_FLAT_ROWS, bool HAS_NULLABLE_TUPLE>
  Status GetNextInternal(ReadIterator* RESTRICT read_iter, RowBatch* batch, bool* eos,
      std::vector<FlatRowPtr>* flat_rows);

  /// Helper function to convert a flattened TupleRow stored starting at '*data' into
  /// 'row'. *data is updated to point to the first byte past the end of the row.
  template <bool HAS_NULLABLE_TUPLE>
  void UnflattenTupleRow(uint8_t** data, TupleRow* row) const;

  /// Helper function for GetNextInternal(). For each string slot in string_slots,
  /// update StringValue's ptr field to point to the corresponding string data stored
  /// inline in the stream (at the current value of read_iter->read_ptr_) advance
  /// read_iter->read_ptr_ by the StringValue's length field.
  static void FixUpStringsForRead(const vector<SlotDescriptor*>& string_slots,
      ReadIterator* RESTRICT read_iter, Tuple* tuple);

  /// Helper function for GetNextInternal(). For each collection slot in collection_slots,
  /// recursively update any pointers in the CollectionValue to point to the corresponding
  /// var len data stored inline in the stream, advancing read_iter->read_ptr_ as data is
  /// read.  Assumes that the collection was serialized to the stream in DeepCopy()'s
  /// format.
  static void FixUpCollectionsForRead(const vector<SlotDescriptor*>& collection_slots,
      ReadIterator* RESTRICT read_iter, Tuple* tuple);

  /// Returns the number of null indicator bytes per row. Only valid if this stream has
  /// nullable tuples.
  int NullIndicatorBytesPerRow() const;

  /// Returns the total bytes pinned. Only called in DCHECKs to validate bytes_pinned_.
  int64_t CalcBytesPinned() const;

  /// DCHECKs if the stream is internally inconsistent. The stream should always be in
  /// a consistent state after returning success from a public API call. The Fast version
  /// has constant runtime and does not check all of 'pages_'. The Full version includes
  /// O(n) checks that require iterating over the whole 'pages_' list (e.g. checking that
  /// each page is in a valid state). 'read_it' is a read iterator that is safe for
  /// the current thread to access ('read_iter_' in most cases except when an internal
  /// read iterator is used.
  void CheckConsistencyFast(const ReadIterator& read_it) const;
  void CheckConsistencyFull(const ReadIterator& read_it) const;
  void CheckPageConsistency(const Page* page) const;
};
}

#endif
