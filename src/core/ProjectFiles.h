// Gridline — project files (J-52): which files File > Open treats as a
// project (Gridline projects, FL Studio projects, zipped projects and FL
// Studio zipped loop packages) and which ones File > Import adds to the open
// project (MIDI, audio), the file types of the open / save dialogs, and the
// formats File > Save as writes.
#pragma once
#include <string>
#include <vector>

namespace gl {

enum class OpenKind {
  GridlineProject,  // .gridline.json (and any other .json)
  FlProject,        // .flp
  ZippedProject,    // .zip: a zipped Gridline project or an FL Studio zipped loop package
  Midi,             // .mid / .midi / .rmi: imported into the open project
  Audio,            // audio files: imported into the open project
  Unknown,
};
OpenKind openKindOf(const std::string& path);
// Gridline projects, FL Studio projects and zipped projects replace the open
// project (unsaved-changes prompt, recent files, window title).
bool opensAsProject(const std::string& path);

// A file type of the open / save dialogs: label shown in the type list and
// the extensions it accepts (lower case, with the dot).
struct FileType {
  std::string label;
  std::vector<std::string> exts;
};
// File > Open: "All projects" first, then one entry per project format.
const std::vector<FileType>& openFileTypes();
// File > Import > MIDI file / audio file.
const std::vector<FileType>& importMidiTypes();
const std::vector<FileType>& importAudioTypes();

// The formats File > Save as writes; the order is the dialog's type list.
enum class SaveFormat {
  Gridline = 0,  // .gridline.json
  GridlineZip,   // J-78: .gridline.zip, the project and the files it uses (ProjectInterop zip)
  Flp,           // FL Studio project
  FlZip,         // FL Studio zipped loop package (.zip): the .flp and the samples it uses
  Count
};
const std::vector<FileType>& saveFileTypes();
// The format a saved file is in, by its name (.gridline.zip, .flp, other
// .zip, otherwise Gridline). The two zipped formats differ by name: a
// Gridline zipped project always ends in ".gridline.zip".
SaveFormat saveFormatOf(const std::string& path);
// `name` with its project extension (.gridline.json / .json / .gridline.zip / .flp / .zip)
// replaced by the one of `f`.
std::string withSaveExtension(const std::string& name, SaveFormat f);
// File name without the project extension ("Song.gridline.json" → "Song").
std::string projectStemOf(const std::string& path);

}  // namespace gl
