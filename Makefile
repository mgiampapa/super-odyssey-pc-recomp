# Super Odyssey PC Recomp build
# Windows: MSYS2 UCRT64 (see docs/BUILD-windows.md), clang + Vulkan renderer: `make gen`, then `make -j16`.
# Linux: clang + Vulkan renderer; macOS: Homebrew clang/SDL3/FFmpeg, Metal renderer (both untested with Odyssey).
UNAME := $(shell uname -s)
comma := ,
ifneq ($(filter MINGW% MSYS% CYGWIN% Windows_NT,$(UNAME) $(OS)),)
PLATFORM := windows
else ifeq ($(UNAME),Linux)
PLATFORM := linux
else
PLATFORM := macos
endif

# Linux: ARCH (x86_64 / aarch64, default this machine's) picks the target, separately from PLATFORM. The generated code
# differs per arch (AArch64 hosts run the guest's FP/SIMD as inline asm, others use the portable C of simd.h), so a
# cross build has its own generated-<arch>/ and build/<arch>/; SYSROOT is the target's root filesystem (headers, libs).
GEN      ?= generated
ifeq ($(PLATFORM),linux)
HOSTARCH := $(patsubst arm64,aarch64,$(shell uname -m))
ARCH     ?= $(HOSTARCH)
ifneq ($(ARCH),$(HOSTARCH))
CROSS    := --target=$(ARCH)-linux-gnu $(if $(SYSROOT),--sysroot=$(SYSROOT))
GEN      := generated-$(ARCH)
B        ?= build/$(ARCH)
endif
GENSIMD  := --simd $(if $(filter aarch64,$(ARCH)),asm,c)
endif

# B: build dir. MACOS_MIN: oldest macOS to run on (default: this one). DEPS: prefix with SDL3 / FFmpeg instead of the
# system's.
B         ?= build
MACOS_MIN ?=
DEPS      ?=
MINFLAG  := $(if $(MACOS_MIN),-mmacosx-version-min=$(MACOS_MIN))
CC       := clang $(MINFLAG) $(CROSS)
CXX      := clang++ $(MINFLAG) $(CROSS)
IMGUI    := third_party/imgui
GENFLAGS := -O1 -fno-strict-aliasing -Wno-return-type -Wno-unused-label -fvisibility=hidden -Iruntime

# The game code (made by `make gen` from your own game files) is its own library next to the app (runtime/gamelib.h);
# the app itself holds none of it
GEN_SRC  := $(wildcard $(GEN)/main/*.c) $(wildcard $(GEN)/subsdk0/*.c) $(wildcard $(GEN)/sdk/*.c)
GEN_OBJ  := $(patsubst %.c,$(B)/obj/%.o,$(patsubst $(GEN)/%,%,$(GEN_SRC))) $(B)/obj/tools/gamelib_entry.o
# the modules made by `make gen`, in load order: their tables go into the library (tools/gamelib_entry.c)
GEN_MODS := $(foreach m,main subsdk0 sdk,$(if $(wildcard $(GEN)/$(m)/*.c),$(m)))
RT_SRC   := $(wildcard runtime/*.c) $(wildcard runtime/hle/*.c) $(wildcard runtime/services/*.c) \
            $(wildcard runtime/nvn/*.c) $(wildcard runtime/nvn/maxwell/*.c) runtime/platform/$(PLATFORM).c
RT_HDRS  := $(wildcard runtime/*.h) $(wildcard runtime/hle/*.h) $(wildcard runtime/platform/*.h) $(wildcard runtime/nvn/*.h) \
            $(wildcard runtime/nvn/maxwell/*.h) $(wildcard runtime/nvn/vulkan/*.h) $(wildcard runtime/ui/*.h)
UI_SRC   := $(wildcard runtime/ui/*.mm)
UI_OBJ   := $(patsubst runtime/%.mm,$(B)/rt/%.o,$(UI_SRC))
IMGUI_OBJ := $(B)/imgui/imgui.o $(B)/imgui/imgui_draw.o $(B)/imgui/imgui_tables.o $(B)/imgui/imgui_widgets.o \
             $(B)/imgui/imgui_impl_sdl3.o

ifeq ($(PLATFORM),macos)
SDL      := $(or $(DEPS),/opt/homebrew/opt/sdl3)
FFMPEG   := $(or $(DEPS),/opt/homebrew/opt/ffmpeg)
EXE      := $(B)/SuperOdysseyRecomp
DEPINC   := -I$(SDL)/include -I$(FFMPEG)/include
LDFLAGS  := -L$(SDL)/lib -lSDL3 -Wl,-rpath,$(SDL)/lib -L$(FFMPEG)/lib -lavformat -lavcodec -lavutil -lswscale -Wl,-rpath,$(FFMPEG)/lib \
            -framework CoreFoundation -framework Foundation -framework Metal -framework MetalFX -framework QuartzCore -framework AppKit -framework ImageIO -framework CoreGraphics -framework UniformTypeIdentifiers
BACKEND_SRC := $(wildcard runtime/nvn/*.m)
BACKEND_OBJ := $(patsubst runtime/%.m,$(B)/rt/%.o,$(BACKEND_SRC))
IMGUI_OBJ += $(B)/imgui/imgui_impl_metal.o
UIFLAGS  := -fobjc-arc
GAMELIB  := $(B)/odyssey_game.dylib
GAMELIB_LDFLAGS := -dynamiclib -undefined dynamic_lookup  # (the runtime functions it calls: the app's own)
GAMELIB_SHARED := -shared -Wl,-undefined,dynamic_lookup  # (the same, for the setup page's build: runtime/gamebuild.c)
else ifeq ($(PLATFORM),linux)
EXE      := $(B)/SuperOdysseyRecomp
DEPINC   := $(if $(DEPS),-I$(DEPS)/include)
ifeq ($(ARCH),x86_64)
GENARCH  ?= -march=x86-64-v3  # as on Windows: AVX2 + FMA for the lifted FP/SIMD code (checked at startup)
endif
GENFLAGS += $(GENARCH) -fPIC
OSFLAGS  := -Wno-format  # (%llx for uint64_t, which is unsigned long here: the same size)
SHADERC  ?= -lshaderc  # (Debian / Ubuntu's libshaderc.so; a shaderc built from source: -lshaderc_shared)
# -rdynamic: crash backtraces name host functions (runtime/platform/linux.c)
LDFLAGS  := -fuse-ld=lld -rdynamic $(if $(DEPS),-L$(DEPS)/lib) -lSDL3 -lavformat -lavcodec -lavutil -lswscale \
            -lvulkan $(SHADERC) -lpthread -ldl -lm
BACKEND_SRC := $(wildcard runtime/nvn/vulkan/*.c) $(wildcard runtime/nvn/vulkan/*.cpp)
BACKEND_OBJ := $(patsubst runtime/%.cpp,$(B)/rt/%.o,$(patsubst runtime/%.c,$(B)/rt/%.o,$(BACKEND_SRC)))
IMGUI_OBJ += $(B)/imgui/imgui_impl_vulkan.o
UIFLAGS  := -x c++  # the .mm UI sources are plain C++ apart from __APPLE__ blocks
GAMELIB  := $(B)/odyssey_game.so
GAMELIB_LDFLAGS := -shared -fuse-ld=lld  # (the runtime functions it calls: the app's own, -rdynamic)
GAMELIB_SHARED := -shared -fuse-ld=lld
else
EXE      := $(B)/SuperOdysseyRecomp.exe
DEPINC   := $(if $(DEPS),-I$(DEPS)/include)
OSFLAGS  := -D_FILE_OFFSET_BITS=64 -D_POSIX_THREAD_SAFE_FUNCTIONS -D_CRT_RAND_S -DWIN32_LEAN_AND_MEAN -DNOMINMAX
GENARCH  ?= -march=x86-64-v3  # AVX2 + FMA for the lifted FP/SIMD code (simd.h); override for older CPUs
GENFLAGS += -D_FILE_OFFSET_BITS=64 $(GENARCH)
LDFLAGS  := -fuse-ld=lld -Wl,--stack,16777216 $(if $(DEPS),-L$(DEPS)/lib) -lSDL3 -lavformat -lavcodec -lavutil -lswscale \
            -lvulkan-1 -lwinmm -lole32 -lshell32 -lcomdlg32 -luuid -lshaderc_shared -lpdh -lbcrypt -lwinhttp \
            $(if $(CONSOLE),,-Wl$(comma)--subsystem$(comma)windows)  # no console window (log -> SuperOdysseyRecomp.log); CONSOLE=1: one
BACKEND_SRC := $(wildcard runtime/nvn/vulkan/*.c) $(wildcard runtime/nvn/vulkan/*.cpp)
BACKEND_OBJ := $(patsubst runtime/%.cpp,$(B)/rt/%.o,$(patsubst runtime/%.c,$(B)/rt/%.o,$(BACKEND_SRC)))
BACKEND_OBJ += $(B)/rt/platform/windows_rc.o  # the .exe's icon (Explorer, taskbar)
WINDRES  ?= windres
IMGUI_OBJ += $(B)/imgui/imgui_impl_vulkan.o
UIFLAGS  := -x c++ $(OSFLAGS)  # the .mm UI sources are plain C++ apart from __APPLE__ blocks
GAMELIB  := $(B)/odyssey_game.dll
# the runtime functions it calls: exported by the .exe, linked through its import library
LDFLAGS  += -Wl,--export-all-symbols -Wl,--out-implib,$(B)/SuperOdysseyRecomp.dll.a
GAMELIB_LDFLAGS := -shared -fuse-ld=lld $(B)/SuperOdysseyRecomp.dll.a
GAMELIB_SHARED := -shared -fuse-ld=lld  # (+ the import library: runtime/gamebuild.c)
endif

# GAMELIB_ID: what the game code library is built from (the generator, the headers it is compiled against, the
# flags, and on Windows releases the compiler runtime it is linked with: tools/zig_slim.sh). A library with another id
# is never loaded (`make gen`, then `make` again; players' setup builds it again).
GAMELIB_SRCS := tools/gen.py tools/lifter.py tools/analyze.py tools/nso.py tools/passes.py tools/odyssey_patches.py \
                tools/simd_c.py tools/gamelib_entry.c runtime/hle/hle_symbols.txt \
                runtime/cpu.h runtime/simd.h runtime/simd_fp.h runtime/gamelib.h tools/zig_slim.sh
GAMELIB_ID := $(shell (cat $(GAMELIB_SRCS); echo '$(GENFLAGS)') | (shasum -a 256 2>/dev/null || sha256sum) | cut -c1-16)

RT_OBJ   := $(patsubst runtime/%.c,$(B)/rt/%.o,$(RT_SRC))
# (Windows: library headers' own paths, e.g. an assert's __FILE__ in vk_mem_alloc.h, name MSYS2's folder - which can be
# in the builder's user folder: written as /msys64 instead; tools/dist_check_paths.sh checks the release)
PREFIX_MAP := $(if $(filter windows,$(PLATFORM)),-ffile-prefix-map=$(shell cygpath -m / 2>/dev/null)=/msys64)
RTFLAGS  := -O2 -g -fno-strict-aliasing -Wall -Wno-unused-function -Iruntime -Iruntime/nvn $(DEPINC) $(OSFLAGS) $(PREFIX_MAP)
CXXFLAGS := -O2 -g -std=c++17 -I$(IMGUI) -I$(IMGUI)/backends -Iruntime -Iruntime/nvn $(DEPINC) $(PREFIX_MAP)

all: $(EXE) $(if $(strip $(GEN_SRC)),$(GAMELIB))  # (strip: with no game code yet GEN_SRC is " ", which $(if) counts as set)

$(B)/obj/%.o: $(GEN)/%.c runtime/cpu.h
	@mkdir -p $(dir $@)
	@$(CC) $(GENFLAGS) -c $< -o $@

$(B)/obj/tools/gamelib_entry.o: tools/gamelib_entry.c $(GAMELIB_SRCS) $(wildcard $(GEN)/*/*_table.c)
	@mkdir -p $(dir $@)
	$(CC) $(GENFLAGS) -DGAMELIB_ID='"$(GAMELIB_ID)"' -DGAMELIB_MODULES='$(foreach m,$(GEN_MODS),X($(m)))' -c $< -o $@
$(B)/rt/gamelib.o: $(GAMELIB_SRCS)
$(B)/rt/gamelib.o: RTFLAGS += -DGAMELIB_ID='"$(GAMELIB_ID)"'
# the setup page's build of it on the player's machine (runtime/gamebuild.c) uses the same flags
$(B)/rt/gamebuild.o: $(GAMELIB_SRCS) Makefile
$(B)/rt/gamebuild.o: RTFLAGS += -DGAMELIB_ID='"$(GAMELIB_ID)"' -DGAMELIB_CFLAGS='"$(strip $(filter-out -Iruntime,$(GENFLAGS)))"' \
                                -DGAMELIB_SHARED='"$(GAMELIB_SHARED)"'

$(B)/rt/%.o: runtime/%.c $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(RTFLAGS) -c $< -o $@

# SMAA (third_party/smaa, MIT): the reference shader as a C string for runtime/nvn/smaa_data.c (both backends build
# their passes from it); the non-ASCII bytes in its comments become '?', and its CRLF line ends plain LF (a CR left in
# the string literals ends them early on macOS clang)
$(B)/inc/smaa_hlsl.inc: third_party/smaa/SMAA.hlsl
	@mkdir -p $(dir $@)
	LC_ALL=C tr -d '\r' < $< | LC_ALL=C tr '\200-\377' '?' | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' > $@
$(B)/rt/nvn/smaa_data.o: $(B)/inc/smaa_hlsl.inc
$(B)/rt/nvn/smaa_data.o: RTFLAGS += -I$(B)/inc -Ithird_party/smaa/Textures
$(B)/rt/nvn/vulkan/fsr3_sdk.o: $(wildcard third_party/fsr3_sdk/api/include/*.h) \
    $(wildcard third_party/fsr3_sdk/api/include/vk/*.h) $(wildcard third_party/fsr3_sdk/upscalers/include/*.h)
$(B)/rt/nvn/vulkan/xess_sdk.o: $(wildcard third_party/xess/include/*.h)

$(B)/rt/%.o: runtime/%.cpp $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(OSFLAGS) -c $< -o $@

$(B)/rt/%.o: runtime/%.m $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(RTFLAGS) -fobjc-arc -c $< -o $@

# linked to a temp file, then renamed over: a copy of the game still running keeps its (old) file - relinking in
# place gets running copies killed by macOS (code signature of the changed pages). The temp file keeps the real name
# (in a folder of its own): on Windows the import library records the name the game code library loads it by.
$(EXE): $(RT_OBJ) $(BACKEND_OBJ) $(UI_OBJ) $(IMGUI_OBJ)
	@mkdir -p $(dir $@)link
	$(CXX) -o $(dir $@)link/$(notdir $@) $^ $(LDFLAGS)
	mv -f $(dir $@)link/$(notdir $@) $@

# its id file (<library>.id) says what it was built from without loading it (runtime/gamelib.c)
$(GAMELIB): $(GEN_OBJ) | $(EXE)
	$(CC) $(GAMELIB_LDFLAGS) -o $@.tmp $^
	mv -f $@.tmp $@
	echo $(GAMELIB_ID) > $@.id

$(B)/rt/ui/%.o: runtime/ui/%.mm $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(UIFLAGS) -c $< -o $@

$(B)/imgui/%.o: $(IMGUI)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(B)/imgui/imgui_impl_%.o: $(IMGUI)/backends/imgui_impl_%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(B)/imgui/imgui_impl_metal.o: $(IMGUI)/backends/imgui_impl_metal.mm
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -fobjc-arc -c $< -o $@

# Python: tools/venv (macOS, Linux: bin/python, Windows: Scripts/python.exe)
PY := $(firstword $(wildcard tools/venv/bin/python) $(wildcard tools/venv/Scripts/python.exe))

gen:
	cd tools && ../$(PY) gen.py sdk ../extracted/exefs/sdk 0x7004000000 ../$(GEN)/sdk --hle ../runtime/hle/hle_symbols.txt $(GENSIMD)
	cd tools && ../$(PY) gen.py main ../extracted/exefs/main 0x7000000000 ../$(GEN)/main --hle ../runtime/hle/hle_symbols.txt $(GENSIMD)
	$(if $(wildcard extracted/exefs/subsdk0),cd tools && ../$(PY) gen.py subsdk0 ../extracted/exefs/subsdk0 0x7003000000 ../$(GEN)/subsdk0 $(GENSIMD))  # (Odyssey's multimedia library)

gen-objs: $(GEN_OBJ)

# Runtime objects only (no generated code, no link): quick compile check
runtime-objs: $(RT_OBJ) $(BACKEND_OBJ) $(UI_OBJ)

$(B)/rt/platform/windows_rc.o: runtime/platform/windows.rc docs/icon/icon.ico
	@mkdir -p $(dir $@)
	$(WINDRES) -I docs/icon -O coff $< -o $@

# Windows: the DLLs the exe loads (MSYS2 UCRT64's SDL3, FFmpeg, shaderc and what they pull in) copied next to it, so
# SuperOdysseyRecomp.exe starts from Explorer or a shortcut without MSYS2 on the PATH. vulkan-1.dll is the system's.
dlls: $(EXE)
	ldd $(EXE) | awk '/ucrt64/ && !/vulkan-1/ {print $$3}' | sort -u | xargs -I{} cp -u {} $(dir $(EXE))

# ---------------------------------------------------------------- release (Windows)
# build/dist/Super Odyssey PC Recomp-<version>-windows.zip, top folder "Super Odyssey PC Recomp/": the exe and the DLLs it loads (runs
# without MSYS2), what the setup page builds the game code with on the player's machine (runtime/gamebuild.c) - the
# generator (make gen-helper), zig (tools/fetch_zig.sh, slimmed by tools/zig_slim.sh: its runtime built ahead of time),
# gamelib-src/ and the exe's import library - the shader list, the menu font, README and LICENSE. No game code and no
# game files: the setup page (runtime/ui/setup.mm) builds them from the player's own. The exe is built in $(WIN_B)
# against the LGPL FFmpeg from `make deps-windows` (MSYS2's is a GPLv3 build); vulkan-1.dll is the system's.
# Order: make deps-windows; make gen-helper; tools/fetch_zig.sh; make dist.
VERSION  := $(shell sed -n 's/^#define APP_VERSION "\(.*\)"/\1/p' runtime/version.h)
RC_VERSION := $(shell sed -n 's/.*"ProductVersion", "\(.*\)"/\1/p' runtime/platform/windows.rc)
DIST     := build/dist/Super Odyssey PC Recomp
WIN_B    := build/win-release
ifeq ($(PLATFORM),windows)
PY312    ?= py -3.12
GEN_VPY  := build/gen-venv/Scripts/python.exe
else  # (macOS: the relocatable Python from `make deps`, so the helper runs on the app's oldest macOS)
PY312    ?= $(firstword $(wildcard build/deps/python/bin/python3.12) python3.12)
GEN_VPY  := build/gen-venv/bin/python
endif
deps-windows:
	tools/build_deps_windows.sh

# The game code generator as a standalone helper (tools/odysseygen.spec, PyInstaller, its own venv build/gen-venv):
# build/helper/odysseygen/, shipped as odysseygen/ next to the exe (macOS: in the .app's Resources). PyInstaller only
# builds for the OS it runs on (Linux: tools/linux/gen_helper.sh in the AppImage's build container).
# Windows: Python's own signed interpreter with the generator's sources and packages (tools/pyembed.py: a PyInstaller
# bundle was taken for a trojan by Windows Defender); macOS, Linux: frozen with PyInstaller (tools/odysseygen.spec).
ifeq ($(PLATFORM),windows)
gen-helper: build/gen-venv/.ok
	$(GEN_VPY) tools/pyembed.py build/helper/odysseygen
	@du -sh build/helper/odysseygen
else
gen-helper: build/gen-venv/.ok
	$(GEN_VPY) -m PyInstaller --noconfirm --clean --log-level WARN --distpath build/helper --workpath build/pyinstaller \
	    tools/odysseygen.spec
	@du -sh build/helper/odysseygen
endif
build/gen-venv/.ok:
	$(PY312) -m venv build/gen-venv
	$(GEN_VPY) -m pip install -q capstone==5.0.9 unicorn==2.1.4 lz4==4.4.5 pyinstaller==6.22.3
	touch $@

# ---------------------------------------------------------------- release (macOS, Linux)
# macOS: make deps (SDL3, LGPL FFmpeg, a relocatable Python - for macOS APP_MIN+), make gen-helper, tools/fetch_zig.sh,
# make dmg -> build/dist/Super Odyssey PC Recomp-<version>-macOS.dmg (Apple Silicon, ad-hoc signed).
# Linux: make appimage (podman, Ubuntu 22.04 containers: tools/linux/appimage.sh) -> build/dist/...-linux-x86_64.AppImage.
APP_MIN  := 15.0
APP_B    := build/mac15
deps:
	MACOS_MIN=$(APP_MIN) tools/build_deps_macos.sh
app:
	@[ -f build/deps/lib/libSDL3.0.dylib ] || { echo "make deps first"; exit 1; }
	$(MAKE) B=$(APP_B) MACOS_MIN=$(APP_MIN) DEPS=$(CURDIR)/build/deps $(APP_B)/SuperOdysseyRecomp
	MACOS_MIN=$(APP_MIN) tools/bundle_macos.sh $(APP_B)/SuperOdysseyRecomp
dmg: app
	tools/make_dmg.sh
appimage:
	tools/linux/appimage.sh $(or $(ARCH),x86_64)

dist:
	@[ "$(VERSION)" = "$(RC_VERSION)" ] || { echo "runtime/version.h ($(VERSION)) and windows.rc ($(RC_VERSION)) differ"; exit 1; }
	@[ -f build/deps-win/lib/libavcodec.dll.a ] || { echo "make deps-windows first (LGPL FFmpeg)"; exit 1; }
	@[ -d build/helper/odysseygen ] || { echo "make gen-helper first (the game code generator)"; exit 1; }
	@[ -x build/zig/zig.exe ] || { echo "tools/fetch_zig.sh first (the compiler)"; exit 1; }
	$(MAKE) B=$(WIN_B) DEPS=$(CURDIR)/build/deps-win $(WIN_B)/SuperOdysseyRecomp.exe
	! nm $(WIN_B)/SuperOdysseyRecomp.exe | grep ' f_70' >/dev/null  # (no game code inside)
	rm -rf build/dist && mkdir -p "$(DIST)/gamelib-src"
	cp $(WIN_B)/SuperOdysseyRecomp.exe $(WIN_B)/SuperOdysseyRecomp.dll.a "$(DIST)/"
	strip --strip-debug "$(DIST)/SuperOdysseyRecomp.exe"  # (its debug info names the build folder; exports stay)
	tools/zig_slim.sh $(strip $(filter-out -Iruntime,$(GENFLAGS)))  # (zig with its runtime built ahead of time)
	cp -R build/helper/odysseygen "$(DIST)/"
	cp -R build/zig-slim "$(DIST)/zig"
	cp runtime/cpu.h runtime/simd.h runtime/simd_fp.h runtime/gamelib.h tools/gamelib_entry.c runtime/hle/hle_symbols.txt \
	    "$(DIST)/gamelib-src/"
	cp runtime/nvn/vulkan/known_programs.txt third_party/imgui/misc/fonts/Roboto-Medium.ttf README.md LICENSE "$(DIST)/"
	tools/dist_licenses.sh "$(DIST)"  # (every shipped component's licence and source: licenses/README.txt)
	PATH="$(CURDIR)/build/deps-win/bin:$$PATH" ldd $(WIN_B)/SuperOdysseyRecomp.exe | awk '(/ucrt64/ || /deps-win/) && !/vulkan-1/ {print $$3}' | \
	    sort -u | xargs -I{} cp -u {} "$(DIST)/"
	! ls "$(DIST)" | grep -iE 'x264|x265|xvid|postproc'  # (no GPL codec libraries in the release)
	tools/dist_check_paths.sh "$(DIST)"  # (no path of this machine inside: debug info, caches, logs)
	cd build/dist && /c/Windows/System32/tar.exe -a -cf "Super Odyssey PC Recomp-$(VERSION)-windows.zip" "Super Odyssey PC Recomp"
	@echo "build/dist/Super Odyssey PC Recomp-$(VERSION)-windows.zip"

.PHONY: all gen gen-objs runtime-objs dlls deps-windows gen-helper dist deps app dmg appimage

build/test_shaders: tools/test_shaders.m runtime/nvn/maxwell/translate.c runtime/nvn/maxwell/shader.h
	clang -O1 -fobjc-arc -framework Metal -framework Foundation -o $@ tools/test_shaders.m runtime/nvn/maxwell/translate.c
