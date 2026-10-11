#pragma once

#include <cassert>
#include <cstddef>
#include <string>
#include <string_view>

// Original FIFO: every consumed prefix is erased, shifting the unread bytes.
// The interface matches InputBuffer so the event loop can use either strategy.
class RegularInputBuffer {
public:
    std::string_view view() const { return storage_; }
    bool empty() const { return storage_.empty(); }
    void append(const char* data, std::size_t length) { storage_.append(data, length); }
    void consume(std::size_t length) {
        assert(length <= storage_.size());
        storage_.erase(0, length);
    }
    void clear() { storage_.clear(); }

private:
    std::string storage_;
};

// FIFO bytes: consume advances an offset; append occasionally reclaims space.
class InputBuffer {
public:
    std::string_view view() const {
        return std::string_view(storage_).substr(begin_);
    }

    bool empty() const { return begin_ == storage_.size(); }

    void append(const char* data, std::size_t length) {
        const std::size_t remaining = storage_.size() - begin_;
        // Prefer unused tail capacity. Compact only when at least half the
        // stored bytes have been consumed, so small reads don't keep moving
        // a large unfinished command. Otherwise std::string grows normally.
        if (length > storage_.capacity() - storage_.size() &&
            begin_ > 0 && begin_ >= remaining) {
            storage_.erase(0, begin_);
            begin_ = 0;
        }
        storage_.append(data, length);
    }

    void consume(std::size_t length) {
        assert(length <= storage_.size() - begin_);
        begin_ += length;
        if (empty()) clear(); // Reuse the allocation once everything is read.
    }

    void clear() {
        storage_.clear();
        begin_ = 0;
    }

private:
    std::string storage_; // Consumed prefix followed by unread bytes.
    std::size_t begin_ = 0;
};
