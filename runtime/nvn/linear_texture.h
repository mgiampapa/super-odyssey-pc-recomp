// NVN pitch-linear storage. LINEAR_RENDER_TARGET selects this layout on its own;
// a pitch field without either flag does not change block-linear storage.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static inline bool nvn_flags_linear(uint32_t flags) { return (flags & 0x50u) != 0; }
static inline uint64_t nvn_linear_alignment(uint32_t flags) { return (flags & 0x40u) ? 128 : 32; }
static inline uint64_t nvn_linear_pitch(uint32_t flags, uint64_t row_bytes, uint64_t stride) {
    uint64_t alignment = nvn_linear_alignment(flags);
    return stride ? stride : (row_bytes + alignment - 1) & ~(alignment - 1);
}

static inline bool nvn_linear_rows_fit(uint64_t capacity, uint64_t pitch, uint64_t row_bytes, uint64_t rows) {
    return row_bytes && rows && pitch >= row_bytes && row_bytes <= capacity &&
        (rows == 1 || pitch <= (capacity - row_bytes) / (rows - 1));
}
static inline bool nvn_linear_copy_rows(void *dst, uint64_t dst_size, const void *src, uint64_t src_size,
                                       uint64_t pitch, uint64_t row_bytes, uint64_t rows) {
    if (!nvn_linear_rows_fit(dst_size, row_bytes, row_bytes, rows) ||
        !nvn_linear_rows_fit(src_size, pitch, row_bytes, rows)) return false;
    for (uint64_t y = 0; y < rows; y++)
        memcpy((uint8_t *)dst + y * row_bytes, (const uint8_t *)src + y * pitch, row_bytes);
    return true;
}

static inline bool nvn_linear_store_rows(void *dst, uint64_t dst_size, uint64_t pitch,
                                        const void *src, uint64_t src_size, uint64_t row_bytes, uint64_t rows) {
    if (!nvn_linear_rows_fit(dst_size, pitch, row_bytes, rows) ||
        !nvn_linear_rows_fit(src_size, row_bytes, row_bytes, rows)) return false;
    for (uint64_t y = 0; y < rows; y++)
        memcpy((uint8_t *)dst + y * pitch, (const uint8_t *)src + y * row_bytes, row_bytes);
    return true;
}
