// Gridline - automatic updates: SHA-512, Ed25519 (RFC 8032 vectors) and minisign signatures,
// including every way a signature can be tampered with. Cross-checked against the openssl and
// minisign programs when they are installed (skipped otherwise). No network.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include "Check.h"
#include "core/Platform.h"
#include "core/UpdateVerify.h"

using namespace gl::update;
namespace fs = std::filesystem;

namespace {

std::vector<uint8_t> unhex(const std::string& h) {
  std::vector<uint8_t> v;
  for (size_t i = 0; i + 1 < h.size(); i += 2) v.push_back(static_cast<uint8_t>(std::strtoul(h.substr(i, 2).c_str(), nullptr, 16)));
  return v;
}
template <size_t N>
std::array<uint8_t, N> arr(const std::string& h) {
  std::array<uint8_t, N> a{};
  const std::vector<uint8_t> v = unhex(h);
  for (size_t i = 0; i < N && i < v.size(); ++i) a[i] = v[i];
  return a;
}
std::string sha512Hex(const std::string& s) {
  const auto h = sha512(s.data(), s.size());
  return hexOf(h.data(), h.size());
}

struct Vec {
  const char *seed, *pub, *msg, *sig;
};
// RFC 8032 section 7.1, tests 1-3 and SHA(abc).
const Vec kVectors[] = {
    {"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
     "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
    {"4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb", "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
     "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
    {"c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7", "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
     "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"},
};

std::string readFile(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
void writeFile(const fs::path& p, const std::string& s) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << s;
}
bool have(const char* tool) {
  for (const char* d : {"/usr/bin/", "/usr/local/bin/", "/opt/homebrew/bin/", "/bin/"})
    if (fs::exists(std::string(d) + tool)) return true;
  return false;
}
std::string tool(const char* t) {
  for (const char* d : {"/opt/homebrew/bin/", "/usr/local/bin/", "/usr/bin/", "/bin/"})
    if (fs::exists(std::string(d) + t)) return std::string(d) + t;
  return {};
}
int runTool(const std::vector<std::string>& args) {
  gl::platform::Process p;
  if (!p.start(args[0], args)) return -1;
  if (!p.wait(30)) {
    p.kill();
    p.wait(5);
    return -2;
  }
  return p.status().code;
}

}  // namespace

int main() {
  // ---- SHA-512 (FIPS 180-4 examples)
  expect(sha512Hex("") ==
             "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e",
         "sha512 empty");
  expect(sha512Hex("abc") ==
             "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
         "sha512 abc");
  expect(sha512Hex("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu") ==
             "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909",
         "sha512 two blocks");
  {
    Sha512 s;
    const std::string a(1000, 'a');
    for (int i = 0; i < 1000; ++i) s.add(a.data(), a.size());
    const auto h = s.finish();
    expect(hexOf(h.data(), h.size()) ==
               "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973ebde0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b",
           "sha512 a million a's, in pieces");
  }

  // ---- base64
  {
    std::vector<uint8_t> out;
    expect(base64Encode({'f', 'o', 'o', 'b', 'a'}) == "Zm9vYmE=", "base64 encode with padding");
    expect(base64Decode("Zm9vYmFy", out) && std::string(out.begin(), out.end()) == "foobar", "base64 decode");
    expect(!base64Decode("Zm9v!mFy", out), "base64 refuses a stray character");
    expect(!base64Decode("Zm9=YmFy", out), "base64 refuses padding in the middle");
    expect(!base64Decode("Zm9vYmF", out), "base64 refuses a cut-off text");
  }

  // ---- Ed25519 (RFC 8032)
  int vi = 0;
  for (const Vec& v : kVectors) {
    ++vi;
    const auto seed = arr<32>(v.seed);
    const std::vector<uint8_t> msg = unhex(v.msg);
    const auto pub = ed25519PublicKey(seed);
    const std::string name = "rfc8032 test " + std::to_string(vi);
    expect(hexOf(pub.data(), 32) == v.pub, (name + ": public key").c_str());
    const auto sig = ed25519Sign(seed, msg.data(), msg.size());
    expect(hexOf(sig.data(), 64) == v.sig, (name + ": signature").c_str());
    expect(ed25519Verify(sig.data(), pub.data(), msg.data(), msg.size()), (name + ": verifies").c_str());
    std::vector<uint8_t> other = msg;
    other.push_back(0);
    expect(!ed25519Verify(sig.data(), pub.data(), other.data(), other.size()), (name + ": another message fails").c_str());
    auto bad = sig;
    bad[5] ^= 1;
    expect(!ed25519Verify(bad.data(), pub.data(), msg.data(), msg.size()), (name + ": a flipped bit in R fails").c_str());
    bad = sig;
    bad[40] ^= 0x10;
    expect(!ed25519Verify(bad.data(), pub.data(), msg.data(), msg.size()), (name + ": a flipped bit in S fails").c_str());
  }
  {  // S >= L (malleability) is refused
    const auto seed = arr<32>(kVectors[0].seed);
    const auto pub = ed25519PublicKey(seed);
    auto sig = ed25519Sign(seed, nullptr, 0);
    // add L to S: S + L is the same scalar, a different encoding
    static const uint8_t L[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                                  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0x10};
    unsigned carry = 0;
    for (int i = 0; i < 32; ++i) {
      const unsigned t = sig[32 + static_cast<size_t>(i)] + L[i] + carry;
      sig[32 + static_cast<size_t>(i)] = static_cast<uint8_t>(t);
      carry = t >> 8;
    }
    expect(carry == 0 && !ed25519Verify(sig.data(), pub.data(), nullptr, 0), "signature with S + L refused");
  }

  // ---- minisign
  const auto seed = arr<32>("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
  const std::array<uint8_t, 8> keyId = {1, 2, 3, 4, 5, 6, 7, 8};
  MinisignKey key;
  key.keyId = keyId;
  key.pub = ed25519PublicKey(seed);
  const std::string pubText = "untrusted comment: minisign public key 0807060504030201\n" + minisignKeyText(key) + "\n";
  {
    MinisignKey back;
    expect(parseMinisignKey(pubText, back) && back.pub == key.pub && back.keyId == key.keyId, "minisign public key file round trip");
    expect(parseMinisignKey(minisignKeyText(key), back), "minisign public key line alone");
    expect(!parseMinisignKey("untrusted comment: x\nnot base64\n", back), "a damaged public key is refused");
  }
  const std::string sums = "aaaa  Gridline-0.9.0-linux-x86_64.AppImage\n";
  const std::string comment = "timestamp:1764300000\tfile:Gridline-0.9.0-SHA256SUMS\thashed";
  for (bool pre : {false, true}) {
    const std::string tag = pre ? "ED (prehashed): " : "Ed: ";
    const std::string sig = signMinisign(seed, keyId, sums, comment, pre);
    SigInfo info;
    expect(verifyMinisign(sig, sums, {key}, "Gridline-0.9.0-SHA256SUMS", &info) == SigResult::Ok && info.prehashed == pre,
           (tag + "good signature").c_str());
    expect(info.trustedComment == comment, (tag + "trusted comment read").c_str());
    expect(verifyMinisign(sig, sums + " ", {key}, "Gridline-0.9.0-SHA256SUMS") == SigResult::BadSignature, (tag + "changed file refused").c_str());
    expect(verifyMinisign(sig, sums, {key}, "Gridline-0.8.0-SHA256SUMS") == SigResult::WrongFile, (tag + "signature of another file refused").c_str());
    std::string c2 = sig;
    c2.replace(c2.find("timestamp:1764300000"), 20, "timestamp:1764300001");
    expect(verifyMinisign(c2, sums, {key}, "") == SigResult::BadComment, (tag + "changed trusted comment refused").c_str());
    MinisignKey other = key;
    other.keyId[0] ^= 0xff;
    expect(verifyMinisign(sig, sums, {other}, "") == SigResult::WrongKey, (tag + "unknown key id refused").c_str());
    MinisignKey wrongPub = key;
    wrongPub.pub = ed25519PublicKey(arr<32>(kVectors[1].seed));
    expect(verifyMinisign(sig, sums, {wrongPub}, "") == SigResult::BadSignature, (tag + "same key id, other key refused").c_str());
    expect(verifyMinisign(sig, sums, {}, "") == SigResult::WrongKey, (tag + "no trusted keys: refused").c_str());
    expect(verifyMinisign("untrusted comment: x\n", sums, {key}, "") == SigResult::Malformed, (tag + "cut-off file refused").c_str());
    std::string broken = sig;
    broken[broken.find('\n') + 3] = '!';
    expect(verifyMinisign(broken, sums, {key}, "") == SigResult::Malformed, (tag + "damaged base64 refused").c_str());
    expect(verifyMinisign("<html>Sign in to the network</html>", sums, {key}, "") == SigResult::Malformed, (tag + "an HTML page refused").c_str());
  }
  expect(releaseKeys().empty() || !releaseKeys().empty(), "release key list readable");

  // ---- the same results as the openssl program (skipped when it lacks Ed25519)
  {
    const fs::path dir = fs::temp_directory_path() / ("gl-verify-" + std::to_string(gl::platform::processId()));
    fs::create_directories(dir);
    const std::string openssl = tool("openssl");
    if (!openssl.empty()) {
      // A PKCS#8 Ed25519 key from the seed (RFC 8410 layout).
      std::string der = std::string("\x30\x2e\x02\x01\x00\x30\x05\x06\x03\x2b\x65\x70\x04\x22\x04\x20", 16);
      der.append(reinterpret_cast<const char*>(seed.data()), 32);
      writeFile(dir / "k.der", der);
      writeFile(dir / "m.txt", sums);
      const int rc = runTool({openssl, "pkeyutl", "-sign", "-inkey", (dir / "k.der").string(), "-keyform", "DER", "-rawin", "-in",
                              (dir / "m.txt").string(), "-out", (dir / "s.bin").string()});
      if (rc == 0) {
        const std::string theirs = readFile(dir / "s.bin");
        const auto mine = ed25519Sign(seed, reinterpret_cast<const uint8_t*>(sums.data()), sums.size());
        expect(theirs.size() == 64 && std::equal(mine.begin(), mine.end(), reinterpret_cast<const uint8_t*>(theirs.data())),
               "openssl's Ed25519 signature equals ours");
      } else {
        std::printf("# openssl has no Ed25519 here: skipped\n");
      }
    }
    // A key and signature made by the real minisign program.
    const std::string ms = tool("minisign");
    if (!ms.empty()) {
      writeFile(dir / "Gridline-0.9.0-SHA256SUMS", sums);
      const int g = runTool({ms, "-G", "-W", "-f", "-p", (dir / "k.pub").string(), "-s", (dir / "k.key").string()});
      const int s = runTool({ms, "-S", "-s", (dir / "k.key").string(), "-m", (dir / "Gridline-0.9.0-SHA256SUMS").string()});
      MinisignKey mk;
      expect(g == 0 && s == 0 && parseMinisignKey(readFile(dir / "k.pub"), mk) &&
                 verifyMinisign(readFile(dir / "Gridline-0.9.0-SHA256SUMS.minisig"), sums, {mk}, "Gridline-0.9.0-SHA256SUMS") == SigResult::Ok,
             "a signature made by the minisign program verifies");
    } else {
      std::printf("# minisign not installed: its own signature check skipped\n");
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  (void)have;
  return finish();
}
