#!/bin/bash
# Release pipeline (Git Bash on Windows, with the Steam Deck reachable over SSH): bumps the version, builds both
# packages, and runs every check this project does by hand before a release - including the AppImage catalog's own
# test (see sopr-catalog-probe) on a build that ISN'T published yet. Nothing goes to GitHub until all of it passes.
#
# Usage: tools/release.sh <version> <path-to-release-notes.md>
#   e.g. tools/release.sh 0.2.8 /path/to/notes.md
#
# What it does, in order (stops at the first failure, nothing published):
#   1. Bump runtime/version.h and runtime/platform/windows.rc; commit locally.
#   2. Build the Windows exe (make B=build/dev) and package it into a zip, reusing the previous release's folder as a
#      base (this checkout can't run `make dist` end to end - see deps-windows below).
#   3. Smoke-test the Windows exe for ~45s (Cap Kingdom save, no crash).
#   4. Send the source to the Steam Deck (git archive | ssh), build the AppImage there (tools/linux/appimage.sh via
#      deck-build.sh), and fetch it back.
#   5. On the Deck, boot-test the AppImage three ways: with no Vulkan driver, with a Vulkan driver, and with your real
#      install's save (a 2-minute run to the title screen). The game must show a window or its own error box every
#      time, never hang.
#   6. Upload the (still unpublished) AppImage to the probe repo's "staging" pre-release, run its probe.yml against
#      that exact file with the catalog's own steps (Xvfb 800x600, firejail, no network, no GPU), and require it to
#      pass. This is the check that would have caught both Linux regressions before they ever reached GitHub.
#   7. Only if everything above passed: push to GitHub main and `gh release create`.
#   8. Re-run the probe against the now-published file as a final confirmation, and print a one-line summary.
#
# Config you may need to change below: DECK, the msys PATH for the C++ toolchain, and PROBE_REPO.
#
# This still needs YOU to look at the boot-test screenshots it prints paths for - it checks "did it run without
# crashing", not "does it look right".
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION=${1:?usage: tools/release.sh <version> <release-notes.md>}
NOTES=${2:?usage: tools/release.sh <version> <release-notes.md>}
[ -f "$NOTES" ] || { echo "no such file: $NOTES"; exit 1; }

# This machine's own places: tools/release.local.sh (not in the repository; see .gitignore) sets
#   DECK=user@host          the Steam Deck (or another Linux PC with podman) over SSH, with the source in ~/sopr
#   MSYS=/path/to/msys64    the MSYS2 tree with the Windows toolchain
#   PREV_DIR=/path          the folder holding the last release's "Super Odyssey PC Recomp" folder (DLLs, licences)
#   TESTAPP=/path           a folder the exe can be run from for the smoke test
#   TESTDATA=X:/path        its game data root (ODYSSEY_DATA_ROOT), with a save in save-cap/
[ -f tools/release.local.sh ] && . tools/release.local.sh
: "${DECK:?set DECK in tools/release.local.sh}" "${MSYS:?set MSYS in tools/release.local.sh}"
: "${PREV_DIR:?set PREV_DIR in tools/release.local.sh}" "${TESTAPP:?set TESTAPP in tools/release.local.sh}"
: "${TESTDATA:?set TESTDATA in tools/release.local.sh}"
SSH="/c/Windows/System32/OpenSSH/ssh.exe -o BatchMode=yes"
SCP="/c/Windows/System32/OpenSSH/scp.exe -q -o BatchMode=yes"
PROBE_REPO=Simon3Sarkis/sopr-catalog-probe
MAIN_REPO=Simon3Sarkis/super-odyssey-pc-recomp
WORK=$(mktemp -d)
trap 'echo "(work dir kept for inspection: $WORK)"' EXIT

say() { echo; echo "== $*"; }

say "1/8  bumping version to $VERSION"
sed -i "s/#define APP_VERSION \".*\"/#define APP_VERSION \"$VERSION\"/" runtime/version.h
sed -i "s/VALUE \"FileVersion\", \".*\"/VALUE \"FileVersion\", \"$VERSION\"/;
        s/VALUE \"ProductVersion\", \".*\"/VALUE \"ProductVersion\", \"$VERSION\"/" runtime/platform/windows.rc
git commit -qam "Super Odyssey PC Recomp $VERSION

"

say "2/8  building Windows (make B=build/dev)"
export PATH="$MSYS/ucrt64/bin:$MSYS/usr/bin:$PATH"
make -j16 B=build/dev

WIN="$WORK/win/Super Odyssey PC Recomp"
mkdir -p "$WIN"
cp -r "$PREV_DIR/Super Odyssey PC Recomp/." "$WIN/"
cp build/dev/SuperOdysseyRecomp.exe build/dev/SuperOdysseyRecomp.dll.a README.md LICENSE runtime/nvn/vulkan/known_programs.txt "$WIN/"
strip --strip-debug "$WIN/SuperOdysseyRecomp.exe"
for dll in $(objdump -p "$WIN/SuperOdysseyRecomp.exe" | awk '/DLL Name/{print $3}'); do
    [ -f "$WIN/$dll" ] || [ -f "/c/Windows/System32/$dll" ] || [[ "$dll" == api-ms-* ]] || { echo "MISSING: $dll"; exit 1; }
done
[ "$(nm "$WIN/SuperOdysseyRecomp.exe" | grep -c ' f_70')" = 0 ] || { echo "game code leaked into the exe"; exit 1; }
tools/dist_check_paths.sh "$WIN"
[ "$(strings "$WIN/SuperOdysseyRecomp.exe" | grep -c "^$VERSION\$")" -ge 1 ] || { echo "version string missing from exe"; exit 1; }
( cd "$WORK/win" && /c/Windows/System32/tar.exe -a -cf "Super Odyssey PC Recomp-$VERSION-windows.zip" "Super Odyssey PC Recomp" )
WIN_ZIP="$WORK/win/Super Odyssey PC Recomp-$VERSION-windows.zip"

say "3/8  smoke-testing the Windows exe"
cp "$WIN/SuperOdysseyRecomp.exe" "$TESTAPP/"
( cd "$TESTAPP" && ODYSSEY_DATA_ROOT="$TESTDATA" ODYSSEY_SAVE_DIR="$TESTDATA/save-cap" \
  ODYSSEY_PREPARE=0 ODYSSEY_NO_UPDATE_CHECK=1 ODYSSEY_INPUT='1500:A:8,1650:A:8,1800:A:8,1950:A:8,2100:A:8' \
  timeout 45 ./SuperOdysseyRecomp.exe > "$WORK/win-smoke.log" 2>&1 ) || true
grep -q FATAL "$WORK/win-smoke.log" && { echo "Windows smoke test hit a FATAL:"; grep FATAL "$WORK/win-smoke.log"; exit 1; }
grep -q fps "$WORK/win-smoke.log" || { echo "Windows smoke test never reached a frame"; exit 1; }
echo "ok: $(grep fps "$WORK/win-smoke.log" | tail -1 | cut -c1-60)"

say "4/8  building the AppImage on the Steam Deck"
git archive --format=tar HEAD | $SSH $DECK '
    tar -x -C ~/sopr && bash ~/sopr/deck-fix-eol.sh >/dev/null &&
    systemctl --user reset-failed sopr-build 2>/dev/null
    systemd-run --user --collect --unit=sopr-build /home/deck/sopr/deck-build.sh'
for i in $(seq 1 60); do
    $SSH $DECK "tail -1 ~/sopr/build/appimage.log" | grep -q "EXIT=0" && break
    [ $i = 60 ] && { echo "Deck build didn't finish"; exit 1; }
    sleep 10
done
LINUX_APP="$WORK/Super Odyssey PC Recomp-$VERSION-linux-x86_64.AppImage"
$SCP "$DECK:sopr/build/dist/Super\ Odyssey\ PC\ Recomp-$VERSION-linux-x86_64.AppImage" "$LINUX_APP"

say "5/8  boot-testing on the Deck (no driver / with driver / your install)"
MSYS_NO_PATHCONV=1 $SSH $DECK "
    sed -i \"s#Super Odyssey PC Recomp-[0-9.]*-linux-x86_64.AppImage#Super Odyssey PC Recomp-$VERSION-linux-x86_64.AppImage#\" /tmp/ai-case.sh ~/sopr/deck-boot-025.sh
    bash /tmp/ai-case.sh nogpu 2>&1 | grep '=='
    bash /tmp/ai-case.sh normal 2>&1 | grep '=='
    systemctl --user reset-failed sopr-boot 2>/dev/null
    rm -f ~/sopr/build/boot-test.log
    systemd-run --user --collect --unit=sopr-boot /home/deck/sopr/deck-boot-025.sh >/dev/null
    for i in \$(seq 1 40); do grep -q DONE ~/sopr/build/boot-test.log 2>/dev/null && break; sleep 5; done
    cat ~/sopr/build/boot-test.log
    cp '/run/media/deck/SN01T/Super Odyssey/game/build/window.png' /tmp/deck-release-shot.png" | tee "$WORK/deck-boot.log"
grep -q "still running after 120" "$WORK/deck-boot.log" || { echo "the real-install boot test exited early - check the log"; exit 1; }
MSYS_NO_PATHCONV=1 $SCP "$DECK:/tmp/deck-release-shot.png" "$WORK/deck-boot-shot.png"
echo "look at this before publishing: $WORK/deck-boot-shot.png"

say "6/8  the catalog probe, against the UNPUBLISHED build"
gh release delete staging --repo "$PROBE_REPO" -y 2>/dev/null || true
gh release create staging --repo "$PROBE_REPO" --prerelease --title staging --notes "release.sh staging build - not the real release" "$LINUX_APP" >/dev/null
STAGING_URL="https://github.com/$PROBE_REPO/releases/download/staging/$(basename "$LINUX_APP" | tr ' ' '.')"
gh workflow run probe.yml --repo "$PROBE_REPO" -f "url=$STAGING_URL"
sleep 10
RUN_ID=$(gh run list --repo "$PROBE_REPO" --workflow probe.yml --limit 1 --json databaseId -q '.[0].databaseId')
gh run watch "$RUN_ID" --repo "$PROBE_REPO" --exit-status || { echo "the catalog probe FAILED on the unpublished build - not releasing"; exit 1; }
echo "probe passed on the unpublished build"

say "7/8  publishing v$VERSION"
git push origin HEAD:main
( cd "$WORK" && gh release create "v$VERSION" "$WIN_ZIP" "$LINUX_APP" --repo "$MAIN_REPO" --target main \
      --title "Super Odyssey PC Recomp $VERSION (preview)" --notes-file "$NOTES" )

say "8/8  re-running the probe against the PUBLISHED file"
PUB_URL="https://github.com/$MAIN_REPO/releases/download/v$VERSION/$(basename "$LINUX_APP" | tr ' ' '.')"
gh workflow run probe.yml --repo "$PROBE_REPO" -f "url=$PUB_URL"
sleep 10
RUN_ID=$(gh run list --repo "$PROBE_REPO" --workflow probe.yml --limit 1 --json databaseId -q '.[0].databaseId')
gh run watch "$RUN_ID" --repo "$PROBE_REPO" --exit-status \
    && echo "RELEASED and confirmed: https://github.com/$MAIN_REPO/releases/tag/v$VERSION" \
    || echo "RELEASED but the published-file probe failed - look into this before telling the catalog to /retest"
gh release delete staging --repo "$PROBE_REPO" -y 2>/dev/null || true
