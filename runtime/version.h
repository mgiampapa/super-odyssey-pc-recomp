// The app's version: the updater compares it with the newest release's tag (runtime/updater.c). Keep it the same as
// runtime/platform/windows.rc's FILEVERSION / ProductVersion (make dist checks).
#pragma once
#define APP_VERSION "0.2.8"

// Where updates come from: the GitHub repository ("owner/name") whose releases carry the Windows zip
// (make dist: "Super Odyssey PC Recomp-<version>-windows.zip"). Empty: no update checks (the menu says so); a fork sets its own.
// ODYSSEY_UPDATE_REPO=<owner/name> overrides it (tests).
#define APP_UPDATE_REPO "Simon3Sarkis/super-odyssey-pc-recomp"
