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
  // Live: File, Edit, Create, View, Options, Tools, Help; the patterns fold into Edit.
  static const std::vector<Menu> live = {
      {N_("File"), {{B::File}}},
      {N_("Edit"), {{B::Edit}, {B::Patterns, N_("Pattern")}}},
      {N_("Create"), {{B::Add}}},
      {N_("View"), {{B::View}}},
      {N_("Options"), {{B::Options}, {B::Record, N_("Recording and metronome")}}},
      {N_("Tools"), {{B::Tools}}},
      {N_("Help"), {{B::Help}}},
  };
  // Logic: the program menu on the mark (with the studio options), then File, Edit, Track,
  // Patterns, Navigate, Record, Mix, View, Tools, Help.
  static const std::vector<Menu> logic = {
      {nullptr, {{B::Program}, {B::Options, N_("Options")}}},
      {N_("File"), {{B::File}}},
      {N_("Edit"), {{B::Edit}}},
      {N_("Track"), {{B::Add}}},
      {N_("Patterns"), {{B::Patterns}}},
      {N_("Navigate"), {{B::Navigate}}},
      {N_("Record"), {{B::Record}}},
      {N_("Mix"), {{B::Mix}}},
      {N_("View"), {{B::View}}},
      {N_("Tools"), {{B::Tools}}},
      {N_("Help"), {{B::Help}}},
  };
  switch (style) {
    case layout::Preset::Live: return live;
    case layout::Preset::Logic: return logic;
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
      if (i.block == block) return true;
  return false;
}

}  // namespace gl::menustyle
