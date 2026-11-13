# Super Odyssey PC Recomp on Linux and Steam Deck (experimental)

> **Experimental.** The Linux and Steam Deck version is new and much less tested than the Windows one: expect rough
> edges, and please report what you find (see the end of this page).

The Linux version is an AppImage: one file that runs on SteamOS and on desktop distributions (glibc 2.35 or newer:
Ubuntu 22.04, Debian 12, Fedora 36, Arch, SteamOS, ...). It's built with `make appimage` (`tools/linux/appimage.sh`,
in Ubuntu 22.04 containers).

You need a Vulkan 1.3 driver (Mesa RADV / ANV / NVK, or NVIDIA's), a CPU with AVX2 (2013 or newer), about 12 GB of free
space, and the extracted **`exefs`** and **`romfs`** folders of your own copy of Super Mario Odyssey (version 1.0.0, or
1.4.1 with the update already applied).

## Installing

    chmod +x Super.Odyssey.PC.Recomp-<version>-linux-x86_64.AppImage
    ./Super.Odyssey.PC.Recomp-<version>-linux-x86_64.AppImage

The installer asks for the folder that holds your game's extracted `exefs` and `romfs` folders, and where to install.
Then it builds the game code for your PC (about 20-25 minutes on a Steam Deck, less on a desktop) and compiles every
shader, once.

If the AppImage won't start with a FUSE error, install your distribution's `fuse3` package, or run it with
`APPIMAGE_EXTRACT_AND_RUN=1`.

## Steam Deck

Do the install once in **Desktop Mode** (the file pickers need it), then play from **Gaming Mode**.

1. Put the AppImage on the SD card (for example `/run/media/deck/<card>/Super Odyssey/`) and make it executable
   (Properties > Permissions > *Is executable*, or `chmod +x` in Konsole).
2. Start it. At **Install the game to**, choose a folder on the SD card: the game data, the compiled shaders and your
   saves go there, nothing big stays on the internal drive. The choice is remembered in
   `~/.config/super-odyssey-pc-recomp/data-folder.txt`.
3. Add the AppImage to Steam (Steam > *Add a Non-Steam Game*), then play it from Gaming Mode.

On a Steam Deck the game starts with the **battery saver** settings: 75% internal resolution (960 x 600) upscaled with
AMD FSR 1, no SMAA, 60 FPS without interpolation, 4x texture filtering. The menu's Performance tab has a *Battery
saver* button to apply them again after changing things. For the biggest savings, also lower the power limit in the
Deck's Quick Access menu (Performance > TDP Limit, e.g. 8-10 W). Don't use the Deck's frame limit below 60: Odyssey's
gameplay runs one step per frame, so a 30 or 40 FPS limit slows the game itself down.

**90 and 120 FPS** (Performance > Frame rate: 60, 90 or 120, experimental) draw up to 90 or 120 frames a second with
gameplay still at 60. They are for a screen that fast: 90 Hz on a Steam Deck OLED, or a faster external display (the
LCD Deck's screen shows 60 at most). On an LCD
Deck in Cap Kingdom with the battery saver settings it measured about 78 FPS walking and 55 in the busiest town view,
with gameplay kept at full speed. It uses noticeably more power than 60 FPS.

## Where things are

The install folder you chose (default `~/.local/share/super-odyssey-pc-recomp`, or `$XDG_DATA_HOME/...`):

- `extracted/` - your game data and the game code built from it
- `save/` - your saves, `cache/` - compiled shaders, `powermoon.ini` - settings

The log, `SuperOdysseyRecomp.log`, is always in `~/.local/share/super-odyssey-pc-recomp` (on the internal drive,
wherever the game is installed); started from a terminal, it prints there instead.

`ODYSSEY_DATA_ROOT=<dir>` uses another folder for one start.

## Updates

The menu's Updates tab (F1 > Updates) downloads the new AppImage from this project's releases and replaces the one you
started, then restarts. It uses the system's `curl`, and the AppImage's folder must be writable (an SD card is fine).

## Reporting a problem

Open an issue with your distribution or SteamOS version, GPU and driver version, what happened, and the log file.
Never attach game files.
