// Counts a process's durability barriers (macOS): every fcntl(F_FULLFSYNC) and fsync, with
// its monotonic time (CLOCK_MONOTONIC_RAW, as kvdriver's KV_PHASE_MARKS),
// duration, thread CPU time (ns), and the file's path, one line each to $BARRIERS_LOG at exit.
//   cc -O2 -dynamiclib bench/kv/barriers.c -o barriers.dylib
//   BARRIERS_LOG=out.tsv DYLD_INSERT_LIBRARIES=$PWD/barriers.dylib KV_PHASE_MARKS=1 ./kv_rocksdb ...
// BARRIERS_WEAK=1 turns each F_FULLFSYNC into a plain fsync (no drive cache flush), to attribute
// time to barriers; such a run is not durable and is never a row.
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <time.h>
#include <unistd.h>

#define DYLD_INTERPOSE(replacement, replacee) \
	__attribute__((used)) static struct { \
		const void* r; \
		const void* e; \
	} interpose_##replacee __attribute__((section("__DATA,__interpose"))) = {(const void*)(unsigned long)&replacement, (const void*)(unsigned long)&replacee};

enum { max_events = 1 << 20 };
struct event {
	unsigned long long at, ns, cpu;
	char kind;  // F: F_FULLFSYNC, S: fsync
	char path[96];
};
static struct event* events;
static volatile long count;

static unsigned long long cpu_now(void) {
	struct timespec ts;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}
static unsigned long long now(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}

static void record(int fd, char kind, unsigned long long at, unsigned long long cpu0) {
	const unsigned long long cpu = cpu_now() - cpu0;
	const unsigned long long end = now();
	const long i = __atomic_fetch_add(&count, 1, __ATOMIC_RELAXED);
	if (!events || i >= max_events) { return; }
	struct event* e = &events[i];
	e->at = at;
	e->ns = end - at;
	e->cpu = cpu;
	e->kind = kind;
	char p[MAXPATHLEN];
	if (fcntl(fd, F_GETPATH, p) == 0) {
		const size_t n = strlen(p);
		strncpy(e->path, n >= sizeof e->path ? p + n - (sizeof e->path - 1) : p, sizeof e->path - 1);
	} else {
		strcpy(e->path, "?");
	}
}

static int my_fcntl(int fd, int cmd, ...) {
	va_list ap;
	va_start(ap, cmd);
	void* arg = va_arg(ap, void*);
	va_end(ap);
	if (cmd != F_FULLFSYNC) { return fcntl(fd, cmd, arg); }
	static int weak = -1;
	if (weak < 0) { weak = getenv("BARRIERS_WEAK") != NULL; }
	const unsigned long long at = now(), c0 = cpu_now();
	const int r = weak ? fsync(fd) : fcntl(fd, cmd, arg);
	record(fd, 'F', at, c0);
	return r;
}
static int my_fsync(int fd) {
	const unsigned long long at = now(), c0 = cpu_now();
	const int r = fsync(fd);
	record(fd, 'S', at, c0);
	return r;
}
DYLD_INTERPOSE(my_fcntl, fcntl)
DYLD_INTERPOSE(my_fsync, fsync)


// Other syscalls' calls and thread CPU, summed per kind and printed to stderr at exit.
static unsigned long long sc_n[12], sc_cpu[12], sc_wall[12];
static const char* const sc_name[12] = {"open", "unlinkat", "renameat", "pwrite", "close", "openat", "ftruncate", "fchmod", "flock", "mmap", "munmap", "fstat"};
#define TIMED(idx, call) \
	const unsigned long long w0 = now(), c0 = cpu_now(); \
	const long long r = (long long)(call); \
	__atomic_fetch_add(&sc_cpu[idx], cpu_now() - c0, __ATOMIC_RELAXED); \
	__atomic_fetch_add(&sc_wall[idx], now() - w0, __ATOMIC_RELAXED); \
	__atomic_fetch_add(&sc_n[idx], 1, __ATOMIC_RELAXED);
static int my_open(const char* path, int flags, ...) {
	va_list ap;
	va_start(ap, flags);
	const int mode = va_arg(ap, int);
	va_end(ap);
	TIMED(0, open(path, flags, mode))
	return (int)r;
}
static int my_unlink(const char* p) { TIMED(1, unlink(p)) return (int)r; }
static int my_unlinkat(int d, const char* p, int f) { TIMED(1, unlinkat(d, p, f)) return (int)r; }
static int my_openat(int d, const char* path, int flags, ...) {
	va_list ap;
	va_start(ap, flags);
	const int mode = va_arg(ap, int);
	va_end(ap);
	TIMED(5, openat(d, path, flags, mode))
	return (int)r;
}
static int my_fchmod(int fd, mode_t m) { TIMED(7, fchmod(fd, m)) return (int)r; }
static int my_rename(const char* a, const char* b) { TIMED(2, rename(a, b)) return (int)r; }
static int my_renameat(int d, const char* a, int e, const char* b) { TIMED(2, renameat(d, a, e, b)) return (int)r; }
static ssize_t my_pwrite(int fd, const void* b, size_t n, off_t o) { TIMED(3, pwrite(fd, b, n, o)) return (ssize_t)r; }
static int my_close(int fd) { TIMED(4, close(fd)) return (int)r; }
DYLD_INTERPOSE(my_open, open)
DYLD_INTERPOSE(my_unlink, unlink)
DYLD_INTERPOSE(my_rename, rename)
DYLD_INTERPOSE(my_pwrite, pwrite)
static int my_ftruncate(int fd, off_t n) { TIMED(6, ftruncate(fd, n)) return (int)r; }
static int my_flock(int fd, int op) { TIMED(8, flock(fd, op)) return (int)r; }
#include <sys/mman.h>
static void* my_mmap(void* a, size_t l, int pr, int fl, int fd, off_t o) { TIMED(9, mmap(a, l, pr, fl, fd, o)) return (void*)r; }
static int my_munmap(void* a, size_t l) { TIMED(10, munmap(a, l)) return (int)r; }
static int my_fstat(int fd, struct stat* st) { TIMED(11, fstat(fd, st)) return (int)r; }
DYLD_INTERPOSE(my_ftruncate, ftruncate)
DYLD_INTERPOSE(my_flock, flock)
DYLD_INTERPOSE(my_mmap, mmap)
DYLD_INTERPOSE(my_munmap, munmap)
DYLD_INTERPOSE(my_fstat, fstat)
DYLD_INTERPOSE(my_close, close)
DYLD_INTERPOSE(my_unlinkat, unlinkat)
DYLD_INTERPOSE(my_openat, openat)
DYLD_INTERPOSE(my_fchmod, fchmod)
DYLD_INTERPOSE(my_renameat, renameat)
static void dump_sc(void) {
	for (int i = 0; i < 12; ++i) { fprintf(stderr, "syscall %s n=%llu cpu_ms=%.2f wall_ms=%.2f\n", sc_name[i], sc_n[i], sc_cpu[i] / 1e6, sc_wall[i] / 1e6); }
	fprintf(stderr, "main thread cpu_ms=%.2f\n", cpu_now() / 1e6);
}
static void dump(void) {
	dump_sc();
	const char* out = getenv("BARRIERS_LOG");
	FILE* f = out ? fopen(out, "w") : NULL;
	if (!f) { return; }
	const long n = count < max_events ? count : max_events;
	for (long i = 0; i < n; ++i) { fprintf(f, "%llu\t%llu\t%llu\t%c\t%s\n", events[i].at, events[i].ns, events[i].cpu, events[i].kind, events[i].path); }
	fclose(f);
}

__attribute__((constructor)) static void init(void) {
	events = calloc(max_events, sizeof *events);
	atexit(dump);
}
