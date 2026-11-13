# Reactive fog CPU readback

Cap Kingdom's fog height queries sample GPU-rendered half-float textures on the
CPU. Without readback, the guest sees stale zeroes, computes an incorrect surface
height and rejects Mario/Cappy's ripple requests. The existing fix intercepts
the reader's NVN MemoryPoolMap call and obtains the requested texel on the render
thread. It copies only that validated halfword into guest memory, with bounded
per-present image caching. Blanket pass-end writeback remains disabled.

## Supported callers

The hook now checks both the loaded main build ID and the return address.
Unknown builds and unrelated MemoryPoolMap callers do not enter the hook.

| Game | Main build ID prefix | F16 reader | MemoryPoolMap return PC |
| --- | --- | --- | --- |
| 1.0.0 | `3ca12dfaaf9c82da` | `0x7000a2b81c` | `0x7000a2b86c` |
| 1.4.1 | `b424be150a8e7d78` | `0x70007ac5f0` | `0x70007ac640` |

The named 1.0.0 symbol is
`_ZN2al26getF32FromLinearTextureF16EPKN3agl11TextureDataEiiii`.
Disassembly of the owner's two extracted main files verifies that both callers
keep component count, component, y, x, pool offset and row elements in x19..x24.
Immediately after `blr x10`, both execute the same 32-bit madd/mul, add the
unsigned pool offset and sign-extended index, then load a halfword with the
signed component index. The existing byte calculation therefore applies to both.
Register assignment and post-call instruction assertions passed on both files.

## Private validation (13 November 2026)

The 1.0.0 game-code library was regenerated from the owner's main/subsdk0/sdk
with the current generator and compiled against the current runtime headers.
Its library ID matches the app (`03e9978a3ce7c0a5`); no compatibility check was
bypassed. Existing extracted assets are read by absolute path, with workspace
settings, caches and copied Cap saves. No production files are changed.

Tests use stock 60 FPS, temporal upscalers off, 1280x720, scripted forward
movement and two Cappy throws, then capture at frame 2700. The 1.4.1 regression
and 1.0.0 readback-on runs exited 0. Both recorded valid, nonzero half-float
samples and local cleared ground fog around Mario. In 1.0.0, the initial depth
sample at (257,390), component 0, is `0x384f` with readback enabled; the
readback-off control recorded zero for every one of its 68 logged samples.

A follow-up trace checks behavior rather than inferring interaction from the
fog's appearance in one still. With readback on, the 1.0.0 surface query under
Mario at (200,-900,29050) returned approximately -1676, and ripple request 800
at (11.8125,-1050,26884.5) returned accepted (`w0=1`). With readback off the
surface stayed at +1600; the traced Mario request 800 was rejected (`w0=0`,
height difference 2500). Call numbers are not synchronized between the cases,
since camera ripples also enter the function. The readback-off still can show
a camera-cleared patch and must not be treated as evidence of working Mario
interaction.

Artifacts are private under `build/fog-v100/`: matching library, `run-probe.ps1`,
version-labelled logs/screenshots and `Test Fog 1.0.0.cmd`. Simon's manual
interaction confirmation is still pending.
This is targeted support for these two builds, not general NVN coherence,
all-game graphics coverage or a fix for the separate poison report. The macOS
renderer still does not implement this Vulkan readback path. Long-session and
120 FPS fog tests remain separate from these bounded checks.

`ODYSSEY_FOG_CPU_READBACK=0` disables the fix; `ODYSSEY_LOG_LINREAD=1` logs sampled
indices/halfwords. `ODYSSEY_LINEAR_WRITEBACK` must remain off for this test.
