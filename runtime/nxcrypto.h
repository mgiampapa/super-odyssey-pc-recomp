// AES-128 (ECB blocks: the game package's CTR keystream, XTS, key unwrapping) and SHA-256 for runtime/nxpkg.c, the same
// on every OS: x86-64 uses the CPU's AES instructions (every CPU with AVX2 has them), others a portable version.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t enc[11][16];  // the key schedule (FIPS-197 order)
    uint8_t dec[11][16];  // the equivalent inverse cipher's (AES-NI aesdec order)
} NxAes;

void nx_aes_init(NxAes *k, const uint8_t key[16]);
// n bytes (a multiple of 16), in place allowed
void nx_aes_encrypt(const NxAes *k, const uint8_t *in, uint8_t *out, size_t n);
void nx_aes_decrypt(const NxAes *k, const uint8_t *in, uint8_t *out, size_t n);
void nx_sha256(const void *data, size_t n, uint8_t out[32]);
void nx_wipe(void *p, size_t n);  // zeroes key material (not optimized away)
bool nx_aes_hw(void);             // the CPU's AES instructions are used
