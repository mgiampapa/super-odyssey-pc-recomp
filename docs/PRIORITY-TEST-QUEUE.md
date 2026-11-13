# Priority queue after upscaler testing

Simon confirmed the current preview works and requested this list ahead of the
remaining frame-generation and HUD-resolution work. Implement and test changes
separately, with private settings/saves and local commits. Do not publish a
release or change the production install as part of a preview test.

The working branch combines the tested upscalers with the local 0.2.5 release
changes (fog readback enabled by default and matching Windows version metadata).
The original release checkout and previous upscaler branch remain intact.

Current checkpoint (13 November 2026): 1.4.1 warp helpers are fixed locally.
The instrumented 0.2.5 source reference reached and captured all 17 kingdoms at
60 FPS. The current renderer also passed Cap/Cascade captures and a Cap tower
stage change; the final GUI preview passed a Cap transition with its optional
upscaler runtimes staged. Cap/Cascade comparisons flag localized animated
fog/water/effect differences for review. The full 17-kingdom candidate comparison
and interaction coverage are still pending; this is not a full-game graphics
sign-off. Simon confirmed the private warp preview works perfectly.

Simon confirmed the crash-report export by sharing a complete diagnostic file.
Export, error-popup copy and normal
quit were exercised on Windows; native fixtures also verified early failures,
bounded logs, safe file failures and recovery after process restart/log replacement.
See CRASH-REPORTS.md. This does not fix the separate poison or Iris Xe crashes.

Simon confirmed reactive fog on 1.0.0 works. The named reader's NVN caller is now
build-ID gated alongside 1.4.1; both register layouts were checked against the
owner's extracted files. Private stock-60-FPS runs passed on both versions.
See FOG-READBACK.md.

Simon confirmed revision 2 completely fixes the 1.0.0 poison issue. A private
Spewart run with the reporter's save found stale CPU mask bytes compared with
the rendered image. Candidate and 1.4.1 fog regression runs passed.
Simon reported the first candidate still failed. Revision 2 also covers the
direct footprint reader, labeling buffers, CPU-edit uploads and 0x40 linear
render-target layout. The 4K clean-ground control kept 3 HP; entering visible
poison produced a poison reaction and 2 HP. The verified 1.0.0 readback paths
are now enabled by default, with a diagnostic opt-out. See POISON-READBACK.md.
No public issue mutation, push, release or production-install changes were made.

Next: reproducible Windows packaging from a clean dependency build, followed by
Windows ZIP/Linux AppImage CI artifacts. Preserve the tested features; do not
reuse an old release directory or include game content.

A separate 120 FPS / 1280x720 hidden-window save-load test crashed before its
warp (TextureDataEx::reset, access 0x20). The 60 FPS tour passed. Its cause and
whether it predates these test hooks are not established. Keep this case open;
do not describe the warp fix as resolving it. The 1.0.0 named warp path is
retained but was not rerun with a matching current game-code library here.

| Order | Work | Verification before calling it finished |
| --- | --- | --- |
| 1a | Repair the 1.4.1 warp helpers | Cross-kingdom travel and a local stage change must reach the logged destination without a crash. Named 1.0.0 helpers must still work. |
| 1b | Automated tour of every kingdom using a copied completed save | Capture actual destination scenes after loading, time out failures, keep per-stage logs and screenshots, compare candidate and last-release images with the same settings. Report visual differences for review; do not equate a screenshot match with complete gameplay correctness. Include explicit fog/poison interaction checks. |
| 1c | Copy crash report button | One attachable file containing log, settings, CPU/GPU/driver, app/game build and exception/call location. Work for early crashes and normal sessions; disclose included data and avoid keys, assets and saves. |
| 2a | Port reactive fog readback to 1.0.0 | Verify the named reader and calling convention, then test Mario/Cappy interaction in Cap Kingdom on both versions. Preserve targeted readback; do not revive blanket pass-end writeback. |
| 2b | Investigate poison issue #3 | Reproduce Spewart in Wooded Kingdom on 1.0.0, identify the CPU/GPU paint reader and compare drawn poison with damage. Readback is a hypothesis until traced and tested, not an established shared root cause. |
| 3a | Windows packaging from a clean checkout | Build generator and declared FFmpeg/other dependencies without copying a prior release or a local scratch prefix. Verify DLL imports, licences, versions and absence of game files/keys in the ZIP. |
| 3b | GitHub Actions Windows ZIP and Linux AppImage | Pinned build inputs, clean runners, reproducible packaging steps and downloadable test artifacts. Game execution still requires privately supplied data; CI must not publish it. Linux builds must not require the Deck. |
| 4a | Shadow resolution, draw distance, LOD distance and effect distance | Separate validated controls, original defaults, bounded ranges, test visuals/performance across kingdoms and both game versions where affected. |
| 4b | Ultrawide polish | 21:9 and 32:9 HUD/menu placement, aspect ratio and cutscene bars, including switching back to 16:9. |
| 4c | VRR and monitor-matched frame cap (for example 144 Hz) | Validate swapchain/presentation and frame pacing on VRR hardware. A display cap must not change gameplay speed or silently imply 144 Hz simulation. |
| 5a | Steam Input layout with gyro and Deck icons | Test actual Deck controls and prompts; distribute the layout in an importable format. |
| 5b | Sleep/resume mid-game | Test audio, input/gyro, renderer, timing and save integrity on Deck hardware. |
| 5c | Add to Steam in Linux setup | Opt-in shortcut creation, correct AppImage/data paths, duplicate detection and rollback. |
| 5d | FSR 4.1 Lite investigation and Deck integration | Confirm the actual ML model/provider and API on Deck, frame cost and quality. Keep a supported fallback. See compatibility note below. |
| 6a | Separate camera sensitivity/inversion | Independent horizontal/vertical controls for stick, mouse and gyro; preserve existing defaults and sensor-settling fixes. |
| 6b | Two-player Cappy setup | Explicit second-controller assignment, player-one retention, disconnect/reconnect and menu navigation tests. |

Issue #4 (Iris Xe/8 GB startup crash) remains open; the crash-report tool must
help collect evidence on this hardware. Do not mark the crash fixed merely
because report collection is added. Existing language/audio/Linux/handheld
reports also remain separate from this queue unless explicitly verified.

## FSR Lite compatibility note

References supplied by Simon:

- https://github.com/Rayekkk/FSR4Injector
- https://github.com/db1522/fsr4-lite-steam-machine

At review, the first project's README says it targets DirectX 12 FSR 3.1 games,
explicitly excludes Vulkan, and lists Steam Deck/Van Gogh Lite support as beta
and untested. The second uses OptiScaler and demonstrates Steam Machine results;
those do not themselves verify this recomp's native Vulkan AppImage on Deck.
Investigate the model and current AMD API rather than install a DX12 injector
into the Vulkan renderer or label analytical FSR as Lite ML.

Real hardware remains necessary for unverified FSR 4, Intel/AMD XeSS, Deck sleep,
gyro and VRR paths. Record which checks are automated, locally observed or
confirmed by testers. Previous preview success does not establish full-game
coverage.
