// Gridline — the export dialog's choices as plain data (File > Export audio): the formats ticked,
// file name patterns read as name pieces ("chips"), the output folder as the dialog shows it, and
// the line that sums up a render. The dialog (app/ExportUi.cpp) draws these; tests check them
// without a window.
#pragma once
#include <functional>
#include <string>
#include <vector>
#include "Export.h"

namespace gl::exportchoice {

// The formats written for the dialog's ticks (one per ExportFormat, in that order), in order.
std::vector<ExportFormat> tickedFormats(const bool* ticks, int count);

// A file name pattern as pieces: a token (%P, %M, ...) or plain text. Patterns are stored as text
// ("%P - %M"), so every pattern saved before the pieces existed reads as pieces unchanged.
struct NamePiece {
  char token = 0;    // a token's letter; 0 = plain text
  std::string text;  // plain text (empty for a token)
  bool operator==(const NamePiece& o) const { return token == o.token && text == o.text; }
};
// `tokens`: the letters this pattern offers (others, and "%%", read as plain text).
std::vector<NamePiece> parseNamePattern(const std::string& pattern, const std::string& tokens);
// Back to the stored text ("%" in plain text is written "%%", so it never reads as a token).
std::string joinNamePattern(const std::vector<NamePiece>& pieces);
// The piece's name in the dialog: Project, Marker, Track, Number, Date, Sample rate, What, Format.
const char* tokenLabel(char token);
// `pattern` with a token added at its end, after " - " unless it is empty or already ends in a
// space, a dash, an underscore or a dot.
std::string appendToken(const std::string& pattern, char token, const std::string& tokens);
// `pattern` with plain text added at its end.
std::string appendText(const std::string& pattern, const std::string& text, const std::string& tokens);
// `pattern` without its piece `index`; a dash or space left over at either end goes too.
std::string removePiece(const std::string& pattern, size_t index, const std::string& tokens);
// The tokens each name offers.
inline constexpr const char* kFileTokens = "PDRS";    // the file (project, date, sample rate, what)
inline constexpr const char* kAreaTokens = "PMNRD";   // a marker area's file (MarkerAreas.h)
inline constexpr const char* kPartTokens = "PTNSRD";  // a split part (ExportJob.h partPattern)

// The folder as the dialog shows it on one line: the home folder as "~", then, while it does not
// fit (`fits`), folders from the middle replaced by one "…", always keeping the first part and the
// last folder whole. The last folder alone (after "…/") when nothing else fits.
std::string shortFolder(const std::string& path, const std::string& home, const std::function<bool(const std::string&)>& fits);

}  // namespace gl::exportchoice
