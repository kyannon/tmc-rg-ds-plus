/*
 * Game-thread sampling profiler for Linux handheld builds (TMC_SAMPLE=1).
 *
 * Finds out WHERE the game thread spends a slow tick on a device we can't
 * attach a profiler to. While the game thread is inside the "game" span of a
 * tick (from the end of frame pacing to the next VBlankIntrWait — the span
 * TMC_PROFILE reports as game=), a per-thread wall-clock POSIX timer
 * interrupts it ~1000 times a second. The SIGPROF handler records the
 * interrupted PC, the link register (the caller, for leaf functions such as
 * memcpy) and up to SAMPLER_MAX_FRAMES return addresses walked through the
 * frame-pointer chain (the build adds -fno-omit-frame-pointer). Wall clock,
 * not CPU time, so time blocked in a syscall (file I/O, a lock) shows up as
 * that syscall under its caller.
 *
 * Every SAMPLER_REPORT_S seconds the game thread itself (outside the game
 * span) folds the samples into per-function self and inclusive percentages
 * and logs the top entries. Executable addresses are named through
 * tmc_pc.syms (generated at build time from the unstripped binary: "hexoffset
 * name" per line, sorted); shared-library addresses through dladdr.
 *
 * The timer only runs during the game span, so it never interrupts the
 * present / vsync wait / frame-pacing sleeps.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "port_sampler.h"

#if defined(__linux__) && defined(PORT_DUAL_SCREEN_LINUX) && (defined(__aarch64__) || defined(__x86_64__))

#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define SAMPLER_HZ 1000
#define SAMPLER_MAX_FRAMES 10 /* pc, lr, then return addresses */
#define SAMPLER_RING 16384
#define SAMPLER_REPORT_S 10
#define SAMPLER_TOP 14

typedef struct {
    uintptr_t a[SAMPLER_MAX_FRAMES];
    uint8_t n;
} Sample;

static Sample sRing[SAMPLER_RING];
static volatile uint32_t sWrite = 0; /* handler only */
static volatile sig_atomic_t sEnabled = 0;
static uintptr_t sStackLo = 0, sStackHi = 0;
static uintptr_t sExeLo = 0, sExeHi = 0; /* executable text range (x86 test path) */
static timer_t sTimer;
static uint32_t sArmCount = 0;
static uint64_t sLastReportNs = 0;
static uint64_t sPhaseNs = 0, sPhaseStartNs = 0;

/* Executable symbols (offsets from the load base). */
typedef struct {
    uintptr_t off;
    const char* name;
} Sym;
static Sym* sSyms = NULL;
static size_t sSymCount = 0;
static char* sSymText = NULL;
static uintptr_t sExeBase = 0;

static uint64_t NowNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void OnProf(int sig, siginfo_t* si, void* vctx) {
    (void)sig;
    (void)si;
    if (!sEnabled) {
        return;
    }
    const ucontext_t* uc = (const ucontext_t*)vctx;
    uint32_t w = sWrite;
    Sample* s = &sRing[w % SAMPLER_RING];
    uint8_t n = 0;
#if defined(__aarch64__)
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.pc;
    uintptr_t lr = (uintptr_t)uc->uc_mcontext.regs[30];
    uintptr_t fp = (uintptr_t)uc->uc_mcontext.regs[29];
    s->a[n++] = pc;
    s->a[n++] = lr;
#else /* x86_64: host testing. No link register; for a library leaf (pc
       * outside the executable, e.g. inside memcpy) the return address sits
       * at [rsp], which is what aarch64 has in x30 — same caller recovery. */
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    uintptr_t fp = (uintptr_t)uc->uc_mcontext.gregs[REG_RBP];
    uintptr_t sp = (uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
    uintptr_t lr = 0;
    if ((pc < sExeLo || pc >= sExeHi) && sp >= sStackLo && sp + 8 <= sStackHi) {
        lr = *(const uintptr_t*)sp;
    }
    s->a[n++] = pc;
    if (lr) {
        s->a[n++] = lr;
    }
#endif
    /* Frame records: [fp] = caller's fp, [fp + 8] = return address. Only
     * follow records that lie inside this thread's stack and move up it. */
    for (int i = 0; i < SAMPLER_MAX_FRAMES - 2; i++) {
        if (fp < sStackLo || fp + 16 > sStackHi || (fp & 7)) {
            break;
        }
        const uintptr_t* rec = (const uintptr_t*)fp;
        uintptr_t next = rec[0], ret = rec[1];
        if (ret == 0) {
            break;
        }
        if (!(i == 0 && ret == lr)) { /* non-leaf: lr was already saved here */
            s->a[n++] = ret;
        }
        if (next <= fp) {
            break;
        }
        fp = next;
    }
    s->n = n;
    sWrite = w + 1; /* past SAMPLER_RING the oldest samples are overwritten */
}

static void LoadSyms(void) {
    Dl_info info;
    if (dladdr((void*)&LoadSyms, &info) && info.dli_fbase) {
        sExeBase = (uintptr_t)info.dli_fbase;
    }
    const char* path = getenv("TMC_SAMPLE_SYMS");
    if (!path || !*path) {
        path = "tmc_pc.syms";
    }
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[sample] no %s: executable frames will print as offsets\n", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    sSymText = (char*)malloc((size_t)size + 1);
    if (!sSymText || fread(sSymText, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(sSymText);
        sSymText = NULL;
        return;
    }
    fclose(f);
    sSymText[size] = '\0';
    size_t lines = 0;
    for (long i = 0; i < size; i++) {
        lines += sSymText[i] == '\n';
    }
    sSyms = (Sym*)malloc((lines + 1) * sizeof(Sym));
    char* p = sSymText;
    while (sSyms && *p) {
        char* nl = strchr(p, '\n');
        if (nl) {
            *nl = '\0';
        }
        char* sp = strchr(p, ' ');
        if (sp) {
            *sp = '\0';
            sSyms[sSymCount].off = (uintptr_t)strtoull(p, NULL, 16);
            sSyms[sSymCount].name = sp + 1;
            sSymCount++;
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
    fprintf(stderr, "[sample] %zu symbols from %s (exe base %p)\n", sSymCount, path, (void*)sExeBase);
}

/* Function name for an address; a static buffer per call slot. */
static const char* Name(uintptr_t addr, char* buf, size_t cap) {
    Dl_info info;
    int inExe = addr >= sExeLo && addr < sExeHi;
    int have = inExe ? 0 : dladdr((void*)addr, &info); /* dladdr only for libraries */
    if (inExe) {
        uintptr_t off = addr - sExeBase;
        if (sSymCount) {
            size_t lo = 0, hi = sSymCount;
            while (hi - lo > 1) {
                size_t mid = (lo + hi) / 2;
                if (sSyms[mid].off <= off) {
                    lo = mid;
                } else {
                    hi = mid;
                }
            }
            if (sSyms[lo].off <= off) {
                return sSyms[lo].name;
            }
        }
        snprintf(buf, cap, "tmc_pc+0x%lx", (unsigned long)off);
        return buf;
    }
    if (have && info.dli_fname) {
        const char* base = strrchr(info.dli_fname, '/');
        base = base ? base + 1 : info.dli_fname;
        if (info.dli_sname) {
            snprintf(buf, cap, "%s (%s)", info.dli_sname, base);
        } else {
            snprintf(buf, cap, "%s+0x%lx", base, (unsigned long)(addr - (uintptr_t)info.dli_fbase));
        }
        return buf;
    }
    snprintf(buf, cap, "?0x%lx", (unsigned long)addr);
    return buf;
}

/* Tiny open-addressing table: function name pointer/string -> counts. */
#define AGG_CAP 4096
typedef struct {
    char name[160];
    uint32_t self, incl, own; /* own: first tmc_pc frame of the sample (its code + library calls) */
    uint32_t seen;            /* last sample index counted for incl */
} Agg;
static Agg sAgg[AGG_CAP];

static Agg* AggGet(const char* name) {
    uint32_t h = 2166136261u;
    for (const char* c = name; *c; c++) {
        h = (h ^ (uint8_t)*c) * 16777619u;
    }
    for (uint32_t i = 0; i < AGG_CAP; i++) {
        Agg* a = &sAgg[(h + i) % AGG_CAP];
        if (a->name[0] == '\0') {
            snprintf(a->name, sizeof(a->name), "%.159s", name);
            a->seen = UINT32_MAX;
            return a;
        }
        if (strncmp(a->name, name, sizeof(a->name) - 1) == 0) {
            return a;
        }
    }
    return NULL;
}

/* Address -> aggregate slot, so each distinct address is named once per
 * report (dladdr and the symbol search are the expensive part). */
#define ADDR_CAP 16384
typedef struct {
    uintptr_t addr;
    Agg* agg;
    uint8_t inExe;
} AddrSlot;
static AddrSlot sAddr[ADDR_CAP];

static AddrSlot* AddrLookup(uintptr_t addr) {
    uint32_t h = (uint32_t)((addr >> 2) * 2654435761u);
    for (uint32_t i = 0; i < ADDR_CAP; i++) {
        AddrSlot* slot = &sAddr[(h + i) & (ADDR_CAP - 1)];
        if (slot->addr == addr) {
            return slot;
        }
        if (slot->addr == 0) {
            char buf[192];
            slot->addr = addr;
            slot->inExe = addr >= sExeLo && addr < sExeHi;
            slot->agg = AggGet(Name(addr, buf, sizeof(buf)));
            return slot;
        }
    }
    return NULL;
}

static int CmpSelf(const void* x, const void* y) {
    const Agg* a = *(const Agg* const*)x;
    const Agg* b = *(const Agg* const*)y;
    return (int)b->self - (int)a->self;
}
static int CmpOwn(const void* x, const void* y) {
    const Agg* a = *(const Agg* const*)x;
    const Agg* b = *(const Agg* const*)y;
    return (int)b->own - (int)a->own;
}
static int CmpIncl(const void* x, const void* y) {
    const Agg* a = *(const Agg* const*)x;
    const Agg* b = *(const Agg* const*)y;
    return (int)b->incl - (int)a->incl;
}

static void Report(void) {
    uint32_t w = sWrite;
    uint32_t count = w > SAMPLER_RING ? SAMPLER_RING : w;
    double phaseS = sPhaseNs / 1e9;
    sPhaseNs = 0;
    sWrite = 0;
    if (count == 0) {
        return;
    }
    uint64_t reportStart = NowNs();
    memset(sAgg, 0, sizeof(sAgg));
    memset(sAddr, 0, sizeof(sAddr));
    for (uint32_t i = 0; i < count; i++) {
        const Sample* s = &sRing[i];
        int ownDone = 0;
        for (int k = 0; k < s->n; k++) {
            if (s->a[k] == 0) {
                continue;
            }
            /* Return addresses point after the call; step back into it. */
            uintptr_t addr = k == 0 ? s->a[k] : s->a[k] - 1;
            AddrSlot* slot = AddrLookup(addr);
            if (!slot || !slot->agg) {
                continue;
            }
            Agg* a = slot->agg;
            int inExe = slot->inExe;
            if (k == 0) {
                a->self++;
            }
            if (!ownDone && inExe) {
                ownDone = 1;
                a->own++;
            }
            if (a->seen != i) {
                a->seen = i;
                a->incl++;
            }
        }
    }
    Agg* list[AGG_CAP];
    int n = 0;
    for (int i = 0; i < AGG_CAP; i++) {
        if (sAgg[i].name[0]) {
            list[n++] = &sAgg[i];
        }
    }
    fprintf(stderr, "[sample] %ds: %u samples over %.2f s of game-thread work (wall clock)%s\n", SAMPLER_REPORT_S,
            count, phaseS, w > SAMPLER_RING ? " [ring full, oldest overwritten]" : "");
    /* own: where the game's code spent it, library calls charged to the
     * game function that made them. self: the exact function interrupted.
     * incl: anywhere on the stack (call roots that are on every sample are
     * left out). Entries under 0.5% are left out of all three. */
    const uint32_t minHits = count / 200 + 1;
    qsort(list, (size_t)n, sizeof(list[0]), CmpOwn);
    for (int i = 0, shown = 0; i < n && shown < SAMPLER_TOP && list[i]->own >= minHits; i++, shown++) {
        fprintf(stderr, "[sample]   own  %5.1f%%  %.140s\n", 100.0 * list[i]->own / count, list[i]->name);
    }
    qsort(list, (size_t)n, sizeof(list[0]), CmpSelf);
    for (int i = 0, shown = 0; i < n && shown < SAMPLER_TOP && list[i]->self >= minHits; i++, shown++) {
        fprintf(stderr, "[sample]   self %5.1f%%  %.140s\n", 100.0 * list[i]->self / count, list[i]->name);
    }
    qsort(list, (size_t)n, sizeof(list[0]), CmpIncl);
    for (int i = 0, shown = 0; i < n && shown < SAMPLER_TOP && list[i]->incl >= minHits; i++) {
        if (list[i]->incl >= count - count / 50) {
            continue; /* on (nearly) every sample: main, AgbMain, libc start */
        }
        fprintf(stderr, "[sample]   incl %5.1f%%  %.140s\n", 100.0 * list[i]->incl / count, list[i]->name);
        shown++;
    }
    fprintf(stderr, "[sample] (report took %.1f ms)\n", (NowNs() - reportStart) / 1e6);
}

void Port_Sampler_Init(void) {
    const char* e = getenv("TMC_SAMPLE");
    if (!e || !*e || e[0] == '0') {
        return;
    }
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        void* addr = NULL;
        size_t size = 0;
        pthread_attr_getstack(&attr, &addr, &size);
        sStackLo = (uintptr_t)addr;
        sStackHi = (uintptr_t)addr + size;
        pthread_attr_destroy(&attr);
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = OnProf;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPROF, &sa, NULL) != 0) {
        fprintf(stderr, "[sample] sigaction failed\n");
        return;
    }
    struct sigevent sev;
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = SIGPROF;
    sev._sigev_un._tid = (pid_t)syscall(SYS_gettid);
    if (timer_create(CLOCK_MONOTONIC, &sev, &sTimer) != 0) {
        fprintf(stderr, "[sample] timer_create failed\n");
        return;
    }
    LoadSyms();
    {
        extern char __executable_start[], etext[];
        sExeLo = (uintptr_t)__executable_start;
        sExeHi = (uintptr_t)etext;
    }
    sLastReportNs = NowNs();
    sEnabled = 1;
    fprintf(stderr, "[sample] game-thread sampling at %d Hz, report every %d s\n", SAMPLER_HZ, SAMPLER_REPORT_S);
}

void Port_Sampler_GameSpan(int inGame) {
    if (!sEnabled) {
        return;
    }
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    if (inGame) {
        /* Stagger the first expiry so the first ms of a span isn't blind. */
        long periodNs = 1000000000L / SAMPLER_HZ;
        sArmCount = sArmCount * 1103515245u + 12345u;
        its.it_value.tv_nsec = 1 + (long)((sArmCount >> 8) % (uint32_t)periodNs);
        its.it_interval.tv_nsec = periodNs;
        sPhaseStartNs = NowNs();
        timer_settime(sTimer, 0, &its, NULL);
    } else {
        timer_settime(sTimer, 0, &its, NULL); /* disarm */
        if (sPhaseStartNs) {
            uint64_t now = NowNs();
            sPhaseNs += now - sPhaseStartNs;
            sPhaseStartNs = 0;
            if (now - sLastReportNs >= (uint64_t)SAMPLER_REPORT_S * 1000000000ull) {
                sLastReportNs = now;
                Report();
            }
        }
    }
}

#else

void Port_Sampler_Init(void) {}
void Port_Sampler_GameSpan(int inGame) {
    (void)inGame;
}

#endif
