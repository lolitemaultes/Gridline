// Gridline - J-191: SHA-256 (FIPS 180-4), used to check downloaded model files.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace gl {

class Sha256 {
 public:
  Sha256();
  void update(const void* data, size_t n);
  // Lower-case hex digest; the object must not be updated afterwards.
  std::string hex();

 private:
  void block(const uint8_t* p);
  uint32_t h_[8];
  uint8_t buf_[64];
  size_t bufLen_ = 0;
  uint64_t total_ = 0;
};

std::string sha256Hex(const void* data, size_t n);
// Digest of a whole file, "" when it cannot be read. `progress(done, total)` may be null.
std::string sha256File(const std::string& path, void (*progress)(uint64_t, uint64_t, void*) = nullptr,
                       void* user = nullptr);

}  // namespace gl
