/* glibc compatibility layer: lets one nfsu2.aarch64 start on glibc >= 2.17 (e.g. ArkOS 2.0, glibc 2.30)
 * and still use the newest glibc code paths on newer systems (dArkOS, glibc 2.36).
 *
 * The binary is linked on a glibc 2.35 box, so by default it would import symbol versions up to
 * GLIBC_2.34 and the dynamic loader of an older system refuses to start it. Three things are done here:
 *
 *  1. Symbols that only changed *version* (pthread_create, timer_create, ...): the executable defines the
 *     plain name as a tiny hidden trampoline that branches to the GLIBC_2.17 version of the same function.
 *     Every object in the link (our code and the static libstdc++) then binds to the trampoline, so no
 *     newer version is imported. glibc 2.34+ keeps the old versions as compat symbols, so this is
 *     also valid on new systems.
 *  2. Math functions that glibc 2.27/2.29 replaced with faster versions (log, exp2, pow, powf, ...): the
 *     trampoline calls through a pointer that starts at the GLIBC_2.17 version and, at startup, is moved to
 *     the newer version (dlvsym) whenever the running glibc has it. Old glibc keeps the old function.
 *  3. Things that only exist from some glibc version on: stat/fstat/lstat (real symbols since 2.33, wrappers
 *     over __xstat before), __libc_single_threaded (2.32), pthread_cond_clockwait (2.30).
 *
 * Verified with: readelf -V nfsu2.aarch64  (no GLIBC_ version above 2.28 may appear). */
#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <stdlib.h>

#define HIDDEN __attribute__((visibility("hidden")))

/* ---- 1. trampolines to the old symbol version -------------------------------------------------- */
#define COMPAT_PIN(name, ver)                                                                    \
    __asm__(".symver compat_old_" #name ", " #name "@" ver "\n"                                    \
            ".text\n.p2align 2\n.globl " #name "\n.hidden " #name "\n.type " #name ", %function\n" \
            #name ":\n\tb compat_old_" #name "\n.size " #name ", .-" #name "\n")

COMPAT_PIN(pthread_attr_setstacksize, "GLIBC_2.17");
COMPAT_PIN(pthread_create,            "GLIBC_2.17");
COMPAT_PIN(pthread_join,              "GLIBC_2.17");
COMPAT_PIN(pthread_mutex_trylock,     "GLIBC_2.17");
COMPAT_PIN(pthread_once,              "GLIBC_2.17");
COMPAT_PIN(pthread_setname_np,        "GLIBC_2.17");
COMPAT_PIN(timer_create,              "GLIBC_2.17");
COMPAT_PIN(timer_settime,             "GLIBC_2.17");
/* used by the static libgcc/libstdc++ (thread-local storage, exceptions, std::thread) */
COMPAT_PIN(pthread_detach,            "GLIBC_2.17");
COMPAT_PIN(pthread_getspecific,       "GLIBC_2.17");
COMPAT_PIN(pthread_setspecific,       "GLIBC_2.17");
COMPAT_PIN(pthread_key_create,        "GLIBC_2.17");
COMPAT_PIN(pthread_key_delete,        "GLIBC_2.17");
COMPAT_PIN(__pthread_key_create,      "GLIBC_2.17");

/* dlopen/dlvsym themselves moved to libc in 2.34; pin them to the old version for the resolver below. */
extern void *compat_old_dlopen(const char *, int);
extern void *compat_old_dlvsym(void *, const char *, const char *);
__asm__(".symver compat_old_dlopen, dlopen@GLIBC_2.17");
__asm__(".symver compat_old_dlvsym, dlvsym@GLIBC_2.17");

/* ---- 3. functions that only exist (or are plain symbols) on newer glibc ---------------------------- */
struct stat;
extern int __xstat(int, const char *, struct stat *);
extern int __lxstat(int, const char *, struct stat *);
extern int __fxstat(int, int, struct stat *);
extern int __fxstatat(int, int, const char *, struct stat *, int);
#define STAT_VER 0 /* _STAT_VER_LINUX on aarch64 */
HIDDEN int stat(const char *p, struct stat *b)   { return __xstat(STAT_VER, p, b); }
HIDDEN int lstat(const char *p, struct stat *b)  { return __lxstat(STAT_VER, p, b); }
HIDDEN int fstat(int fd, struct stat *b)         { return __fxstat(STAT_VER, fd, b); }
HIDDEN int fstatat(int fd, const char *p, struct stat *b, int fl) { return __fxstatat(STAT_VER, fd, p, b, fl); }
HIDDEN int stat64(const char *p, struct stat *b)  { return __xstat(STAT_VER, p, b); }
HIDDEN int lstat64(const char *p, struct stat *b) { return __lxstat(STAT_VER, p, b); }
HIDDEN int fstat64(int fd, struct stat *b)        { return __fxstat(STAT_VER, fd, b); }

/* libstdc++ headers read this (glibc 2.32+) to skip atomics in single-threaded programs. We always run
 * multi-threaded code paths, so a constant 0 is correct on every glibc. */
HIDDEN char __libc_single_threaded = 0;

/* ---- 2. startup resolution of faster glibc functions ------------------------------------------ */
typedef struct { void **slot; const char *name; const char *newer[3]; } Resolve;

#define MATH1(T, name, newer)                                                                    \
    extern T compat_old_##name(T);                                                               \
    __asm__(".symver compat_old_" #name ", " #name "@GLIBC_2.17");                                \
    static T (*name##_fn)(T) = compat_old_##name;                                                 \
    HIDDEN T name(T x) { return name##_fn(x); }
#define MATH2(T, name, newer)                                                                    \
    extern T compat_old_##name(T, T);                                                            \
    __asm__(".symver compat_old_" #name ", " #name "@GLIBC_2.17");                                \
    static T (*name##_fn)(T, T) = compat_old_##name;                                              \
    HIDDEN T name(T x, T y) { return name##_fn(x, y); }

MATH1(double, exp,   "GLIBC_2.29")
MATH1(double, exp2,  "GLIBC_2.29")
MATH1(double, log,   "GLIBC_2.29")
MATH1(double, log2,  "GLIBC_2.29")
MATH2(double, pow,   "GLIBC_2.29")
MATH1(float,  expf,  "GLIBC_2.27")
MATH1(float,  exp2f, "GLIBC_2.27")
MATH1(float,  logf,  "GLIBC_2.27")
MATH1(float,  log2f, "GLIBC_2.27")
MATH2(float,  powf,  "GLIBC_2.27")
MATH1(float,  sinf,  "GLIBC_2.27")
MATH1(float,  cosf,  "GLIBC_2.27")

/* pthread_cond_clockwait (2.30): fall back to timedwait with the deadline moved to CLOCK_REALTIME. */
static int cond_clockwait_fallback(pthread_cond_t *c, pthread_mutex_t *m, clockid_t clk, const struct timespec *abs) {
    if (clk == CLOCK_REALTIME) return pthread_cond_timedwait(c, m, abs);
    struct timespec a, r, t;
    clock_gettime(clk, &a);
    clock_gettime(CLOCK_REALTIME, &r);
    t.tv_sec = abs->tv_sec - a.tv_sec + r.tv_sec;
    t.tv_nsec = abs->tv_nsec - a.tv_nsec + r.tv_nsec;
    while (t.tv_nsec < 0) { t.tv_nsec += 1000000000L; t.tv_sec--; }
    while (t.tv_nsec >= 1000000000L) { t.tv_nsec -= 1000000000L; t.tv_sec++; }
    return pthread_cond_timedwait(c, m, &t);
}
static int (*cond_clockwait_fn)(pthread_cond_t *, pthread_mutex_t *, clockid_t, const struct timespec *) = cond_clockwait_fallback;
HIDDEN int pthread_cond_clockwait(pthread_cond_t *c, pthread_mutex_t *m, clockid_t clk, const struct timespec *abs) {
    return cond_clockwait_fn(c, m, clk, abs);
}

#define SLOT(fn, nm, v) { (void **)&fn, nm, { v, 0, 0 } }
/* 1 when the faster libm / clockwait versions of the running glibc are in use (reported in the game log).
 * NFSU2_GLIBC_COMPAT_OLD=1 forces the GLIBC_2.17 versions, to test the old-glibc path on a new system. */
int compat_newer_libm = 0;
__attribute__((constructor(101))) static void compat_resolve_newer(void) {
    if (getenv("NFSU2_GLIBC_COMPAT_OLD")) return;
    void *libm = compat_old_dlopen("libm.so.6", 2 /*RTLD_NOW*/ | 4 /*RTLD_NOLOAD*/);
    void *libc = compat_old_dlopen("libc.so.6", 2 | 4);
    if (!libm) libm = compat_old_dlopen("libm.so.6", 2);
    if (!libc) libc = compat_old_dlopen("libc.so.6", 2);
    const Resolve m[] = {
        SLOT(exp_fn, "exp", "GLIBC_2.29"),   SLOT(exp2_fn, "exp2", "GLIBC_2.29"),
        SLOT(log_fn, "log", "GLIBC_2.29"),   SLOT(log2_fn, "log2", "GLIBC_2.29"),
        SLOT(pow_fn, "pow", "GLIBC_2.29"),
        SLOT(expf_fn, "expf", "GLIBC_2.27"), SLOT(exp2f_fn, "exp2f", "GLIBC_2.27"),
        SLOT(logf_fn, "logf", "GLIBC_2.27"), SLOT(log2f_fn, "log2f", "GLIBC_2.27"),
        SLOT(powf_fn, "powf", "GLIBC_2.27"), SLOT(sinf_fn, "sinf", "GLIBC_2.27"),
        SLOT(cosf_fn, "cosf", "GLIBC_2.27"),
    };
    if (libm)
        for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) {
            void *p = compat_old_dlvsym(libm, m[i].name, m[i].newer[0]);
            if (p) { *m[i].slot = p; compat_newer_libm = 1; }
        }
    if (libc) {
        void *p = compat_old_dlvsym(libc, "pthread_cond_clockwait", "GLIBC_2.34");
        if (!p) p = compat_old_dlvsym(libc, "pthread_cond_clockwait", "GLIBC_2.30");
        if (p) *(void **)&cond_clockwait_fn = p;
    }
}

/* ---- startup/shutdown routines handed to __libc_start_main by compat_start.S --------------------- */
extern void (*__preinit_array_start[])(int, char **, char **);
extern void (*__preinit_array_end[])(int, char **, char **);
extern void (*__init_array_start[])(int, char **, char **);
extern void (*__init_array_end[])(int, char **, char **);
extern void (*__fini_array_start[])(void);
extern void (*__fini_array_end[])(void);
extern void _init(void);
extern void _fini(void);

HIDDEN void compat_csu_init(int argc, char **argv, char **envp) {
    for (size_t i = 0, n = __preinit_array_end - __preinit_array_start; i < n; i++) __preinit_array_start[i](argc, argv, envp);
    _init();
    for (size_t i = 0, n = __init_array_end - __init_array_start; i < n; i++) __init_array_start[i](argc, argv, envp);
}
HIDDEN void compat_csu_fini(void) {
    for (size_t i = __fini_array_end - __fini_array_start; i-- > 0;) __fini_array_start[i]();
    _fini();
}
