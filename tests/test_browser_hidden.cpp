// Gridline — folders hidden from the browser (GitHub #58): hiding and showing, nested folders,
// whole path parts only, renames and moves, the saved list and the search filter.
#include <string>
#include <vector>
#include "Check.h"
#include "core/BrowserHidden.h"

using namespace gl::browse;

int main() {
  HiddenFolders h;
  expect(h.empty() && !h.covers("/home/u/Loops/kick.wav"), "nothing hidden at first");
  expect(h.hide("/home/u/Loops/Old/"), "hide a folder (trailing separator)");
  expect(!h.hide("/home/u/Loops/Old"), "hiding it again changes nothing");
  expect(h.isHidden("/home/u/Loops/Old") && h.isHidden("/home/u/Loops/./Old/"), "the folder is hidden, however it is written");
  expect(h.covers("/home/u/Loops/Old/kick.wav") && h.covers("/home/u/Loops/Old/Deep/snare.wav"), "everything inside it is covered");
  expect(!h.covers("/home/u/Loops/Older/kick.wav") && !h.covers("/home/u/Loops/kick.wav"), "a folder whose name starts the same is not");
  expect(!h.isHidden("/home/u/Loops/Old/Deep"), "a folder inside is covered but not hidden by itself");
  // Nested: the inner folder hidden too; showing the outer one keeps the inner one hidden.
  h.hide("/home/u/Loops/Old/Deep");
  expect(h.coveringFolder("/home/u/Loops/Old/Deep/a.wav") == "/home/u/Loops/Old/Deep", "the nearest hidden folder covers a file");
  expect(h.show("/home/u/Loops/Old") && !h.covers("/home/u/Loops/Old/a.wav") && h.covers("/home/u/Loops/Old/Deep/a.wav"),
         "showing the outer folder keeps the inner one hidden");
  expect(!h.show("/home/u/Loops/Old"), "showing a folder that is not hidden changes nothing");
  // Renames and moves carry hidden folders along.
  h.hide("/home/u/Loops/Old");
  expect(h.moved("/home/u/Loops", "/home/u/Samples") && h.isHidden("/home/u/Samples/Old") && h.isHidden("/home/u/Samples/Old/Deep") &&
             !h.isHidden("/home/u/Loops/Old"),
         "a renamed parent takes its hidden folders along");
  expect(h.showWithin("/home/u/Samples/Old") && h.empty(), "a folder taken out of the browser forgets the hidden folders in it");
  // Saved and read back.
  h.hide("/b/two");
  h.hide("/a/one");
  h.hide("C:\\Users\\u\\Music\\Junk");
  const std::string text = h.toText();
  expect(text.rfind("gridline-browser-hidden 1\n", 0) == 0, "the saved form starts with its header");
  const HiddenFolders back = HiddenFolders::fromText(text + "\n\nnot\tvalid\n");
  expect(back.folders() == h.folders() && back.folders().size() == 3, "the list reads back the same (bad lines skipped)");
  expect(HiddenFolders::fromText("").empty() && HiddenFolders::fromText("gridline-browser-hidden 1\n").empty(), "an empty file hides nothing");
  // Search results and starred files: hidden contents left out unless Show hidden is on.
  const std::vector<std::string> found = {"/a/one/x.wav", "/a/onex/y.wav", "/b/two/three/z.wav", "/c/w.wav"};
  expect(withoutHidden(back, found, false) == std::vector<std::string>{"/a/onex/y.wav", "/c/w.wav"}, "search leaves hidden folders' files out");
  expect(withoutHidden(back, found, true) == found, "with Show hidden every file is listed");
  return finish();
}
