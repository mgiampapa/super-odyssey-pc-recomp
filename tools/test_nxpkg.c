// Test of runtime/nxpkg.c on a real package: test_nxpkg <nsp|xci> <prod.keys> <outdir> [update.nsp]
// Writes <outdir>/exefs/*, <outdir>/romfs.bin and <outdir>/romfs/MovieData; compare with the vendored Python extractors.
// With an update: its ExeFS to <outdir>/exefs-update and the patched RomFS (BKTR over the base) to <outdir>/romfs-update.bin
// instead of the base RomFS - all six hash levels of the result checked against the update's own master hash.
// Build: cc -O2 -Iruntime tools/test_nxpkg.c runtime/nxpkg.c runtime/nxcrypto.c -o test_nxpkg
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#define make_dir(p) mkdir(p)
#else
#define make_dir(p) mkdir(p, 0755)
#endif

#include "nxpkg.h"

static void progress(uint64_t done, uint64_t total) {
    static int last = -1;
    int pct = (int)(done * 100 / (total ? total : 1));
    if (pct / 10 != last / 10) fprintf(stderr, "  %d%%\n", pct), last = pct;
}

int main(int argc, char **argv) {
    if (argc != 4 && argc != 5) { fprintf(stderr, "usage: test_nxpkg <nsp|xci> <prod.keys> <outdir> [update.nsp]\n"); return 2; }
    char why[512] = "", path[1100];
    clock_t t0 = clock();
    NxPkg *p = nxpkg_open(argv[1], argv[2], why, sizeof why);
    if (!p) { fprintf(stderr, "open: %s\n", why); return 1; }
    if (argc == 5) {
        NxPkg *u = nxpkg_open_update(argv[4], argv[2], why, sizeof why);
        if (!u) { fprintf(stderr, "open update: %s\n", why); return 1; }
        snprintf(path, sizeof path, "%s/exefs-update", argv[3]);
        if (!nxpkg_extract_exefs(u, path, why, sizeof why)) { fprintf(stderr, "update exefs: %s\n", why); return 1; }
        fprintf(stderr, "patched romfs: %llu bytes\n", (unsigned long long)nxpkg_romfs_size(u));
        snprintf(path, sizeof path, "%s/romfs-update.bin", argv[3]);
        if (!nxpkg_extract_romfs_patched(p, u, path, progress, why, sizeof why)) { fprintf(stderr, "patched romfs: %s\n", why); return 1; }
        nxpkg_close(u);
        nxpkg_close(p);
        fprintf(stderr, "ok in %.0f s\n", (double)(clock() - t0) / CLOCKS_PER_SEC);
        return 0;
    }
    snprintf(path, sizeof path, "%s/exefs", argv[3]);
    if (!nxpkg_extract_exefs(p, path, why, sizeof why)) { fprintf(stderr, "exefs: %s\n", why); return 1; }
    fprintf(stderr, "romfs: %llu bytes\n", (unsigned long long)nxpkg_romfs_size(p));
    snprintf(path, sizeof path, "%s/romfs.bin", argv[3]);
    if (!nxpkg_extract_romfs(p, path, progress, why, sizeof why)) { fprintf(stderr, "romfs: %s\n", why); return 1; }
    nxpkg_close(p);
    char movies[1100];
    snprintf(movies, sizeof movies, "%s/romfs", argv[3]);
    make_dir(movies);
    snprintf(movies, sizeof movies, "%s/romfs/MovieData", argv[3]);
    if (!romfs_image_extract_dir(path, "MovieData", movies, why, sizeof why)) { fprintf(stderr, "movies: %s\n", why); return 1; }
    fprintf(stderr, "ok in %.0f s\n", (double)(clock() - t0) / CLOCKS_PER_SEC);
    return 0;
}
