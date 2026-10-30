// Gridline - J-191: reads PyTorch checkpoint files (the format `torch.save` writes since 1.6: a
// zip archive with `<root>/data.pkl`, a pickle of the saved object, and one `<root>/data/<key>`
// entry per tensor storage) without Python. Only data is read: the pickle is interpreted by a
// small stack machine that builds plain values (none / bool / int / float / string / bytes /
// tuple / list / dict) and records any other class or function call as an opaque object; it
// never runs code. Tensors (float32 / float16 / bfloat16 storages) are resolved to float32.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace gl::stems {

struct PyValue;
using PyRef = std::shared_ptr<PyValue>;

struct PyValue {
  enum class Kind { None, Bool, Int, Float, Str, Bytes, Tuple, List, Dict, Global, Object, Storage, Tensor };
  Kind kind = Kind::None;
  bool b = false;
  int64_t i = 0;
  double f = 0.0;
  std::string s;                             // Str / Bytes; Global: "module name"; Storage: key
  std::vector<PyRef> items;                  // Tuple / List; Object: [callable, args, state]
  std::vector<std::pair<PyRef, PyRef>> dict; // Dict
  // Storage: dtype ("float32", "float16", "bfloat16", other), numel.
  std::string dtype;
  int64_t numel = 0;
  // Tensor: storage + offset + shape + stride.
  PyRef storage;
  int64_t offset = 0;
  std::vector<int64_t> shape, stride;

  bool isNumber() const { return kind == Kind::Int || kind == Kind::Float || kind == Kind::Bool; }
  double number() const { return kind == Kind::Float ? f : double(kind == Kind::Bool ? int64_t(b) : i); }
  // Dict lookup by string key (nullptr when missing or not a dict).
  PyRef get(const std::string& key) const;
};

// Parses pickle bytes. `err` gets a message on failure (unsupported opcode, malformed data).
PyRef unpickle(const uint8_t* data, size_t size, std::string* err);

struct Tensor {
  std::vector<int64_t> shape;
  std::vector<float> data;  // contiguous, row-major
  int64_t numel() const {
    int64_t n = 1;
    for (int64_t d : shape) n *= d;
    return n;
  }
};

class TorchFile {
 public:
  bool open(const std::string& path, std::string* err);
  const PyRef& root() const { return root_; }
  // Copies a tensor's values (any of the supported dtypes) as float32.
  bool load(const PyRef& tensor, Tensor& out, std::string* err) const;
  // Every tensor of a dict of tensors (a state dict), by name.
  bool loadAll(const PyRef& dict, std::map<std::string, Tensor>& out, std::string* err) const;

 private:
  struct Entry {
    uint64_t dataOffset = 0, size = 0;
  };
  bool readEntry(const std::string& name, std::vector<uint8_t>& out, std::string* err) const;
  std::string path_;
  std::string prefix_;  // "<root>/"
  std::map<std::string, Entry> entries_;
  PyRef root_;
};

// Writes a minimal checkpoint in the same format (used by tests to build tiny models): `meta`
// pairs become a dict {"klass": <global>, "args": (), "kwargs": {...}, "state": OrderedDict}.
// Values in `kwargs` are written as Python ints / floats / bools / strings / lists of strings.
struct KwValue {
  enum class Kind { Int, Float, Bool, Str, StrList, None } kind = Kind::None;
  int64_t i = 0;
  double f = 0;
  bool b = false;
  std::string s;
  std::vector<std::string> list;
  static KwValue integer(int64_t v) { KwValue k; k.kind = Kind::Int; k.i = v; return k; }
  static KwValue real(double v) { KwValue k; k.kind = Kind::Float; k.f = v; return k; }
  static KwValue boolean(bool v) { KwValue k; k.kind = Kind::Bool; k.b = v; return k; }
  static KwValue str(std::string v) { KwValue k; k.kind = Kind::Str; k.s = std::move(v); return k; }
  static KwValue strings(std::vector<std::string> v) { KwValue k; k.kind = Kind::StrList; k.list = std::move(v); return k; }
  static KwValue none() { return KwValue{}; }
};
bool writeTorchCheckpoint(const std::string& path, const std::string& klassModule, const std::string& klassName,
                          const std::vector<std::pair<std::string, KwValue>>& kwargs,
                          const std::vector<std::pair<std::string, Tensor>>& state, bool half, std::string* err);

// IEEE half-precision conversion (round to nearest even).
float stemsHalfToFloat(uint16_t h);
uint16_t stemsFloatToHalf(float f);

}  // namespace gl::stems
