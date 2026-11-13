#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "nvn/linear_texture.h"
#include "nvn/tegra.h"

int main(void) {
    assert(nvn_flags_linear(0x10) && nvn_flags_linear(0x40) && nvn_flags_linear(0x50));
    assert(!nvn_flags_linear(0) && !nvn_flags_linear(0x20) && !nvn_flags_linear(8));
    assert(nvn_linear_pitch(0x40, 76, 0) == 128);
    assert(nvn_linear_pitch(0x10, 76, 0) == 96);
    assert(nvn_linear_pitch(0x40, 76, 192) == 192);
    assert(nvn_linear_pitch(0x40, 1026, 0) == 1152);
    assert(nvn_linear_pitch(0x10, 1026, 0) == 1056);
    assert(nvn_linear_pitch(0x40, 64, 0) * 16 == 2048);
    assert(nvn_linear_pitch(0x40, 76, 0) * 19 == 2432);

    // A nonuniform RGBA image with padded rows: the old flag test sent these
    // bytes through the GOB decoder, changing which pixel appeared at (x,y).
    uint8_t src[16384], tight[19 * 19 * 4 + 16], gob[19 * 19 * 4];
    memset(src, 0xcd, sizeof src);
    memset(tight, 0xee, sizeof tight);
    for (unsigned y = 0; y < 19; y++) for (unsigned x = 0; x < 19; x++) {
        uint8_t *p = src + y * 128 + x * 4;
        p[0] = x; p[1] = y; p[2] = x ^ y; p[3] = (x + y) & 1 ? 255 : 0;
    }
    assert(nvn_linear_copy_rows(tight + 8, sizeof gob, src, 2432, 128, 76, 19));
    for (unsigned y = 0; y < 19; y++) for (unsigned x = 0; x < 19; x++)
        assert(!memcmp(tight + 8 + (y * 19 + x) * 4, src + y * 128 + x * 4, 4));
    for (int i = 0; i < 8; i++) assert(tight[i] == 0xee && tight[sizeof tight - 1 - i] == 0xee);
    TexLayout t = {.width = 19, .height = 19, .depth = 1, .layers = 1, .levels = 1, .fmt = nvn_format_info(0x25)};
    tegra_layout_init(&t);
    tegra_deswizzle(src, gob, 19, 19, 1, 4, t.block_height_log2, t.block_depth_log2);
    assert(memcmp(gob, tight + 8, sizeof gob));

    uint8_t restored[2432]; memset(restored, 0x7b, sizeof restored);
    assert(nvn_linear_store_rows(restored, sizeof restored, 128, tight + 8, sizeof gob, 76, 19));
    for (unsigned y = 0; y < 19; y++) {
        assert(!memcmp(restored + y * 128, src + y * 128, 76));
        for (unsigned x = 76; x < 128; x++) assert(restored[y * 128 + x] == 0x7b);
    }
    uint8_t saved[sizeof restored]; memcpy(saved, restored, sizeof saved);
    assert(!nvn_linear_store_rows(restored, 2304, 128, tight + 8, sizeof gob, 76, 19));
    assert(!nvn_linear_store_rows(restored, sizeof restored, 128, tight + 8, 100, 76, 19));
    assert(!nvn_linear_store_rows(restored, sizeof restored, UINT64_MAX, tight + 8, sizeof gob, 76, 19));
    assert(!memcmp(saved, restored, sizeof restored));

    uint8_t untouched[sizeof tight]; memcpy(untouched, tight, sizeof tight);
    assert(!nvn_linear_copy_rows(tight, sizeof tight, src, 2432, 64, 76, 19));
    assert(!nvn_linear_copy_rows(tight, sizeof tight, src, 2304, 128, 76, 19));
    assert(!nvn_linear_copy_rows(tight, 100, src, 2432, 128, 76, 19));
    assert(!nvn_linear_copy_rows(tight, sizeof tight, src, sizeof src, UINT64_MAX, 76, 19));
    assert(!memcmp(untouched, tight, sizeof tight));
    puts("linear texture flags, pitch, row layout and bounds: PASS");
}
