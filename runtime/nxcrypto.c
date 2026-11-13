// AES-128 and SHA-256 (nxcrypto.h). Written from FIPS-197 and FIPS-180-4; the AES-NI path follows Intel's AES-NI white
// paper (the key schedule is the standard one, so both paths share it).
#include "nxcrypto.h"

#include <string.h>

#if (defined(__x86_64__) || defined(_M_X64)) && !defined(NX_NO_HW)  // (NX_NO_HW: test the portable path)
#include <cpuid.h>
#include <wmmintrin.h>
#define NX_X86 1
#endif

static const uint8_t SBOX[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d,
    0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc,
    0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2,
    0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf, 0xd0, 0xef, 0xaa, 0xfb,
    0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
    0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d,
    0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d,
    0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08, 0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6,
    0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9,
    0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};
static uint8_t INV[256];  // the inverse S-box, made from SBOX once

static uint8_t xt(uint8_t a) { return (uint8_t)(a << 1 ^ (a & 0x80 ? 0x1b : 0)); }
static uint8_t mul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    for (; b; b >>= 1, a = xt(a))
        if (b & 1) r ^= a;
    return r;
}

#ifdef NX_X86
static int g_hw = -1;
bool nx_aes_hw(void) {
    if (g_hw < 0) {
        unsigned a, b, c, d;
        g_hw = __get_cpuid(1, &a, &b, &c, &d) && (c & bit_AES) && (d & bit_SSE2);
    }
    return g_hw;
}
__attribute__((target("aes,sse2"))) static void hw_crypt(const NxAes *k, bool enc, const uint8_t *in, uint8_t *out, size_t n) {
    __m128i r[11];
    for (int i = 0; i < 11; i++) r[i] = _mm_loadu_si128((const __m128i *)(enc ? k->enc[i] : k->dec[i]));
    size_t i = 0;
    for (; i + 64 <= n; i += 64) {  // four blocks at a time (the instructions pipeline)
        __m128i a = _mm_xor_si128(_mm_loadu_si128((const __m128i *)(in + i)), r[0]);
        __m128i b = _mm_xor_si128(_mm_loadu_si128((const __m128i *)(in + i + 16)), r[0]);
        __m128i c = _mm_xor_si128(_mm_loadu_si128((const __m128i *)(in + i + 32)), r[0]);
        __m128i d = _mm_xor_si128(_mm_loadu_si128((const __m128i *)(in + i + 48)), r[0]);
        for (int j = 1; j < 10; j++) {
            if (enc) a = _mm_aesenc_si128(a, r[j]), b = _mm_aesenc_si128(b, r[j]), c = _mm_aesenc_si128(c, r[j]), d = _mm_aesenc_si128(d, r[j]);
            else a = _mm_aesdec_si128(a, r[j]), b = _mm_aesdec_si128(b, r[j]), c = _mm_aesdec_si128(c, r[j]), d = _mm_aesdec_si128(d, r[j]);
        }
        if (enc) a = _mm_aesenclast_si128(a, r[10]), b = _mm_aesenclast_si128(b, r[10]), c = _mm_aesenclast_si128(c, r[10]), d = _mm_aesenclast_si128(d, r[10]);
        else a = _mm_aesdeclast_si128(a, r[10]), b = _mm_aesdeclast_si128(b, r[10]), c = _mm_aesdeclast_si128(c, r[10]), d = _mm_aesdeclast_si128(d, r[10]);
        _mm_storeu_si128((__m128i *)(out + i), a), _mm_storeu_si128((__m128i *)(out + i + 16), b);
        _mm_storeu_si128((__m128i *)(out + i + 32), c), _mm_storeu_si128((__m128i *)(out + i + 48), d);
    }
    for (; i < n; i += 16) {
        __m128i a = _mm_xor_si128(_mm_loadu_si128((const __m128i *)(in + i)), r[0]);
        for (int j = 1; j < 10; j++) a = enc ? _mm_aesenc_si128(a, r[j]) : _mm_aesdec_si128(a, r[j]);
        a = enc ? _mm_aesenclast_si128(a, r[10]) : _mm_aesdeclast_si128(a, r[10]);
        _mm_storeu_si128((__m128i *)(out + i), a);
    }
    for (int j = 0; j < 11; j++) r[j] = _mm_setzero_si128();
}
#else
bool nx_aes_hw(void) { return false; }
#endif

void nx_aes_init(NxAes *k, const uint8_t key[16]) {
    if (!INV[SBOX[1]]) for (int i = 0; i < 256; i++) INV[SBOX[i]] = (uint8_t)i;
    memcpy(k->enc[0], key, 16);
    uint8_t rcon = 1;
    for (int r = 1; r < 11; r++) {
        const uint8_t *p = k->enc[r - 1];
        uint8_t *q = k->enc[r];
        uint8_t t[4] = {(uint8_t)(SBOX[p[13]] ^ rcon), SBOX[p[14]], SBOX[p[15]], SBOX[p[12]]};
        for (int i = 0; i < 4; i++) q[i] = p[i] ^ t[i];
        for (int i = 4; i < 16; i++) q[i] = p[i] ^ q[i - 4];
        rcon = xt(rcon);
    }
    // the equivalent inverse cipher's schedule: reversed, InvMixColumns on the inner rounds
    memcpy(k->dec[0], k->enc[10], 16);
    memcpy(k->dec[10], k->enc[0], 16);
    for (int r = 1; r < 10; r++) {
        const uint8_t *s = k->enc[10 - r];
        for (int c = 0; c < 4; c++) {
            const uint8_t *a = s + c * 4;
            uint8_t *o = k->dec[r] + c * 4;
            o[0] = mul(a[0], 14) ^ mul(a[1], 11) ^ mul(a[2], 13) ^ mul(a[3], 9);
            o[1] = mul(a[0], 9) ^ mul(a[1], 14) ^ mul(a[2], 11) ^ mul(a[3], 13);
            o[2] = mul(a[0], 13) ^ mul(a[1], 9) ^ mul(a[2], 14) ^ mul(a[3], 11);
            o[3] = mul(a[0], 11) ^ mul(a[1], 13) ^ mul(a[2], 9) ^ mul(a[3], 14);
        }
    }
}

// ---- portable (FIPS-197 as written; slow, for CPUs without AES instructions)
static void sw_block(const NxAes *k, bool enc, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16], t[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ (enc ? k->enc[0][i] : k->enc[10][i]);
    for (int r = 1; r <= 10; r++) {
        if (enc) {
            for (int i = 0; i < 16; i++) t[i] = SBOX[s[(i + 4 * (i % 4)) % 16]];  // SubBytes + ShiftRows
            if (r < 10)
                for (int c = 0; c < 4; c++) {  // MixColumns
                    uint8_t *a = t + c * 4, a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
                    a[0] = xt(a0) ^ xt(a1) ^ a1 ^ a2 ^ a3;
                    a[1] = a0 ^ xt(a1) ^ xt(a2) ^ a2 ^ a3;
                    a[2] = a0 ^ a1 ^ xt(a2) ^ xt(a3) ^ a3;
                    a[3] = xt(a0) ^ a0 ^ a1 ^ a2 ^ xt(a3);
                }
            for (int i = 0; i < 16; i++) s[i] = t[i] ^ k->enc[r][i];
        } else {
            for (int i = 0; i < 16; i++) t[(i + 4 * (i % 4)) % 16] = INV[s[i]];  // InvShiftRows + InvSubBytes
            for (int i = 0; i < 16; i++) t[i] ^= k->enc[10 - r][i];
            if (r < 10)
                for (int c = 0; c < 4; c++) {  // InvMixColumns
                    uint8_t *a = t + c * 4, a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
                    a[0] = mul(a0, 14) ^ mul(a1, 11) ^ mul(a2, 13) ^ mul(a3, 9);
                    a[1] = mul(a0, 9) ^ mul(a1, 14) ^ mul(a2, 11) ^ mul(a3, 13);
                    a[2] = mul(a0, 13) ^ mul(a1, 9) ^ mul(a2, 14) ^ mul(a3, 11);
                    a[3] = mul(a0, 11) ^ mul(a1, 13) ^ mul(a2, 9) ^ mul(a3, 14);
                }
            memcpy(s, t, 16);
        }
    }
    memcpy(out, s, 16);
    nx_wipe(s, 16), nx_wipe(t, 16);
}

void nx_aes_encrypt(const NxAes *k, const uint8_t *in, uint8_t *out, size_t n) {
#ifdef NX_X86
    if (nx_aes_hw()) { hw_crypt(k, true, in, out, n); return; }
#endif
    for (size_t i = 0; i < n; i += 16) sw_block(k, true, in + i, out + i);
}
void nx_aes_decrypt(const NxAes *k, const uint8_t *in, uint8_t *out, size_t n) {
#ifdef NX_X86
    if (nx_aes_hw()) { hw_crypt(k, false, in, out, n); return; }
#endif
    for (size_t i = 0; i < n; i += 16) sw_block(k, false, in + i, out + i);
}

void nx_wipe(void *p, size_t n) {
    volatile uint8_t *v = p;
    while (n--) *v++ = 0;
}

// ---- SHA-256
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))

static void sha_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
}

void nx_sha256(const void *data, size_t n, uint8_t out[32]) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const uint8_t *p = data;
    size_t full = n / 64 * 64;
    for (size_t i = 0; i < full; i += 64) sha_block(h, p + i);
    uint8_t tail[128] = {0};
    size_t r = n - full;
    memcpy(tail, p + full, r);
    tail[r] = 0x80;
    size_t tl = r < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) tail[tl - 1 - i] = (uint8_t)(bits >> (8 * i));
    sha_block(h, tail);
    if (tl == 128) sha_block(h, tail + 64);
    for (int i = 0; i < 8; i++) out[i * 4] = (uint8_t)(h[i] >> 24), out[i * 4 + 1] = (uint8_t)(h[i] >> 16), out[i * 4 + 2] = (uint8_t)(h[i] >> 8), out[i * 4 + 3] = (uint8_t)h[i];
}
