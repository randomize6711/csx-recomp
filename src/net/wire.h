// Bounds-checked little-endian serialization and a per-source rate limit, shared by the transport (transport.cpp),
// the server query (query.cpp) and the master server protocol (master.cpp). Host code only.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

#include "net/transport.h"

namespace net::wire {

struct Writer {
    uint8_t* p;
    size_t cap, n = 0;
    bool ok = true;
    void Bytes(const void* d, size_t len) {
        if (!ok || cap - n < len) {
            ok = false;
            return;
        }
        std::memcpy(p + n, d, len);
        n += len;
    }
    void U8(uint8_t v) { Bytes(&v, 1); }
    void U16(uint16_t v) {
        uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)};
        Bytes(b, 2);
    }
    void U32(uint32_t v) {
        uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
        Bytes(b, 4);
    }
    void U64(uint64_t v) {
        U32(uint32_t(v));
        U32(uint32_t(v >> 32));
    }
    void Zero(size_t len) {
        if (!ok || cap - n < len) {
            ok = false;
            return;
        }
        std::memset(p + n, 0, len);
        n += len;
    }
    // a string of at most max bytes (and 255), its length in one byte before it
    void Str(const std::string& s, size_t max) {
        size_t len = std::min({s.size(), max, size_t(255)});
        U8(uint8_t(len));
        Bytes(s.data(), len);
    }
};

struct Reader {
    const uint8_t* p;
    size_t len, n = 0;
    bool ok = true;
    const uint8_t* Bytes(size_t k) {
        if (!ok || len - n < k) {
            ok = false;
            return nullptr;
        }
        const uint8_t* r = p + n;
        n += k;
        return r;
    }
    uint8_t U8() {
        const uint8_t* b = Bytes(1);
        return b ? b[0] : 0;
    }
    uint16_t U16() {
        const uint8_t* b = Bytes(2);
        return b ? uint16_t(b[0] | b[1] << 8) : 0;
    }
    uint32_t U32() {
        const uint8_t* b = Bytes(4);
        return b ? uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24 : 0;
    }
    uint64_t U64() {
        uint64_t lo = U32();
        return lo | uint64_t(U32()) << 32;
    }
    // a length-prefixed string; longer than max is malformed (the reader fails)
    std::string Str(size_t max) {
        uint8_t k = U8();
        if (k > max) ok = false;
        const uint8_t* b = Bytes(k);
        return b ? std::string(reinterpret_cast<const char*>(b), k) : std::string();
    }
    size_t Left() const { return ok ? len - n : 0; }
};

inline void Prefix(Writer& w, Type t) {
    w.U32(kMagic);
    w.U8(kVersion);
    w.U8(uint8_t(t));
}

// token buckets per source address (its IP) and for every source together: a flood from many (spoofed) addresses
// costs a bounded amount of work a second; quiet sources are forgotten, a full table takes no new source
class RateLimiter {
public:
    RateLimiter(double per_source, double total, size_t max_sources = 4096) :
        rate_(per_source), total_(total), max_sources_(max_sources) {}
    bool Allow(const Address& from, double now) {
        if (!Take(global_, total_, now)) return false;
        if (now - cleaned_ >= 1.0) {
            cleaned_ = now;
            for (auto it = buckets_.begin(); it != buckets_.end();) it = now - it->second.last > 10 ? buckets_.erase(it) : ++it;
        }
        uint64_t ip = from.Key() >> 16;
        if (buckets_.size() >= max_sources_ && !buckets_.count(ip)) return false;
        return Take(buckets_[ip], rate_, now);
    }
    size_t Sources() const { return buckets_.size(); }

private:
    struct Bucket {
        double tokens = 0, last = -1;
    };
    // a burst of twice the rate, then the rate
    static bool Take(Bucket& b, double rate, double now) {
        double burst = 2 * rate;
        if (b.last < 0) b.tokens = burst;
        else b.tokens = std::min(burst, b.tokens + std::max(0.0, now - b.last) * rate);
        b.last = now;
        if (b.tokens < 1) return false;
        b.tokens -= 1;
        return true;
    }
    double rate_, total_, cleaned_ = -1e9;
    size_t max_sources_;
    Bucket global_;
    std::unordered_map<uint64_t, Bucket> buckets_;
};

// text from the network for the menus and the log: printable ASCII only (no control characters, nothing the
// menus' fonts or the engine's console could take as markup or a command separator), runs of spaces collapsed,
// trimmed, at most max bytes
inline std::string CleanText(const std::string& s, size_t max) {
    std::string out;
    for (unsigned char c : s) {
        if (out.size() >= max) break;
        if (c < 0x20 || c > 0x7E || c == '"' || c == ';' || c == '\\' || c == '%' || c == '#') c = ' ';
        if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
        out += char(c);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

}  // namespace net::wire
