# FSR 3.1 Vulkan preview (Windows)

Graphics > Temporal upscaler selects DLSS or FSR 3.1. FSR supports Off, Native AA,
Quality, Balanced and Performance, with live mode/backend switching. Each backend
keeps its own mode and sharpening settings. FSR sharpness controls the SDK's RCAS;
optional CAS is a separate pass and saved independently. Match display allows
the mode to choose its normal input resolution; a fixed internal resolution
overrides it, as with DLSS.

The implementation uses AMD's official signed Vulkan runtime from FSR SDK
v2.4.0, explicitly selecting the supported 3.1 provider (currently 3.1.6).
This is temporal reconstruction, not frame generation or FSR 4.

After building, stage the optional runtime beside the executable:

```powershell
powershell -ExecutionPolicy Bypass -File tools/fetch_fsr3_runtime.ps1 -Destination build
```

For an isolated build, use that executable's directory instead of `build`.
The script pins the revision and SHA-256 values, verifies AMD's signatures, and
copies the SDK licence. The DLLs are not tracked in Git. Missing runtimes or an
unsupported provider leave ordinary rendering active; the settings description
reports that FSR is unavailable. No runtime is downloaded by the game itself.

Example settings:

```ini
temporal_backend = fsr3
fsr3_mode = quality
fsr3_sharpness = 20
fsr3_cas = 0
fsr3_cas_sharpness = 20
```

The shared scene capture currently supplies camera-only motion vectors. Separate
object motion and reactive/composition masks are not yet available, and the
current reconstruction point includes the game's composed HUD. Fast independent
movement, particles and transparent effects need visual testing. This preview
has been exercised on the test machine's NVIDIA GPU; AMD/Intel hardware testing
is still needed before claiming support is verified on those devices.

Validation so far: 4K Quality at 120 FPS, Native AA/all quality modes, 0/100%
sharpening, optional CAS, Off, switching to DLSS and back, and startup without
the optional AMD DLLs. Capture and test logs are local artifacts, not game assets
to distribute.
