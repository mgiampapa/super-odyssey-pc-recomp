// Keyboard and mouse button prompts (runtime/nvn/kbprompts.c): while the player uses the keyboard / mouse, the game's
// button icons (its DeviceFont80 glyph sheet) show key caps with the keys bound in the Controls tab, WASD for the left
// stick and a mouse for the camera stick. The Vulkan texture upload (vulkan/textures.c) asks for them.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// (and the Xbox / PlayStation controller icons, drawn the same way)
// The keyboard icons have been wanted at all this run (the keyboard / mouse used, or the setting): until then nothing
// here runs - no texture is looked at or uploaded again - so a controller player's game is as it was without them
bool kbprompts_active(void);
// A texture of w x h texels in 4x4 blocks of bpb bytes could be the button icon sheet (its shape)
bool kbprompts_candidate(uint32_t w, uint32_t h, uint32_t bw, uint32_t bh, uint32_t bpb);
// Such a texture's level 0, deswizzled to rows of blocks (wb x hb), is the sheet (compared with the game's font file)
bool kbprompts_identify(const uint8_t *blocks, uint32_t wb, uint32_t hb);
// What the icons show now: changes when the player switches between keyboard and controller or rebinds a key
// (an identified texture is uploaded again then)
uint32_t kbprompts_generation(void);
// Level lv of the sheet, deswizzled to rows of BC4 blocks (wb x hb): the keyboard icons drawn in, when they are due
void kbprompts_apply(uint32_t lv, uint8_t *blocks, uint32_t wb, uint32_t hb);
// The input device the player used last (runtime/hle/input.c): true keyboard / mouse, false controller
void kbprompts_note_device(bool keyboard);

#ifdef __cplusplus
}
#endif
