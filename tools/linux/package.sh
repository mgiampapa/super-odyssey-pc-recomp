#!/bin/bash
# tools/linux/package.sh <x86_64|aarch64> (inside the amd64 build container, tools/linux/appimage.sh): the app built
# against build/linux/<arch>/deps and the game code generator helper (tools/linux/gen_helper.sh) put together as an
# AppDir and packed by appimagetool: build/dist/Super Odyssey PC Recomp-<version>-linux-<arch>.AppImage.
# Shipped next to the executable (usr/bin, where runtime/paths.c rt_resource looks): what the setup page builds the
# game code with on the player's machine (runtime/gamebuild.c: odysseygen/, zig/ from tools/fetch_zig.sh,
# gamelib-src/), the window icon, the menu font, the shader list, licences. No game code: that's built from the
# player's own files. usr/lib: the libraries from deps/ only - glibc, libstdc++, the Vulkan loader and the GPU drivers
# are the system's (a bundled loader would not find the user's drivers). No game files or fonts.
set -euo pipefail
cd "$(dirname "$0")/../.."
ARCH=$1
L=$PWD/build/linux/$ARCH
P=$L/deps
W=$L/deps-build
B=$L/game
APPDIR=$L/AppDir
VERSION=$(sed -n 's/.*"ProductVersion", "\(.*\)"/\1/p' runtime/platform/windows.rc)
OUT="build/dist/Super Odyssey PC Recomp-$VERSION-linux-$ARCH.AppImage"
JOBS=$(nproc)

tools/linux/build_deps.sh "$ARCH"

if [ "$ARCH" != "$(uname -m)" ]; then
    : "${SYSROOT:?cross build: SYSROOT (the arm64 root filesystem)}"
    MAKEARGS=(ARCH="$ARCH" SYSROOT="$SYSROOT")
else
    MAKEARGS=(ARCH="$ARCH")
fi

# the app alone (the game code is built on the player's machine)
make -j"$JOBS" B="$B" DEPS="$P" SHADERC=-lshaderc_shared "${MAKEARGS[@]}" "$B/SuperOdysseyRecomp"
if nm "$B/SuperOdysseyRecomp" | grep ' f_70' >/dev/null; then echo "$B/SuperOdysseyRecomp has game code linked in"; exit 1; fi
ZIG_TARGET=$ARCH-linux tools/fetch_zig.sh

# ---- AppDir
rm -rf "$APPDIR" && mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib"
U=$APPDIR/usr/bin
cp "$B/SuperOdysseyRecomp" "$U/"
llvm-strip --strip-debug "$U/SuperOdysseyRecomp"  # (symbols stay: crash backtraces name functions)
cp docs/icon/icon_512.png "$U/icon.png"
cp third_party/imgui/misc/fonts/Roboto-Medium.ttf runtime/nvn/vulkan/known_programs.txt data/pipeline-seed.bin README.md \
   LICENSE "$U/"
[ -d "$L/helper/odysseygen" ] || { echo "no game code generator helper: tools/linux/gen_helper.sh $ARCH first"; exit 1; }
cp -R "$L/helper/odysseygen" build/zig "$U/"
mkdir "$U/gamelib-src"
cp runtime/cpu.h runtime/simd.h runtime/simd_fp.h runtime/gamelib.h tools/gamelib_entry.c runtime/hle/hle_symbols.txt \
   "$U/gamelib-src/"
cp tools/linux/AppRun "$APPDIR/" && chmod +x "$APPDIR/AppRun"
cp tools/linux/superodysseyrecomp.desktop "$APPDIR/"
cp docs/icon/icon_512.png "$APPDIR/superodysseyrecomp.png" && ln -sf superodysseyrecomp.png "$APPDIR/.DirIcon"

# licences of what ships (the sources' own texts), as in the Windows release
D=$U/licenses
mkdir -p "$D"
cp third_party/imgui/LICENSE.txt "$D/Dear-ImGui-MIT.txt"
cp third_party/smaa/LICENSE.txt "$D/SMAA-MIT.txt"
cp third_party/fsr/LICENSE.txt "$D/AMD-FSR1-MIT.txt"
cp third_party/ryujinx/LICENSE.txt "$D/Ryujinx-MIT.txt"
cp third_party/imgui/misc/fonts/Roboto-LICENSE.txt "$D/Roboto-Apache-2.0.txt"
cp "$W/vma/LICENSE.txt" "$D/VulkanMemoryAllocator-MIT.txt"
cp "$W/sdl/LICENSE.txt" "$D/SDL3-zlib.txt"
cp "$W/ffmpeg/COPYING.LGPLv2.1" "$D/FFmpeg-LGPL-2.1.txt"
cp "$W/ffmpeg/COPYING.GPLv2" "$D/GPL-2.0.txt"
cp "$W/shaderc/LICENSE" "$D/shaderc-Apache-2.0.txt"
cp build/zig/LICENSE "$D/Zig-MIT.txt"
SP=$(echo "$L"/gen-venv/lib/python3.12/site-packages)
cp "$SP"/pyinstaller-*.dist-info/licenses/COPYING.txt "$D/PyInstaller.txt"
cp "$SP"/capstone-*.dist-info/LICENSE.TXT "$D/Capstone-BSD.txt" 2>/dev/null || true
cp "$SP"/lz4-*.dist-info/licenses/LICENSE "$D/python-lz4-BSD.txt" 2>/dev/null || true
cp /usr/share/doc/python3.12/copyright "$D/Python-PSF.txt" 2>/dev/null || true
cat > "$D/README.txt" <<'EOF'
Super Odyssey PC Recomp - third-party components in this AppImage and their licences
- SDL 3.4.16 (zlib) - SDL3-zlib.txt
- FFmpeg 9.0.2, an LGPL-only build, linked dynamically (usr/lib) - FFmpeg-LGPL-2.1.txt; source:
  https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz, configured as tools/linux/build_deps.sh in the source repository
- shaderc 2026.3 (Apache 2.0) - shaderc-Apache-2.0.txt; Vulkan Memory Allocator (MIT); Dear ImGui (MIT); SMAA (MIT);
  AMD FSR 1 (MIT); the Roboto font (Apache 2.0); parts of the audio renderer and shader translator follow an
  MIT-licensed project (Ryujinx-MIT.txt)
- Setup's tools: zig/ (Zig 0.17.0, MIT, with LLVM / Clang / LLD: Apache 2.0 with LLVM exception), odysseygen/ (this
  project's generator frozen with PyInstaller: GPL 2.0 with bootloader exception; CPython 3.12: PSF licence;
  Capstone: BSD; Unicorn 2.1.4: GPL 2.0 - GPL-2.0.txt, https://github.com/unicorn-engine/unicorn/tree/2.1.4;
  python-lz4: BSD)
EOF

# libraries: what the app needs (DT_NEEDED, recursively) that deps/ provides; anything else must be a system one
needed() { readelf -d "$1" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'; }
SYSTEM_LIBS='^(libc|libm|libdl|libpthread|librt|libresolv|libstdc\+\+|libgcc_s|libvulkan)\.so|^ld-linux'
todo=("$U/SuperOdysseyRecomp")
while [ ${#todo[@]} -gt 0 ]; do
    f=${todo[0]}; todo=("${todo[@]:1}")
    for n in $(needed "$f"); do
        [ -e "$APPDIR/usr/lib/$n" ] && continue
        if [ -e "$P/lib/$n" ]; then
            cp -L "$P/lib/$n" "$APPDIR/usr/lib/$n"
            patchelf --set-rpath '$ORIGIN' "$APPDIR/usr/lib/$n"
            todo+=("$APPDIR/usr/lib/$n")
        elif ! [[ $n =~ $SYSTEM_LIBS ]]; then
            echo "package: $(basename "$f") needs $n, neither bundled nor a base system library"; exit 1
        fi
    done
done
patchelf --set-rpath '$ORIGIN/../lib' "$U/SuperOdysseyRecomp"
if ls "$APPDIR/usr/lib" | grep -iE 'x264|x265|xvid|postproc|vulkan'; then  # (no GPL codecs, no Vulkan loader)
    echo "package: library that must not ship"; exit 1
fi
echo "bundled: $(ls "$APPDIR/usr/lib" | tr '\n' ' ')"

# ---- AppImage (appimagetool runs extracted: no FUSE in the container)
fetch() {  # url sha256 file
    [ -f "build/linux/src/$3" ] || { curl -fsSL -o "build/linux/src/$3.part" "$1" && mv "build/linux/src/$3.part" "build/linux/src/$3"; }
    echo "$2  build/linux/src/$3" | sha256sum -c --quiet
}
fetch https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage \
      ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0 appimagetool-x86_64.AppImage
case $ARCH in
    x86_64) fetch https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64 \
                  2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d runtime-x86_64 ;;
    aarch64) fetch https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-aarch64 \
                   00cbdfcf917cc6c0ff6d3347d59e0ca1f7f45a6df1a428a0d6d8a78664d87444 runtime-aarch64 ;;
esac
chmod +x build/linux/src/appimagetool-x86_64.AppImage
mkdir -p build/dist && rm -f "$OUT"
APPIMAGE_EXTRACT_AND_RUN=1 ARCH=$ARCH build/linux/src/appimagetool-x86_64.AppImage --no-appstream \
    --runtime-file "build/linux/src/runtime-$ARCH" "$APPDIR" "$OUT"
ls -la "$OUT"
