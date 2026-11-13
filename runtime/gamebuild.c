// Builds the game code library on the player's machine (runtime/gamebuild.h).
#include "gamebuild.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "gamelib.h"
#include "paths.h"
#include "platform.h"
#include "rt.h"

// From the Makefile: the flags the game code is compiled and linked with (as `make` builds it in a source checkout)
#ifndef GAMELIB_CFLAGS
#define GAMELIB_CFLAGS "-O1 -fno-strict-aliasing -fvisibility=hidden"
#endif
#ifndef GAMELIB_SHARED
#define GAMELIB_SHARED "-shared"
#endif

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

// ------------------------------------------------------------------ child processes
// Every one running, so that quitting mid-build ends them too (atexit) instead of leaving compilers behind.
#define MAX_PROCS 80
static pthread_mutex_t g_plock = PTHREAD_MUTEX_INITIALIZER;
static PlatProc g_procs[MAX_PROCS];
static int g_nprocs;
static bool g_quitting;

static void end_all(void) {
    pthread_mutex_lock(&g_plock);
    g_quitting = true;
    for (int i = 0; i < g_nprocs; i++) plat_proc_kill(g_procs[i]);
    g_nprocs = 0;
    pthread_mutex_unlock(&g_plock);
}

// generator: Windows starts it with Control Flow Guard off (platform/windows.c plat_spawn_no_cfg)
static PlatProc spawn_proc(char **argv, const char *log, bool generator) {
    pthread_mutex_lock(&g_plock);
    PlatProc p = 0;
    if (!g_quitting) {
#ifdef _WIN32
        if (generator) {
            unsigned long error = 0;
            p = plat_spawn_no_cfg((const char *const *)argv, log, false, &error);
            static bool told;
            if (p && !told) {
                told = true;
                if (!error) LOG("[gamebuild] generator started with Control Flow Guard off\n");
                else LOG("[gamebuild] generator started normally: Control Flow Guard could not be turned off for it (error %lu)\n", error);
            }
        } else
#endif
        p = plat_spawn((const char *const *)argv, log, false);
    }
    (void)generator;
    if (p && g_nprocs < MAX_PROCS) g_procs[g_nprocs++] = p;
    pthread_mutex_unlock(&g_plock);
    return p;
}
static PlatProc spawn(char **argv, const char *log) { return spawn_proc(argv, log, false); }

// plat_proc_wait without blocking, then forgotten once it has ended
static int finished(PlatProc p) {
    pthread_mutex_lock(&g_plock);
    int r = plat_proc_wait(p, false);
    if (r >= 0)
        for (int i = 0; i < g_nprocs; i++)
            if (g_procs[i] == p) { g_procs[i] = g_procs[--g_nprocs]; break; }
    pthread_mutex_unlock(&g_plock);
    return r;
}

static void stop(PlatProc p) {  // ends one that is still running, then forgotten
    pthread_mutex_lock(&g_plock);
    bool ours = false;
    for (int i = 0; i < g_nprocs; i++)
        if (g_procs[i] == p) { g_procs[i] = g_procs[--g_nprocs]; ours = true; break; }
    pthread_mutex_unlock(&g_plock);
    if (ours) plat_proc_kill(p);
}

static void sleep_ms(int ms) { plat_sleep_until_ns(plat_now_ns() + (uint64_t)ms * 1000000ull); }

static int wait_for(PlatProc p) {
    int r;
    while ((r = finished(p)) < 0) sleep_ms(20);
    return r;
}

// ------------------------------------------------------------------ argument lists
typedef struct { char **v; int n, cap; } Args;
static void add(Args *a, const char *s) {
    if (a->n + 2 > a->cap) {
        a->cap = a->cap ? a->cap * 2 : 32;
        a->v = (char **)realloc(a->v, sizeof *a->v * a->cap);
    }
    a->v[a->n++] = strdup(s);
    a->v[a->n] = NULL;
}
static void add_split(Args *a, const char *flags) {  // (space-separated, no quoting)
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", flags);
    char *save = NULL;
    for (char *t = strtok_r(buf, " ", &save); t; t = strtok_r(NULL, " ", &save)) add(a, t);
}
static void args_free(Args *a) {
    for (int i = 0; i < a->n; i++) free(a->v[i]);
    free(a->v);
    memset(a, 0, sizeof *a);
}

// ------------------------------------------------------------------ the tools
typedef struct {
    char gen[1100], gen_script[1100];  // generator: odysseygen, or (source checkout) the venv's python + tools/gen.py
    char cc[1100];                     // compiler: zig (as `zig cc`), or ODYSSEY_CC (a source checkout: MSYS2's clang)
    bool zig;
    char inc[1100], entry[1100], hle[1100];  // headers the game code includes, tools/gamelib_entry.c, HLE names
#ifdef _WIN32
    char implib[1100];  // the .exe's import library (the runtime functions the game code calls)
    char zig_rt[1100];  // the release's zig: its runtime built ahead of time (zig/rt, tools/zig_slim.sh), "" = none
#endif
} Tools;

static bool file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static bool find_tools(Tools *t) {
    memset(t, 0, sizeof *t);
    // The generator: Windows releases ship Python's own signed embeddable interpreter with tools/gen.py and its packages
    // (odysseygen/python.exe; tools/pyembed.py) - the PyInstaller-built odysseygen.exe was taken for a trojan by
    // Windows Defender's machine-learning check (Trojan:Win32/Bearfoos.B!ml, 2026-11-02) and removed; Linux and macOS
    // keep that one.
    const char *e = getenv("ODYSSEY_GEN");
    if (e && *e) snprintf(t->gen, sizeof t->gen, "%s", e);
    else if (!(rt_resource("odysseygen" SEP "python" PLAT_EXE_SUFFIX, NULL, t->gen, sizeof t->gen) &&
               rt_resource("odysseygen" SEP "tools" SEP "gen.py", NULL, t->gen_script, sizeof t->gen_script)) &&
             !rt_resource("odysseygen" SEP "odysseygen" PLAT_EXE_SUFFIX, NULL, t->gen, sizeof t->gen) &&
             !(rt_resource(NULL, "tools/" PLAT_VENV_PYTHON, t->gen, sizeof t->gen) &&
               rt_resource(NULL, "tools/gen.py", t->gen_script, sizeof t->gen_script)))
        return false;
    e = getenv("ODYSSEY_CC");
    if (e && *e) snprintf(t->cc, sizeof t->cc, "%s", e);
    else if (rt_resource("zig" SEP "zig" PLAT_EXE_SUFFIX, NULL, t->cc, sizeof t->cc)) t->zig = true;
#ifndef _WIN32
    else if (rt_dev_mode() && file_exists("/usr/bin/clang")) snprintf(t->cc, sizeof t->cc, "/usr/bin/clang");
#endif
    else return false;
    if (!rt_resource("gamelib-src" SEP "cpu.h", "runtime/cpu.h", t->inc, sizeof t->inc) ||
        !rt_resource("gamelib-src" SEP "gamelib_entry.c", "tools/gamelib_entry.c", t->entry, sizeof t->entry) ||
        !rt_resource("gamelib-src" SEP "hle_symbols.txt", "runtime/hle/hle_symbols.txt", t->hle, sizeof t->hle))
        return false;
    char *s = strrchr(t->inc, '/'), *b = strrchr(t->inc, '\\');
    if (b > s) s = b;
    if (s) *s = 0;
#ifdef _WIN32
    if (!rt_resource("SuperOdysseyRecomp.dll.a", "build/SuperOdysseyRecomp.dll.a", t->implib, sizeof t->implib)) return false;
    t->zig_rt[0] = 0;
    if (t->zig && rt_resource("zig" SEP "rt" SEP "link.rsp", NULL, t->zig_rt, sizeof t->zig_rt)) {
        char *s = strrchr(t->zig_rt, '/'), *b = strrchr(t->zig_rt, '\\');
        if (b > s) s = b;
        if (s) *s = 0;
        else t->zig_rt[0] = 0;
    }
#endif
    return true;
}

bool gamebuild_available(void) {
    Tools t;
    return find_tools(&t);
}

// zig cc on Linux: an explicit target, so it uses its own glibc headers and stubs instead of looking for the system's
// (SteamOS and other systems without development packages have none: "failed to find libc installation")
#if defined(__linux__) && defined(__x86_64__)
#define ZIG_TARGET "x86_64-linux-gnu"
#elif defined(__linux__) && defined(__aarch64__)
#define ZIG_TARGET "aarch64-linux-gnu"
#else
#define ZIG_TARGET ""
#endif

static void compiler(Args *a, const Tools *t) {
    add(a, t->cc);
    if (t->zig) add(a, "cc");
    if (t->zig && ZIG_TARGET[0]) add(a, "-target"), add(a, ZIG_TARGET);
}

// ------------------------------------------------------------------ the modules (as `make gen`)
// main is the game, sdk the system libraries (both with the HLE replacements: runtime/hle/hle_symbols.txt), subsdk0
// the video library (optional). Generated in this order; the library's tables list them as GEN_MODS does (main
// subsdk0 sdk). share: of the generator's work (functions: 74,823 / 27,352 / 13,379).
typedef struct {
    const char *name, *base;
    bool hle;
    double share;
} GenModule;
static const GenModule MODULES[] = {
    {"main", "0x7000000000", true, 0.65},
    {"sdk", "0x7004000000", true, 0.24},
    {"subsdk0", "0x7003000000", false, 0.11},
};
enum { NMOD = sizeof MODULES / sizeof MODULES[0] };

// ------------------------------------------------------------------ generator progress (its log)
typedef struct {
    const GenModule *m;
    char log[1100], out[1100];
    PlatProc p;
    int exit;                // -1 while running
    int files;               // chunk files written so far (complete)
    unsigned long long ci, n;  // functions written / all
} Gen;

static void read_gen_log(Gen *g) {
    FILE *f = fopen(g->log, "rb");
    if (!f) return;
    static char buf[1 << 16];
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long from = size > (long)sizeof buf - 1 ? size - (long)sizeof buf + 1 : 0;
    fseek(f, from, SEEK_SET);
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    const char *last = NULL;
    for (const char *p = strstr(buf, ": progress "); p; p = strstr(p + 1, ": progress ")) last = p;
    unsigned long long ci, all;
    int files;
    if (last && sscanf(last, ": progress %llu/%llu files %d", &ci, &all, &files) == 3) {
        g->ci = ci, g->n = all;
        if (files > g->files) g->files = files;
    }
}

static void log_tail(const char *path, char *out, size_t n) {  // the last line of a log (an error message)
    out[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char buf[4096];
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, size > (long)sizeof buf - 1 ? size - (long)sizeof buf + 1 : 0, SEEK_SET);
    size_t k = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[k] = 0;
    while (k && (buf[k - 1] == '\n' || buf[k - 1] == '\r')) buf[--k] = 0;
    char *nl = strrchr(buf, '\n');
    snprintf(out, n, "%s", nl ? nl + 1 : buf);
}

// ------------------------------------------------------------------ compiling
typedef struct {
    char src[1100], obj[1100], log[1100];
    unsigned long long bytes;
    bool generated;  // (deleted once compiled; the entry file is the shipped one)
    bool started;
} Unit;

typedef struct {
    Unit *u;
    int n, cap;
} Units;

static void add_unit(Units *us, const char *src, const char *work, const char *stem, bool generated) {
    if (us->n == us->cap) {
        us->cap = us->cap ? us->cap * 2 : 64;
        us->u = (Unit *)realloc(us->u, sizeof *us->u * us->cap);
    }
    Unit *u = &us->u[us->n++];
    snprintf(u->src, sizeof u->src, "%s", src);
    snprintf(u->obj, sizeof u->obj, "%s" SEP "%s.o", work, stem);
    snprintf(u->log, sizeof u->log, "%s" SEP "%s.log", work, stem);
    u->generated = generated;
    u->started = false;
    struct stat st;
    u->bytes = stat(src, &st) == 0 ? (unsigned long long)st.st_size : 0;
}

// The flags the game code was built with (GAMELIB_CFLAGS, part of its id). zig cc reads -march=<cpu> with its own CPU
// names (x86_64_v3) and takes "-<feature>" off the end, so x86-64-v3 would be CPU "x86": the same CPU as -mcpu there.
static void add_cflags(Args *a, const Tools *t) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", GAMELIB_CFLAGS);
    char *save = NULL;
    for (char *f = strtok_r(buf, " ", &save); f; f = strtok_r(NULL, " ", &save)) {
        if (t->zig && !strncmp(f, "-march=", 7)) {
            char cpu[128];
            snprintf(cpu, sizeof cpu, "-mcpu=%s", f + 7);
            for (char *c = cpu + 6; *c; c++) if (*c == '-') *c = '_';
            add(a, cpu);
        } else {
            add(a, f);
        }
    }
    // No debug info: zig cc emits it unless told not to, and for the few huge lifted functions (sdk_053, main_160:
    // 7,000-10,000 lines each) it made the build crawl - on a Steam Deck one file took 57+ minutes with it and 6.7
    // without. Crash reports still name functions (the symbol table). zig cc ignores -g0 (it adds -gdwarf-4 after it,
    // and the last one wins); -s, its "strip", is what leaves the debug info out.
    if (t->zig) add(a, "-s");
    // The optimization level the flags ask for: zig cc turns -O1 into clang's -O2 (its own "release" level). -O2 adds
    // loop-load elimination, which with LICM took most of the time on the biggest files (clang -ftime-report on a
    // Steam Deck); -O1 is what the project's own build (make, clang -O1) uses and was tested with.
    if (t->zig) add(a, "-Xclang"), add(a, "-O1");
}

// The longest function in a generated C file, in lines: a function starts at a line that begins with a name and ends
// in ") {", ends at a "}" alone at the start of a line (gen.py's layout). 0 if unreadable.
static int longest_function(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char line[4096];
    int n = 0, start = -1, best = 0;
    while (fgets(line, sizeof line, f)) {
        n++;
        size_t l = strcspn(line, "\r\n");
        if (l >= 3 && ((line[0] >= 'a' && line[0] <= 'z') || (line[0] >= 'A' && line[0] <= 'Z') || line[0] == '_') &&
            !strncmp(line + l - 3, ") {", 3))
            start = n;
        else if (line[0] == '}' && start >= 0) {
            if (n - start > best) best = n - start;
            start = -1;
        }
    }
    fclose(f);
    return best;
}

static char g_modules_def[200];  // -DGAMELIB_MODULES=X(main) X(subsdk0) X(sdk): the modules this build has

static PlatProc start_compile(const Tools *t, const Unit *u, bool entry) {
    Args a = {0};
    compiler(&a, t);
    add_cflags(&a, t);
    // A file holding a giant function (over 3,000 lines: about 60 of the ~500, mostly system codecs and the video
    // decoder) without LICM's promotion: on those it alone took many minutes per file (sdk_053: 13 min, 104 s
    // without it, on a Steam Deck). The rest keep it - it keeps guest registers in host ones in loops.
    if (t->zig && u->generated && longest_function(u->src) > 3000) add(&a, "-mllvm"), add(&a, "-disable-licm-promotion");
    char inc[1200];
    snprintf(inc, sizeof inc, "-I%s", t->inc);
    add(&a, inc);
    add(&a, "-DGAMELIB_ID=\"" GAMELIB_ID "\"");
    if (entry) add(&a, g_modules_def);
    add(&a, "-c");
    add(&a, u->src);
    add(&a, "-o");
    add(&a, u->obj);
    PlatProc p = spawn(a.v, u->log);
    args_free(&a);
    return p;
}

// ------------------------------------------------------------------ the build
bool gamebuild_run(const char *game_dir, const GameBuildOpts *o, char *why, size_t why_size) {
    uint64_t t0 = plat_now_ns();
    static bool registered;
    if (!registered) registered = true, atexit(end_all);
    Tools t;
    if (!find_tools(&t)) {
        snprintf(why, why_size, "the game code builder is missing from this copy of " RT_APP_NAME);
        return false;
    }
    char work[1100], lib[1100], tmp[1100];
    snprintf(work, sizeof work, "%s" SEP ".gamebuild", game_dir);
    snprintf(lib, sizeof lib, "%s" SEP "%s", game_dir, GAMELIB_NAME);
    snprintf(tmp, sizeof tmp, "%s" SEP "%s.tmp", work, GAMELIB_NAME);
    rt_remove_game_file(game_dir, ".gamebuild");
    rt_remove_game_file(game_dir, GAMELIB_NAME);
    rt_remove_game_file(game_dir, GAMELIB_NAME ".id");
    mkdir(work, 0755);
    if (t.zig) {  // its caches in the work folder too (deleted below), not ~/.cache: hundreds of MB, used once
        char zc[1200];
        snprintf(zc, sizeof zc, "%s" SEP "zig-cache", work);
#ifdef _WIN32
        _putenv_s("ZIG_GLOBAL_CACHE_DIR", zc);
        _putenv_s("ZIG_LOCAL_CACHE_DIR", zc);
#else
        setenv("ZIG_GLOBAL_CACHE_DIR", zc, 1);
        setenv("ZIG_LOCAL_CACHE_DIR", zc, 1);
#endif
    }
    LOG("[gamebuild] generator %s%s%s, compiler %s%s, %d jobs\n", t.gen, t.gen_script[0] ? " " : "", t.gen_script,
        t.cc, t.zig ? " cc" : "", o->jobs);

    // 1. the generator, all modules at once; their finished files are compiled while it goes on
    Gen gens[NMOD];
    int ngen = 0;
    bool ok = true;
    for (int i = 0; i < NMOD && ok; i++) {
        char nso[1100];
        snprintf(nso, sizeof nso, "%s" SEP "exefs" SEP "%s", game_dir, MODULES[i].name);
        if (!file_exists(nso)) {
            if (!strcmp(MODULES[i].name, "subsdk0")) continue;  // (optional: no videos without it)
            snprintf(why, why_size, "the game files have no exefs/%s", MODULES[i].name);
            ok = false;
            break;
        }
        Gen *g = &gens[ngen++];
        memset(g, 0, sizeof *g);
        g->m = &MODULES[i];
        g->exit = -1;
        snprintf(g->log, sizeof g->log, "%s" SEP "gen_%s.log", work, g->m->name);
        snprintf(g->out, sizeof g->out, "%s" SEP "%s", work, g->m->name);
        Args a = {0};
        add(&a, t.gen);
        if (t.gen_script[0]) add(&a, t.gen_script);
        add(&a, g->m->name);
        add(&a, nso);
        add(&a, g->m->base);
        add(&a, g->out);
        if (g->m->hle) { add(&a, "--hle"); add(&a, t.hle); }
        g->p = spawn_proc(a.v, g->log, true);
        args_free(&a);
        if (!g->p) { snprintf(why, why_size, "the game code builder didn't start"); ok = false; }
    }
    {   // the library's module list in GEN_MODS order (main, subsdk0, sdk)
        static const char *const order[] = {"main", "subsdk0", "sdk"};
        int n = snprintf(g_modules_def, sizeof g_modules_def, "-DGAMELIB_MODULES=");
        for (int k = 0; k < 3; k++)
            for (int i = 0; i < ngen; i++)
                if (!strcmp(gens[i].m->name, order[k]))
                    n += snprintf(g_modules_def + n, sizeof g_modules_def - n, "%sX(%s)", n > 18 ? " " : "", order[k]);
    }

    Units units = {0};
    int queued[NMOD] = {0};  // chunk files of each module put in units so far
    bool tables[NMOD] = {false};
    int next = 0, jobs = o->jobs < 1 ? 1 : o->jobs > 64 ? 64 : o->jobs;
    struct { PlatProc p; int unit; } run[64];
    int nrun = 0;
    unsigned long long done_bytes = 0;
    double shown = 0;
    add_unit(&units, t.entry, work, "gamelib_entry", false);
    while (ok) {
        // generator progress, newly finished files
        bool gens_done = true;
        double gen_frac = 0;
        unsigned long long written = 0;
        for (int i = 0; i < ngen; i++) {
            Gen *g = &gens[i];
            if (g->exit < 0) {
                read_gen_log(g);
                int r = finished(g->p);
                if (r >= 0) {
                    g->exit = r;
                    read_gen_log(g);
                    if (r != 0) {
                        char last[300];
                        log_tail(g->log, last, sizeof last);
                        LOG("[gamebuild] generator (%s) failed (exit %d): %s\n", g->m->name, r, last);
                        snprintf(why, why_size, "the game code couldn't be prepared");
                        ok = false;
                    }
                }
            }
            if (g->exit != 0) gens_done = false;
            for (; queued[i] < g->files; queued[i]++) {
                char src[1100], stem[64];
                snprintf(stem, sizeof stem, "%s_%03d", g->m->name, queued[i]);
                snprintf(src, sizeof src, "%s" SEP "%s.c", g->out, stem);
                add_unit(&units, src, work, stem, true);
            }
            if (g->exit == 0 && !tables[i]) {
                char src[1100], stem[64];
                snprintf(stem, sizeof stem, "%s_table", g->m->name);
                snprintf(src, sizeof src, "%s" SEP "%s.c", g->out, stem);
                add_unit(&units, src, work, stem, true);
                tables[i] = true;
            }
            gen_frac += g->m->share * (g->exit == 0 ? 1 : g->n ? (double)g->ci / (double)g->n : 0);
        }
        if (!ok) break;
        for (int k = 0; k < units.n; k++) written += units.u[k].bytes;

        // compiles (unit 0, the entry, last: it lists the modules' tables, which come at the end). The biggest file
        // waiting goes first: a few files hold enormous functions (7,000-10,000 lines) that take many minutes alone,
        // and started late they were the whole build's tail (the generator itself takes seconds, so they're all
        // waiting soon). next: the files started so far.
        while (nrun < jobs) {
            int pick = -1;
            for (int k = 1; k < units.n; k++)
                if (!units.u[k].started && (pick < 0 || units.u[k].bytes > units.u[pick].bytes)) pick = k;
            if (pick < 0) break;
            PlatProc p = start_compile(&t, &units.u[pick], false);
            if (!p) { snprintf(why, why_size, "the compiler didn't start"); ok = false; break; }
            units.u[pick].started = true;
            run[nrun].p = p, run[nrun].unit = pick;
            nrun++, next++;
        }
        for (int k = 0; k < nrun; k++) {
            int r = finished(run[k].p);
            if (r < 0) continue;
            Unit *u = &units.u[run[k].unit];
            if (r != 0) {
                char last[300];
                log_tail(u->log, last, sizeof last);
                LOG("[gamebuild] compiling %s failed (exit %d): %s\n", u->src, r, last);
                snprintf(why, why_size, "the game code didn't compile");
                ok = false;
            }
            done_bytes += u->bytes;
            if (u->generated) remove(u->src);  // (generated C, done with: not kept)
            run[k--] = run[--nrun];
        }
        if (!ok) break;
        if (gens_done && next == units.n - 1 && nrun == 0) break;  // (all but the entry, unit 0, done)

        // progress: the generator's share (its functions written), the compiler's (bytes of C compiled, of an
        // estimate of all of it while the generator still writes)
        double est_total = gens_done ? (double)written : gen_frac > 0.02 ? (double)written / gen_frac : 0;
        double comp_frac = est_total > 0 ? (double)done_bytes / est_total : 0;
        if (comp_frac > 1) comp_frac = 1;
        double f = 0.15 * gen_frac + 0.8 * comp_frac;
        double secs = (double)(plat_now_ns() - t0) / 1e9;
        if (gen_frac == 0) f = 0.03 * (secs / (secs + 20));  // (the generator's first pass: no count yet)
        if (f > shown) shown = f;
        if (o->progress) o->progress(shown, !gens_done && comp_frac < 0.02 ? "Reading the game code..." : "Recompiling the game code...");
        sleep_ms(50);
    }
    for (int k = 0; k < nrun; k++) stop(run[k].p);  // (a failure: the rest is of no use - the failure shows at once)
    for (int i = 0; i < ngen; i++)
        if (gens[i].p && gens[i].exit < 0) stop(gens[i].p);

    // 2. the entry (the modules' tables), then link
    if (ok) {
        if (o->progress) o->progress(shown > 0.95 ? shown : 0.95, "Finishing...");
        PlatProc p = start_compile(&t, &units.u[0], true);
        int r = p ? wait_for(p) : 255;
        if (r != 0) {
            char last[300];
            log_tail(units.u[0].log, last, sizeof last);
            LOG("[gamebuild] compiling %s failed (exit %d): %s\n", units.u[0].src, r, last);
            snprintf(why, why_size, "the game code didn't compile");
            ok = false;
        }
    }
    if (ok) {
        char rsp[1100], log[1100];
        snprintf(rsp, sizeof rsp, "%s" SEP "link.rsp", work);
        snprintf(log, sizeof log, "%s" SEP "link.log", work);
        FILE *f = fopen(rsp, "wb");
        if (f) {
            for (int k = 0; k < units.n; k++) {  // (forward slashes: the response file's backslashes are escapes)
                fputc('"', f);
                for (const char *c = units.u[k].obj; *c; c++) fputc(*c == '\\' ? '/' : *c, f);
                fputs("\"\n", f);
            }
            fclose(f);
        }
        Args a = {0};
        char at[1200];
        snprintf(at, sizeof at, "@%s", rsp);
#ifdef _WIN32
        if (t.zig_rt[0]) {  // the linker itself, with the runtime built ahead of time (its flags and libraries: link.rsp)
            char opt[1200];
            add(&a, t.cc);
            add(&a, "lld-link");
            snprintf(opt, sizeof opt, "@%s" SEP "link.rsp", t.zig_rt);
            add(&a, opt);
            snprintf(opt, sizeof opt, "-LIBPATH:%s", t.zig_rt);
            add(&a, opt);
            snprintf(opt, sizeof opt, "-OUT:%s", tmp);
            add(&a, opt);
            add(&a, at);
            add(&a, t.implib);
        } else
#endif
        {
            compiler(&a, &t);
            add_split(&a, GAMELIB_SHARED);
            add(&a, "-o");
            add(&a, tmp);
            add(&a, at);
#ifdef _WIN32
            add(&a, t.implib);
#endif
        }
        PlatProc p = f ? spawn(a.v, log) : 0;
        args_free(&a);
        int r = p ? wait_for(p) : 255;
        if (r != 0) {
            char last[300];
            log_tail(log, last, sizeof last);
            LOG("[gamebuild] linking failed (exit %d): %s\n", r, last);
            snprintf(why, why_size, "the game code didn't link");
            ok = false;
        }
    }
    if (ok) {
        remove(lib);
        char idp[1100];
        snprintf(idp, sizeof idp, "%s.id", lib);
        FILE *f = NULL;
        if (rename(tmp, lib) != 0 || !(f = fopen(idp, "wb")) || fprintf(f, "%s\n", GAMELIB_ID) < 0) {
            snprintf(why, why_size, "the game code couldn't be saved");
            ok = false;
        }
        if (f) fclose(f);
    }
    free(units.u);
    if (!getenv("ODYSSEY_KEEP_GAMEBUILD")) rt_remove_game_file(game_dir, ".gamebuild");  // the C, the objects, the logs
    LOG("[gamebuild] %s in %.0fs\n", ok ? "done" : "failed", (double)(plat_now_ns() - t0) / 1e9);
    if (ok && o->progress) o->progress(1, "Finishing...");
    return ok;
}
