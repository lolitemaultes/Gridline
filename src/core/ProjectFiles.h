// Gridline — project files (J-52): which files File > Open treats as a
// project (Gridline projects, FL Studio projects, zipped projects and FL
// Studio zipped loop packages) and which ones File > Import adds to the open
// project (MIDI, audio), the file types of the open / save dialogs, and the
// formats File > Save as writes.
#pragma once
#include <string>
#include <vector>

namespace gl {

// K-121: Gridline's own file names. One extension each, because macOS and Windows name a file's
// type by its last extension only (a ".gridline.json" file is JSON to them, so a double click
// opens a text editor). New projects are saved with these; the older two-part names still open
// everywhere and can still be chosen in Save as.
inline constexpr const char* kProjectExt = ".gridline";          // the project (JSON text)
inline constexpr const char* kZippedProjectExt = ".gridlinez";   // the project and its files, zipped
inline constexpr const char* kOldProjectExt = ".gridline.json";  // the names earlier versions saved
inline constexpr const char* kOldZippedProjectExt = ".gridline.zip";
// By name (case ignored; a name that is only the extension is no project): a Gridline project
// file (.gridline, .gridline.json) / a Gridline zipped project (.gridlinez, .gridline.zip).
bool isGridlineProjectName(const std::string& path);
bool isGridlineZipName(const std::string& path);

enum class OpenKind {
  GridlineProject,  // .gridline, .gridline.json (and any other .json)
  FlProject,        // .flp
  ZippedProject,    // .gridlinez, .zip: a zipped Gridline project or an FL Studio zipped loop package
  TrackerModule,    // K-76: .xm / .mod: a tracker module, converted into a new project
  Midi,             // .mid / .midi / .rmi: imported into the open project
  Audio,            // audio files: imported into the open project
  Unknown,
};
OpenKind openKindOf(const std::string& path);
// Gridline projects, FL Studio projects, zipped projects and tracker modules replace the open
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
  Gridline = 0,    // .gridline
  GridlineZip,     // J-78: .gridlinez, the project and the files it uses (ProjectInterop zip)
  Flp,             // FL Studio project
  FlZip,           // FL Studio zipped loop package (.zip): the .flp and the samples it uses
  GridlineJson,    // K-121: a Gridline project under the older name, .gridline.json (the same text)
  GridlineZipOld,  // K-121: a Gridline zipped project under the older name, .gridline.zip
  Count
};
const std::vector<FileType>& saveFileTypes();
// The format a saved file is in, by its name (.gridlinez / .gridline.zip, .flp, other .zip,
// .json, otherwise Gridline). The zipped formats differ by name: a Gridline zipped project
// always ends in ".gridlinez" or ".gridline.zip".
SaveFormat saveFormatOf(const std::string& path);
// A Gridline project's text under either name / a Gridline zipped project under either name.
inline bool isGridlineText(SaveFormat f) { return f == SaveFormat::Gridline || f == SaveFormat::GridlineJson; }
inline bool isGridlineZip(SaveFormat f) { return f == SaveFormat::GridlineZip || f == SaveFormat::GridlineZipOld; }
// `name` with its project extension (.gridline / .gridline.json / .json / .gridlinez /
// .gridline.zip / .flp / .zip) replaced by the one of `f`.
std::string withSaveExtension(const std::string& name, SaveFormat f);
// File name without the project extension ("Song.gridline", "Song.gridline.json" → "Song").
std::string projectStemOf(const std::string& path);
// The project file's name inside a Gridline zipped project written to `zipPath`: the new name in
// a .gridlinez, the older one in a .gridline.zip (so an older Gridline opens what it unpacks).
std::string projectFileNameInZip(const std::string& zipPath, const std::string& stem);

}  // namespace gl
