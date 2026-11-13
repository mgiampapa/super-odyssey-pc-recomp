# Super Odyssey PC Recomp

<p align="center"><img src="docs/banner.svg" alt="Super Odyssey PC Recomp" width="100%"></p>

<p align="center"><b>A native PC port of Super Mario Odyssey for Windows and Linux, made by static recompilation - from your own copy of the game.</b></p>

> [!IMPORTANT]
> **You must bring your own game.** This project contains **no game files, no game code, no assets and no keys** -
> only the installer and the source code. To play, you need the **`exefs` and `romfs` folders of your own copy of
> Super Mario Odyssey**, already extracted. Instructions for getting game files are
> not provided, and requests for game files or keys will be closed.

> [!WARNING]
> **This is an early preview.** The whole game can be played, but there are known issues (see
> [Known issues](#known-issues)) and things that still need fixing. Please report what you find.

## What it is

The game's ARM64 code is translated to C ahead of time and compiled into a regular Windows program - no emulator runs
while you play. The parts of the Switch the game talks to (kernel, system services, the NVN graphics API, audio) are
reimplemented on the PC, with Vulkan for graphics.

- **Installer** that takes your game's extracted `exefs` and `romfs` folders (1.0.0, or 1.4.1 with the update applied)
- **Every shader compiled at install time**, so effects don't stutter the first time they appear
- **Up to 4x internal resolution**, NVIDIA **DLSS / DLAA**, AMD FSR 1, SMAA, ultrawide *Fill window*
- **Native 90 or 120 FPS** (experimental) or frame interpolation, anisotropic filtering, smooth gradients
- Keyboard and controller rebinding, RomFS mods, an in-game settings menu, and **built-in updates** from this page

Windows 10 / 11 (x64). **Linux and Steam Deck: experimental** - an AppImage, see [docs/linux.md](docs/linux.md).

## Install

1. Download the latest `...-windows.zip` from [Releases](../../releases) and extract it anywhere
   you own (for example `D:\Games`, **not** Program Files). Start `SuperOdysseyRecomp.exe`.
   It isn't code-signed: if SmartScreen says "Windows protected your PC", choose *More info*, then *Run anyway*.
2. The installer asks for:
   1. **Your game**: the folder that holds the extracted **`exefs`** and **`romfs`** folders of your own Super Mario
      Odyssey - version **1.0.0**, or **1.4.1** with the update already applied (its full `romfs`, not only the
      update's). `exefs` holds `main`, `main.npdm` and `sdk`; `romfs` holds folders such as `StageData` and
      `SystemData`.
   2. **Install the game to**: where the game data, saves and shaders go (about 12 GB).
3. Press **Install**. It copies the game data, recompiles the game code for your PC (a few minutes) and compiles every
   shader (several minutes, once).
4. Press **Play**. The game opens in fullscreen; later starts go straight to the game.

<p align="center"><img src="docs/installer.png" alt="The installer" width="80%"></p>

## Updating

New versions are published on this page. The game checks for them at start (in the background - only GitHub is
contacted and nothing about you or your game is sent; you can switch this off) and shows a note when one is out.
Open the menu (**F1**) > **Updates** > **Download and install**, then **Restart now**. Your game files, saves,
settings and mods stay; if the new version needs it, the game code is rebuilt and the shaders re-prepared once, by
themselves.

You can also download the new zip from [Releases](../../releases) and extract it over your current folder.

## Known issues

This is a preview. Known problems and things not done yet:

- **Version 1.4.1** is newer in this port than 1.0.0 and less tested: native 90/120 FPS and the frame interpolation's
  pose smoothing are tuned on 1.0.0, so 1.4.1 can show more jitter or small glitches with them on.
- **Native 90 and 120 FPS** are experimental on both versions: some objects or effects can move unevenly or look doubled.
- **A short stutter can still happen** the first time a rare effect is drawn whose pipeline state nobody has recorded
  yet. It's prepared then and kept for next time.
- **Online features** of the game (anything that needs Nintendo's servers) don't work.
- **Code mods** (exefs patches, IPS / pchtxt, online mods) can't be used: the game code is rebuilt from the original.
  RomFS mods work.
- **Windows Defender** may flag the new, unsigned program by its machine-learning guess (a `!ml` detection) - it
  decrypts a file, builds code and starts helper programs, which looks unusual. Please report it as a false positive at
  https://www.microsoft.com/wdsi/filesubmission and open an issue with the detection name.
- **Linux and Steam Deck are experimental**: less tested than Windows ([docs/linux.md](docs/linux.md)). A CPU with
  AVX2 and a Vulkan 1.3 GPU are required everywhere.
- Graphics glitches can show up in places nobody has tested yet - please report them with a screenshot.

## Reporting a problem

Open an [issue](../../issues) with:

- what happened and where in the game (a screenshot helps),
- your game version (1.0.0 or 1.4.1), Windows version, CPU and GPU,
- the log, `SuperOdysseyRecomp.log`, from the install folder (it doesn't contain any game files).

For a graphics glitch, pressing **F9** where it happens saves the frame's details into the log, which helps a lot.

## What you need

- Windows 10 / 11 (x64), a CPU with AVX2 (Intel Haswell / AMD Zen or newer), a GPU with Vulkan 1.3 and an up-to-date
  driver
- 8 GB RAM (16 GB recommended), about 12 GB free disk space
- The extracted `exefs` and `romfs` folders of your own Super Mario Odyssey (1.0.0, or 1.4.1 with the update applied)

## Settings

Open the menu with **F1** or **Esc** (controller: Home / Guide button). Switch tabs with L / R (Q / E); the text on
the right explains the selected setting.

- **Display**: windowed, borderless or exclusive fullscreen, display resolution, aspect ratio (ultrawide *Fill window*)
- **Graphics**: internal resolution, NVIDIA DLSS / DLAA (presets, sharpening), AMD FSR 1, SMAA, anisotropic
  filtering, smooth gradients
- **Performance**: 90 or 120 FPS (native) or 120 interpolated, shader preloading and warm-up, FPS counter and performance
  overlay, compile shaders again at the next start
- **Audio**: volume, reverb, movie audio, mute in background
- **Controls**: language, motion controls, vibration, stick deadzone, mouse camera (sensitivity, invert), button
  prompts (keyboard, Xbox, PlayStation, Switch), skip controller tips, key and button rebinding
- **Mods**: switch RomFS mods on and off
- **Updates**: this version, check / install updates, the project page

## Controls

| Switch | Keyboard | Controller |
|---|---|---|
| Left stick | W A S D | left stick |
| Right stick (camera) | mouse, or Num 8 / 4 / 5 / 6 | right stick |
| A / B / X / Y | K / J / L / I | by position (east = A, south = B) |
| L / R, ZL / ZR | Q / E, 1 / 3 | shoulder buttons / triggers |
| + / - | Enter / Backspace | Start / Back |
| Stick clicks | F / Num 0 | stick clicks |
| D-pad | arrow keys | d-pad |

Fullscreen: F11 or Alt+Enter. Everything can be rebound in the menu.

The mouse turns the camera while you play: the pointer stays in the window until you open the menu (Esc) or switch to
another window. Sensitivity, invert up / down, or switching it off: menu > Controls > Mouse.

The game's button icons follow what you play with: after a key press or mouse movement they show the keys you bound
(WASD for moving, a mouse for the camera), after a controller press that controller's own buttons: Xbox (A B X Y,
LB / RB / LT / RT), PlayStation (cross circle square triangle, L1 / R1 / L2 / R2) or the Switch's. With two controllers
connected, the one you press a button on takes over. Menu > Controls > *Button prompts* can fix them to one style.

Motion controls (shake to throw Cappy at a target, and the game's other motion moves) use the controller's gyro and
accelerometer: PlayStation and Switch Pro controllers. Menu > Controls > *Motion controls* turns them off.

## Files

The program folder holds the program; the install folder (the same one unless you chose another) holds:

- `extracted\` - your game data as the installer wrote it and the game code built from it (`odyssey_game.dll`)
- `save\` - your saves, `cache\` - compiled shaders, `powermoon.ini` - settings, `SuperOdysseyRecomp.log` - the log
- `mods\` - optional RomFS mods, each in its own folder (`mods\<Mod name>\romfs\...`; the Atmosphere layout works too)

Never share `extracted\` or anything built from it: it's Nintendo's game.

## Building from source

Needs MSYS2 (UCRT64: clang, SDL3, FFmpeg, shaderc, Python) - `tools\setup_windows.ps1` installs them. See
[docs/BUILD-windows.md](docs/BUILD-windows.md).

1. Put your extracted game folders into `extracted\` (`extracted\exefs\`, `extracted\romfs\`), or start the built
   program and let its installer copy them there.
2. From an MSYS2 UCRT64 shell in this folder:

       make gen        # recompile the game from extracted/exefs into generated/
       make -j16       # build/SuperOdysseyRecomp.exe and the game code, build/odyssey_game.dll
       make dlls       # copy SDL3 / FFmpeg / shaderc DLLs next to the exe

3. A release: `make deps-windows` (an LGPL-only FFmpeg), `make gen-helper`, `tools/fetch_zig.sh`, then `make dist`.
   The zip holds no game code.

## How it works

- `tools/` - the recompiler (Python): `gen.py` lifts every function of the game's NSO modules to C (`lifter.py`);
  `odyssey_patches.py` holds the few game-specific patches for 1.0.0 and 1.4.1.
- `runtime/` - the program: module loader, a small Horizon kernel, IPC services, HLE of SDK functions, NVN on Vulkan
  with a Maxwell shader translator (`runtime/nvn`), movies with FFmpeg, the menu (Dear ImGui), the installer
  (`runtime/ui/setup.mm`), the game code build (`runtime/gamebuild.c`) and the updater (`runtime/updater.c`).
- Debugging tools are environment variables (`ODYSSEY_*`), documented where they're read.

Built with heavy help from AI coding agents, directed and tested by a human.

## Credits

- **Power Moon Recomp** by its contributors - the Super Mario Odyssey port this project continues (MIT license); its
  original repository is no longer online.
- **[LAS Recomp](https://github.com/peiikai/LASRecomp)** by Peiikai and its contributors - the static recompiler,
  runtime and renderer Power Moon Recomp is built on, and the frame interpolation (MIT license).
- **[MacNativeGames](https://github.com/ly05010419/MacNativeGames)** - the game package reading code `runtime/nxpkg.c`
  is ported from (MIT license, `tools/vendor/macnativegames`), and
  **[TotK PC Recomp](https://github.com/valentderah/totk-pc-recomp)**'s hactool build script (`tools/build_hactool.py`,
  MIT) used for checking
- [OdysseyDecomp](https://github.com/MonsterDruide1/OdysseyDecomp) - names and structure of the game's code, used for
  reading and debugging (no code from it is in this repository)
- [Switchbrew](https://switchbrew.org) and [envytools](https://github.com/envytools/envytools) - documentation of the
  Switch's system interfaces and the Maxwell GPU
- An MIT-licensed open-source project's audio renderer and shader instruction decoders (`third_party/ryujinx/LICENSE.txt`)
- Libraries: [SDL 3](https://libsdl.org) (zlib), [FFmpeg](https://ffmpeg.org) (LGPL, linked dynamically),
  [Dear ImGui](https://github.com/ocornut/imgui) (MIT), [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) (MIT),
  [shaderc](https://github.com/google/shaderc) (Apache 2.0), [SMAA](https://github.com/iryoku/smaa) (MIT),
  [AMD FSR 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR) (MIT), the [Roboto](https://github.com/googlefonts/roboto)
  font (Apache 2.0), NVIDIA DLSS (NVIDIA RTX SDK license)
- In the release, to build the game code on your PC: [Zig](https://ziglang.org) (MIT) with LLVM / Clang / LLD (Apache
  2.0 with LLVM exception) and MinGW-w64 headers; the generator runs on [Python](https://www.python.org) (PSF license)
  with [Capstone](https://www.capstone-engine.org) (BSD), [Unicorn](https://www.unicorn-engine.org) (GPL 2.0) and
  [LZ4](https://github.com/python-lz4/python-lz4) (BSD). The release carries every licence in `licenses/`.

The installer's pixel-art background and the icon are original artwork made for this project.

## Legal

This is a fan project. It is not affiliated with, endorsed by or sponsored by Nintendo. *Super Mario Odyssey* and
*Nintendo Switch* are trademarks of Nintendo. No game files, game code, assets, fonts or keys are included in this
repository or its releases, and nothing built from your game files (`extracted/`, `generated/`, `odyssey_game.dll`)
may be shared. You must provide the extracted files of your own legally obtained copy of the game.

## License

The code of this repository is under the [MIT License](LICENSE) (it keeps LAS Recomp's and Power Moon Recomp's
copyright notices). Third-party components keep their own licenses (above, and in `third_party/`). The license covers
this project's code only - it grants nothing for Nintendo's game.
