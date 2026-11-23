// Gridline - the Track Receipt window: your release and your writers and publishers (see
// TrackReceiptUiKit.h).
#include "App.h"
#include "TrackReceiptUiKit.h"
#include "Widgets.h"

namespace gl::receiptui {

using theme::pal;
using namespace gl::receipt;

void releaseSection(App& a) {
  State& s = S();
  const auto& P = pal();
  ImGui::Spacing();
  ImGui::PushFont(a.fontBold, 0.0f);
  ImGui::TextUnformatted(i18n::tr("Your release"));
  ImGui::PopFont();
  Release& rel = s.saved.release;
  const float lw = 110, half = (kW - 16) / 2;
  const float x0 = ImGui::GetCursorPosX();
  struct F {
    const char* label;
    std::string* v;
    const char* hint;
  };
  const F rows[] = {{i18n::tr("Title"), &rel.title, ""},          {i18n::tr("Performer"), &rel.performer, ""},
                    {i18n::tr("Licensee"), &rel.licensee, ""},    {i18n::tr("ISRC"), &rel.isrc, ""},
                    {i18n::tr("Release date"), &rel.date, "YYYY-MM-DD"}, {i18n::tr("Distributor"), &rel.distributor, ""}};
  for (size_t i = 0; i < 6; ++i) {
    ImGui::PushID(static_cast<int>(i));
    if (i % 2) ImGui::SameLine(x0 + half + theme::kS2);
    const float bx = i % 2 ? x0 + half + theme::kS2 : x0;
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(P.textDim), "%s", rows[i].label);
    ImGui::SameLine(bx + lw);
    if (field("##rel", *rows[i].v, half - lw - theme::kS2, rows[i].hint)) commit(a);
    ImGui::PopID();
  }
  ImGui::Spacing();
  ImGui::PushFont(a.fontBold, 0.0f);
  ImGui::TextUnformatted(i18n::tr("Your writers and publishers"));
  ImGui::PopFont();
  {
    const char* hdr[5] = {i18n::tr("Name"), i18n::tr("Role"), i18n::tr("PRO"), i18n::tr("IPI"), i18n::tr("Share of yours %")};
    const float cols[5] = {0, 0.30f, 0.50f, 0.66f, 0.84f};
    const float cw = kW - 16 - 30;
    for (int i = 0; i < 5; ++i) {
      if (i) ImGui::SameLine(x0 + cw * cols[i]);
      ImGui::TextColored(theme::vec(P.textFaint), "%s", hdr[i]);
    }
    int remove = -1;
    for (size_t k = 0; k < s.saved.writers.size(); ++k) {
      Writer& w = s.saved.writers[k];
      ImGui::PushID(static_cast<int>(k));
      std::string* v[5] = {&w.name, &w.role, &w.pro, &w.ipi, &w.share};
      for (int i = 0; i < 5; ++i) {
        if (i) ImGui::SameLine(x0 + cw * cols[i]);
        ImGui::PushID(i);
        const float next = i < 4 ? cols[i + 1] : 1.0f;
        if (field("##w", *v[i], cw * (next - cols[i]) - theme::kS1)) commit(a);
        ImGui::PopID();
      }
      ImGui::SameLine(x0 + cw + theme::kS1);
      if (ui::TextButton("x", false, 24, i18n::tr("Remove"))) remove = static_cast<int>(k);
      ImGui::PopID();
    }
    if (remove >= 0) {
      s.saved.writers.erase(s.saved.writers.begin() + remove);
      commit(a);
    }
    if (ui::TextButton(i18n::tr("Add writer"), false, 0)) {
      s.saved.writers.push_back({});
      commit(a);
    }
  }
  ImGui::Spacing();
}

}  // namespace gl::receiptui
