// Native exporter integration checks. Own temporary fixtures only; no game data.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "rt.h"
#include "diagnostics.h"

static char root[1024], log_file[1200], settings[1200];
static bool loaded;
static Module module = {.name = "main", .build_id = {1, 2, 3, 4, 5, 6, 7, 8}};
const char *rt_data_root(void) { return root; }
const char *plat_log_path(void) { return log_file; }
const char *settings_current_file(void) { return settings; }
uint64_t plat_total_ram(void) { return 8ull << 30; }
bool plat_exe_path(char *buf, size_t n) { return snprintf(buf, n, "%s/fixture.exe", root) < (int)n; }
Module *loader_module(const char *name) { (void)name; return loaded ? &module : NULL; }

static char *read_report(const char *path) {
    FILE *f = fopen(path, "rb"); assert(f);
    fseek(f, 0, SEEK_END); long size = ftell(f); rewind(f);
    assert(size >= 0 && size < 1200000);
    char *text = calloc((size_t)size + 1, 1); assert(text);
    assert(fread(text, 1, (size_t)size, f) == (size_t)size);
    fclose(f); return text;
}

int main(int argc, char **argv) {
    assert(argc == 3);
    snprintf(root, sizeof root, "%s", argv[1]);
    snprintf(log_file, sizeof log_file, "%s/session.log", root);
    snprintf(settings, sizeof settings, "%s/test.ini", root);
    if (!strcmp(argv[2], "restart")) {
        FILE *fresh = fopen(log_file, "wb"); assert(fresh); fprintf(fresh, "NEW_PROCESS_LOG\n"); fclose(fresh);
        char last[1200]; assert(diagnostics_last_crash(last, sizeof last));
        char *text = read_report(last); assert(strstr(text, "TAIL_CRASH_CHAIN_MARKER")); free(text);
        printf("Crash report survived process restart and log replacement\n");
        return 0;
    }
    assert(!mkdir(root, 0755)); // existing fixture directories are refused
    char executable[1200]; snprintf(executable, sizeof executable, "%s/fixture.exe", root);
    FILE *binary = fopen(executable, "wb"); assert(binary); fprintf(binary, "abc"); fclose(binary);
    diagnostics_init();
    bool early = !strcmp(argv[2], "early");
    FILE *f = fopen(log_file, "wb"); assert(f);
    fprintf(f, "STARTUP_HARDWARE_MARKER\n");
    if (!early) for (unsigned i = 0; i < 2 * 1024 * 1024; i++) fputc('x', f);
    fprintf(f, "\n*** exception fixture accessing 0x20\nTAIL_CRASH_CHAIN_MARKER\n"); fclose(f);
    if (!early) {
        f = fopen(settings, "wb"); assert(f); fprintf(f, "dlss_mode = off\nSETTINGS_MARKER\n"); fclose(f);
        loaded = true;
        diagnostics_gpu("FIXTURE GPU; FIXTURE DRIVER 123");
        diagnostics_fault("exception fixture", 0x20, 0x1234, 0x5678);
        diagnostics_fault("secondary fault", 0x99, 0x9999, 0x9999); // first fault wins
    }
    char other[1200]; snprintf(other, sizeof other, "%s/do-not-attach.bin", root);
    f = fopen(other, "wb"); assert(f); fprintf(f, "UNRELATED_FILE_MUST_NOT_BE_INCLUDED"); fclose(f);
    if (!strcmp(argv[2], "dialog")) {
        diagnostics_fatal_dialog("Diagnostics test only. No game has crashed.\n\nCheck the Copy crash report button.");
        return 0;
    }
    char first[1200], second[1200], last[1200], tiny[4];
    assert(diagnostics_capture_crash(first, sizeof first));
    if (!strcmp(argv[2], "capture")) return 0;
    assert(diagnostics_last_crash(last, sizeof last) && !strcmp(first, last));
    char *text = read_report(first);
    assert(strstr(text, "STARTUP_HARDWARE_MARKER") && strstr(text, "TAIL_CRASH_CHAIN_MARKER"));
    assert(strstr(text, "fnv1a64:e71fa2190541574b (3 bytes)"));
    assert(!strstr(text, "UNRELATED_FILE_MUST_NOT_BE_INCLUDED"));
    if (early) {
        assert(strstr(text, "not loaded") && strstr(text, "unavailable (see log)"));
        assert(strstr(text, "--- Saved settings ---\nPath:") && strstr(text, "Unavailable:"));
    } else {
        assert(strstr(text, "SETTINGS_MARKER") && strstr(text, "FIXTURE DRIVER 123"));
        assert(strstr(text, "host PC 0x1234") && strstr(text, "Game main build ID: 0102030405060708"));
        assert(strstr(text, "Middle of log omitted"));
    }
    free(text);
    f = fopen(log_file, "wb"); assert(f); fprintf(f, "NEW_SESSION_MARKER\n"); fclose(f);
    assert(diagnostics_write_report(second, sizeof second) && strcmp(first, second));
    assert(diagnostics_last_crash(last, sizeof last) && !strcmp(last, first));
    text = read_report(last); assert(strstr(text, "TAIL_CRASH_CHAIN_MARKER")); free(text);
    assert(!diagnostics_write_report(tiny, sizeof tiny) && !tiny[0]);
    char marker[1200]; snprintf(marker, sizeof marker, "%s/reports/last-crash.txt", root);
    f = fopen(marker, "wb"); assert(f); fprintf(f, "../do-not-attach.bin\n"); fclose(f);
    assert(!diagnostics_last_crash(last, sizeof last) && !last[0]);
    log_file[0] = 0;
    snprintf(root, sizeof root, "%s", other); // file, not a directory
    assert(!diagnostics_write_report(last, sizeof last) && !last[0]);
    printf("Exporter %s checks passed\n", early ? "early-startup" : "fault/truncation/restart");
    return 0;
}
