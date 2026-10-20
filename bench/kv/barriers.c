// Counts a process's durability barriers (macOS): every fcntl(F_FULLFSYNC) and fsync, with
// its monotonic time (CLOCK_MONOTONIC_RAW, as kvdriver's KV_PHASE_MARKS),
// duration, and the file's path, one line each to $BARRIERS_LOG at exit.
//   cc -O2 -dynamiclib bench/kv/barriers.c -o barriers.dylib
//   BARRIERS_LOG=out.tsv DYLD_INSERT_LIBRARIES=$PWD/barriers.dylib KV_PHASE_MARKS=1 ./kv_rocksdb ...
// BARRIERS_WEAK=1 turns each F_FULLFSYNC into a plain fsync (no drive cache flush), to attribute
// time to barriers; such a run is not durable and is never a row.
#include <fcntl.h>
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
	unsigned long long at, ns;
	char kind;  // F: F_FULLFSYNC, S: fsync
	char path[96];
};
static struct event* events;
static volatile long count;

static unsigned long long now(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}

static void record(int fd, char kind, unsigned long long at) {
	const unsigned long long end = now();
	const long i = __atomic_fetch_add(&count, 1, __ATOMIC_RELAXED);
	if (!events || i >= max_events) { return; }
	struct event* e = &events[i];
	e->at = at;
	e->ns = end - at;
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
	const unsigned long long at = now();
	const int r = weak ? fsync(fd) : fcntl(fd, cmd, arg);
	record(fd, 'F', at);
	return r;
}
static int my_fsync(int fd) {
	const unsigned long long at = now();
	const int r = fsync(fd);
	record(fd, 'S', at);
	return r;
}
DYLD_INTERPOSE(my_fcntl, fcntl)
DYLD_INTERPOSE(my_fsync, fsync)

static void dump(void) {
	const char* out = getenv("BARRIERS_LOG");
	FILE* f = out ? fopen(out, "w") : NULL;
	if (!f) { return; }
	const long n = count < max_events ? count : max_events;
	for (long i = 0; i < n; ++i) { fprintf(f, "%llu\t%llu\t%c\t%s\n", events[i].at, events[i].ns, events[i].kind, events[i].path); }
	fclose(f);
}

__attribute__((constructor)) static void init(void) {
	events = calloc(max_events, sizeof *events);
	atexit(dump);
}
