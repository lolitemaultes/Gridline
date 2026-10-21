// Gridline — ZIP archives for zipped projects (PJ-04): writing (stored
// entries — audio barely compresses) and reading (stored and deflated
// entries; the inflater is Gridline's own, written from RFC 1951).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace gl {

uint32_t crc32(const uint8_t* data, size_t n, uint32_t crc = 0);

// The archive is written to a temporary file beside `path` and only replaces
// `path` when close() succeeds (J-78): a failed or abandoned archive leaves
// an existing file untouched and no temporary file behind.
class ZipWriter {
 public:
  bool open(const std::string& path, std::string* err = nullptr);
  // `name` uses '/' separators (no leading slash).
  bool add(const std::string& name, const std::vector<uint8_t>& data);
  bool addFile(const std::string& name, const std::string& sourcePath, std::string* err = nullptr);
  bool close(std::string* err = nullptr);
  ~ZipWriter();

 private:
  struct Entry {
    std::string name;
    uint32_t crc = 0, size = 0, offset = 0;
  };
  std::vector<Entry> entries_;
  std::FILE* f_ = nullptr;
  std::string path_, tmp_;
  uint32_t pos_ = 0;
  int errno_ = 0;  // first write error
  bool ok_ = true;
  void raw(const void* p, size_t n);
};

struct ZipEntry {
  std::string name;
  uint32_t size = 0, packed = 0, crc = 0, offset = 0;
  uint16_t method = 0;  // 0 stored, 8 deflated
  bool dir() const { return !name.empty() && name.back() == '/'; }
};

// Lists a ZIP file's central directory.
bool readZipDirectory(const std::string& path, std::vector<ZipEntry>& out, std::string* err = nullptr);
// Extracts one entry (CRC checked).
bool readZipEntry(const std::string& path, const ZipEntry& e, std::vector<uint8_t>& out, std::string* err = nullptr);
// Extracts everything below `dir` (entries that would escape it are refused).
bool extractZip(const std::string& path, const std::string& dir, std::vector<std::string>* files = nullptr,
                std::string* err = nullptr);

// Raw DEFLATE (RFC 1951) decoder. `expected` is a size hint.
bool inflateRaw(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expected = 0);

}  // namespace gl
