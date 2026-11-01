// Gridline - J-219: the Track Receipt spreadsheet (SpreadsheetML 2003: Excel
// and LibreOffice open it). Sheets: Licenses, Usage, Release, Splits.
#include <cmath>
#include <cstdio>
#include "TrackReceipt.h"

namespace gl::receipt {

namespace {

std::string esc(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    const auto c = static_cast<unsigned char>(s[i]);
    switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      case '\n': o += "&#10;"; break;
      default:
        if (c < 0x20 && c != '\t') break;  // not allowed in XML 1.0
        o += static_cast<char>(c);
    }
  }
  return o;
}

std::string numText(double v) {
  char b[40];
  std::snprintf(b, sizeof b, "%.10g", v);
  return b;
}

struct Sheet {
  std::string xml;
  void begin(const std::string& name, const std::vector<int>& widths) {
    xml += " <Worksheet ss:Name=\"" + esc(name) + "\">\n  <Table>\n";
    for (int w : widths) xml += "   <Column ss:Width=\"" + std::to_string(w) + "\"/>\n";
  }
  void row() { xml += "   <Row>\n"; }
  void endRow() { xml += "   </Row>\n"; }
  void str(const std::string& v, const char* style = nullptr) {
    xml += "    <Cell" + std::string(style ? std::string(" ss:StyleID=\"") + style + "\"" : "") + "><Data ss:Type=\"String\">" + esc(v) +
           "</Data></Cell>\n";
  }
  void num(double v, const char* style = "n1") {
    xml += "    <Cell ss:StyleID=\"" + std::string(style) + "\"><Data ss:Type=\"Number\">" + numText(v) + "</Data></Cell>\n";
  }
  void formula(const std::string& f, double cached, const char* style = "n1") {
    xml += "    <Cell ss:StyleID=\"" + std::string(style) + "\" ss:Formula=\"" + esc(f) + "\"><Data ss:Type=\"Number\">" + numText(cached) +
           "</Data></Cell>\n";
  }
  void empty(const char* style = "fill") { xml += "    <Cell ss:StyleID=\"" + std::string(style) + "\"/>\n"; }
  void end() { xml += "  </Table>\n </Worksheet>\n"; }
};

std::string originalText(const SongRow& s) {
  std::string t;
  for (const Timeline& tl : s.timelines) {
    std::string p;
    for (const RangeFrom& rf : tl.parts) {
      p += (p.empty() ? "" : ", ") + timeText(rf.r.a) + "-" + timeText(rf.r.b) + " (" + secondsText(rf.r.b - rf.r.a) + " s";
      if (rf.estimated) p += ", not measured";
      p += ")";
    }
    if (p.empty()) continue;
    if (tl.part > 0) p = "Part " + std::to_string(tl.part) + ": " + p;
    if (tl.part < 0) p = "Derived, part unknown: " + p;
    t += (t.empty() ? "" : "; ") + p;
  }
  return t;
}

}  // namespace

std::string spreadsheetXml(const Report& r) {
  std::string x =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<?mso-application progid=\"Excel.Sheet\"?>\n"
      "<Workbook xmlns=\"urn:schemas-microsoft-com:office:spreadsheet\" xmlns:o=\"urn:schemas-microsoft-com:office:office\" "
      "xmlns:x=\"urn:schemas-microsoft-com:office:excel\" xmlns:ss=\"urn:schemas-microsoft-com:office:spreadsheet\" "
      "xmlns:html=\"http://www.w3.org/TR/REC-html40\">\n"
      " <DocumentProperties xmlns=\"urn:schemas-microsoft-com:office:office\"><Title>" +
      esc((r.project.empty() ? std::string("Untitled") : r.project) + " Track Receipt") +
      "</Title><Author>Gridline</Author></DocumentProperties>\n"
      " <Styles>\n"
      "  <Style ss:ID=\"Default\" ss:Name=\"Normal\"><Font ss:FontName=\"Arial\" ss:Size=\"10\"/></Style>\n"
      "  <Style ss:ID=\"title\"><Font ss:FontName=\"Arial\" ss:Size=\"14\" ss:Bold=\"1\"/></Style>\n"
      "  <Style ss:ID=\"h\"><Font ss:FontName=\"Arial\" ss:Size=\"10\" ss:Bold=\"1\" ss:Color=\"#FFFFFF\"/>"
      "<Interior ss:Color=\"#262C30\" ss:Pattern=\"Solid\"/></Style>\n"
      "  <Style ss:ID=\"note\"><Font ss:FontName=\"Arial\" ss:Size=\"9\" ss:Italic=\"1\" ss:Color=\"#5D676C\"/></Style>\n"
      "  <Style ss:ID=\"warn\"><Font ss:FontName=\"Arial\" ss:Size=\"10\" ss:Bold=\"1\" ss:Color=\"#D9443F\"/></Style>\n"
      "  <Style ss:ID=\"n1\"><NumberFormat ss:Format=\"0.0\"/></Style>\n"
      "  <Style ss:ID=\"n0\"><NumberFormat ss:Format=\"0\"/></Style>\n"
      "  <Style ss:ID=\"pct\"><NumberFormat ss:Format=\"0.00\"/></Style>\n"
      "  <Style ss:ID=\"fill\"><Interior ss:Color=\"#FFF4E8\" ss:Pattern=\"Solid\"/></Style>\n"
      "  <Style ss:ID=\"fillpct\"><Interior ss:Color=\"#FFF4E8\" ss:Pattern=\"Solid\"/><NumberFormat ss:Format=\"0.00\"/></Style>\n"
      " </Styles>\n";
  // ---- Licenses
  Sheet L;
  L.begin("Licenses", {130, 170, 150, 70, 80, 60, 300, 70, 70, 70, 160, 220});
  L.row(), L.str("Track Receipt: " + (r.project.empty() ? std::string("Untitled") : r.project), "title"), L.endRow();
  L.row(), L.str("A helper, not legal advice. Check the figures against your license on Tracklib.", "note"), L.endRow();
  L.row(), L.endRow();
  L.row();
  for (const char* h : {"Artist", "Title", "Stems used", "Seconds used", "Tier to choose (s)", "Over 60 s", "Parts used (original)",
                        "Category", "Share %", "Fee", "Rights holders", "Notes"})
    L.str(h, "h");
  L.endRow();
  const int firstSongRow = 5;  // 1-based row of the first song
  std::vector<int> songRowOf;
  int rowNo = firstSongRow;
  for (const SongRow& s : r.songs) {
    L.row();
    L.str(s.artist);
    L.str(s.title);
    std::string stems;
    for (const auto& st : s.stems) stems += (stems.empty() ? "" : ", ") + st;
    L.str(stems);
    L.num(s.seconds);
    if (s.seconds > 0 && !s.over) L.num(s.tier, "n0");
    else L.str(s.seconds <= 0 ? "not played" : "none");
    s.over ? L.str("YES", "warn") : L.str("no");
    L.str(originalText(s));
    s.fields.category.empty() ? L.empty() : L.str(s.fields.category, "fill");
    s.share >= 0 ? L.num(s.share, "fillpct") : L.empty();
    s.fields.fee.empty() ? L.empty() : L.str(s.fields.fee, "fill");
    s.fields.holders.empty() ? L.empty() : L.str(s.fields.holders, "fill");
    std::string notes;
    for (const auto& w : s.warnings) notes += (notes.empty() ? "" : " ") + w;
    L.str(notes);
    L.endRow();
    songRowOf.push_back(rowNo++);
  }
  L.end();
  // ---- Usage
  Sheet U;
  U.begin("Usage", {150, 230, 110, 230, 70, 200, 200, 50, 160});
  U.row();
  for (const char* h : {"Song", "File", "Stem", "Where in the project", "Bars", "Part of the file", "Part of the original", "Starts", "Counted"})
    U.str(h, "h");
  U.endRow();
  for (const FileRow& f : r.files) {
    std::string songTitle = "-";
    for (const SongRow& s : r.songs)
      if (s.key == f.song && f.ticked) songTitle = s.title;
    auto useRow = [&](const Use* u) {
      U.row();
      U.str(songTitle);
      U.str(f.name);
      U.str(f.info.stem);
      U.str(u ? u->where : "-");
      if (u && u->bar0 > 0) U.str(std::to_string(static_cast<int>(u->bar0)) + "-" + std::to_string(static_cast<int>(u->bar1)));
      else U.str("");
      Ranges fr = u ? unite(u->ranges) : Ranges{};
      const bool est = u && u->estimated && !f.manual;
      U.str(u && u->muted ? "" : est ? "whole file (not measured)" : rangesText(fr));
      Ranges orig = fr;
      std::string os;
      if (f.derived && !(u && u->muted)) {
        // FileIn chains are not in the report: the mapped part is in the Licenses sheet.
        os = f.unknownRange ? "unknown (" + f.originNote + ")" : f.originNote;
      } else if (!(u && u->muted) && !est) {
        os = rangesText(orig);
      }
      U.str(os);
      if (u && u->plays > 0) U.num(static_cast<double>(u->plays), "n0");
      else U.str("");
      std::string counted = !f.ticked ? "no (not ticked)" : !u ? "no (never plays)" : u->muted ? "no (" + (u->note.empty() ? std::string("muted") : u->note) + ")"
                            : est ? "yes, estimated: edit in Track Receipt" : "yes";
      U.str(counted);
      U.endRow();
    };
    for (const Use& u : f.uses) useRow(&u);
    for (const Use& u : f.muted) useRow(&u);
    if (f.uses.empty() && f.muted.empty()) useRow(nullptr);
  }
  U.end();
  // ---- Release
  Sheet R;
  R.begin("Release", {170, 320});
  R.row(), R.str("Your release", "title"), R.endRow();
  R.row(), R.str("Field", "h"), R.str("Value", "h"), R.endRow();
  const Release& rel = r.saved.release;
  const std::pair<const char*, std::string> rows[] = {{"Song title", rel.title},     {"Performer / artist", rel.performer},
                                                      {"Licensee", rel.licensee},    {"ISRC", rel.isrc},
                                                      {"Release date", rel.date},    {"Distributor", rel.distributor}};
  for (const auto& [k, v] : rows) {
    R.row(), R.str(k);
    v.empty() ? R.empty() : R.str(v, "fill");
    R.endRow();
  }
  R.row(), R.str("Reference copy"), R.str("MP3, at most 10 MB"), R.endRow();
  R.row(), R.endRow();
  R.row(), R.str("Report master revenue to Tracklib at least every 6 months.", "note"), R.endRow();
  const auto dates = reportDates(rel.date, 6);
  for (size_t i = 0; i < dates.size(); ++i) R.row(), R.str("Report by (" + std::to_string(i + 1) + ")"), R.str(dates[i]), R.endRow();
  R.row(), R.str("Beat sales and leases: each buyer needs a license of their own.", "note"), R.endRow();
  R.end();
  // ---- Splits
  Sheet S;
  S.begin("Splits", {220, 110, 220});
  S.row(), S.str("Splits (master and publishing)", "title"), S.endRow();
  S.row(), S.str("The share of each license applies to the master and to the publishing. Type shares in the Licenses sheet.", "note"),
      S.endRow();
  S.row(), S.endRow();
  for (int table = 0; table < 2; ++table) {
    S.row(), S.str(table == 0 ? "Master" : "Publishing (PRO registration)", "h"), S.str("Share %", "h"), S.str("Rights holders", "h"), S.endRow();
    // rows referencing Licenses!Share% (column 9)
    std::string sum;
    for (size_t i = 0; i < r.songs.size(); ++i) {
      const SongRow& s = r.songs[i];
      if (s.seconds <= 0) continue;
      S.row();
      S.str(s.title + (s.artist.empty() ? "" : " (" + s.artist + ")"));
      const std::string ref = "Licenses!R" + std::to_string(songRowOf[i]) + "C9";
      S.formula("=" + ref, std::max(0.0, s.share), "pct");
      s.fields.holders.empty() ? S.empty() : S.str(s.fields.holders, "fill");
      S.endRow();
      sum += (sum.empty() ? "" : "+") + ref;
    }
    S.row(), S.str("Total to sample rightsholders");
    S.formula(sum.empty() ? "=0" : "=" + sum, r.totalShare, "pct");
    S.endRow();
    S.row(), S.str(table == 0 ? "Yours (you / your label)" : "Your writers (100 minus the samples)");
    S.formula(sum.empty() ? "=100" : "=100-(" + sum + ")", std::max(0.0, 100.0 - r.totalShare), "pct");
    S.endRow();
    S.row(), S.endRow();
  }
  S.row(), S.str("Your writers and publishers", "h"), S.str("Share of yours %", "h"), S.str("Role / PRO / IPI", "h"), S.endRow();
  const size_t nw = std::max<size_t>(r.saved.writers.size(), 4);
  for (size_t i = 0; i < nw; ++i) {
    S.row();
    if (i < r.saved.writers.size()) {
      const Writer& w = r.saved.writers[i];
      S.str(w.name, "fill");
      const double v = parseShare(w.share);
      v >= 0 ? S.num(v, "fillpct") : S.empty();
      S.str(w.role + (w.pro.empty() ? "" : " / " + w.pro) + (w.ipi.empty() ? "" : " / " + w.ipi), "fill");
    } else {
      S.empty(), S.empty(), S.empty();
    }
    S.endRow();
  }
  S.row(), S.str("Tracklib registers each sample on its side (for example 90 / 10), listing only your writers and publishers.", "note"),
      S.endRow();
  S.end();
  x += L.xml + U.xml + R.xml + S.xml + "</Workbook>\n";
  return x;
}

}  // namespace gl::receipt
