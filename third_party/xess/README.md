# Intel XeSS Vulkan API

API headers and notices from the reviewed Intel XeSS SDK snapshot
`de0fb9c1c510661c571164e1418ceca8101dab69`:
https://github.com/intel/xess/tree/de0fb9c1c510661c571164e1418ceca8101dab69

The headers carry Intel's MIT notice (`LICENSE-HEADERS.txt`). Only whitespace is
normalized. The optional `libxess.dll` is covered by `LICENSE-RUNTIME.txt` and
`third-party-programs.txt`; it is not committed. This SDK snapshot's runtime
reports XeSS-SR 2.0.2. This integration does not include XeSS frame generation.

Stage the signed runtime beside a built executable:

```powershell
powershell -ExecutionPolicy Bypass -File tools/fetch_xess_runtime.ps1 -Destination build
```

The script pins the revision and SHA-256, verifies Intel's Authenticode signature,
and includes the binary notices. No runtime is downloaded by the game. Missing
libraries or unsupported Vulkan requirements leave ordinary rendering active.
