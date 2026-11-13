# Local kingdom regression tour

`tools/kingdom_tour.py` boots a separate bounded run for each of the 17 kingdoms,
using a copied completed save. Each run must log an accepted world warp and a
subsequent arrival at the requested stage, then remain there for the settling
budget before capturing. An exit code alone does not count as success. This
requires the 1.4.1 warp address table added with this tool (1.0.0 uses its named
functions). Unknown builds without all required helpers refuse the warp.
Timeouts and Ctrl+C terminate only the launched test process tree, including a
Windows address-layout re-exec child.

Install Pillow in your development Python environment. Use a console build for
automation so failures are logged without waiting on a fatal-error dialog. The
executable still needs its normal SDL/shaderc/FFmpeg runtime dependencies on the
DLL search path or beside it.

```powershell
python tools/kingdom_tour.py --exe build/tour/SuperOdysseyRecomp.exe `
  --data-root D:/Odyssey-Test-Data --save D:/Completed-Save-Copy `
  --out build/tour-reference

python tools/kingdom_tour.py --exe build/candidate/SuperOdysseyRecomp.exe `
  --data-root D:/Odyssey-Test-Data --save D:/Completed-Save-Copy `
  --out build/tour-candidate --baseline build/tour-reference
```

Use **private copies** of game data and a save directory containing `save_0`.
The script only reads the supplied save; each kingdom gets a fresh writable
copy inside the new output directory. Existing output paths and recursive save
copy destinations are refused. The runtime can update caches and metadata in
the supplied data root, so do not point it at the production installation.

Default captures are 1280x720, native rendering with temporal upscaling and
frame interpolation off, 16x anisotropy, and 60 FPS. This is a regression setting,
not a performance benchmark or the player's saved configuration. Controller
input/gyro, mouse camera and vibration are disabled for repeatability. Runs are
hidden by default; `--visible` shows them. Startup test variables inherited from
the shell are cleared. All screenshots have unique explicit paths rather than
overwriting the data folder's `build/window.png`.
`--frame-rate 120` exercises the experimental draw path separately; increase
frame budgets if you want the same amount of settled gameplay time as at 60.

The default frame budgets assume this completed-save/title input sequence.
`--warp-frame`, `--frames`, `--settle` and `--timeout` are adjustable. A slower
loading run that has not reached the destination and settled is a failure, not
a valid loading-screen comparison. Start a small test first:

```powershell
python tools/kingdom_tour.py --exe build/tour/SuperOdysseyRecomp.exe `
  --data-root D:/Odyssey-Test-Data --save D:/Completed-Save-Copy `
  --out build/tour-smoke --kingdoms cap cascade
```

## Baselines and reports

`report.json` records executable and game-main hashes, source-save hashes, test
settings, budgets, actual arrival logs, exit status and comparison metrics.
`report.html` links local captures, logs and amplified pixel differences.
Equivalent settings, game-main, save and budgets are required for a baseline.
Keep GPU/driver and build-dependency versions equivalent too; those details are
in the per-run logs and are not automatically enforced by the image comparator.

Return codes: 0 = captures completed and any comparisons were within the current
threshold; 1 = a run failed; 2 = visual review needed (including different image
dimensions or missing baseline captures). With no baseline, results say
`no_baseline`: capture completion does not mean a release comparison passed.
Review is flagged at mean absolute RGB difference above 1% across the whole
image or above 8% in any 32x32 tile. The local check catches small errors that
a whole-screen average can hide. Difference images are also saved below those
thresholds.

These are spawn-view screenshots. Animated fog, NPCs, wind, water, time of day
and load timing can cause legitimate differences. A small average difference
can also miss a serious localized fault. Review images and keep interaction
tests for Mario/Cappy fog, poison damage, enemies, interiors and cutscenes. Do
not describe a completed tour as testing every object or all gameplay.

To compare with an older release lacking 1.4.1 warps, build that release's source
in a separate checkout with **only the identical warp/destination/capture test
hooks**. Record the release commit and instrumentation diff. Do not use the new
renderer as its own baseline or call an instrumented source build the exact
published binary. Keep that checkout and its artifacts separate from releases.

Captures, logs, test saves, generated code and game files remain local and
untracked. Never package test data or save copies into a public source commit,
release, CI artifact or issue report.
