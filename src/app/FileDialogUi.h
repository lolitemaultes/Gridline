// Gridline: the Open / Save dialog (App::fileDialog). A places sidebar (drives and volumes, the
// standard folders, Gridline's own folders, favourites, recent folders), a breadcrumb path bar
// that also takes a typed path (Tab completes), back / forward / up, a sortable list with Name,
// Size, Kind and Modified, a filter, hidden files, keyboard navigation and audio preview. The
// model lives in core/FileDialogModel.
#pragma once
#include <string>
#include <vector>

namespace gl {
class App;
namespace filedlgui {

// Draws the dialog while it is open (called once a frame).
void draw(App& a);

// Dev check ("filedialog" in --dev-input):
//   open [dir]          File > Open, starting in `dir`
//   saveas [dir]        File > Save as, starting in `dir`
//   place <name>        clicks a sidebar row by its name (a drive, a place, a favourite)
//   go <dir>            opens a folder as a double click would
//   sort <name|size|kind|modified> [desc]
//   path <text>         types into the path bar and presses Enter
//   complete <text>     types into the path bar and presses Tab (state shows the result)
//   filter <text>       the filter box
//   hidden on|off       hidden files
//   select <name>       selects a row (a file sets the name field)
//   pin / unpin         the current folder in the favourites
//   back / forward / up
//   accept              the Open / Save button
//   state               the folder, sort, rows, places, drives and the layout to stdout
void devCommand(App& a, const std::vector<std::string>& args, int frame);

}  // namespace filedlgui
}  // namespace gl
