// Gridline - J-191: PyTorch checkpoint reader (see TorchFile.h). Written from the pickle
// protocol description (Python's pickletools documentation) and the zip format (PKWARE APPNOTE).
#include "TorchFile.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include "../../core/Zip.h"

namespace gl::stems {

namespace fs = std::filesystem;

PyRef PyValue::get(const std::string& key) const {
  if (kind != Kind::Dict) return nullptr;
  for (const auto& kv : dict)
    if (kv.first && kv.first->kind == Kind::Str && kv.first->s == key) return kv.second;
  return nullptr;
}

namespace {

PyRef make(PyValue::Kind k) {
  auto v = std::make_shared<PyValue>();
  v->kind = k;
  return v;
}

std::string storageDtype(const std::string& global) {
  // "torch FloatStorage" etc.
  if (global == "torch FloatStorage") return "float32";
  if (global == "torch HalfStorage") return "float16";
  if (global == "torch BFloat16Storage") return "bfloat16";
  if (global == "torch DoubleStorage") return "float64";
  return global;
}

float halfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000) << 16;
  uint32_t exp = (h >> 10) & 0x1f;
  uint32_t mant = h & 0x3ff;
  uint32_t bits;
  if (exp == 0) {
    if (mant == 0) {
      bits = sign;
    } else {  // subnormal: normalise
      int e = -1;
      do {
        ++e;
        mant <<= 1;
      } while ((mant & 0x400) == 0);
      mant &= 0x3ff;
      bits = sign | (uint32_t(127 - 15 - e) << 23) | (mant << 13);
    }
  } else if (exp == 31) {
    bits = sign | 0x7f800000u | (mant << 13);
  } else {
    bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
  }
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

uint16_t floatToHalf(float f) {
  uint32_t x;
  std::memcpy(&x, &f, 4);
  const uint16_t sign = uint16_t((x >> 16) & 0x8000);
  int exp = int((x >> 23) & 0xff) - 127 + 15;
  uint32_t mant = x & 0x7fffff;
  if (((x >> 23) & 0xff) == 0xff) return uint16_t(sign | 0x7c00 | (mant ? 0x200 : 0));
  if (exp >= 31) return uint16_t(sign | 0x7c00);
  if (exp <= 0) {
    if (exp < -10) return sign;
    mant |= 0x800000;
    const int shift = 14 - exp;
    uint32_t half = mant >> shift;
    const uint32_t rem = mant & ((1u << shift) - 1);
    const uint32_t mid = 1u << (shift - 1);
    if (rem > mid || (rem == mid && (half & 1))) ++half;
    return uint16_t(sign | half);
  }
  uint32_t half = (uint32_t(exp) << 10) | (mant >> 13);
  const uint32_t rem = mant & 0x1fff;
  if (rem > 0x1000 || (rem == 0x1000 && (half & 1))) ++half;  // may carry into the exponent: fine
  return uint16_t(sign | half);
}

class Unpickler {
 public:
  Unpickler(const uint8_t* d, size_t n) : p_(d), end_(d + n) {}

  PyRef run(std::string* err) {
    while (p_ < end_) {
      const uint8_t op = *p_++;
      if (!step(op)) {
        if (err) *err = err_.empty() ? "unsupported pickle opcode " + std::to_string(int(op)) : err_;
        return nullptr;
      }
      if (done_) {
        if (stack_.empty()) {
          if (err) *err = "empty pickle";
          return nullptr;
        }
        return stack_.back();
      }
    }
    if (err) *err = "truncated pickle";
    return nullptr;
  }

 private:
  bool need(size_t n) {
    if (size_t(end_ - p_) < n) {
      err_ = "truncated pickle";
      return false;
    }
    return true;
  }
  uint64_t le(int n) {
    uint64_t v = 0;
    for (int k = 0; k < n; ++k) v |= uint64_t(p_[k]) << (8 * k);
    p_ += n;
    return v;
  }
  std::string line() {
    const uint8_t* s = p_;
    while (p_ < end_ && *p_ != '\n') ++p_;
    std::string out(reinterpret_cast<const char*>(s), size_t(p_ - s));
    if (p_ < end_) ++p_;
    return out;
  }
  PyRef pop() {
    if (stack_.empty()) {
      err_ = "pickle stack underflow";
      return nullptr;
    }
    PyRef v = stack_.back();
    stack_.pop_back();
    return v;
  }
  // Items above the topmost mark (removed with the mark).
  bool popMark(std::vector<PyRef>& out) {
    if (marks_.empty()) {
      err_ = "pickle mark missing";
      return false;
    }
    const size_t m = marks_.back();
    marks_.pop_back();
    if (m > stack_.size()) {
      err_ = "pickle mark corrupt";
      return false;
    }
    out.assign(stack_.begin() + std::ptrdiff_t(m), stack_.end());
    stack_.resize(m);
    return true;
  }
  void push(PyRef v) { stack_.push_back(std::move(v)); }
  PyRef str(std::string s, PyValue::Kind k = PyValue::Kind::Str) {
    PyRef v = make(k);
    v->s = std::move(s);
    return v;
  }
  bool pushString(size_t n, PyValue::Kind k) {
    if (!need(n)) return false;
    push(str(std::string(reinterpret_cast<const char*>(p_), n), k));
    p_ += n;
    return true;
  }
  PyRef tuple(std::vector<PyRef> items) {
    PyRef v = make(PyValue::Kind::Tuple);
    v->items = std::move(items);
    return v;
  }
  PyRef global(const std::string& module, const std::string& name) {
    PyRef v = make(PyValue::Kind::Global);
    v->s = module + " " + name;
    return v;
  }
  bool memoPut(uint64_t idx) {
    if (stack_.empty()) {
      err_ = "pickle memo of empty stack";
      return false;
    }
    if (idx > 50000000) {
      err_ = "pickle memo index too large";
      return false;
    }
    if (memo_.size() <= idx) memo_.resize(size_t(idx) + 1);
    memo_[size_t(idx)] = stack_.back();
    return true;
  }
  bool memoGet(uint64_t idx) {
    if (idx >= memo_.size() || !memo_[size_t(idx)]) {
      err_ = "pickle memo miss";
      return false;
    }
    push(memo_[size_t(idx)]);
    return true;
  }
  static void setItem(const PyRef& d, PyRef k, PyRef v) {
    for (auto& kv : d->dict)
      if (kv.first && k && kv.first->kind == PyValue::Kind::Str && k->kind == PyValue::Kind::Str && kv.first->s == k->s) {
        kv.second = std::move(v);
        return;
      }
    d->dict.emplace_back(std::move(k), std::move(v));
  }

  PyRef call(const PyRef& fn, const PyRef& args) {
    const std::string name = fn && fn->kind == PyValue::Kind::Global ? fn->s : std::string();
    const std::vector<PyRef> empty;
    const std::vector<PyRef>& a = args && args->kind == PyValue::Kind::Tuple ? args->items : empty;
    if (name == "collections OrderedDict") return make(PyValue::Kind::Dict);
    if (name == "torch._utils _rebuild_tensor_v2" || name == "torch._utils _rebuild_tensor") {
      if (a.size() >= 4 && a[0] && a[0]->kind == PyValue::Kind::Storage) {
        PyRef t = make(PyValue::Kind::Tensor);
        t->storage = a[0];
        t->offset = a[1] ? a[1]->i : 0;
        for (const PyRef& d : a[2]->items) t->shape.push_back(d ? d->i : 0);
        for (const PyRef& d : a[3]->items) t->stride.push_back(d ? d->i : 0);
        return t;
      }
    }
    if (name == "torch._utils _rebuild_parameter" && !a.empty()) return a[0];
    if (name == "fractions Fraction" && !a.empty() && a[0] && a[0]->kind == PyValue::Kind::Str) {
      // Fraction("39/5")
      PyRef v = make(PyValue::Kind::Float);
      const std::string& s = a[0]->s;
      const size_t slash = s.find('/');
      try {
        v->f = slash == std::string::npos ? std::stod(s) : std::stod(s.substr(0, slash)) / std::stod(s.substr(slash + 1));
      } catch (...) {
        v->f = 0;
      }
      v->s = s;
      return v;
    }
    PyRef o = make(PyValue::Kind::Object);
    o->items = {fn, args, nullptr};
    return o;
  }

  bool step(uint8_t op) {
    using K = PyValue::Kind;
    switch (op) {
      case 0x80:  // PROTO
        if (!need(1)) return false;
        ++p_;
        return true;
      case 0x95:  // FRAME
        if (!need(8)) return false;
        p_ += 8;
        return true;
      case '.':
        done_ = true;
        return true;
      case '(':
        marks_.push_back(stack_.size());
        return true;
      case '0':
        return pop() != nullptr || err_.empty();
      case '1': {
        std::vector<PyRef> tmp;
        return popMark(tmp);
      }
      case '2':
        if (stack_.empty()) return false;
        push(stack_.back());
        return true;
      case 'N':
        push(make(K::None));
        return true;
      case 0x88:
      case 0x89: {
        PyRef v = make(K::Bool);
        v->b = op == 0x88;
        push(v);
        return true;
      }
      case 'J': {
        if (!need(4)) return false;
        PyRef v = make(K::Int);
        v->i = int32_t(uint32_t(le(4)));
        push(v);
        return true;
      }
      case 'K': {
        if (!need(1)) return false;
        PyRef v = make(K::Int);
        v->i = int64_t(le(1));
        push(v);
        return true;
      }
      case 'M': {
        if (!need(2)) return false;
        PyRef v = make(K::Int);
        v->i = int64_t(le(2));
        push(v);
        return true;
      }
      case 0x8a:  // LONG1
      case 0x8b: {  // LONG4
        const int lenBytes = op == 0x8a ? 1 : 4;
        if (!need(size_t(lenBytes))) return false;
        const uint64_t n = le(lenBytes);
        if (n > 8 || !need(size_t(n))) {
          err_ = "pickle integer too large";
          return false;
        }
        uint64_t raw = 0;
        for (uint64_t k = 0; k < n; ++k) raw |= uint64_t(p_[k]) << (8 * k);
        if (n > 0 && n < 8 && (p_[n - 1] & 0x80)) raw |= ~uint64_t(0) << (8 * n);  // sign extend
        p_ += n;
        PyRef v = make(K::Int);
        v->i = int64_t(raw);
        push(v);
        return true;
      }
      case 'I': {
        const std::string s = line();
        PyRef v;
        if (s == "00" || s == "01") {
          v = make(K::Bool);
          v->b = s == "01";
        } else {
          v = make(K::Int);
          v->i = std::strtoll(s.c_str(), nullptr, 10);
        }
        push(v);
        return true;
      }
      case 'G': {
        if (!need(8)) return false;
        uint64_t bits = 0;
        for (int k = 0; k < 8; ++k) bits = (bits << 8) | p_[k];
        p_ += 8;
        double d;
        std::memcpy(&d, &bits, 8);
        PyRef v = make(K::Float);
        v->f = d;
        push(v);
        return true;
      }
      case 'X':
      case 'B':
      case 'T': {
        if (!need(4)) return false;
        return pushString(size_t(le(4)), op == 'B' ? K::Bytes : K::Str);
      }
      case 0x8c:
      case 'C':
      case 'U': {
        if (!need(1)) return false;
        return pushString(size_t(le(1)), op == 'C' ? K::Bytes : K::Str);
      }
      case 0x8d:
      case 0x8e: {
        if (!need(8)) return false;
        const uint64_t n = le(8);
        if (n > uint64_t(end_ - p_)) {
          err_ = "truncated pickle";
          return false;
        }
        return pushString(size_t(n), op == 0x8e ? K::Bytes : K::Str);
      }
      case ')':
        push(tuple({}));
        return true;
      case 't': {
        std::vector<PyRef> items;
        if (!popMark(items)) return false;
        push(tuple(std::move(items)));
        return true;
      }
      case 0x85:
      case 0x86:
      case 0x87: {
        const size_t n = size_t(op - 0x84);
        if (stack_.size() < n) return false;
        std::vector<PyRef> items(stack_.end() - std::ptrdiff_t(n), stack_.end());
        stack_.resize(stack_.size() - n);
        push(tuple(std::move(items)));
        return true;
      }
      case ']':
        push(make(K::List));
        return true;
      case 'l': {
        std::vector<PyRef> items;
        if (!popMark(items)) return false;
        PyRef v = make(K::List);
        v->items = std::move(items);
        push(v);
        return true;
      }
      case 'a': {
        PyRef v = pop();
        if (!v || stack_.empty()) return false;
        stack_.back()->items.push_back(v);
        return true;
      }
      case 'e':
      case 0x90: {  // APPENDS / ADDITEMS
        std::vector<PyRef> items;
        if (!popMark(items) || stack_.empty()) return false;
        auto& dst = stack_.back()->items;
        dst.insert(dst.end(), items.begin(), items.end());
        return true;
      }
      case '}':
        push(make(K::Dict));
        return true;
      case 0x8f:
        push(make(K::List));  // EMPTY_SET: a list is enough here
        return true;
      case 0x91: {  // FROZENSET
        std::vector<PyRef> items;
        if (!popMark(items)) return false;
        PyRef v = make(K::List);
        v->items = std::move(items);
        push(v);
        return true;
      }
      case 'd':
      case 'u': {
        std::vector<PyRef> items;
        if (!popMark(items)) return false;
        PyRef d;
        if (op == 'd') {
          d = make(K::Dict);
        } else {
          if (stack_.empty()) return false;
          d = stack_.back();
        }
        if (d->kind != K::Dict) {
          err_ = "pickle SETITEMS on a non-dict";
          return false;
        }
        for (size_t k = 0; k + 1 < items.size(); k += 2) setItem(d, items[k], items[k + 1]);
        if (op == 'd') push(d);
        return true;
      }
      case 's': {
        PyRef v = pop(), k = pop();
        if (!v || !k || stack_.empty() || stack_.back()->kind != K::Dict) return false;
        setItem(stack_.back(), k, v);
        return true;
      }
      case 'c': {
        const std::string module = line();
        const std::string name = line();
        push(global(module, name));
        return true;
      }
      case 0x93: {
        PyRef name = pop(), module = pop();
        if (!name || !module) return false;
        push(global(module->s, name->s));
        return true;
      }
      case 'R': {
        PyRef args = pop(), fn = pop();
        if (!args || !fn) return false;
        push(call(fn, args));
        return true;
      }
      case 0x81: {  // NEWOBJ cls args
        PyRef args = pop(), cls = pop();
        if (!args || !cls) return false;
        push(call(cls, args));
        return true;
      }
      case 0x92: {  // NEWOBJ_EX cls args kwargs
        PyRef kw = pop(), args = pop(), cls = pop();
        if (!kw || !args || !cls) return false;
        push(call(cls, args));
        return true;
      }
      case 'b': {  // BUILD
        PyRef state = pop();
        if (!state || stack_.empty()) return false;
        PyRef& obj = stack_.back();
        if (obj->kind == K::Object && obj->items.size() == 3) obj->items[2] = state;
        return true;  // OrderedDict / tensor states carry nothing needed here
      }
      case 'q':
        if (!need(1)) return false;
        return memoPut(le(1));
      case 'r':
        if (!need(4)) return false;
        return memoPut(le(4));
      case 0x94:
        return memoPut(memo_.size());
      case 'p':
        return memoPut(uint64_t(std::strtoull(line().c_str(), nullptr, 10)));
      case 'h':
        if (!need(1)) return false;
        return memoGet(le(1));
      case 'j':
        if (!need(4)) return false;
        return memoGet(le(4));
      case 'g':
        return memoGet(uint64_t(std::strtoull(line().c_str(), nullptr, 10)));
      case 'Q': {  // BINPERSID: ('storage', storage_type, key, location, numel)
        PyRef pid = pop();
        if (!pid || pid->kind != K::Tuple || pid->items.size() < 5) {
          err_ = "unsupported persistent id";
          return false;
        }
        PyRef st = make(K::Storage);
        st->dtype = storageDtype(pid->items[1] ? pid->items[1]->s : std::string());
        st->s = pid->items[2] ? pid->items[2]->s : std::string();
        st->numel = pid->items[4] ? pid->items[4]->i : 0;
        push(st);
        return true;
      }
      default:
        return false;
    }
  }

  const uint8_t* p_;
  const uint8_t* end_;
  std::vector<PyRef> stack_;
  std::vector<size_t> marks_;
  std::vector<PyRef> memo_;
  std::string err_;
  bool done_ = false;
};

uint64_t rd(const uint8_t* p, int n) {
  uint64_t v = 0;
  for (int k = 0; k < n; ++k) v |= uint64_t(p[k]) << (8 * k);
  return v;
}

bool fail(std::string* err, const std::string& msg) {
  if (err) *err = msg;
  return false;
}

}  // namespace

PyRef unpickle(const uint8_t* data, size_t size, std::string* err) {
  Unpickler u(data, size);
  return u.run(err);
}

bool TorchFile::open(const std::string& path, std::string* err) {
  path_ = path;
  entries_.clear();
  root_.reset();
  std::ifstream f(fs::u8path(path), std::ios::binary);
  if (!f) return fail(err, "cannot open " + path);
  f.seekg(0, std::ios::end);
  const uint64_t fileSize = uint64_t(f.tellg());
  if (fileSize < 22) return fail(err, "not a model file (too small)");
  const uint64_t tail = std::min<uint64_t>(fileSize, 65536 + 22);
  std::vector<uint8_t> buf(static_cast<size_t>(tail));
  f.seekg(std::streamoff(fileSize - tail));
  f.read(reinterpret_cast<char*>(buf.data()), std::streamsize(tail));
  int64_t eocd = -1;
  for (int64_t k = int64_t(tail) - 22; k >= 0; --k)
    if (rd(&buf[size_t(k)], 4) == 0x06054b50) {
      eocd = k;
      break;
    }
  if (eocd < 0) return fail(err, "not a PyTorch model file (no zip directory)");
  uint64_t cdCount = rd(&buf[size_t(eocd) + 10], 2);
  uint64_t cdSize = rd(&buf[size_t(eocd) + 12], 4);
  uint64_t cdOffset = rd(&buf[size_t(eocd) + 16], 4);
  if ((cdOffset == 0xffffffffu || cdCount == 0xffff || cdSize == 0xffffffffu) && eocd >= 20 &&
      rd(&buf[size_t(eocd) - 20], 4) == 0x07064b50) {
    const uint64_t z64 = rd(&buf[size_t(eocd) - 20 + 8], 8);
    uint8_t rec[56];
    f.seekg(std::streamoff(z64));
    f.read(reinterpret_cast<char*>(rec), 56);
    if (!f || rd(rec, 4) != 0x06064b50) return fail(err, "corrupt zip64 directory");
    cdCount = rd(rec + 32, 8);
    cdSize = rd(rec + 40, 8);
    cdOffset = rd(rec + 48, 8);
  }
  if (cdOffset + cdSize > fileSize || cdSize > (uint64_t(1) << 30)) return fail(err, "corrupt zip directory");
  std::vector<uint8_t> cd(static_cast<size_t>(cdSize));
  f.clear();
  f.seekg(std::streamoff(cdOffset));
  f.read(reinterpret_cast<char*>(cd.data()), std::streamsize(cdSize));
  if (!f) return fail(err, "cannot read zip directory");
  size_t pos = 0;
  std::string pklName;
  for (uint64_t n = 0; n < cdCount; ++n) {
    if (pos + 46 > cd.size() || rd(&cd[pos], 4) != 0x02014b50) return fail(err, "corrupt zip directory entry");
    const uint64_t method = rd(&cd[pos + 10], 2);
    uint64_t csize = rd(&cd[pos + 20], 4);
    uint64_t usize = rd(&cd[pos + 24], 4);
    const size_t nameLen = size_t(rd(&cd[pos + 28], 2));
    const size_t extraLen = size_t(rd(&cd[pos + 30], 2));
    const size_t commentLen = size_t(rd(&cd[pos + 32], 2));
    uint64_t local = rd(&cd[pos + 42], 4);
    if (pos + 46 + nameLen + extraLen + commentLen > cd.size()) return fail(err, "corrupt zip directory entry");
    const std::string name(reinterpret_cast<const char*>(&cd[pos + 46]), nameLen);
    // zip64 extra field
    size_t e = pos + 46 + nameLen;
    const size_t eEnd = e + extraLen;
    while (e + 4 <= eEnd) {
      const uint64_t id = rd(&cd[e], 2), len = rd(&cd[e + 2], 2);
      if (id == 1) {
        size_t q = e + 4;
        if (usize == 0xffffffffu && q + 8 <= eEnd) usize = rd(&cd[q], 8), q += 8;
        if (csize == 0xffffffffu && q + 8 <= eEnd) csize = rd(&cd[q], 8), q += 8;
        if (local == 0xffffffffu && q + 8 <= eEnd) local = rd(&cd[q], 8);
      }
      e += 4 + size_t(len);
    }
    pos += 46 + nameLen + extraLen + commentLen;
    if (method != 0) return fail(err, "compressed model file entries are not supported (" + name + ")");
    uint8_t lh[30];
    f.seekg(std::streamoff(local));
    f.read(reinterpret_cast<char*>(lh), 30);
    if (!f || rd(lh, 4) != 0x04034b50) return fail(err, "corrupt zip entry " + name);
    Entry en;
    en.dataOffset = local + 30 + rd(lh + 26, 2) + rd(lh + 28, 2);
    en.size = usize;
    if (en.dataOffset + en.size > fileSize) return fail(err, "truncated model file");
    entries_[name] = en;
    if (name.size() >= 9 && name.compare(name.size() - 9, 9, "/data.pkl") == 0) pklName = name;
  }
  if (pklName.empty()) return fail(err, "not a PyTorch model file (no data.pkl)");
  prefix_ = pklName.substr(0, pklName.size() - 8);  // keep trailing '/'
  std::vector<uint8_t> pkl;
  if (!readEntry(pklName, pkl, err)) return false;
  std::string perr;
  root_ = unpickle(pkl.data(), pkl.size(), &perr);
  if (!root_) return fail(err, "cannot read the model description: " + perr);
  return true;
}

bool TorchFile::readEntry(const std::string& name, std::vector<uint8_t>& out, std::string* err) const {
  auto it = entries_.find(name);
  if (it == entries_.end()) return fail(err, "missing entry " + name);
  std::ifstream f(fs::u8path(path_), std::ios::binary);
  out.resize(size_t(it->second.size));
  f.seekg(std::streamoff(it->second.dataOffset));
  f.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size()));
  if (!f) return fail(err, "cannot read entry " + name);
  return true;
}

bool TorchFile::load(const PyRef& t, Tensor& out, std::string* err) const {
  if (!t || t->kind != PyValue::Kind::Tensor || !t->storage) return fail(err, "not a tensor");
  const PyRef& st = t->storage;
  int elem = 0;
  if (st->dtype == "float32") elem = 4;
  else if (st->dtype == "float16" || st->dtype == "bfloat16") elem = 2;
  else if (st->dtype == "float64") elem = 8;
  else return fail(err, "unsupported tensor type " + st->dtype);
  out.shape = t->shape;
  const int64_t n = out.numel();
  // Contiguity check (row-major strides).
  int64_t expect = 1;
  for (size_t d = t->shape.size(); d-- > 0;) {
    if (t->shape[d] != 1 && d < t->stride.size() && t->stride[d] != expect) return fail(err, "non-contiguous tensor");
    expect *= t->shape[d];
  }
  if (t->offset < 0 || t->offset + n > st->numel) return fail(err, "tensor outside its storage");
  auto it = entries_.find(prefix_ + "data/" + st->s);
  if (it == entries_.end()) return fail(err, "missing storage " + st->s);
  if (uint64_t(st->numel) * uint64_t(elem) > it->second.size) return fail(err, "short storage " + st->s);
  std::vector<uint8_t> raw(size_t(n) * size_t(elem));
  std::ifstream f(fs::u8path(path_), std::ios::binary);
  f.seekg(std::streamoff(it->second.dataOffset + uint64_t(t->offset) * uint64_t(elem)));
  f.read(reinterpret_cast<char*>(raw.data()), std::streamsize(raw.size()));
  if (!f) return fail(err, "cannot read storage " + st->s);
  out.data.resize(size_t(n));
  for (int64_t k = 0; k < n; ++k) {
    const uint8_t* p = &raw[size_t(k) * size_t(elem)];
    if (elem == 4) {
      const uint32_t b = uint32_t(rd(p, 4));
      std::memcpy(&out.data[size_t(k)], &b, 4);
    } else if (elem == 8) {
      const uint64_t b = rd(p, 8);
      double d;
      std::memcpy(&d, &b, 8);
      out.data[size_t(k)] = float(d);
    } else if (st->dtype == "float16") {
      out.data[size_t(k)] = halfToFloat(uint16_t(rd(p, 2)));
    } else {
      const uint32_t b = uint32_t(rd(p, 2)) << 16;
      std::memcpy(&out.data[size_t(k)], &b, 4);
    }
  }
  return true;
}

bool TorchFile::loadAll(const PyRef& dict, std::map<std::string, Tensor>& out, std::string* err) const {
  if (!dict || dict->kind != PyValue::Kind::Dict) return fail(err, "no tensor table");
  for (const auto& kv : dict->dict) {
    if (!kv.first || kv.first->kind != PyValue::Kind::Str || !kv.second || kv.second->kind != PyValue::Kind::Tensor)
      continue;
    Tensor t;
    if (!load(kv.second, t, err)) return false;
    out[kv.first->s] = std::move(t);
  }
  return true;
}

// ---- writer (tests) ----

namespace {

struct PickleWriter {
  std::vector<uint8_t> o;
  void op(uint8_t c) { o.push_back(c); }
  void u32(uint32_t v) {
    for (int k = 0; k < 4; ++k) o.push_back(uint8_t(v >> (8 * k)));
  }
  void str(const std::string& s) {
    op('X');
    u32(uint32_t(s.size()));
    o.insert(o.end(), s.begin(), s.end());
  }
  void integer(int64_t v) {
    if (v >= 0 && v < 256) {
      op('K');
      o.push_back(uint8_t(v));
    } else if (v >= INT32_MIN && v <= INT32_MAX) {
      op('J');
      u32(uint32_t(int32_t(v)));
    } else {
      op(0x8a);
      o.push_back(8);
      for (int k = 0; k < 8; ++k) o.push_back(uint8_t(uint64_t(v) >> (8 * k)));
    }
  }
  void real(double d) {
    op('G');
    uint64_t b;
    std::memcpy(&b, &d, 8);
    for (int k = 7; k >= 0; --k) o.push_back(uint8_t(b >> (8 * k)));
  }
  void global(const std::string& m, const std::string& n) {
    op('c');
    o.insert(o.end(), m.begin(), m.end());
    o.push_back('\n');
    o.insert(o.end(), n.begin(), n.end());
    o.push_back('\n');
  }
};

}  // namespace

bool writeTorchCheckpoint(const std::string& path, const std::string& klassModule, const std::string& klassName,
                          const std::vector<std::pair<std::string, KwValue>>& kwargs,
                          const std::vector<std::pair<std::string, Tensor>>& state, bool half, std::string* err) {
  PickleWriter w;
  w.op(0x80);
  w.o.push_back(2);
  w.op('}');
  w.op('(');
  w.str("klass");
  w.global(klassModule, klassName);
  w.str("args");
  w.op(')');
  w.str("kwargs");
  w.op('}');
  w.op('(');
  for (const auto& kv : kwargs) {
    w.str(kv.first);
    const KwValue& v = kv.second;
    switch (v.kind) {
      case KwValue::Kind::Int: w.integer(v.i); break;
      case KwValue::Kind::Float: w.real(v.f); break;
      case KwValue::Kind::Bool: w.op(v.b ? 0x88 : 0x89); break;
      case KwValue::Kind::Str: w.str(v.s); break;
      case KwValue::Kind::None: w.op('N'); break;
      case KwValue::Kind::StrList:
        w.op(']');
        w.op('(');
        for (const auto& s : v.list) w.str(s);
        w.op('e');
        break;
    }
  }
  w.op('u');
  w.str("state");
  w.global("collections", "OrderedDict");
  w.op(')');
  w.op('R');
  w.op('(');
  std::vector<std::pair<std::string, std::vector<uint8_t>>> blobs;
  for (size_t k = 0; k < state.size(); ++k) {
    const Tensor& t = state[k].second;
    const std::string key = std::to_string(k);
    w.str(state[k].first);
    w.global("torch._utils", "_rebuild_tensor_v2");
    w.op('(');
    // persistent id tuple
    w.op('(');
    w.str("storage");
    w.global("torch", half ? "HalfStorage" : "FloatStorage");
    w.str(key);
    w.str("cpu");
    w.integer(int64_t(t.data.size()));
    w.op('t');
    w.op('Q');
    w.integer(0);
    w.op('(');
    for (int64_t d : t.shape) w.integer(d);
    w.op('t');
    w.op('(');
    std::vector<int64_t> strides(t.shape.size());
    int64_t s = 1;
    for (size_t d = t.shape.size(); d-- > 0;) {
      strides[d] = s;
      s *= t.shape[d];
    }
    for (int64_t d : strides) w.integer(d);
    w.op('t');
    w.op(0x89);
    w.global("collections", "OrderedDict");
    w.op(')');
    w.op('R');
    w.op('t');
    w.op('R');
    std::vector<uint8_t> blob;
    for (float v : t.data) {
      if (half) {
        const uint16_t h = floatToHalf(v);
        blob.push_back(uint8_t(h));
        blob.push_back(uint8_t(h >> 8));
      } else {
        uint32_t b;
        std::memcpy(&b, &v, 4);
        for (int q = 0; q < 4; ++q) blob.push_back(uint8_t(b >> (8 * q)));
      }
    }
    blobs.emplace_back("archive/data/" + key, std::move(blob));
  }
  w.op('u');
  w.op('u');
  w.op('.');
  ZipWriter z;
  if (!z.open(path, err)) return false;
  if (!z.add("archive/data.pkl", w.o)) return fail(err, "cannot write " + path);
  for (auto& b : blobs)
    if (!z.add(b.first, b.second)) return fail(err, "cannot write " + path);
  const std::string ver = "3\n";
  if (!z.add("archive/version", std::vector<uint8_t>(ver.begin(), ver.end()))) return fail(err, "cannot write " + path);
  return z.close(err);
}

float stemsHalfToFloat(uint16_t h) { return halfToFloat(h); }
uint16_t stemsFloatToHalf(float f) { return floatToHalf(f); }

}  // namespace gl::stems
