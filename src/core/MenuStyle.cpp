// Gridline - menu bar styles of the layout presets (K-79). See MenuStyle.h.
#include "MenuStyle.h"
#include "I18n.h"

namespace gl::menustyle {

using i18n::N_;

const std::vector<Menu>& menus(layout::Preset style) {
  using B = Block;
  static const std::vector<Menu> studio = {
      {N_("File"), {{B::File}}},       {N_("Edit"), {{B::Edit}}},       {N_("Add"), {{B::Add}}},   {N_("Patterns"), {{B::Patterns}}},
      {N_("View"), {{B::View}}},       {N_("Options"), {{B::Options}}}, {N_("Tools"), {{B::Tools}}}, {N_("Help"), {{B::Help}}},
  };
  // Live: File, Edit, Create, View, Options, Navigate, Tools, Help (the reference's bar with Tools,
  // Gridline's own, before Help). K-112c (round 45): Create has the reference's new tracks and
  // imports; Navigate goes to the views. Checkpoint 49 (N47-13): File and Edit are Clips' own, and
  // Studio's Add and Patterns are no submenus: their rows that mean something for clips are in Edit
  // and Create (a MIDI clip is a clip there, not a pattern).
  static const std::vector<Menu> live = {
      {N_("File"), {{B::ClipsFile}}},
      {N_("Edit"), {{B::ClipsEdit}}},
      {N_("Create"), {{B::ClipsCreate}}},
      {N_("View"), {{B::View}}},
      {N_("Options"), {{B::Options}, {B::ClipsRecord, N_("Recording and metronome")}}},
      {N_("Navigate"), {{B::ClipsNavigate}, {B::Navigate}}},
      {N_("Tools"), {{B::Tools}}},
      {N_("Help"), {{B::Help}}},
  };
  // Logic: the program menu on the mark (with the studio options), then File, Edit, Track (the
  // track commands, everything Add has), Navigate, Record, Mix, View, Window
  // (with the tools), Help. R01-16: the reference's menu bar has no Patterns or Tools menu;
  // the patterns are a submenu of Edit.
  static const std::vector<Menu> logic = {
      {nullptr, {{B::Program}, {B::Options, N_("Options")}}},
      {N_("File"), {{B::File}, {B::TracksFile}}},
      // The studio's pattern commands stay reachable, at the foot of Edit (R06-13: not in Track).
      {N_("Edit"), {{B::TracksEdit}, {B::Patterns, N_("Patterns")}}},
      {N_("Track"), {{B::Track}, {B::Add, N_("Add")}}},  // R06-13: no Patterns (the reference has none)
      {N_("Navigate"), {{B::Navigate}}},
      {N_("Record"), {{B::TracksRecord}}},
      {N_("Mix"), {{B::Mix}}},
      // R04-09: View opens with the layout's own panes; the studio's windows and views follow in a submenu.
      {N_("View"), {{B::TracksView}, {B::View, N_("More windows and views")}}},
      {N_("Window"), {{B::Window}, {B::Tools, N_("Tools")}}},
      {N_("Help"), {{B::Help}}},
  };
  // Tracker: File, Edit, View (the pages and header screens first, as the header names them; the
  // other windows and panels in a submenu), Pattern, Song (where the song plays, its modes, what
  // can be added), Options (with recording and the metronome), Tools, Help.
  static const std::vector<Menu> tracker = {
      {N_("File"), {{B::File}}},
      {N_("Edit"), {{B::TrackerEdit}}},
      {N_("View"), {{B::Pages}, {B::TrackerView, N_("Windows and panels")}}},
      {N_("Pattern"), {{B::TrackerPattern}, {B::Patterns}}},
      {N_("Song"), {{B::Navigate}, {B::Add, N_("Add")}}},
      {N_("Options"), {{B::Options}, {B::Record, N_("Recording and metronome")}}},
      {N_("Tools"), {{B::Tools}}},
      {N_("Help"), {{B::Help}}},
  };
  switch (style) {
    case layout::Preset::Live: return live;
    case layout::Preset::Logic: return logic;
    case layout::Preset::Tracker: return tracker;
    default: return studio;
  }
}

const std::vector<Block>& studioBlocks() {
  static const std::vector<Block> b = {Block::File, Block::Edit, Block::Add, Block::Patterns, Block::View, Block::Options, Block::Tools, Block::Help};
  return b;
}

bool reaches(layout::Preset style, Block block) {
  for (const Menu& m : menus(style))
    for (const Item& i : m.items)
      if (i.block == block || (i.block == Block::TracksEdit && block == Block::Edit) ||  // K-112b: the same commands, regrouped
          (i.block == Block::TracksRecord && block == Block::Record) ||
          (i.block == Block::TrackerEdit && block == Block::Edit) ||  // K-106: Studio's Edit and View commands, regrouped
          (i.block == Block::TrackerView && block == Block::View) ||
          (i.block == Block::ClipsFile && block == Block::File) ||  // checkpoint 49: regrouped for clips and tracks
          (i.block == Block::ClipsEdit && (block == Block::Edit || block == Block::Patterns)) ||
          (i.block == Block::ClipsCreate && block == Block::Add))
        return true;
  return false;
}

}  // namespace gl::menustyle
