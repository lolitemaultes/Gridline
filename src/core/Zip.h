// Gridline — ZIP archives for zipped projects (PJ-04): writing (stored
// entries — audio barely compresses) and reading (stored and deflated
// entries; the inflater is Gridline's own, written from RFC 1951).
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>
#include "RemoteFs.h"

namespace gl {

uint32_t crc32(const uint8_t* data, size_t n, uint32_t crc = 0);

// The archive is written to a temporary file beside `path` and only replaces
// `path` when close() succeeds (J-78): a failed or abandoned archive leaves
// an existing file untouched and no temporary file behind.
//
// There is no size limit: ZIP64 records are written only for an entry or an archive that needs them
// (4 GB, 65535 entries), so anything smaller keeps the classic layout every tool reads. addFile
// streams, so a large file needs a fixed amount of memory.
class ZipWriter {
 public:
  bool open(const std::string& path, std::string* err = nullptr);
  // `name` uses '/' separators (no leading slash).
  bool add(const std::string& name, const std::vector<uint8_t>& data);
  bool addFile(const std::string& name, const std::string& sourcePath, std::string* err = nullptr);
  bool close(std::string* err = nullptr);
  // K-47: addFile calls this after each chunk it read, with the chunk's bytes; false stops it (the
  // writer is then failed, and the target stays as it was). Empty: none.
  void setChunkHook(std::function<bool(uint64_t bytes)> hook) { chunkHook_ = std::move(hook); }
  ~ZipWriter();

 private:
  struct Entry {
    std::string name;
    uint32_t crc = 0;
    uint64_t size = 0, offset = 0;
  };
  std::vector<Entry> entries_;
  std::FILE* f_ = nullptr;
  std::string path_, tmp_;
  uint64_t pos_ = 0;
  int errno_ = 0;  // first write error
  bool ok_ = true;
  std::function<bool(uint64_t)> chunkHook_;
  void raw(const void* p, size_t n);
  void localHeader(const std::string& name, uint64_t size, uint32_t crc);
};

struct ZipEntry {
  std::string name;  // UTF-8, '/' separators
  uint64_t size = 0, packed = 0, offset = 0;  // (ZIP64 archives can pass 4 GB)
  uint32_t crc = 0;
  uint16_t method = 0;  // 0 stored, 8 deflated
  bool encrypted = false;
  bool dir() const { return !name.empty() && name.back() == '/'; }
};

// Reading goes through a ByteSource and touches each byte once: the directory from the end of the
// file, then the entries in file order (K-42: an archive on a web drive downloads only once, and
// memory use does not depend on the archive's size).
//
// Lists a ZIP file's central directory (only the end of the file is read).
bool readZipDirectory(remotefs::ByteSource& src, std::vector<ZipEntry>& out, std::string* err = nullptr);
bool readZipDirectory(const std::string& path, std::vector<ZipEntry>& out, std::string* err = nullptr);
// Extracts one entry (CRC checked).
bool readZipEntry(const std::string& path, const ZipEntry& e, std::vector<uint8_t>& out, std::string* err = nullptr);
// Extracts everything below `dir` (entries that would escape it are refused).
bool extractZip(const std::string& path, const std::string& dir, std::vector<std::string>* files = nullptr,
                std::string* err = nullptr);

struct ZipExtractOptions {
  remotefs::ReadWatch watch;                                   // cancel, and bytes as they are read
  std::function<void(const ZipEntry&, size_t index)> onEntry;  // each entry as it starts
};
// Extracts `entries` below `dir`, in file order, streaming each one to disk (CRC checked). `files`
// gets the written paths in the order of `entries`. A name that would escape `dir` refuses the whole
// archive before anything is written. On cancel `err` is remotefs::kCancelled.
bool extractZipEntries(remotefs::ByteSource& src, const std::vector<ZipEntry>& entries, const std::string& dir, std::vector<std::string>* files,
                       const ZipExtractOptions& opt, std::string* err = nullptr);

// Raw DEFLATE (RFC 1951) decoder. `expected` is a size hint.
bool inflateRaw(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expected = 0);

}  // namespace gl
