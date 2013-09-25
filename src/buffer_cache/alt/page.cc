#include "buffer_cache/alt/page.hpp"

#include "arch/runtime/coroutines.hpp"
#include "concurrency/auto_drainer.hpp"
#include "serializer/serializer.hpp"

namespace alt {

page_cache_t::page_cache_t(serializer_t *serializer)
    : serializer_(serializer),
      free_list_(serializer),
      drainer_(new auto_drainer_t) {
    {
        // RSI: Don't you hate this?
        on_thread_t thread_switcher(serializer->home_thread());
        // RSI: These priority values were configurable in the old cache.  Did
        // anything configure them?
        reads_io_account.init(serializer->make_io_account(CACHE_READS_IO_PRIORITY));
        writes_io_account.init(serializer->make_io_account(CACHE_WRITES_IO_PRIORITY));
    }
}

page_cache_t::~page_cache_t() {
    drainer_.reset();
    for (auto it = current_pages_.begin(); it != current_pages_.end(); ++it) {
        delete *it;
    }

    {
        /* IO accounts must be destroyed on the thread they were created on */
        on_thread_t thread_switcher(serializer_->home_thread());
        reads_io_account.reset();
        writes_io_account.reset();
    }
}

current_page_t *page_cache_t::page_for_block_id(block_id_t block_id) {
    if (current_pages_.size() <= block_id) {
        current_pages_.resize(block_id + 1, NULL);
    }

    if (current_pages_[block_id] == NULL) {
        current_pages_[block_id] = new current_page_t(block_id, this);
    }

    return current_pages_[block_id];
}

current_page_t *page_cache_t::page_for_new_block_id(block_id_t *block_id_out) {
    block_id_t block_id = free_list_.acquire_block_id();
    // RSI: Have a user-specifiable block size.
    current_page_t *ret = new current_page_t(serializer_->get_block_size(),
                                             serializer_->malloc());
    *block_id_out = block_id;
    return ret;
}

current_page_acq_t::current_page_acq_t(current_page_t *current_page,
                                       alt_access_t access)
    : access_(access),
      current_page_(current_page),
      snapshotted_page_(NULL) {
    current_page_->add_acquirer(this);
}

current_page_acq_t::~current_page_acq_t() {
    if (current_page_ != NULL) {
        current_page_->remove_acquirer(this);
    }
    if (snapshotted_page_ != NULL) {
        snapshotted_page_->remove_snapshotter(this);
    }
}

void current_page_acq_t::declare_snapshotted() {
    rassert(access_ == alt_access_t::read);
    // Allow redeclaration of snapshottedness.
    if (!declared_snapshotted_) {
        declared_snapshotted_ = true;
        rassert(current_page_ != NULL);
        current_page_->pulse_pulsables(this);
    }
}

signal_t *current_page_acq_t::read_acq_signal() {
    return &read_cond_;
}

signal_t *current_page_acq_t::write_acq_signal() {
    rassert(access_ == alt_access_t::write);
    return &write_cond_;
}

page_t *current_page_acq_t::page_for_read() {
    rassert(snapshotted_page_ != NULL || current_page_ != NULL);
    read_cond_.wait();
    if (snapshotted_page_ != NULL) {
        return snapshotted_page_;
    }
    rassert(current_page_ != NULL);
    return current_page_->the_page_for_read();
}

page_t *current_page_acq_t::page_for_write() {
    rassert(access_ == alt_access_t::write);
    rassert(current_page_ != NULL);
    write_cond_.wait();
    rassert(current_page_ != NULL);
    return current_page_->the_page_for_write();
}

current_page_t::current_page_t(block_id_t block_id, page_cache_t *page_cache)
    : block_id_(block_id),
      page_cache_(page_cache),
      page_(NULL) {
}

current_page_t::current_page_t(block_size_t block_size,
                               scoped_malloc_t<ser_buffer_t> buf)
    : block_id_(NULL_BLOCK_ID),
      page_cache_(NULL),
      page_(new page_t(block_size, std::move(buf))) {
}

current_page_t::~current_page_t() {
    rassert(acquirers_.empty());
}

void current_page_t::add_acquirer(current_page_acq_t *acq) {
    acquirers_.push_back(acq);
    pulse_pulsables(acq);
}

void current_page_t::remove_acquirer(current_page_acq_t *acq) {
    current_page_acq_t *next = acquirers_.next(acq);
    acquirers_.remove(acq);
    if (next != NULL) {
        pulse_pulsables(next);
    }
}

void current_page_t::pulse_pulsables(current_page_acq_t *const acq) {
    // First, avoid pulsing when there's nothing to pulse.
    {
        current_page_acq_t *prev = acquirers_.prev(acq);
        if (!(prev == NULL || (prev->access_ == alt_access_t::read
                               && prev->read_cond_.is_pulsed()))) {
            return;
        }
    }

    // Second, avoid re-pulsing already-pulsed chains.
    if (acq->access_ == alt_access_t::read && acq->read_cond_.is_pulsed()) {
        return;
    }

    // It's time to pulse the pulsables.
    current_page_acq_t *cur = acq;
    while (cur != NULL) {
        // We know that the previous node has read access and has been pulsed as
        // readable, so we pulse the current node as readable.
        cur->read_cond_.pulse_if_not_already_pulsed();

        if (cur->access_ == alt_access_t::read) {
            current_page_acq_t *next = acquirers_.next(cur);
            if (cur->declared_snapshotted_) {
                // Snapshotters get kicked out of the queue, to make way for
                // write-acquirers.
                cur->snapshotted_page_ = the_page_for_read();
                cur->current_page_ = NULL;
                cur->snapshotted_page_->add_snapshotter(cur);
                acquirers_.remove(cur);
            }
            cur = next;
        } else {
            // Even the first write-acquirer gets read access (there's no need for an
            // "intent" mode).  But subsequent acquirers need to wait, because the
            // write-acquirer might modify the value.
            if (acquirers_.prev(cur) == NULL) {
                // (It gets exclusive write access if there's no preceding reader.)
                cur->write_cond_.pulse_if_not_already_pulsed();
            }
            break;
        }
    }
}

void current_page_t::convert_from_serializer_if_necessary() {
    if (page_ == NULL) {
        page_ = new page_t(block_id_, page_cache_);
        page_cache_ = NULL;
        block_id_ = NULL_BLOCK_ID;
    }
}

page_t *current_page_t::the_page_for_read() {
    convert_from_serializer_if_necessary();
    return page_;
}

page_t *current_page_t::the_page_for_write() {
    convert_from_serializer_if_necessary();
    if (page_->has_snapshot_references()) {
        page_ = page_->make_copy();
    }
    return page_;
}

page_t::page_t(block_id_t block_id, page_cache_t *page_cache)
    : destroy_ptr_(NULL),
      buf_size_(block_size_t::undefined()),
      snapshot_refcount_(0) {
    coro_t::spawn_now_dangerously(std::bind(&page_t::load_with_block_id,
                                            this,
                                            block_id,
                                            page_cache));
}

page_t::page_t(block_size_t block_size, scoped_malloc_t<ser_buffer_t> buf)
    : destroy_ptr_(NULL),
      buf_size_(block_size),
      buf_(std::move(buf)),
      snapshot_refcount_(0) {
    rassert(buf_.has());
}


void page_t::load_with_block_id(page_t *page, block_id_t block_id,
                                page_cache_t *page_cache) {
    // This is called using spawn_now_dangerously.  We need to atomically set
    // destroy_ptr_.
    bool page_destroyed = false;
    rassert(page->destroy_ptr_ == NULL);
    page->destroy_ptr_ = &page_destroyed;

    // Okay, now it's safe to block.  We do so by going to another thread.  RSI: Can
    // the lock constructor yield?  (It's okay if it can, but I'd be curious if we're
    // worried about on_thread_t not yielding.)
    auto_drainer_t::lock_t lock(page_cache->drainer_.get());

    scoped_malloc_t<ser_buffer_t> buf;
    counted_t<standard_block_token_t> block_token;
    {
        serializer_t *serializer = page_cache->serializer_;
        // RSI: It would be nice if we _always_ yielded here (not just when the
        // thread's different.  spawn_now_dangerously is dangerous, after all.
        on_thread_t th(serializer->home_thread());
        block_token = serializer->index_read(block_id);
        // RSI: Figure out if block_token can be empty (if a page is deleted?).
        rassert(block_token.has());
        // RSI: Support variable block size.
        buf = serializer->malloc();
        serializer->block_read(block_token,
                               buf.get(),
                               NULL  /* RSI: file account */);
    }

    ASSERT_FINITE_CORO_WAITING;
    if (page_destroyed) {
        return;
    }

    rassert(!page->block_token_.has());
    rassert(!page->buf_.has());
    rassert(block_token.has());
    page->buf_size_ = block_token->block_size();
    page->buf_ = std::move(buf);
    page->block_token_ = std::move(block_token);
}

void page_t::add_snapshotter(current_page_acq_t *acq) {
    (void)acq;  // RSI: remove param.
    ++snapshot_refcount_;
}

void page_t::remove_snapshotter(current_page_acq_t *acq) {
    (void)acq;  // RSI: remove param.
    rassert(snapshot_refcount_ > 0);
    --snapshot_refcount_;
}

bool page_t::has_snapshot_references() {
    return snapshot_refcount_ > 0;
}




}  // namespace alt

