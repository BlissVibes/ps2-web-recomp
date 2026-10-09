#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ps2::gs::queue {

enum Type : std::uint32_t { kWrap, kGif, kGifReset, kReset, kVertexReset, kVblank, kPresent, kDownload };

struct Header {
    std::uint32_t type, bytes, arg0, arg1;
};

bool active();
void start(std::size_t capacity);

void post(Type type, std::uint32_t arg0, std::uint32_t arg1, const void *data, std::uint32_t bytes);

bool peek(Header &header, const std::uint8_t *&payload);
void pop(const Header &header);
void wait_for_work(int timeout_ms);
void wake();

void wait(std::atomic<std::uint32_t> &value, std::uint32_t seen);
void notify(std::atomic<std::uint32_t> &value);

}
