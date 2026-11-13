# Diagnostic and crash reports

F1 / Esc > Quit > **Copy crash report** creates one text file and copies its
full path. Paste that path into the attachment file picker when reporting a
problem. This action saves the current settings before collecting them.
Nothing is uploaded automatically.

The Windows/Linux error dialog also has **Copy crash report**. A report is
saved before the dialog opens, even if the player chooses Close. After restarting
the same installation, **Copy last crash report** appears in the Quit page when
the saved report is available. The old fault details survive replacement of the
normal log. The existing quit confirmation remains.

Files are `report-<UTC>-<process>-<sequence>.txt` under `reports/` beside the
current file log. Without a file log, the game-data folder is used, or the
executable's folder before data initialization. `last-crash.txt` is an internal
filename pointer; attach the `report-...txt` file whose path the button copies.

The report contains:

- App version and a cached FNV-1a executable fingerprint for distinguishing
  private builds sharing a version number (not a security verification hash).
- OS/platform, CPU name where available, logical processors and total RAM.
- Selected Vulkan GPU, driver name/information, raw driver version, device IDs
  and API version, cached while the device is healthy.
- Loaded game main build ID, first recorded host exception/address/PC/LR and
  the fault/backtrace/guest call chain already written to the log.
- Saved settings (up to 64 KiB), startup log (64 KiB for oversized logs) and
  most recent log entries (up to 1 MiB).

Only the host executable is read for the startup fingerprint. Bundle contents
come from the settings and log, without collecting game assets, keys, saves, a
directory archive or a process memory dump. The text can contain local paths
and mod names; review it before sharing. Terminal-only runs explicitly report
when no file log is available.

Fatal export is best-effort: memory/stack corruption, full or unwritable disks,
forced termination, or failures before logging/handlers initialize can prevent
capture. Original logging remains in place. Failed exports do not overwrite an
existing report. Missing GPU/game metadata is labeled unavailable rather than
queried from a failed device.

Windows clipboard ownership works before a game window exists. Other platforms
queue copying on SDL's event thread without blocking rendering; the menu shows
Copy pending until completion, following the requirements of
[SDL_SetClipboardText](https://wiki.libsdl.org/SDL3/SDL_SetClipboardText) and
[SDL_RunOnMainThread](https://wiki.libsdl.org/SDL3/SDL_RunOnMainThread).

## Validation

Windows error-dialog and in-game buttons were exercised; the saved report was
inspected for actual CPU/GPU/driver/OS/game-build, settings and log. Quit
confirmation and exit were exercised. Native fixtures cover early startup and
missing settings, bounded logs retaining startup and crash tail, first-fault
preservation, unique files, small buffers, unavailable output directories,
rejecting an outside-folder marker, a known executable fingerprint, and recovery
in a new process after the old log is replaced.

Linux/macOS still need native builds and runtime tests. macOS has no new fatal
popup; its existing fatal/terminal behavior is retained and the in-game action
is available. Metal GPU metadata is not supplied by the Vulkan cache hook.

Fixtures require no game data. In an MSYS2 UCRT64 development shell:

```sh
mkdir -p build/report-tests
clang -O2 -Iruntime tools/test_diagnostics.c runtime/diagnostics.c -lSDL3 -o build/report-tests/check.exe
```

Run with a **new** directory as the first argument and mode `fault`, `early`,
or `capture` as the second. Mode `restart` reuses the directory from `capture`
to check cross-process preservation. Other modes refuse existing directories.
Mode `dialog` opens a labeled test popup; build that interactive fixture as a
GUI executable and launch normally so its dialog is visible. Never use
production folders for fixtures.
