# 1.0.0 poison collision readback preview, revision 2

Issue [#3](https://github.com/Simon3Sarkis/super-odyssey-pc-recomp/issues/3)
reports damage from invisible poison during the Wooded Kingdom Spewart fight
on game 1.0.0 / GTX 1060. The supplied save and log were used locally with a
freshly generated, compatible 1.0.0 game-code library. No report or save was
uploaded, and no production install or saves were changed.

## Evidence and candidate

`PaintObj::checkPaintAlpha` calls
`al::getColorFromLinearTexture(TextureData,x,y)` and tests the returned alpha's
high bit. The reader packs RGBA8 bytes into a color; its CPU memory pool was not
receiving the corresponding GPU render-target contents. However, this is not
the only CPU reader: `PaintObj::attackSensor` directly samples rows covering a
player's footprint. The first candidate missed that path and Simon confirmed
the issue persisted. Its successful single-texel comparisons were insufficient
to establish a fix.

The private Spewart run observed a native 128x128 RGBA8 (`0x25`) linear render
target, pitch 512. At (64,64), CPU memory contained `0x91890005` while the GPU
image contained `0x00000000`. Rendering into this exact texture was also logged;
this was not merely a comparison against an unrelated allocated image. Across
later queries CPU and GPU values continued to differ at clear mask samples.

Revision 2 also synchronizes the separate 372x372 RGBA8 damage mask (pitch 1536)
before `attackSensor` accesses its rows. Paint labeling consumes two additional
128x128 buffers and can edit them on the CPU. Both are synchronized before the
labeling loop; its inner per-pixel Map calls are left alone so they do not
overwrite edits in progress. The verified median-filter Map site uses the same
bounded snapshot helper, although that branch was not observed in these runs.

GPU data goes into private staging, then a bounded cache and a caller-owned
snapshot. The game thread copies rows into its requested allocation after
checking pitch, dimensions and pool bounds. The per-present GPU cache is reused
for footprint reads; labeling/filter snapshots explicitly refresh it after GPU
drawing within a present. Only native, single-layer, single-sample, GPU-owned
RGBA8 images with known linear storage qualify for bulk snapshots. Unknown
builds, unsupported formats and unregistered textures retain their behavior.
Blanket pass-end writeback remains off.

An explicit CPU flush over a fully synchronized paint texture then marks its
CPU contents for upload. This runs in queue order on the render thread, waits
for preceding work, invalidates the texture-view memo and paint readback cache,
and forces an upload at its next use, including render-target use. It does not
change ownership for arbitrary textures or vertex buffers.

The shared layout correction recognizes `LINEAR_RENDER_TARGET` (`0x40`) as
pitch-linear storage independently of `LINEAR` (`0x10`). Previously those rows
could be treated as GOB-swizzled data. Builder/object storage size, alignment,
implicit pitch, Vulkan row upload and CPU-upload checks now agree. Linear render
targets use 128-byte implicit row alignment, ordinary linear textures 32 bytes;
explicit pitches are preserved. A stride field alone does not make a texture
linear. These textures are excluded from scene-resolution scaling. This behavior
is also documented by the reference renderer's
[linear-target storage checks](https://github.com/seedds/nxnative/blob/main/docs/reference/nvn-objects.md).

Paint readback is confined to main build ID `3ca12dfaaf9c82da`. Verified
MemoryPoolMap return PCs and preserved inputs:

| Reader | Return PC | TextureData input |
| --- | --- | --- |
| Color texel | `0x7000a2b7cc` | x21; x/y in w20/w19, pool offset w22 |
| Damage footprint | `0x70002da5e0` | x21; pool offset w22 |
| Labeling entry | `0x70002d99a0` | actor x26, buffers at +0x79a8 / +0x7968 |
| Median filter | `0x70002dc49c` | x22; pool offset w19 |

Disassembly of the owner's 1.0.0 file verified these layouts and Map calls.
Pitch comes from TextureData+0x4c. The point hook preserves the reader's 32-bit
indexing. Existing F16 requests retain their two-byte interpretation.

Simon confirmed revision 2 completely fixed the issue. The verified 1.0.0 paint
paths are now **enabled by default**; `ODYSSEY_PAINT_CPU_READBACK=0` disables them
for comparison. The independent linear-layout correction
applies normally. `ODYSSEY_LOG_PAINT_READS=1` enables sampled CPU/GPU comparison
logs without itself changing CPU bytes.

## Checks and limits

- Windows build passed. `tools/test_linear_texture.c` exercises 0x10/0x40 flags,
  implicit/explicit pitches, nonuniform padded RGBA rows (including a case where
  the old GOB path changes pixels), round-trip row storage, untouched padding and
  rejected short/overflowing spans without partial writes.
- Bounded RTX 4090 runs at 3840x2160 exercised damage-mask snapshots, both label
  snapshots and CPU-edit flush/upload. With otherwise identical scripted input,
  the first candidate ended at 1 HP on clean ground; revision 2 ended at 3 HP.
  A separate forward-walk control entered visible poison and showed a purple
  damage reaction and 2 HP. Poison damage is preserved.
- The 1.4.1 Cap Kingdom regression also exited normally with valid nonzero F16
  samples. Its existing fog hook remains enabled and its copy size remains two
  bytes. The approved fog-preview executable was preserved.
- These controls support the specific clean-ground/damage correction; they do
  not establish every interaction in the fight. Two earlier attempted edge
  placements landed outside the arena and respawned; discard those as health
  comparisons.
- Manual checks must include crossing ground after clearing it with Cappy,
  taking damage from actual visible poison, and completing/retrying the fight.
  Simon confirmed the issue completely fixed in his manual test. The original
  reporter's GTX 1060 and other
  hardware, long sessions, experimental 120 FPS, other paint effects and Linux
  remain unverified. The Metal renderer does not implement this readback path.
- Only the verified reader/filter paths are covered. It is not a general NVN
  memory-coherence implementation or proof that every poison effect is fixed.

## Private reproduction

Artifacts are under `build/poison-review/` and must not be committed: reporter
save/log, game-derived placement JSON, settings, per-case copied saves, logs and
captures. The test helper `ODYSSEY_TEST_PLAYER_POS=2400:-4350:6800:-20400` moves
Mario near Spewart after frame 2400 on 1.0.0. It calls the game's
PlayerPuppet::resetPosition rather than changing simulation timing or leaving
stale custom collision state. It is inactive without the explicit variable.
`boss002` is the later flower-field entrance, not Spewart's arena.

`build/poison-layout/Test Poison Fix v2.cmd` enables the candidate, uses a copied reporter save and
private settings/cache, and allows normal controls. Resume, wait for the test
placement if necessary, advance/skip the Broodal introduction, and test the
fight. Restart the launcher to repeat the one-time placement after a death.
