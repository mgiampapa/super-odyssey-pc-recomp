# FSR 4 and XeSS beta (Windows Vulkan)

Graphics > Temporal upscaler now includes AMD FSR 4 (Beta) and Intel XeSS
(Beta), alongside DLSS and FSR 3.1. Each backend saves its own mode, reconstruction
sharpness and optional CAS settings. Off, Native AA, Quality, Balanced and
Performance can be changed during play. Context rebuilding can briefly pause
the game when switching; a restart is not required.

## Runtime setup

Build the executable normally, then stage the optional signed runtimes next to
it (replace `build` with the executable's directory):

```powershell
powershell -ExecutionPolicy Bypass -File tools/fetch_fsr3_runtime.ps1 -Destination build
powershell -ExecutionPolicy Bypass -File tools/fetch_xess_runtime.ps1 -Destination build
```

The AMD script supplies both FSR 3.1 and FSR 4 providers from SDK 2.4.0. The Intel
script supplies XeSS-SR 2.0.2 from the pinned XeSS SDK snapshot. Both scripts
verify SHA-256 hashes and vendor signatures and copy redistribution notices.
No DLL is downloaded by the game. These libraries remain optional. Intel's
runtime may also require the Microsoft Visual C++ x64 runtime.

After staging and committing the source, `tools/package_upscaler_beta.ps1`
creates an allowlisted runtime overlay ZIP with notices, hashes, instructions
and the source revision. Its default build directory is
`build/fsr4-xess-preview`. It excludes game assets, keys, saves, generated game
libraries and NVIDIA's separately supplied runtime. Use the included launcher
with the path to a **copied** working game-data directory; it selects separate
settings and saves. The overlay still needs the working install's compatible
SDL/shaderc/FFmpeg/compiler runtime dependencies.

FSR 4 queries the AMD runtime's supported providers for the selected GPU and
explicitly requests a 4.x ML provider. The menu's **Upscaler status** must say
**FSR 4 ML**, and the log must contain `[fsr4] context created` with a 4.x
provider, to count as an ML test. On unsupported hardware or ML context failure,
the game explicitly reports **FSR 3.1 fallback** and uses the analytical provider
when available. It does not relabel that output as ML. If neither provider is
available, ordinary rendering remains active.

XeSS checks required instance/device extensions and features before Vulkan
device creation. Unsupported requirements or a missing runtime leave ordinary
rendering active. Successful initialization logs `[xess] context created`.
XeSS can run on compatible non-Intel GPUs; the path used depends on the device.
The game itself requires Vulkan 1.3 and its existing renderer features.

## Integration and limits

FSR uses the SDK's sharpening. XeSS uses the same float-precision external RCAS
pass as DLSS, clearly named in the menu; optional CAS is independent. Strengths
and toggles remain separate for every backend. XeSS receives linearized SDR
colour and converts its result back into the game's display-referred pipeline.
FSR receives the SDK's explicit nonlinear/sRGB flags.

Match display lets the selected quality mode choose its input resolution. A
fixed internal resolution overrides it. XeSS uses its current SDK quality
ratios, which differ from FSR's. This is super resolution/antialiasing, with
no frame generation in this change.

A remaining beta issue: several title-screen menu captures with FSR 4 selected
and the FSR 3.1 fallback active showed partially missing text, at both 60 and
120 FPS. It also occurred without the XeSS DLL. The comparable FSR 3.1-selected,
XeSS-selected and missing-runtime captures were clean. No ML provider executed
in those captures, so this does not establish an FSR 4 ML rendering fault. The
cause is unresolved; use FSR 3.1 directly if it occurs and include a capture in
the report. The beta is not promoted to the main build on this evidence.

The shared capture still supplies camera-only motion vectors, without separate
object motion or reactive/composition masks. Reconstruction currently includes
the composed HUD. Fast objects, particles, transparencies and HUD text therefore
need continued visual review. This beta does not claim perfect temporal quality
or full-game coverage.

## Validation and community checklist

Local NVIDIA testing passed: 4K XeSS Quality, Native AA/Balanced/Performance,
0/100% sharpening, CAS, Off, and live switches through FSR 4 fallback, DLSS,
FSR 3.1 and back. A 5,800-frame mixed-backend run exited normally. The final
GUI build and missing-runtime fallback are checked separately. Actual FSR 4 ML
execution still needs compatible AMD hardware; Intel and AMD XeSS paths also
need testers. Local NVIDIA fallback testing does not verify those paths.

Use a separate copy of a working installation. Keep its compatible dependency
DLLs and game data. An overlay is not a standalone installer and contains no
game assets, keys or saves. Do not overwrite the only copy of your installation.

For each available backend:

1. Record GPU, driver, CPU, Windows version, display resolution and game version.
2. Confirm the active status and context-created log before judging the result.
3. Try Native AA, Quality, Balanced, Performance and Off during play. Leave
   internal resolution on Match display for this comparison.
4. Try sharpness 0/100 and CAS off/on; switch to a different backend and back
   and check that each one's settings remain independent.
5. Rotate the camera near Cap Kingdom fog, Cascade waterfall mist, distant
   fences, Mario's hair, moving enemies and transparent effects. Check HUD and
   menus, save loading and a controller held still.
6. Report crashes, ghosting, colour changes, flicker and performance differences
   with the area/steps, settings file and `SuperOdysseyRecomp.log` next to the
   executable. Logs can contain local paths; review them before sharing. Do not
   attach keys, game data or saves to a public report.

An `FSR 3.1 fallback` result is useful compatibility feedback, but is not an
FSR 4 ML quality/performance result.
