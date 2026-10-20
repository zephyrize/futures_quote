#ifndef FQ_BYTES_HPP
#define FQ_BYTES_HPP
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <limits>
namespace fqimpl {
struct ParseError : std::runtime_error {
    explicit ParseError(const char *s) : std::runtime_error(s) {}
};
struct Bytes {
    const uint8_t *p;
    size_t n;
    Bytes(const void *data, size_t size) : p(static_cast<const uint8_t *>(data)), n(size) {}
    void require(size_t o, size_t count) const {
        if (o > n || count > n - o)
            throw ParseError("truncated message");
    }
    uint8_t u8(size_t o) const {
        require(o, 1);
        return p[o];
    }
    uint16_t u16(size_t o) const {
        require(o, 2);
        return uint16_t(p[o]) | uint16_t(p[o + 1]) << 8;
    }
    uint32_t u32(size_t o) const {
        require(o, 4);
        return uint32_t(u16(o)) | uint32_t(u16(o + 2)) << 16;
    }
    uint64_t u64(size_t o) const {
        require(o, 8);
        return uint64_t(u32(o)) | uint64_t(u32(o + 4)) << 32;
    }
    int32_t i32(size_t o) const {
        uint32_t u = u32(o);
        int32_t v;
        std::memcpy(&v, &u, 4);
        return v;
    }
    int64_t i64(size_t o) const {
        uint64_t u = u64(o);
        int64_t v;
        std::memcpy(&v, &u, 8);
        return v;
    }
    uint16_t be16(size_t o) const {
        require(o, 2);
        return uint16_t(p[o]) << 8 | p[o + 1];
    }
    Bytes sub(size_t o, size_t count) const {
        require(o, count);
        return Bytes(p + o, count);
    }
};
inline void copy_text(char *dst, size_t cap, const char *src, size_t n) {
    if (!cap)
        return;
    size_t count = n < cap - 1 ? n : cap - 1;
    std::memmove(dst, src, count);
    while (count && (dst[count - 1] == ' ' || dst[count - 1] == '\0'))
        --count;
    dst[count] = 0;
}
inline int64_t quantity(uint64_t v) {
    if (v > uint64_t(INT64_MAX))
        throw ParseError("unsigned quantity exceeds int64 range");
    return static_cast<int64_t>(v);
}
} // namespace fqimpl
#endif
