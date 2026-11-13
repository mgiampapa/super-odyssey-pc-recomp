// Known-answer test of runtime/nxcrypto.c: FIPS-197 C.1 (AES-128), FIPS-180-4 "abc" and a two-block message (SHA-256),
// plus a 4096-byte round trip (the four-blocks-at-a-time path). Build with and without -DNX_NO_HW:
//   cc -O2 -Iruntime tools/test_nxcrypto.c runtime/nxcrypto.c -o build/test_nxcrypto && build/test_nxcrypto
#include <stdio.h>
#include <string.h>

#include "nxcrypto.h"

static int hex(const char *s, uint8_t *o) {
    int n = 0;
    for (; s[0] && s[1]; s += 2) { unsigned v; sscanf(s, "%2x", &v); o[n++] = (uint8_t)v; }
    return n;
}

int main(void) {
    int bad = 0;
    uint8_t key[16], pt[16], ct[16], out[16], want[32];
    hex("000102030405060708090a0b0c0d0e0f", key);
    hex("00112233445566778899aabbccddeeff", pt);
    hex("69c4e0d86a7b0430d8cdb78070b4c55a", ct);
    NxAes k;
    nx_aes_init(&k, key);
    nx_aes_encrypt(&k, pt, out, 16);
    if (memcmp(out, ct, 16)) printf("AES encrypt: wrong\n"), bad++;
    nx_aes_decrypt(&k, ct, out, 16);
    if (memcmp(out, pt, 16)) printf("AES decrypt: wrong\n"), bad++;
    static uint8_t big[4096], enc[4096], dec[4096];
    for (int i = 0; i < 4096; i++) big[i] = (uint8_t)(i * 7 + 3);
    nx_aes_encrypt(&k, big, enc, sizeof big);
    for (int i = 0; i < 4096; i += 16) {  // each block as encrypting it alone gives
        nx_aes_encrypt(&k, big + i, out, 16);
        if (memcmp(out, enc + i, 16)) { printf("AES bulk: block %d differs\n", i / 16); bad++; break; }
    }
    nx_aes_decrypt(&k, enc, dec, sizeof enc);
    if (memcmp(dec, big, sizeof big)) printf("AES bulk round trip: wrong\n"), bad++;
    uint8_t h[32];
    nx_sha256("abc", 3, h);
    hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", want);
    if (memcmp(h, want, 32)) printf("SHA-256 abc: wrong\n"), bad++;
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    nx_sha256(m, strlen(m), h);
    hex("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", want);
    if (memcmp(h, want, 32)) printf("SHA-256 two blocks: wrong\n"), bad++;
    printf("%s (AES instructions: %s)\n", bad ? "FAILED" : "all known answers match", nx_aes_hw() ? "yes" : "no");
    return bad != 0;
}
