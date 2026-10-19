// Gridline — "creative" pack: DSP (Octa Filter).
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include "../Packs.h"
#include "creative.h"
#include "octafilter_dsp.h"

namespace gl::packs::creative {

namespace {

float P(const PluginState& s, int i, float d = 0.f) {
  return i >= 0 && i < static_cast<int>(s.params.size()) ? s.params[static_cast<size_t>(i)] : d;
}
int I(const PluginState& s, int i, int d = 0) { return static_cast<int>(std::lround(P(s, i, static_cast<float>(d)))); }
bool B(const PluginState& s, int i, bool d = false) { return P(s, i, d ? 1.f : 0.f) >= 0.5f; }

constexpr int kTick = 64;

// Pattern position with the envelope's loop (beats).
double loopPos(const OEnv& e, int ls, int le, double p) {
  if (ls >= 0 && le > ls && le < e.n) {
    double xs = e.x[ls], xe = e.x[le];
    if (xe > xs && p > xe) p = xs + std::fmod(p - xs, xe - xs);
  }
  return p;
}

}  // namespace

OctaFilterFx::OctaFilterFx() : bank_(std::make_unique<OctaBank>()) {
  for (int u = 0; u < kUnits; ++u)
    for (int t = 0; t < kTargets; ++t) cur_[u][t] = prev_[u][t] = additiveTarget(t) ? 0.0 : 1.0;
}

void OctaFilterFx::reset() {
  for (auto& u : state_)
    for (auto& s : u)
      for (auto& c : s) c.reset();
  ief_ = 0, tickPos_ = 0, lastPattern_ = -1, freeBeat_ = 0;
  for (auto& u : lfoPh_)
    for (double& p : u) p = 0;
  dc_[0].reset(), dc_[1].reset();
}

int OctaFilterFx::meters(float* out, int max) const {
  if (max < 4) return 0;
  out[0] = ieMeter_;
  out[1] = static_cast<float>(modX_);
  out[2] = static_cast<float>(modY_);
  out[3] = static_cast<float>(freeBeat_);
  return 4;
}

void OctaFilterFx::tick(const PluginState& s, const ProcessContext& ctx, double beat) {
  const OctaBank& bk = *bank_;
  // Modulation X / Y with optional smoothing.
  double tx = P(s, OfModX, 0.5f), ty = P(s, OfModY, 0.5f);
  if (B(s, OfSmooth)) {
    double c = B(s, OfHqEnvelopes) ? 0.996 : 0.9685;
    modX_ = (modX_ - tx) * c + tx, modY_ = (modY_ - ty) * c + ty;
  } else {
    modX_ = tx, modY_ = ty;
  }
  // Pattern number and position.
  int pat = std::clamp(I(s, OfPattern), 0, kPatterns - 1);
  if (pat != lastPattern_) {
    patStart_ = B(s, OfSyncPatterns) || lastPattern_ < 0 ? 0.0 : beat;
    lastPattern_ = pat;
  }
  const double pos = beat - patStart_;
  const double lfoLevel = signedVol(P(s, OfMasterLfo));
  const double spb = 60.0 / std::max(1.0, ctx.tempo) * sr_;
  for (int u = 0; u < kUnits; ++u) {
    const double env = P(s, unitParam(u, UEnv), 1.f);
    for (int t = 0; t < kTargets; ++t) {
      prev_[u][t] = cur_[u][t];
      const bool add = additiveTarget(t);
      const double amount = t == TCut ? env : 1.0;
      double v = add ? 0.0 : 1.0;
      const double src[3] = {modX_, modY_, ief_};
      for (int m = 0; m < 3; ++m)
        if (const OEnv* e = bk.env(u, t, KMapX + m)) {
          double f = e->eval(std::clamp(src[m], 0.0, 1.0));
          if (m == 2) f = add ? 0.5 + (f - 0.5) * amount : f;
          if (add) v += f - 0.5;
          else v *= f;
        }
      if (const OEnv* e = bk.env(u, t, KPattern0 + pat)) {
        double p = loopPos(*e, bk.loopS[u][t][pat], bk.loopE[u][t][pat], pos);
        double y = e->eval(p);
        if (add) v += (y - 0.5) * amount;
        else v *= y * amount;
      }
      if (const OEnv* e = bk.env(u, t, KLfo); e && std::fabs(lfoLevel) > 1e-9) {
        // LFO: one-cycle shape; speed maps to cycles per beat (approximate scale).
        double sp = P(s, lfoParam(u, t, LSpeed), 40000.f / 65536.f);
        double cpb = 0.25 * std::pow(2.0, (sp - 40000.0 / 65536.0) * 16.0);
        double ph = std::fmod(lfoPh_[u][t] + P(s, lfoParam(u, t, LPhase)), 1.0);
        double skew = P(s, lfoParam(u, t, LSkew));
        double m = 0.5 + 0.45 * skew;
        double w = ph < m ? ph / m * 0.5 : 0.5 + (ph - m) / (1 - m) * 0.5;
        double o = e->eval(w);
        float tens = P(s, lfoParam(u, t, LTension));
        if (std::fabs(tens) > 1e-4) o = menv::shape(menv::SingleCurve, menv::coef(menv::SingleCurve, tens), std::clamp(o, 0.0, 1.0));
        double pw = P(s, lfoParam(u, t, LPulse));
        if (std::fabs(pw) > 1e-4) o = o > 0.5 - 0.5 * pw ? 1.0 : 0.0;
        double amt = lfoLevel * (t == TCut ? env : 1.0);
        if (add) v += (o - 0.5) * amt;
        else v *= 1.0 + 2.0 * (o - 0.5) * amt;
        lfoPh_[u][t] = std::fmod(lfoPh_[u][t] + cpb * kTick / std::max(1.0, spb), 1.0);
      }
      cur_[u][t] = v;
    }
    // Filter coefficients for this tick.
    int order = std::clamp(I(s, unitParam(u, UOrder), 1), 0, 5);
    int type = std::clamp(I(s, unitParam(u, UType)), 0, FilterTypeCount - 1);
    int stages = order == 0 ? 0 : order <= 3 ? order : order - 2;
    stages_[u] = stages;
    double c = P(s, unitParam(u, UCut), 0.5f) + cur_[u][TCut];
    double r = P(s, unitParam(u, URes), 0.5f) + cur_[u][TRes];
    bool alt = B(s, unitParam(u, UAlt));
    double low = P(s, unitParam(u, ULow), 1.f), band = P(s, unitParam(u, UBand)), high = P(s, unitParam(u, UHigh));
    double lowT = type == FtSvf ? volCurve(low) * cur_[u][TLow] : std::clamp(low * cur_[u][TLow], 0.0, 1.0);
    double fcn = octaFcn(type, c);
    for (int k = 0; k < stages; ++k) {
      coef_[u][k] = octaStage(type, fcn, r, alt, lowT, volCurve(band) * cur_[u][TBand], volCurve(high) * cur_[u][THigh], stages, sr_);
      if (order == 4) fcn = std::sqrt(fcn);
      else if (order == 5) fcn = std::pow(fcn, 0.75);
    }
  }
}

void OctaFilterFx::process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) {
  if (sr_ != ctx.sampleRate) {
    sr_ = ctx.sampleRate;
    dc_[0].setFreq(sr_, 10.0), dc_[1].setFreq(sr_, 10.0);
    tickPos_ = 0;
  }
  size_t h = std::hash<std::string>{}(s.data);
  if (h != dataHash_) {
    dataHash_ = h;
    parse(s.data.c_str(), *bank_);
    for (int u = 0; u < kUnits; ++u) {
      OEnv tmp;
      const OEnv* e = bank_->wsCurve(u);
      if (!e) defaultWs(tmp), e = &tmp;
      for (int i = 0; i <= 1024; ++i) wsTab_[u][i] = static_cast<float>(e->eval(i / 1024.0));
    }
  }
  const double spb = 60.0 / std::max(1.0, ctx.tempo) * sr_;
  const double master = volCurve(P(s, OfMaster, 1.f));
  const int solo = I(s, OfSolo) - 1;
  const bool center = B(s, OfCenter), use1 = B(s, OfIefUse1), hqWs = B(s, OfHqShaper, true);
  const double iefPre = volCurve(P(s, OfIefLevel, 1.f));
  const double iefA = std::exp(-std::log(100.0) / std::max(1.0, P(s, OfIefAttack, 0.025f) * sr_));
  const double iefR = std::exp(-std::log(100.0) / std::max(1.0, P(s, OfIefRelease, 0.1f) * sr_));
  bool en[kUnits];
  double inG[kUnits], nextG[kUnits], outG[kUnits], panK[kUnits], wsPre[kUnits], wsMix[kUnits];
  bool wsOn[kUnits], wsBip[kUnits];
  for (int u = 0; u < kUnits; ++u) {
    en[u] = B(s, unitParam(u, UEnabled), u == 0);
    inG[u] = signedVol(P(s, unitParam(u, UIn), 1.f));
    nextG[u] = signedVol(P(s, unitParam(u, UNext)));
    outG[u] = volCurve(P(s, unitParam(u, UOut), 1.f)) * master;
    panK[u] = P(s, unitParam(u, UPan));
    wsOn[u] = B(s, unitParam(u, UWsOn)), wsBip[u] = B(s, unitParam(u, UWsBipolar));
    wsPre[u] = P(s, unitParam(u, UWsPre), 1.f), wsMix[u] = P(s, unitParam(u, UWsMix), 1.f);
  }
  for (int i = 0; i < n; ++i) {
    double beat = ctx.playing ? ctx.tick / std::max(1, ctx.ppq) + i / spb : freeBeat_;
    if (!ctx.playing) freeBeat_ += 1.0 / spb;
    else freeBeat_ = beat;
    if (tickPos_ == 0) tick(s, ctx, beat);
    const double fr = static_cast<double>(tickPos_) / kTick;
    tickPos_ = (tickPos_ + 1) % kTick;
    const double inL = L[i], inR = R[i];
    double outL = 0, outR = 0, carryL = 0, carryR = 0, unit1L = 0, unit1R = 0;
    bool haveCarry = false;
    double carryGain = 0;
    for (int u = 0; u < kUnits; ++u) {
      if (!en[u]) {
        haveCarry = false;
        continue;
      }
      auto tv = [&](int t) { return prev_[u][t] + (cur_[u][t] - prev_[u][t]) * fr; };
      double xl = inL * inG[u], xr = inR * inG[u];
      if (haveCarry) xl += carryL * carryGain, xr += carryR * carryGain;
      for (int k = 0; k < stages_[u]; ++k) {
        xl = runStage(coef_[u][k], state_[u][k][0], xl);
        xr = runStage(coef_[u][k], state_[u][k][1], xr);
      }
      if (wsOn[u]) {
        double mix = std::clamp(wsMix[u] * tv(TWsMix), 0.0, 1.0);
        auto shape = [&](double x) {
          x *= wsPre[u];
          double y;
          auto look = [&](double a) {  // a in 0..1
            double f = std::clamp(a, 0.0, 1.0) * 1024.0;
            if (!hqWs) return static_cast<double>(wsTab_[u][static_cast<int>(f + 0.5)]);
            int i0 = std::min(1023, static_cast<int>(f));
            double t = f - i0;
            return wsTab_[u][i0] + (wsTab_[u][i0 + 1] - wsTab_[u][i0]) * t;
          };
          if (wsBip[u]) y = 2.0 * look((x + 1.0) * 0.5) - 1.0;
          else y = x < 0 ? -look(-x) : look(x);
          return y;
        };
        xl = xl * (1 - mix) + shape(xl) * mix;
        xr = xr * (1 - mix) + shape(xr) * mix;
      }
      if (u == 0) unit1L = xl, unit1R = xr;
      // To-next feed taps the signal before the out stage.
      carryL = xl, carryR = xr, carryGain = nextG[u], haveCarry = std::fabs(nextG[u]) > 0;
      if (solo >= 0 && solo != u) continue;
      double p = std::clamp(panK[u] + 2.0 * tv(TPan), -1.0, 1.0);
      double g = outG[u] * tv(TVol);
      double gl = p > 0 ? 1 - p : 1, gr = p < 0 ? 1 + p : 1, lr = p > 0 ? 0.5 * p : 0, rl = p < 0 ? -0.5 * p : 0;
      outL += (xl * gl + xr * rl) * g;
      outR += (xr * gr + xl * lr) * g;
    }
    // Envelope follower (input or unit 1).
    double fsrc = (use1 ? std::max(std::fabs(unit1L), std::fabs(unit1R)) : std::max(std::fabs(inL), std::fabs(inR))) * iefPre;
    ief_ = fsrc > ief_ ? fsrc + (ief_ - fsrc) * iefA : fsrc + (ief_ - fsrc) * iefR;
    ief_ = std::min(ief_, 1.0);
    if (center) outL = dc_[0].process(outL), outR = dc_[1].process(outR);
    L[i] = static_cast<float>(outL);
    R[i] = static_cast<float>(outR);
  }
  ieMeter_ = static_cast<float>(ief_);
}

std::unique_ptr<Generator> createGenerator(const std::string&) { return nullptr; }

std::unique_ptr<Effect> createEffect(const std::string& type) {
  if (type == "gl.octafilter") return std::make_unique<OctaFilterFx>();
  return nullptr;
}

bool prepareAux(const PluginState&, ChannelAux&) { return false; }

}  // namespace gl::packs::creative
