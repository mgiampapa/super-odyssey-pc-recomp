// The player's game package read in place (nxpkg.h): NSP (PFS0) or XCI (HFS0 secure partition) -> the base game's
// Program NCA -> its ExeFS (PFS0, block hashes) and RomFS (IVFC, six hash levels). Decryption with the player's own
// prod.keys: the NCA header (AES-XTS, header_key), the section key from the key area (key_area_key_application_XX) or a
// common ticket in the package (titlekek_XX), the sections (AES-CTR). A C port of MacNativeGames' extract_exefs.py and
// extract_romfs.py (MIT, tools/vendor/macnativegames). AES and SHA-256: runtime/nxcrypto.c (the same on every OS). File
// names come in as UTF-8 (Windows: wide-character calls). Key values stay in this process's memory, are wiped on close
// and never logged.
#include "nxpkg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "nxcrypto.h"
#include "rt.h"

#define BASE_TITLE 0x0100000000010000ull
#define UPDATE_TITLE 0x0100000000010800ull
#define CHUNK (8u << 20)

#define FAIL(...) do { if (why && why_size) snprintf(why, why_size, __VA_ARGS__); goto fail; } while (0)

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

// ------------------------------------------------------------------ files (UTF-8 names)
#ifdef _WIN32
typedef HANDLE Fh;
#define FH_NONE INVALID_HANDLE_VALUE
#define strcmp_nocase _stricmp
static bool fh_close(Fh h) { return CloseHandle(h) != 0; }
static bool fh_size(Fh h, uint64_t *size) {
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) return false;
    *size = (uint64_t)sz.QuadPart;
    return true;
}
static HANDLE open_w(const char *path, bool write) {
    wchar_t w[1100];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1100)) return INVALID_HANDLE_VALUE;
    return CreateFileW(w, write ? GENERIC_WRITE : GENERIC_READ, FILE_SHARE_READ, NULL, write ? CREATE_ALWAYS : OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | (write ? 0 : FILE_FLAG_SEQUENTIAL_SCAN), NULL);
}
static bool mkdir_w(const char *path) {
    wchar_t w[1100];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1100)) return false;
    return CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}
static bool read_at(HANDLE h, uint64_t off, void *dst, size_t n) {
    uint8_t *d = dst;
    while (n) {
        OVERLAPPED o = {0};
        o.Offset = (DWORD)off, o.OffsetHigh = (DWORD)(off >> 32);
        DWORD want = n > (1u << 30) ? (1u << 30) : (DWORD)n, got = 0;
        if (!ReadFile(h, d, want, &got, &o) || !got) return false;
        d += got, off += got, n -= got;
    }
    return true;
}
static bool write_all(HANDLE h, const void *src, size_t n) {
    const uint8_t *s = src;
    while (n) {
        DWORD want = n > (1u << 30) ? (1u << 30) : (DWORD)n, put = 0;
        if (!WriteFile(h, s, want, &put, NULL) || !put) return false;
        s += put, n -= put;
    }
    return true;
}
#else  // POSIX
typedef int Fh;
#define FH_NONE (-1)
#define strcmp_nocase strcasecmp
static bool fh_close(Fh h) { return close(h) == 0; }
static bool fh_size(Fh h, uint64_t *size) {
    struct stat st;
    if (fstat(h, &st)) return false;
    *size = (uint64_t)st.st_size;
    return true;
}
static Fh open_w(const char *path, bool write) {
    return open(path, write ? O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC : O_RDONLY | O_CLOEXEC, 0644);
}
static bool mkdir_w(const char *path) { return mkdir(path, 0755) == 0 || errno == EEXIST; }
static bool read_at(Fh h, uint64_t off, void *dst, size_t n) {
    uint8_t *d = dst;
    while (n) {
        ssize_t got = pread(h, d, n > (1u << 30) ? (1u << 30) : n, (off_t)off);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
        d += got, off += (uint64_t)got, n -= (size_t)got;
    }
    return true;
}
static bool write_all(Fh h, const void *src, size_t n) {
    const uint8_t *s = src;
    while (n) {
        ssize_t put = write(h, s, n > (1u << 30) ? (1u << 30) : n);
        if (put < 0 && errno == EINTR) continue;
        if (put <= 0) return false;
        s += put, n -= (size_t)put;
    }
    return true;
}
#endif
static bool write_file(const char *path, const void *data, size_t n) {
    Fh h = open_w(path, true);
    if (h == FH_NONE) return false;
    bool ok = write_all(h, data, n);
    return fh_close(h) && ok;
}

// ------------------------------------------------------------------ crypto (runtime/nxcrypto.c)
static bool ecb_once(const uint8_t key[16], bool enc, const uint8_t *in, uint8_t *out, size_t n) {
    NxAes k;
    nx_aes_init(&k, key);
    if (enc) nx_aes_encrypt(&k, in, out, n);
    else nx_aes_decrypt(&k, in, out, n);
    nx_wipe(&k, sizeof k);
    return true;
}
static void sha256(const void *data, size_t n, uint8_t out[32]) { nx_sha256(data, n, out); }

// AES-XTS decryption of the NCA header: 0x200-byte sectors, the sector number as a big-endian tweak (Nintendo's).
static bool xts_decrypt(const uint8_t key[32], uint8_t *buf, size_t n) {
    NxAes k1, k2;
    nx_aes_init(&k1, key), nx_aes_init(&k2, key + 16);
    for (size_t sec = 0; sec * 0x200 < n; sec++) {
        uint8_t tw[16] = {0}, t[16];
        for (int i = 0; i < 8; i++) tw[15 - i] = (uint8_t)(sec >> (8 * i));
        nx_aes_encrypt(&k2, tw, t, 16);
        for (size_t b = 0; b < 0x200; b += 16) {
            uint8_t *p = buf + sec * 0x200 + b, x[16];
            for (int i = 0; i < 16; i++) x[i] = p[i] ^ t[i];
            nx_aes_decrypt(&k1, x, x, 16);
            for (int i = 0; i < 16; i++) p[i] = x[i] ^ t[i];
            uint8_t carry = t[15] >> 7;  // t *= alpha in GF(2^128), little-endian
            for (int i = 15; i > 0; i--) t[i] = (uint8_t)(t[i] << 1 | t[i - 1] >> 7);
            t[0] = (uint8_t)(t[0] << 1) ^ (carry ? 0x87 : 0);
        }
    }
    nx_wipe(&k1, sizeof k1), nx_wipe(&k2, sizeof k2);
    return true;
}

// ------------------------------------------------------------------ prod.keys
typedef struct { char name[48]; uint8_t v[32]; int len; } Key;
typedef struct { Key *k; int n; } Keys;

static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static bool keys_load(Keys *ks, const char *path) {
    Fh h = open_w(path, false);
    if (h == FH_NONE) return false;
    uint64_t sz = 0;
    if (!fh_size(h, &sz) || sz > (1 << 20)) { fh_close(h); return false; }
    char *t = malloc((size_t)sz + 1);
    bool ok = t && read_at(h, 0, t, (size_t)sz);
    fh_close(h);
    if (!ok) { free(t); return false; }
    t[sz] = 0;
    ks->k = NULL, ks->n = 0;
    for (char *line = t, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        char *a = line, *b = eq;
        while (*a == ' ' || *a == '\t') a++;
        while (b > a && (b[-1] == ' ' || b[-1] == '\t')) b--;
        char *v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        if (b - a <= 0 || b - a >= 48) continue;
        Key k = {0};
        memcpy(k.name, a, (size_t)(b - a));
        for (char *c = k.name; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
        int len = 0;
        while (len < 32 && hexval(v[0]) >= 0 && hexval(v[1]) >= 0) k.v[len++] = (uint8_t)(hexval(v[0]) << 4 | hexval(v[1])), v += 2;
        if (len != 16 && len != 32) continue;  // (only AES keys are needed here)
        k.len = len;
        Key *grown = realloc(ks->k, sizeof(Key) * (size_t)(ks->n + 1));
        if (!grown) break;
        ks->k = grown;
        ks->k[ks->n++] = k;
        nx_wipe(&k, sizeof k);
    }
    nx_wipe(t, (size_t)sz);
    free(t);
    return true;
}
static const Key *key_get(const Keys *ks, const char *name, int len) {
    for (int i = 0; i < ks->n; i++)
        if (ks->k[i].len == len && !strcmp(ks->k[i].name, name)) return &ks->k[i];
    return NULL;
}
static void keys_free(Keys *ks) {
    if (ks->k) nx_wipe(ks->k, sizeof(Key) * (size_t)ks->n);
    free(ks->k);
    ks->k = NULL, ks->n = 0;
}

// ------------------------------------------------------------------ the package
typedef struct { char name[256]; uint64_t off, size; } Entry;

struct NxPkg {
    Fh f;
    uint64_t size;
    Keys keys;
    Entry *e;
    int ne;
    uint64_t nca_off, nca_size;   // the base Program NCA in the package
    uint8_t hdr[0xC00];           // its header, decrypted
    uint8_t ctr_key[16];          // its section key
    bool is_update;               // the update's Program NCA (its RomFS a BKTR patch over the base game's)
};

// A PFS0 (stride 24) or HFS0 (stride 64) table at off, bounded by bound bytes: its files, absolute offsets.
static bool partition(NxPkg *p, uint64_t off, uint64_t bound, bool hfs, Entry **out, int *nout) {
    uint8_t h[16];
    unsigned stride = hfs ? 64 : 24;
    if (bound < 16 || !read_at(p->f, off, h, 16) || memcmp(h, hfs ? "HFS0" : "PFS0", 4)) return false;
    uint32_t count = le32(h + 4), strsz = le32(h + 8);
    if (count > 4096 || strsz > (1u << 20)) return false;
    uint64_t tsize = 16 + (uint64_t)count * stride + strsz;
    if (tsize > bound) return false;
    uint8_t *t = malloc(tsize);
    Entry *e = calloc(count ? count : 1, sizeof *e);
    bool ok = t && e && read_at(p->f, off, t, tsize);
    for (uint32_t i = 0; ok && i < count; i++) {
        const uint8_t *r = t + 16 + i * stride;
        uint64_t o = le64(r), len = le64(r + 8);
        uint32_t no = le32(r + 16);
        const char *s = (const char *)t + 16 + count * stride;
        ok = no < strsz && memchr(s + no, 0, strsz - no) && tsize + o + len <= bound;
        if (ok) snprintf(e[i].name, sizeof e[i].name, "%s", s + no), e[i].off = off + tsize + o, e[i].size = len;
    }
    free(t);
    if (!ok) { free(e); return false; }
    *out = e, *nout = (int)count;
    return true;
}

static bool ends_with(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcmp_nocase(s + a - b, suf);
}

// The NCA header at off (size bytes), decrypted into hdr. false: not an NCA3 these keys open.
static bool nca_header(NxPkg *p, uint64_t off, uint64_t size, uint8_t hdr[0xC00]) {
    const Key *hk = key_get(&p->keys, "header_key", 32);
    if (!hk || size < 0xC00 || !read_at(p->f, off, hdr, 0xC00) || !xts_decrypt(hk->v, hdr, 0xC00)) return false;
    if (memcmp(hdr + 0x200, "NCA3", 4) || le64(hdr + 0x208) != size) return false;
    for (int i = 0; i < 4; i++) {
        uint64_t s = (uint64_t)le32(hdr + 0x240 + i * 16) * 0x200, en = (uint64_t)le32(hdr + 0x244 + i * 16) * 0x200;
        if (!(s == 0 && en == 0) && !(0xC00 <= s && s < en && en <= size)) return false;
    }
    return true;
}

// The common ticket's title key for rights id rid (encrypted), from the package's .tik files.
static bool ticket_key(NxPkg *p, const uint8_t rid[16], uint8_t key[16]) {
    for (int i = 0; i < p->ne; i++) {
        if (!ends_with(p->e[i].name, ".tik") || p->e[i].size < 4) continue;
        uint8_t s[4];
        if (!read_at(p->f, p->e[i].off, s, 4)) continue;
        uint32_t sig = le32(s), body;
        switch (sig) {
        case 0x10000: case 0x10003: body = 0x240; break;
        case 0x10001: case 0x10004: body = 0x140; break;
        case 0x10002: case 0x10005: body = 0x80; break;
        default: continue;
        }
        uint8_t d[0x180];
        if (p->e[i].size < body + 0x180 || !read_at(p->f, p->e[i].off + body, d, sizeof d)) continue;
        if (d[0x141] != 0) continue;  // personalized: not a common title key
        if (!memcmp(d + 0x160, rid, 16)) { memcpy(key, d + 0x40, 16); nx_wipe(d, sizeof d); return true; }
    }
    return false;
}

// update: the update's Program NCA (a BKTR section patching the base game's RomFS), else the base game's
static NxPkg *open_pkg(const char *package, const char *keys, bool update, char *why, size_t why_size) {
    NxPkg *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->f = FH_NONE;
    bool found_update = false, found_other = false, found_base = false;
    if (!keys_load(&p->keys, keys)) FAIL("prod.keys couldn't be read. Choose the prod.keys file dumped from your console.");
    if (!key_get(&p->keys, "header_key", 32))
        FAIL("That keys file has no header_key. Choose the prod.keys file dumped from your own console (not title.keys).");
    p->f = open_w(package, false);
    if (p->f == FH_NONE || !fh_size(p->f, &p->size)) FAIL("The game file couldn't be opened.");
    uint8_t m[4];
    if (!read_at(p->f, 0, m, 4)) FAIL("The game file couldn't be read.");
    if (!memcmp(m, "PFS0", 4)) {
        if (!partition(p, 0, p->size, false, &p->e, &p->ne)) FAIL("The NSP is damaged or incomplete.");
    } else {
        uint8_t h[0x38];
        if (p->size < 0x138 || !read_at(p->f, 0x100, h, sizeof h) || memcmp(h, "HEAD", 4))
            FAIL("That file is neither an NSP nor an XCI. (Compressed NSZ / XCZ files: decompress them first.)");
        uint64_t root = le64(h + 0x30);
        Entry *parts = NULL;
        int np = 0;
        if (root >= p->size || !partition(p, root, p->size - root, true, &parts, &np)) FAIL("The XCI is damaged or incomplete.");
        for (int i = 0; i < np && !p->e; i++)
            if (!strcmp(parts[i].name, "secure") && !partition(p, parts[i].off, parts[i].size, true, &p->e, &p->ne)) break;
        free(parts);
        if (!p->e) FAIL("The XCI has no readable secure partition.");
    }
    int programs = 0;
    for (int i = 0; i < p->ne; i++) {
        if (!ends_with(p->e[i].name, ".nca")) continue;
        uint8_t hdr[0xC00];
        if (!nca_header(p, p->e[i].off, p->e[i].size, hdr)) {
            if (ends_with(p->e[i].name, ".cnmt.nca") || p->e[i].size < (1u << 20)) continue;
            FAIL("The game file couldn't be decrypted with that prod.keys. Use keys dumped from your console with "
                 "current firmware, and a game file that isn't damaged.");
        }
        uint64_t title = le64(hdr + 0x210);
        int type = hdr[0x205];
        bool bktr = false;
        for (int s = 0; s < 4; s++) bktr |= hdr[0x400 + s * 0x200 + 4] == 4;
        if (type == 1 && title == UPDATE_TITLE) found_update = true;
        if (title != BASE_TITLE && title != UPDATE_TITLE) found_other = true;
        if (type != 0 || title != BASE_TITLE) continue;
        if (bktr != update) {
            if (bktr) found_update = true;
            else found_base = true;
            continue;
        }
        programs++;
        p->nca_off = p->e[i].off, p->nca_size = p->e[i].size;
        memcpy(p->hdr, hdr, sizeof hdr);
    }
    if (!update && found_update && !programs) FAIL("That's an update. Choose the base game (version 1.0.0) here.");
    if (update && found_base && !programs) FAIL("That's the base game. Choose the update (version 1.4.1) here.");
    if (programs != 1)
        FAIL(found_other ? "That isn't Super Mario Odyssey (or it's a package with several games or DLC)."
                         : update ? "No update found in that file." : "No base game found in that file.");
    {
        int gen = (p->hdr[0x206] > p->hdr[0x220] ? p->hdr[0x206] : p->hdr[0x220]) - 1;
        if (gen < 0) gen = 0;
        char name[48];
        static const uint8_t zero[16];
        const uint8_t *rid = p->hdr + 0x230;
        if (memcmp(rid, zero, 16)) {
            uint8_t enc[16];
            if (!ticket_key(p, rid, enc)) FAIL("The NSP has no common ticket for the game, so it can't be decrypted. Dump it again.");
            snprintf(name, sizeof name, "titlekek_%02x", gen);
            const Key *k = key_get(&p->keys, name, 16);
            if (!k) FAIL("prod.keys has no %s: dump the keys from your console again (current firmware).", name);
            bool ok = ecb_once(k->v, false, enc, p->ctr_key, 16);
            nx_wipe(enc, sizeof enc);
            if (!ok) FAIL("The title key couldn't be decrypted.");
        } else {
            if (p->hdr[0x207] != 0) FAIL("Unsupported key area in the game file.");
            snprintf(name, sizeof name, "key_area_key_application_%02x", gen);
            const Key *k = key_get(&p->keys, name, 16);
            if (!k) FAIL("prod.keys has no %s: dump the keys from your console again (current firmware).", name);
            uint8_t area[64];
            bool ok = ecb_once(k->v, false, p->hdr + 0x300, area, 64);
            memcpy(p->ctr_key, area + 32, 16);
            nx_wipe(area, sizeof area);
            if (!ok) FAIL("The game file's key area couldn't be decrypted.");
        }
    }
    p->is_update = update;
    LOG("[nxpkg] %s: %d files, %s Program NCA %llu MB\n", package, p->ne, update ? "update" : "base",
        (unsigned long long)(p->nca_size >> 20));
    return p;
fail:
    LOG("[nxpkg] %s: %s\n", package, why ? why : "failed");
    nxpkg_close(p);
    return NULL;
}

NxPkg *nxpkg_open(const char *package, const char *keys, char *why, size_t why_size) {
    return open_pkg(package, keys, false, why, why_size);
}
NxPkg *nxpkg_open_update(const char *package, const char *keys, char *why, size_t why_size) {
    return open_pkg(package, keys, true, why, why_size);
}

void nxpkg_close(NxPkg *p) {
    if (!p) return;
    if (p->f != FH_NONE) fh_close(p->f);
    keys_free(&p->keys);
    free(p->e);
    nx_wipe(p, sizeof *p);
    free(p);
}

// ------------------------------------------------------------------ sections
typedef struct { NxPkg *p; const uint8_t *fs; uint64_t start, size; NxAes key; uint8_t *ks; } Section;

static bool section_open(NxPkg *p, int i, Section *s) {
    uint64_t st = (uint64_t)le32(p->hdr + 0x240 + i * 16) * 0x200, en = (uint64_t)le32(p->hdr + 0x244 + i * 16) * 0x200;
    if (!st) return false;
    const uint8_t *fs = p->hdr + 0x400 + i * 0x200;
    uint8_t h[32];
    sha256(fs, 0x200, h);
    if (memcmp(h, p->hdr + 0x280 + i * 32, 32)) return false;
    s->p = p, s->fs = fs, s->start = st, s->size = en - st;
    nx_aes_init(&s->key, p->ctr_key);
    s->ks = malloc(CHUNK + 32);
    return s->ks != NULL;
}
static void section_close(Section *s) {
    nx_wipe(&s->key, sizeof s->key);
    free(s->ks);
}
// size bytes at rel in the section, decrypted (AES-CTR: the section's counter, then the 16-byte block number).
// hi (8 bytes, may be NULL): the counter's upper half instead of the section's (a BKTR subsection's: hi[4..7] is its
// counter value).
static bool section_read_ctr(Section *s, uint64_t rel, uint8_t *dst, size_t size, const uint8_t *hi) {
    if (rel + size > s->size) return false;
    while (size) {
        uint64_t abs = s->start + rel, aligned = abs & ~15ull;
        size_t skip = (size_t)(abs - aligned), n = size > CHUNK - 16 ? CHUNK - 16 : size, blocks = (skip + n + 15) / 16;
        uint8_t *ctr = s->ks;
        for (size_t b = 0; b < blocks; b++) {
            uint8_t *c = ctr + b * 16;
            for (int j = 0; j < 8; j++) c[j] = hi ? hi[j] : s->fs[0x147 - j];
            uint64_t blk = aligned / 16 + b;
            for (int j = 0; j < 8; j++) c[15 - j] = (uint8_t)(blk >> (8 * j));
        }
        nx_aes_encrypt(&s->key, ctr, ctr, blocks * 16);
        if (skip) {
            uint8_t first[16];
            if (!read_at(s->p->f, s->p->nca_off + aligned, first, 16)) return false;
            size_t k = 16 - skip < n ? 16 - skip : n;
            for (size_t j = 0; j < k; j++) dst[j] = first[skip + j] ^ ctr[skip + j];
            if (n > k && !read_at(s->p->f, s->p->nca_off + aligned + 16, dst + k, n - k)) return false;
        } else if (!read_at(s->p->f, s->p->nca_off + abs, dst, n)) {
            return false;
        }
        for (size_t j = skip ? 16 - skip : 0; j < n; j++) dst[j] ^= ctr[skip + j];
        rel += n, dst += n, size -= n;
    }
    return true;
}
static bool section_read(Section *s, uint64_t rel, uint8_t *dst, size_t size) {
    return section_read_ctr(s, rel, dst, size, NULL);
}

bool nxpkg_extract_exefs(NxPkg *p, const char *dir, char *why, size_t why_size) {
    uint8_t *hashes = NULL, *pfs = NULL;
    Section s = {0};
    for (int i = 0; i < 4; i++) {
        const uint8_t *fs = p->hdr + 0x400 + i * 0x200;
        if (!le32(p->hdr + 0x240 + i * 16) || fs[2] != 1 || fs[3] != 2) continue;
        if (fs[4] != 3 || !section_open(p, i, &s)) FAIL("The game file's code section can't be read (damaged?).");
        uint32_t bs = le32(fs + 0x28), levels = le32(fs + 0x2C);
        uint64_t hoff = le64(fs + 0x30), hsize = le64(fs + 0x38), poff = le64(fs + 0x40), psize = le64(fs + 0x48);
        if (levels != 2 || !bs || psize > (512u << 20) || hsize > (16u << 20)) FAIL("Unsupported code section layout.");
        hashes = malloc(hsize), pfs = malloc(psize);
        if (!hashes || !pfs || !section_read(&s, hoff, hashes, hsize) || !section_read(&s, poff, pfs, psize))
            FAIL("The game file's code couldn't be read.");
        uint8_t h[32];
        sha256(hashes, hsize, h);
        if (memcmp(h, fs + 8, 32)) FAIL("The game file's code fails its hash check: the file is damaged. Dump it again.");
        for (uint64_t pos = 0, b = 0; pos < psize; pos += bs, b++) {
            if ((b + 1) * 32 > hsize) FAIL("The game file's code hash table is too short.");
            sha256(pfs + pos, psize - pos < bs ? psize - pos : bs, h);
            if (memcmp(h, hashes + b * 32, 32)) FAIL("The game file's code fails its hash check: the file is damaged. Dump it again.");
        }
        if (psize < 16 || memcmp(pfs, "PFS0", 4)) FAIL("The code section isn't a PFS0.");
        uint32_t count = le32(pfs + 4), strsz = le32(pfs + 8);
        uint64_t data = 16 + (uint64_t)count * 24 + strsz;
        if (count > 64 || data > psize) FAIL("The code section's file table is broken.");
        if (!mkdir_w(dir)) FAIL("The folder %s couldn't be made.", dir);
        bool npdm = false;
        for (uint32_t k = 0; k < count; k++) {
            const uint8_t *r = pfs + 16 + k * 24;
            uint64_t o = le64(r), len = le64(r + 8);
            uint32_t no = le32(r + 16);
            const char *name = (const char *)pfs + 16 + count * 24 + no;
            if (no >= strsz || !memchr(name, 0, strsz - no) || data + o + len > psize || strpbrk(name, "/\\:") || !*name)
                FAIL("The code section's file table is broken.");
            npdm |= !strcmp(name, "main.npdm");
            char out[1100];
            snprintf(out, sizeof out, "%s/%s", dir, name);
            if (!write_file(out, pfs + data + o, len)) FAIL("Writing %s failed (disk full?).", out);
        }
        if (!npdm) FAIL("The code section has no main.npdm.");
        free(hashes), free(pfs), section_close(&s);
        return true;
    }
    FAIL("The game file has no code section.");
fail:
    free(hashes), free(pfs), section_close(&s);
    LOG("[nxpkg] ExeFS: %s\n", why ? why : "failed");
    return false;
}

static int romfs_section(NxPkg *p) {
    for (int i = 0; i < 4; i++) {
        const uint8_t *fs = p->hdr + 0x400 + i * 0x200;
        // (an update's: AES-CTR with BKTR, crypt type 4)
        if (le32(p->hdr + 0x240 + i * 16) && fs[2] == 0 && fs[3] == 3 && fs[4] == (p->is_update ? 4 : 3) &&
            !memcmp(fs + 8, "IVFC", 4))
            return i;
    }
    return -1;
}

uint64_t nxpkg_romfs_size(NxPkg *p) {
    int i = romfs_section(p);
    return i < 0 ? 0 : le64(p->hdr + 0x400 + i * 0x200 + 0x18 + 5 * 24 + 8);
}

// The RomFS image of an IVFC section (fs: its FS header, read through rd): every block of all six hash levels checked,
// the last level (the image) written to out.
typedef bool (*ReadFn)(void *ctx, uint64_t off, uint8_t *dst, size_t n);
static bool extract_ivfc(const uint8_t *fs, ReadFn rd, void *rctx, const char *out, void (*progress)(uint64_t, uint64_t),
                         char *why, size_t why_size) {
    uint8_t *parent = NULL, *next = NULL, *buf = malloc(CHUNK);
    Fh o = FH_NONE;
    if (!buf) FAIL("Out of memory.");
    if (le32(fs + 12) != 0x20000 || le32(fs + 16) != 32 || le32(fs + 20) != 7) FAIL("Unsupported game data layout.");
    size_t plen = 32;
    parent = malloc(32);
    if (!parent) FAIL("Out of memory.");
    memcpy(parent, fs + 0xC8, 32);
    uint64_t total = 0;
    for (int l = 0; l < 6; l++) total += le64(fs + 0x18 + l * 24 + 8);
    uint64_t done = 0;
    o = open_w(out, true);
    if (o == FH_NONE) FAIL("%s couldn't be written.", out);
    for (int l = 0; l < 6; l++) {
        const uint8_t *lv = fs + 0x18 + l * 24;
        uint64_t off = le64(lv), size = le64(lv + 8);
        uint32_t pw = le32(lv + 16), bs = 1u << pw;
        if (pw < 12 || pw > 22 || le32(lv + 20) || (size + bs - 1) / bs * 32 > plen) FAIL("Unsupported game data layout.");
        if (l < 5 && size > (256u << 20)) FAIL("Unsupported game data layout.");
        if (l < 5 && !(next = malloc(size ? size : 1))) FAIL("Out of memory.");
        uint64_t block = 0, step = CHUNK / bs * bs;
        for (uint64_t pos = 0; pos < size; pos += step) {
            size_t n = (size_t)(size - pos < step ? size - pos : step);
            if (!rd(rctx, off + pos, buf, n)) FAIL("Reading the game data failed (the file is damaged or incomplete).");
            for (size_t j = 0; j < n; j += bs, block++) {
                uint8_t h[32];
                if (n - j >= bs) {
                    sha256(buf + j, bs, h);
                } else {  // the last block, zero-padded to the block size
                    uint8_t *pad = calloc(1, bs);
                    if (!pad) FAIL("Out of memory.");
                    memcpy(pad, buf + j, n - j);
                    sha256(pad, bs, h);
                    free(pad);
                }
                if (memcmp(h, parent + block * 32, 32))
                    FAIL("The game data fails its hash check (level %d, block %llu): the file is damaged. Dump it again.",
                         l, (unsigned long long)block);
            }
            if (l < 5) memcpy(next + pos, buf, n);
            else if (!write_all(o, buf, n)) FAIL("Writing the game data failed (disk full?).");
            done += n;
            if (progress) progress(done, total);
        }
        if (l < 5) { free(parent); parent = next; next = NULL; plen = (size_t)size; }
    }
    if (!fh_close(o)) { o = FH_NONE; FAIL("Writing the game data failed (disk full?)."); }
    o = FH_NONE;
    free(buf), free(parent);
    LOG("[nxpkg] RomFS: %llu bytes, all six hash levels verified\n", (unsigned long long)le64(fs + 0x18 + 5 * 24 + 8));
    return true;
fail:
    if (o != FH_NONE) fh_close(o);
    free(buf), free(parent), free(next);
    LOG("[nxpkg] RomFS: %s\n", why ? why : "failed");
    return false;
}

static bool read_section(void *s, uint64_t off, uint8_t *dst, size_t n) { return section_read((Section *)s, off, dst, n); }

bool nxpkg_extract_romfs(NxPkg *p, const char *out, void (*progress)(uint64_t, uint64_t), char *why, size_t why_size) {
    Section s = {0};
    int i = romfs_section(p);
    if (p->is_update || i < 0 || !section_open(p, i, &s)) {
        section_close(&s);
        if (why && why_size) snprintf(why, why_size, "The game file has no readable game data (RomFS) section.");
        return false;
    }
    bool ok = extract_ivfc(s.fs, read_section, &s, out, progress, why, why_size);
    section_close(&s);
    return ok;
}

// ------------------------------------------------------------------ an update's RomFS (BKTR) over the base game's
// The update's section is the patched IVFC image, virtually: its relocation table maps ranges of the image either to
// the base game's RomFS section or to the update's own data, which is AES-CTR with a counter value per subsection
// (the subsection table). Both tables sit at the end of the section, encrypted with the section's usual counter.
// Layout as hactool reads it (SciresM/hactool bktr.c / nca.c, ISC): a table is a 0x4000-byte header (u32, u32 bucket
// count, u64 total size, the buckets' start offsets) and 0x4000-byte buckets (u32, u32 entry count, u64 end, entries).
typedef struct { uint64_t virt, phys; uint32_t patch; } Reloc;
typedef struct { uint64_t off; uint32_t ctr; } Subsec;
typedef struct {
    Section base, upd;
    Reloc *r; size_t nr; uint64_t total;
    Subsec *ss; size_t ns; uint64_t data_end;
    uint8_t hi[8];  // the counter's upper half: the section's top 4 bytes, then a subsection's value
} Bktr;

static bool bktr_tables(Bktr *b, char *why, size_t why_size) {
    const uint8_t *fs = b->upd.fs;
    uint64_t roff = le64(fs + 0x100), rsize = le64(fs + 0x108), soff = le64(fs + 0x120), ssize = le64(fs + 0x128);
    uint8_t *rt = NULL, *st = NULL;
    if (memcmp(fs + 0x110, "BKTR", 4) || memcmp(fs + 0x130, "BKTR", 4) || roff + rsize != soff || soff + ssize > b->upd.size ||
        rsize < 0x4000 || ssize < 0x4000 || rsize > (256u << 20) || ssize > (256u << 20))
        FAIL("The update's game data tables aren't in the expected layout.");
    rt = malloc(rsize), st = malloc(ssize);
    if (!rt || !st || !section_read(&b->upd, roff, rt, rsize) || !section_read(&b->upd, soff, st, ssize))
        FAIL("The update's game data tables couldn't be read.");
    {   // relocations
        uint32_t nb = le32(rt + 4);
        b->total = le64(rt + 8);
        if (!nb || 0x4000ull * (nb + 1) > rsize) FAIL("The update's relocation table is broken.");
        for (uint32_t k = 0; k < nb; k++) {
            const uint8_t *bk = rt + 0x4000 * (k + 1);
            uint32_t ne = le32(bk + 4);
            if (0x10 + 0x14ull * ne > 0x4000) FAIL("The update's relocation table is broken.");
            Reloc *g = realloc(b->r, sizeof *g * (b->nr + ne));
            if (!g) FAIL("Out of memory.");
            b->r = g;
            for (uint32_t e = 0; e < ne; e++) {
                const uint8_t *en = bk + 0x10 + 0x14 * e;
                b->r[b->nr++] = (Reloc){le64(en), le64(en + 8), le32(en + 16)};
            }
        }
    }
    {   // subsections
        uint32_t nb = le32(st + 4);
        if (!nb || 0x4000ull * (nb + 1) > ssize) FAIL("The update's subsection table is broken.");
        for (uint32_t k = 0; k < nb; k++) {
            const uint8_t *bk = st + 0x4000 * (k + 1);
            uint32_t ne = le32(bk + 4);
            if (0x10 + 0x10ull * ne > 0x4000) FAIL("The update's subsection table is broken.");
            Subsec *g = realloc(b->ss, sizeof *g * (b->ns + ne));
            if (!g) FAIL("Out of memory.");
            b->ss = g;
            for (uint32_t e = 0; e < ne; e++) {
                const uint8_t *en = bk + 0x10 + 0x10 * e;
                b->ss[b->ns++] = (Subsec){le64(en), le32(en + 12)};
            }
        }
        b->data_end = roff;  // (the patch data ends where the tables begin)
    }
    for (size_t k = 1; k < b->nr; k++) if (b->r[k].virt <= b->r[k - 1].virt) FAIL("The update's relocation table is broken.");
    for (size_t k = 1; k < b->ns; k++) if (b->ss[k].off <= b->ss[k - 1].off) FAIL("The update's subsection table is broken.");
    if (!b->nr || b->r[0].virt || !b->ns) FAIL("The update's game data tables are empty.");
    for (int j = 0; j < 4; j++) b->hi[j] = fs[0x147 - j];
    free(rt), free(st);
    LOG("[nxpkg] update: %zu relocations, %zu subsections, image %llu bytes\n", b->nr, b->ns, (unsigned long long)b->total);
    return true;
fail:
    free(rt), free(st);
    return false;
}

static bool bktr_read_patch(Bktr *b, uint64_t p, uint8_t *dst, size_t n) {
    while (n) {
        size_t lo = 0, hi = b->ns;  // the last subsection starting at or before p
        while (hi - lo > 1) { size_t mid = (lo + hi) / 2; if (b->ss[mid].off <= p) lo = mid; else hi = mid; }
        if (b->ss[lo].off > p) return false;
        uint64_t end = lo + 1 < b->ns ? b->ss[lo + 1].off : b->data_end;
        if (end <= p) return false;
        size_t m = (size_t)(end - p < n ? end - p : n);
        uint8_t h[8];
        memcpy(h, b->hi, 4);
        for (int j = 0; j < 4; j++) h[7 - j] = (uint8_t)(b->ss[lo].ctr >> (8 * j));
        if (!section_read_ctr(&b->upd, p, dst, m, h)) return false;
        p += m, dst += m, n -= m;
    }
    return true;
}

static bool bktr_read(void *ctx, uint64_t v, uint8_t *dst, size_t n) {
    Bktr *b = ctx;
    while (n) {
        if (v >= b->total) return false;
        size_t lo = 0, hi = b->nr;
        while (hi - lo > 1) { size_t mid = (lo + hi) / 2; if (b->r[mid].virt <= v) lo = mid; else hi = mid; }
        uint64_t end = lo + 1 < b->nr ? b->r[lo + 1].virt : b->total;
        size_t m = (size_t)(end - v < n ? end - v : n);
        uint64_t phys = v - b->r[lo].virt + b->r[lo].phys;
        if (!(b->r[lo].patch ? bktr_read_patch(b, phys, dst, m) : section_read(&b->base, phys, dst, m))) return false;
        v += m, dst += m, n -= m;
    }
    return true;
}

bool nxpkg_extract_romfs_patched(NxPkg *base, NxPkg *update, const char *out, void (*progress)(uint64_t, uint64_t),
                                 char *why, size_t why_size) {
    Bktr b = {0};
    int bi = romfs_section(base), ui = romfs_section(update);
    bool ok = false;
    if (base->is_update || !update->is_update || bi < 0 || ui < 0 || !section_open(base, bi, &b.base) ||
        !section_open(update, ui, &b.upd)) {
        if (why && why_size) snprintf(why, why_size, "The base game's or the update's game data section can't be read.");
    } else if (bktr_tables(&b, why, why_size)) {
        ok = extract_ivfc(b.upd.fs, bktr_read, &b, out, progress, why, why_size);
    }
    section_close(&b.base), section_close(&b.upd);
    free(b.r), free(b.ss);
    return ok;
}

// ------------------------------------------------------------------ a folder out of a RomFS image
static bool copy_dir(Fh img, const uint8_t *hd, const uint8_t *dirs, uint64_t ndirs, const uint8_t *files, uint64_t nfiles,
                     uint32_t d, const char *out, uint8_t *buf, int depth) {
    if (depth > 32 || (uint64_t)d + 24 > ndirs || !mkdir_w(out)) return false;
    uint32_t child = le32(dirs + d + 8), file = le32(dirs + d + 12);
    for (int guard = 0; file != 0xFFFFFFFFu && guard < 100000; guard++) {
        if ((uint64_t)file + 32 > nfiles) return false;
        const uint8_t *e = files + file;
        uint64_t data = le64(e + 8), size = le64(e + 16);
        uint32_t nl = le32(e + 28);
        if ((uint64_t)file + 32 + nl > nfiles || !nl || nl > 255) return false;
        char name[256], path[1100];
        memcpy(name, e + 32, nl), name[nl] = 0;
        if (strpbrk(name, "/\\:") || !strcmp(name, "..")) return false;
        snprintf(path, sizeof path, "%s/%s", out, name);
        Fh w = open_w(path, true);
        if (w == FH_NONE) return false;
        bool ok = true;
        for (uint64_t pos = 0; ok && pos < size; pos += CHUNK) {
            size_t n = (size_t)(size - pos < CHUNK ? size - pos : CHUNK);
            ok = read_at(img, le64(hd + 72) + data + pos, buf, n) && write_all(w, buf, n);
        }
        if (!fh_close(w) || !ok) return false;
        file = le32(e + 4);
    }
    for (int guard = 0; child != 0xFFFFFFFFu && guard < 100000; guard++) {
        if ((uint64_t)child + 24 > ndirs) return false;
        uint32_t nl = le32(dirs + child + 20);
        if ((uint64_t)child + 24 + nl > ndirs || !nl || nl > 255) return false;
        char name[256], path[1100];
        memcpy(name, dirs + child + 24, nl), name[nl] = 0;
        if (strpbrk(name, "/\\:") || !strcmp(name, "..")) return false;
        snprintf(path, sizeof path, "%s/%s", out, name);
        if (!copy_dir(img, hd, dirs, ndirs, files, nfiles, child, path, buf, depth + 1)) return false;
        child = le32(dirs + child + 4);
    }
    return true;
}

bool romfs_image_extract_dir(const char *image, const char *dir, const char *out, char *why, size_t why_size) {
    uint8_t hd[80], *dirs = NULL, *files = NULL, *buf = NULL;
    Fh img = open_w(image, false);
    if (img == FH_NONE || !read_at(img, 0, hd, 80) || le64(hd) != 80) FAIL("The game data image can't be read.");
    uint64_t doff = le64(hd + 24), dsz = le64(hd + 32), foff = le64(hd + 56), fsz = le64(hd + 64);
    if (dsz > (64u << 20) || fsz > (64u << 20)) FAIL("The game data image is broken.");
    dirs = malloc(dsz ? dsz : 1), files = malloc(fsz ? fsz : 1), buf = malloc(CHUNK);
    if (!dirs || !files || !buf || !read_at(img, doff, dirs, dsz) || !read_at(img, foff, files, fsz))
        FAIL("The game data image can't be read.");
    // the folder named dir among the root's children
    uint32_t c = dsz >= 24 ? le32(dirs + 8) : 0xFFFFFFFFu;
    size_t dl = strlen(dir);
    for (int guard = 0; c != 0xFFFFFFFFu && guard < 100000; guard++) {
        if ((uint64_t)c + 24 > dsz) break;
        uint32_t nl = le32(dirs + c + 20);
        if (nl == dl && (uint64_t)c + 24 + nl <= dsz && !memcmp(dirs + c + 24, dir, dl)) break;
        c = le32(dirs + c + 4);
    }
    if (c == 0xFFFFFFFFu || (uint64_t)c + 24 > dsz) FAIL("The game data has no %s folder.", dir);
    if (!copy_dir(img, hd, dirs, dsz, files, fsz, c, out, buf, 0)) FAIL("Copying %s out of the game data failed.", dir);
    fh_close(img);
    free(dirs), free(files), free(buf);
    return true;
fail:
    if (img != FH_NONE) fh_close(img);
    free(dirs), free(files), free(buf);
    LOG("[nxpkg] %s from %s: %s\n", dir, image, why ? why : "failed");
    return false;
}
