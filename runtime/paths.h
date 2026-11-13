// Where Super Odyssey PC Recomp keeps its files (runtime/paths.c).
//
// Data root: everything the app writes or the user brings - extracted/ (game files), save/, cache/ (shaders /
// pipelines), mods/ (RomFS mods), fonts/ (system fonts), powermoon.ini. rt_paths_init() picks it and makes it the
// working directory, so the rest of the runtime opens those by their relative names. In order:
//   1. ODYSSEY_DATA_ROOT (tests, scratch installs)
//   2. dev / portable mode: the starting directory has extracted/ (a source checkout: `./build/SuperOdysseyRecomp`, the
//      test scripts), or the executable's parent has (build/SuperOdysseyRecomp.exe started from Explorer or a shortcut)
//   3. the folder chosen in the installer ("Install the game to", DATA_FOLDER_FILE: Windows, Linux), else
//      macOS: ~/Library/Application Support/io.github.powermoonrecomp (the .app bundle itself is never written to)
//      Windows: the folder of the .exe (portable, no installer); refuses to start, with a message, if not writable
//      Linux: $XDG_DATA_HOME/super-odyssey-pc-recomp (~/.local/share/super-odyssey-pc-recomp)
//
// Resources: files that ship with the app (helper programs, the window icon) - next to the executable (the
// Windows folder, the .app's Contents/MacOS), in the .app's Contents/Resources, or in the source tree.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RT_APP_NAME "Super Odyssey PC Recomp"  // (window title, message boxes)
#define RT_BUNDLE_ID "io.github.powermoonrecomp"
#define RT_LINUX_DIR "super-odyssey-pc-recomp"  // Linux: the data folder's and the settings folder's name

void rt_paths_init(void);         // first thing in main(), after plat_init
const char *rt_data_root(void);   // absolute
// The installer's "Install the game to": the data root becomes dir (made if needed; the working directory too) and is
// remembered in data-folder.txt - Windows next to the exe (removed again for the exe's own folder), Linux in
// ~/.config/super-odyssey-pc-recomp (an SD card, another drive). false: not usable.
#define DATA_FOLDER_FILE "data-folder.txt"
bool rt_set_data_root(const char *dir);
bool rt_dev_mode(void);           // dev / portable mode (rule 2 above): e.g. a source checkout

// A shipped file: `name` next to the executable or in Contents/Resources, else `dev_rel` in the source tree (the
// starting directory or the executable's parent; NULL = shipped builds only). Absolute path in out; false = none.
bool rt_resource(const char *name, const char *dev_rel, char *out, size_t n);

// The only way the app deletes game files. Removes the named entry of game_dir (an extracted/ folder) only if
// game_dir is <data root>/extracted itself (symlinks resolved; a symlinked extracted/ is refused) and the entry is
// one the setup creates (exefs, romfs, romfs.bin, leftover folders) and not a link. The folder the user picked
// never gets here. true = removed or not there.
bool rt_remove_game_file(const char *game_dir, const char *name);
// All the game files set up there (rt_remove_game_file on each, then the empty folder). Saves, seeds, settings stay.
bool rt_remove_game_files(const char *game_dir);
// Bytes taken by the extracted game files in game_dir (links not followed).
unsigned long long rt_game_files_size(const char *game_dir);

// A message box with msg (and stderr), then exit(1): what the player has to fix before the game can start.
void rt_fail(const char *msg);

typedef struct SDL_Window SDL_Window;
void rt_window_icon(SDL_Window *w);  // the shipped icon.png, if any

#ifdef __cplusplus
}
#endif
