// Gridline — "utility" pack: loudness (BS.1770-4 / R128), true peak, notes
// encoding and the free-filter design. Part of the core library (no UI).
#include <cstdio>
#include "utility_rt.h"

namespace gl::packs::utility {

// ------------------------------------------------------------------ loudness

void LoudnessMeter::setSampleRate(double sr) {
  if (sr == sr_) return;
  sr_ = sr;
  const double pi = 3.14159265358979323846;
  // Stage 1: high shelf (head acoustics), stage 2: high pass (RLB weighting),
  // designed from their analogue prototypes for any sample rate.
  {
    double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
    double K = std::tan(pi * f0 / sr), Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
    double a0 = 1.0 + K / Q + K * K;
    for (auto& b : shelf_) {
      b.b0 = (Vh + Vb * K / Q + K * K) / a0;
      b.b1 = 2.0 * (K * K - Vh) / a0;
      b.b2 = (Vh - Vb * K / Q + K * K) / a0;
      b.a1 = 2.0 * (K * K - 1.0) / a0;
      b.a2 = (1.0 - K / Q + K * K) / a0;
    }
  }
  {
    double f0 = 38.13547087602444, Q = 0.5003270373238773;
    double K = std::tan(pi * f0 / sr), a0 = 1.0 + K / Q + K * K;
    for (auto& b : hp_) {
      b.b0 = 1.0;
      b.b1 = -2.0;
      b.b2 = 1.0;
      b.a1 = 2.0 * (K * K - 1.0) / a0;
      b.a2 = (1.0 - K / Q + K * K) / a0;
    }
  }
  subLen_ = std::max(1, static_cast<int>(std::lround(sr * 0.1)));
  reset();
}

void LoudnessMeter::reset() {
  for (auto* arr : {shelf_, hp_})
    for (int c = 0; c < 2; ++c) arr[c].z1 = arr[c].z2 = 0;
  subPos_ = 0;
  subSum_ = 0;
  subCount_ = subHead_ = 0;
  std::memset(subs_, 0, sizeof subs_);
  std::memset(gCount_, 0, sizeof gCount_);
  std::memset(gEnergy_, 0, sizeof gEnergy_);
  std::memset(sCount_, 0, sizeof sCount_);
  std::memset(sEnergy_, 0, sizeof sEnergy_);
  momentary_ = shortTerm_ = integrated_ = maxMomentary_ = kNoLoudness;
  lra_ = 0;
}

void LoudnessMeter::process(const float* L, const float* R, int n) {
  for (int i = 0; i < n; ++i) {
    double l = hp_[0].run(shelf_[0].run(L[i]));
    double r = hp_[1].run(shelf_[1].run(R[i]));
    subSum_ += l * l + r * r;
    if (++subPos_ >= subLen_) {
      addSubBlock(subSum_ / subLen_);
      subSum_ = 0;
      subPos_ = 0;
    }
  }
  // Flush denormals in the filter states.
  for (auto* arr : {shelf_, hp_})
    for (int c = 0; c < 2; ++c) {
      if (std::fabs(arr[c].z1) < 1e-25) arr[c].z1 = 0;
      if (std::fabs(arr[c].z2) < 1e-25) arr[c].z2 = 0;
    }
}

namespace {
int binOf(float lufs, int bins) { return std::clamp(static_cast<int>((lufs + 70.0f) * 10.0f), 0, bins - 1); }
float binCentre(int b) { return -70.0f + (static_cast<float>(b) + 0.5f) * 0.1f; }
}  // namespace

void LoudnessMeter::addSubBlock(double energy) {
  subs_[subHead_] = energy;
  subHead_ = (subHead_ + 1) % 30;
  if (subCount_ < 1000000) ++subCount_;
  auto meanLast = [&](int count) {
    double s = 0;
    for (int k = 1; k <= count; ++k) s += subs_[(subHead_ - k + 30) % 30];
    return s / count;
  };
  if (subCount_ >= 4) {
    double em = meanLast(4);
    momentary_ = toLufs(em);
    maxMomentary_ = std::max(maxMomentary_, momentary_);
    if (momentary_ > -70.0f) {
      int b = binOf(momentary_, kBins);
      ++gCount_[b];
      gEnergy_[b] += em;
      updateIntegrated();
    }
    double es = meanLast(std::min(subCount_, 30));
    shortTerm_ = toLufs(es);
    if (subCount_ >= 30 && shortTerm_ > -70.0f) {
      int b = binOf(shortTerm_, kBins);
      ++sCount_[b];
      sEnergy_[b] += es;
      // Loudness range: 10th..95th percentile of short-term values above a -20 LU relative gate.
      double e = 0;
      uint64_t c = 0;
      for (int k = 0; k < kBins; ++k) {
        e += sEnergy_[k];
        c += sCount_[k];
      }
      float gate = toLufs(e / static_cast<double>(c)) - 20.0f;
      uint64_t total = 0;
      for (int k = 0; k < kBins; ++k)
        if (binCentre(k) >= gate) total += sCount_[k];
      float lo = 0, hi = 0;
      uint64_t acc = 0;
      bool haveLo = false;
      for (int k = 0; k < kBins && total; ++k) {
        if (binCentre(k) < gate) continue;
        acc += sCount_[k];
        if (!haveLo && acc * 10 >= total) {
          lo = binCentre(k);
          haveLo = true;
        }
        if (acc * 100 >= total * 95) {
          hi = binCentre(k);
          break;
        }
      }
      lra_ = std::max(0.0f, hi - lo);
    }
  }
}

void LoudnessMeter::updateIntegrated() {
  double e = 0;
  uint64_t c = 0;
  for (int k = 0; k < kBins; ++k) {
    e += gEnergy_[k];
    c += gCount_[k];
  }
  if (!c) {
    integrated_ = kNoLoudness;
    return;
  }
  float gate = toLufs(e / static_cast<double>(c)) - 10.0f;
  double e2 = 0;
  uint64_t c2 = 0;
  for (int k = 0; k < kBins; ++k)
    if (binCentre(k) > gate) {
      e2 += gEnergy_[k];
      c2 += gCount_[k];
    }
  integrated_ = c2 ? toLufs(e2 / static_cast<double>(c2)) : kNoLoudness;
}

// ----------------------------------------------------------------- true peak

TruePeak::TruePeak() {
  const double pi = 3.14159265358979323846;
  const int len = kPhases * kTaps;
  const double centre = (len - 1) * 0.5, cut = 0.92;
  for (int p = 0; p < kPhases; ++p) {
    double sum = 0;
    for (int k = 0; k < kTaps; ++k) {
      int m = k * kPhases + p;
      double t = (m - centre) / kPhases * cut;
      double s = std::fabs(t) < 1e-9 ? 1.0 : std::sin(pi * t) / (pi * t);
      double w = 0.42 - 0.5 * std::cos(2 * pi * (m + 0.5) / len) + 0.08 * std::cos(4 * pi * (m + 0.5) / len);
      h_[p][k] = static_cast<float>(s * w);
      sum += s * w;
    }
    for (int k = 0; k < kTaps; ++k) h_[p][k] = static_cast<float>(h_[p][k] / sum);
  }
}

float TruePeak::process(const float* x, int n) {
  float peak = 0;
  for (int i = 0; i < n; ++i) {
    pos_ = (pos_ + kTaps - 1) % kTaps;
    hist_[pos_] = hist_[pos_ + kTaps] = x[i];
    const float* hx = hist_ + pos_;
    for (int p = 0; p < kPhases; ++p) {
      float y = 0;
      for (int k = 0; k < kTaps; ++k) y += hx[k] * h_[p][k];
      peak = std::max(peak, std::fabs(y));
    }
    peak = std::max(peak, std::fabs(x[i]));
  }
  return peak;
}

// ---------------------------------------------------------------- free filter

Coefs freeFilterCoefs(int type, double sr, double hz, double q, double gainDb) {
  const double pi = 3.14159265358979323846;
  hz = std::clamp(hz, 10.0, sr * 0.49);
  q = std::max(q, 0.05);
  double A = std::pow(10.0, gainDb / 40.0), w = 2 * pi * hz / sr, cw = std::cos(w), sw = std::sin(w);
  double alpha = sw / (2 * q);
  double b0, b1, b2, a0, a1, a2;
  switch (type) {
    case FtLowPass:
      b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
      break;
    case FtBandPass:  // 0 dB at the centre
      b0 = alpha; b1 = 0; b2 = -alpha; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
      break;
    case FtHighPass:
      b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
      break;
    case FtNotch:
      b0 = 1; b1 = -2 * cw; b2 = 1; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
      break;
    case FtLowShelf: {
      double s = 2 * std::sqrt(A) * alpha;
      b0 = A * ((A + 1) - (A - 1) * cw + s); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - s);
      a0 = (A + 1) + (A - 1) * cw + s; a1 = -2 * ((A - 1) + (A + 1) * cw); a2 = (A + 1) + (A - 1) * cw - s;
      break;
    }
    case FtHighShelf: {
      double s = 2 * std::sqrt(A) * alpha;
      b0 = A * ((A + 1) + (A - 1) * cw + s); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - s);
      a0 = (A + 1) - (A - 1) * cw + s; a1 = 2 * ((A - 1) - (A + 1) * cw); a2 = (A + 1) - (A - 1) * cw - s;
      break;
    }
    case FtPeak:
    default:
      b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A; a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
      break;
  }
  Coefs c;
  c.b0 = b0 / a0;
  c.b1 = b1 / a0;
  c.b2 = b2 / a0;
  c.a1 = a1 / a0;
  c.a2 = a2 / a0;
  return c;
}

// ---------------------------------------------------------------------- notes

std::vector<std::string> decodeNotes(const std::string& data) {
  std::vector<std::string> pages(kNotePages);
  static const std::string magic = "GLNOTES1\n";
  if (data.compare(0, magic.size(), magic) != 0) {
    pages[0] = data;  // plain text: first page
    return pages;
  }
  size_t p = magic.size();
  while (p < data.size() && data[p] == '#') {
    size_t colon = data.find(':', p), nl = data.find('\n', p);
    if (colon == std::string::npos || nl == std::string::npos || colon > nl) break;
    int page = std::atoi(data.substr(p + 1, colon - p - 1).c_str());
    size_t len = static_cast<size_t>(std::strtoull(data.substr(colon + 1, nl - colon - 1).c_str(), nullptr, 10));
    size_t start = nl + 1;
    if (start + len > data.size()) break;
    if (page >= 1 && page <= kNotePages) pages[static_cast<size_t>(page - 1)] = data.substr(start, len);
    p = start + len + 1;  // skip the separator newline
  }
  return pages;
}

std::string encodeNotes(const std::vector<std::string>& pages) {
  bool any = false;
  for (const auto& s : pages) any = any || !s.empty();
  if (!any) return {};
  std::string out = "GLNOTES1\n";
  for (size_t i = 0; i < pages.size() && i < static_cast<size_t>(kNotePages); ++i) {
    if (pages[i].empty()) continue;
    char head[48];
    std::snprintf(head, sizeof head, "#%zu:%zu\n", i + 1, pages[i].size());
    out += head;
    out += pages[i];
    out += '\n';
  }
  return out;
}

}  // namespace gl::packs::utility
