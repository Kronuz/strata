// Probe (macOS): bytes written and barriers per phase and per file class, for the rocksdb-parity
// design. Interposes write, pwrite, writev, pwritev, fsync and fcntl(F_FULLFSYNC); the phase is
// the last `phase NAME` line kvdriver wrote to stderr (KV_PHASE_MARKS=1). A file's class is its
// basename with digits removed; writes whose bytes are all zero are counted apart.
//   cc -O2 -dynamiclib wbytes.c -o wbytes.dylib
//   WBYTES_LOG=out.tsv DYLD_INSERT_LIBRARIES=wbytes.dylib KV_PHASE_MARKS=1 kv_strata ...
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/uio.h>
#include <unistd.h>

#define DYLD_INTERPOSE(replacement, replacee) \
	__attribute__((used)) static struct { \
		const void* r; \
		const void* e; \
	} interpose_##replacee __attribute__((section("__DATA,__interpose"))) = {(const void*)(unsigned long)&replacement, (const void*)(unsigned long)&replacee};

enum { max_rows = 4096 };
struct row {
	char phase[24], cls[64];
	int zero;
	unsigned long long bytes, calls, full, fsyncs;
};
static struct row rows[max_rows];
static int nrows;
static char phase[24] = "start";
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

static void classify(int fd, char* out, size_t n) {
	char p[MAXPATHLEN];
	if (fcntl(fd, F_GETPATH, p) != 0) {
		snprintf(out, n, "fd?");
		return;
	}
	const char* base = strrchr(p, '/');
	base = base ? base + 1 : p;
	// the parent directory's name too: host/ (the turn log) or an index's
	char dir[64] = "";
	if (base > p + 1) {
		const char* end = base - 1;
		const char* start = end;
		while (start > p && start[-1] != '/') { --start; }
		snprintf(dir, sizeof dir, "%.*s", (int)(end - start), start);
		for (char* c = dir; *c; ++c) {
			if (*c >= '0' && *c <= '9') { *c = '#'; }
		}
	}
	size_t j = 0;
	j += snprintf(out, n, "%s/", dir);
	for (const char* c = base; *c && j + 1 < n; ++c) {
		if (*c >= '0' && *c <= '9') { continue; }
		out[j++] = *c;
	}
	out[j] = 0;
}

static struct row* at(const char* cls, int zero) {
	for (int i = 0; i < nrows; ++i) {
		if (rows[i].zero == zero && !strcmp(rows[i].cls, cls) && !strcmp(rows[i].phase, phase)) { return &rows[i]; }
	}
	if (nrows == max_rows) { return NULL; }
	struct row* r = &rows[nrows++];
	strcpy(r->phase, phase);
	snprintf(r->cls, sizeof r->cls, "%s", cls);
	r->zero = zero;
	return r;
}

static int all_zero(const void* b, size_t n) {
	const unsigned char* c = b;
	if (!n || c[0] || c[n - 1] || c[n / 2]) { return 0; }
	for (size_t i = 0; i < n; ++i) {
		if (c[i]) { return 0; }
	}
	return 1;
}

static void note(int fd, size_t n, int zero, int full, int fs) {
	char cls[64];
	classify(fd, cls, sizeof cls);
	pthread_mutex_lock(&mu);
	struct row* r = at(cls, zero);
	if (r) {
		r->bytes += n;
		r->calls += n ? 1 : 0;
		r->full += full;
		r->fsyncs += fs;
	}
	pthread_mutex_unlock(&mu);
}

static ssize_t my_write(int fd, const void* b, size_t n) {
	if (fd == 2 && n > 6 && !memcmp(b, "phase ", 6)) {
		const char* s = (const char*)b + 6;
		size_t k = 0;
		pthread_mutex_lock(&mu);
		while (k + 1 < sizeof phase && k + 6 < n && s[k] != ' ') {
			phase[k] = s[k];
			++k;
		}
		phase[k] = 0;
		pthread_mutex_unlock(&mu);
	}
	const ssize_t r = write(fd, b, n);
	if (r > 0 && fd > 2) { note(fd, (size_t)r, all_zero(b, (size_t)r), 0, 0); }
	return r;
}
static ssize_t my_pwrite(int fd, const void* b, size_t n, off_t off) {
	const ssize_t r = pwrite(fd, b, n, off);
	if (r > 0) { note(fd, (size_t)r, all_zero(b, (size_t)r), 0, 0); }
	return r;
}
static ssize_t my_writev(int fd, const struct iovec* v, int c) {
	const ssize_t r = writev(fd, v, c);
	if (r > 0 && fd > 2) { note(fd, (size_t)r, 0, 0, 0); }
	return r;
}
static ssize_t my_pwritev(int fd, const struct iovec* v, int c, off_t off) {
	const ssize_t r = pwritev(fd, v, c, off);
	if (r > 0) { note(fd, (size_t)r, 0, 0, 0); }
	return r;
}
static int my_fcntl(int fd, int cmd, ...) {
	va_list ap;
	va_start(ap, cmd);
	void* arg = va_arg(ap, void*);
	va_end(ap);
	const int r = fcntl(fd, cmd, arg);
	if (cmd == F_FULLFSYNC) { note(fd, 0, 0, 1, 0); }
	return r;
}
static int my_fsync(int fd) {
	const int r = fsync(fd);
	note(fd, 0, 0, 0, 1);
	return r;
}
DYLD_INTERPOSE(my_write, write)
DYLD_INTERPOSE(my_pwrite, pwrite)
DYLD_INTERPOSE(my_writev, writev)
DYLD_INTERPOSE(my_pwritev, pwritev)
DYLD_INTERPOSE(my_fcntl, fcntl)
DYLD_INTERPOSE(my_fsync, fsync)

static void dump(void) {
	const char* out = getenv("WBYTES_LOG");
	FILE* f = out ? fopen(out, "w") : NULL;
	if (!f) { return; }
	fprintf(f, "phase\tclass\tzero\tbytes\tcalls\tfullfsync\tfsync\n");
	for (int i = 0; i < nrows; ++i) {
		fprintf(f, "%s\t%s\t%d\t%llu\t%llu\t%llu\t%llu\n", rows[i].phase, rows[i].cls, rows[i].zero, rows[i].bytes, rows[i].calls, rows[i].full, rows[i].fsyncs);
	}
	fclose(f);
}
__attribute__((constructor)) static void init(void) { atexit(dump); }
