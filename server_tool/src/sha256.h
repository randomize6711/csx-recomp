// SHA-256 (FIPS 180-4), for checking the user's default.xbe against the version the port is built for.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace tool {

class Sha256 {
public:
    Sha256();
    void Update(const void* data, size_t n);
    std::string HexDigest();  // finishes; 64 lower-case hex digits

private:
    void Block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    size_t used_ = 0;
    uint64_t bits_ = 0;
};

// a file's digest, empty when it cannot be read
std::string Sha256File(const std::string& path);

}  // namespace tool
