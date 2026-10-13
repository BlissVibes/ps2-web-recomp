#pragma once

// The bookkeeping of the texture upload cache, apart from the GPU so that it can be tested alone.
//
// Texture data that the game uploads again and again is kept in one GPU buffer, so that a repeat costs a
// compare here instead of another copy to the GPU. The buffer is used as a ring of 32-bit words: data is
// written from the front to the back and then from the front again, and an entry is given up when the
// write position reaches it. This class decides where data goes and which entries go. The caller owns the
// GPU buffer and copies into it the words that insert() names; data() is the copy of the buffer on this
// side, which find() compares with.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <vector>

namespace ps2::gs::gpu {

class UploadCache {
public:
    // An empty cache of `words` 32-bit words.
    void reset(std::uint32_t words) {
        copy_.assign(words, 0);
        ring_.clear();
        index_.clear();
        head_ = 0;
    }

    const std::uint32_t *data() const { return copy_.data(); }
    std::size_t entries() const { return ring_.size(); }
    std::uint64_t hits() const { return hits_; }         // uploads found
    std::uint64_t inserts() const { return inserts_; }   // uploads stored
    std::uint64_t refused() const { return refused_; }   // uploads not stored because an entry in use was in the way

    // Looks for these very bytes. `serial` numbers the GPU submit being put together: an entry found is
    // marked as used by it.
    bool find(std::uint64_t key, const std::uint8_t *bytes, std::size_t n, std::uint64_t serial, std::uint32_t &offset) {
        const auto [first, last] = index_.equal_range(key);
        for (auto it = first; it != last; ++it) {
            Entry &e = *it->second;
            if (e.bytes != n || std::memcmp(copy_.data() + e.offset, bytes, n) != 0) continue;
            e.used = serial;
            offset = e.offset;
            ++hits_;
            return true;
        }
        return false;
    }

    // Stores the bytes at the write position, giving up the entries in their way. On success the caller
    // copies `words` words from data() + offset to the same place in the GPU buffer. False, with nothing
    // stored, when the data is longer than a quarter of the cache or an entry in the way is used by submit
    // `serial`, which will still read it.
    bool insert(std::uint64_t key, const std::uint8_t *bytes, std::size_t n, std::uint64_t serial, std::uint32_t &offset, std::uint32_t &words) {
        const std::uint32_t capacity = static_cast<std::uint32_t>(copy_.size());
        words = static_cast<std::uint32_t>((n + 3) / 4 + 1);
        if (words > capacity / 4) return false;
        const auto evict_front = [this, serial] {
            Entry &front = ring_.front();
            if (front.used == serial) return false;
            const auto [first, last] = index_.equal_range(front.key);
            for (auto it = first; it != last; ++it)
                if (it->second == &front) {
                    index_.erase(it);
                    break;
                }
            ring_.pop_front();
            return true;
        };
        // The oldest entries are at the front of ring_. Those left from the round before lie at or after the
        // write position, in the order they will be reached; once they are gone the front is an entry of this
        // round, before the write position, and nothing is in the way.
        if (head_ + words > capacity) {
            while (!ring_.empty() && ring_.front().offset >= head_)
                if (!evict_front()) return refuse();
            head_ = 0;
        }
        while (!ring_.empty() && ring_.front().offset >= head_ && ring_.front().offset < head_ + words)
            if (!evict_front()) return refuse();
        offset = head_;
        std::uint32_t *to = copy_.data() + offset;
        to[words - 1] = 0;
        to[words - 2] = 0;
        std::memcpy(to, bytes, n);
        ring_.push_back({key, offset, words, n, serial});
        index_.emplace(key, &ring_.back());
        head_ += words;
        ++inserts_;
        return true;
    }

private:
    struct Entry {
        std::uint64_t key;
        std::uint32_t offset, words;
        std::size_t bytes;
        std::uint64_t used;
    };

    std::vector<std::uint32_t> copy_;
    std::deque<Entry> ring_;                                   // oldest first
    std::unordered_multimap<std::uint64_t, Entry *> index_;
    std::uint32_t head_ = 0;                                   // the write position, in words
    std::uint64_t hits_ = 0, inserts_ = 0, refused_ = 0;

    bool refuse() {
        ++refused_;
        return false;
    }
};

}
