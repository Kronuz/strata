// Floors of the file work a commit does, in a plain C loop (no strata code): today's
// manifest-per-commit protocol against an append-only manifest log, with and without a new
// segment file, a turn-log-like append into preallocated space, the rollover, reopen, and
// the disk a tiny file takes. CPU is this process's user+system time per operation.
// macOS only (F_FULLFSYNC, F_BARRIERFSYNC); not part of the CMake build. docs/design/manifest-log.md
// quotes its output (results/2026-11-04-floors.txt).
// Usage: cc -O2 -o mlog_probe mlog_probe.c && mlog_probe DIR [rounds], under a strict hold.
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef F_BARRIERFSYNC
#define F_BARRIERFSYNC 85
#endif

static double cpu(void) {
	struct rusage r;
	getrusage(RUSAGE_SELF, &r);
	return r.ru_utime.tv_sec + r.ru_utime.tv_usec / 1e6 + r.ru_stime.tv_sec + r.ru_stime.tv_usec / 1e6;
}
static double wall(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}
static void die(const char* what) {
	perror(what);
	exit(1);
}
static int dirfd_g;
static char dir_g[1024];
static void path(char* out, size_t n, const char* name) { snprintf(out, n, "%s/%s", dir_g, name); }
static void xfsync(int fd) {
	if (fsync(fd) < 0) die("fsync");
}
static void xfull(int fd) {
	if (fcntl(fd, F_FULLFSYNC) < 0) die("F_FULLFSYNC");
}
static void xbarrier(int fd) {
	if (fcntl(fd, F_BARRIERFSYNC) < 0) xfull(fd);
}
// completion's PosixIO::sync_directory on macOS: fsync the directory, then F_FULLFSYNC.
static void sync_directory(void) {
	xfsync(dirfd_g);
	xfull(dirfd_g);
}
static void write_all(int fd, const char* buf, size_t n, off_t at) {
	if (pwrite(fd, buf, n, at) != (ssize_t)n) die("pwrite");
}
static int create_file(const char* name) {
	char p[1200];
	path(p, sizeof p, name);
	int fd = open(p, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
	if (fd < 0) die("create");
	return fd;
}
static void rename_name(const char* from, const char* to) {
	char a[1200], b[1200];
	path(a, sizeof a, from);
	path(b, sizeof b, to);
	if (rename(a, b) < 0) die("rename");
}
static void unlink_name(const char* name) {
	char p[1200];
	path(p, sizeof p, name);
	if (unlink(p) < 0 && errno != ENOENT) die("unlink");
}

static char seg[2048], man[700], edit[100], snap1[1024], snap64[65536];

struct result {
	const char* name;
	double cpu_us, wall_us;
};

#define RUN(label, n, body)                                         \
	do {                                                            \
		double c0 = cpu(), w0 = wall();                             \
		for (int i = 0; i < (n); i++) { body; }                     \
		double c1 = cpu(), w1 = wall();                             \
		printf("%-46s %8.1f us cpu/op %10.1f us wall/op  (n=%d)\n", label, (c1 - c0) / (n)*1e6, (w1 - w0) / (n)*1e6, (n)); \
		fflush(stdout);                                             \
	} while (0)

int main(int argc, char** argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s DIR [rounds]\n", argv[0]);
		return 2;
	}
	const int rounds = argc > 2 ? atoi(argv[2]) : 1;
	memset(seg, 's', sizeof seg);
	memset(man, 'm', sizeof man);
	memset(edit, 'e', sizeof edit);
	memset(snap1, 'p', sizeof snap1);
	memset(snap64, 'q', sizeof snap64);
	for (int r = 0; r < rounds; r++) {
		snprintf(dir_g, sizeof dir_g, "%s/r%d", argv[1], r);
		if (mkdir(dir_g, 0700) < 0 && errno != EEXIST) die("mkdir");
		dirfd_g = open(dir_g, O_RDONLY | O_DIRECTORY);
		if (dirfd_g < 0) die("open dir");
		printf("== round %d in %s\n", r, dir_g);
		char name[64], prev[64], old[64];
		const int N = 2000, NF = 300;

		// A-C: append a 100-byte edit to an open file, then each barrier kind.
		int log = create_file("log-a");
		off_t at = 0;
		RUN("A append 100 B + fsync", N, { write_all(log, edit, sizeof edit, at); at += sizeof edit; xfsync(log); });
		RUN("B append 100 B + F_FULLFSYNC", NF, { write_all(log, edit, sizeof edit, at); at += sizeof edit; xfull(log); });
		RUN("C append 100 B + F_BARRIERFSYNC", N, { write_all(log, edit, sizeof edit, at); at += sizeof edit; xbarrier(log); });
		close(log);

		// D-E: reopen the log for each edit, against open and close alone.
		char lp[1200];
		path(lp, sizeof lp, "log-a");
		RUN("D open + append 100 B + fsync + close", N, {
			int fd = open(lp, O_RDWR | O_CLOEXEC);
			if (fd < 0) die("open");
			write_all(fd, edit, sizeof edit, at);
			at += sizeof edit;
			xfsync(fd);
			close(fd);
		});
		RUN("E open existing + close", N, {
			int fd = open(lp, O_RDWR | O_CLOEXEC);
			if (fd < 0) die("open");
			close(fd);
		});

		// F-G: a rollover: the snapshot under a temporary name, synced, renamed, the directory
		// synced (completion's macOS sync_directory); the old log unlinked lazily, without a sync.
		RUN("F rollover, 1 KiB snapshot (no unlink)", NF, {
			snprintf(name, sizeof name, "m1-%d", i);
			int fd = create_file("m1.tmp");
			write_all(fd, snap1, sizeof snap1, 0);
			xfsync(fd);
			close(fd);
			rename_name("m1.tmp", name);
			sync_directory();
		});
		RUN("F' unlink old log, no directory sync", NF, {
			snprintf(name, sizeof name, "m1-%d", i);
			unlink_name(name);
		});
		RUN("G rollover, 64 KiB snapshot (no unlink)", NF, {
			snprintf(name, sizeof name, "m64-%d", i);
			int fd = create_file("m64.tmp");
			write_all(fd, snap64, sizeof snap64, 0);
			xfsync(fd);
			close(fd);
			rename_name("m64.tmp", name);
			sync_directory();
		});
		for (int i = 0; i < NF; i++) {
			snprintf(name, sizeof name, "m64-%d", i);
			unlink_name(name);
		}

		// H: today's commit protocol's file work (index.hh commit_files + retain): a new segment
		// and the manifest temporary written, one ordering point (fsync each, fsync the directory,
		// a barrier), the rename, sync_directory, then retention removes the previous manifest and
		// syncs the directory, and reclaim unlinks an older segment.
		{
			int fd = create_file("manifest-0");
			write_all(fd, man, sizeof man, 0);
			xfsync(fd);
			close(fd);
		}
		RUN("H today: segment + manifest tmp/rename/unlink", NF, {
			snprintf(name, sizeof name, "seg-%d.sx", i);
			int s = create_file(name);
			write_all(s, seg, sizeof seg, 0);
			int t = create_file("manifest.tmp");
			write_all(t, man, sizeof man, 0);
			xfsync(s);
			xfsync(t);
			xfsync(dirfd_g);
			xbarrier(dirfd_g);
			close(t);
			close(s);
			snprintf(name, sizeof name, "manifest-%d", i + 1);
			rename_name("manifest.tmp", name);
			sync_directory();
			snprintf(prev, sizeof prev, "manifest-%d", i);
			unlink_name(prev);
			sync_directory();
			if (i >= 2) {
				snprintf(old, sizeof old, "seg-%d.sx", i - 2);
				unlink_name(old);
			}
		});

		// I: the manifest log without a turn log: the segment made durable with its directory
		// entry (one ordering point), then one edit appended and synced; the older segment
		// unlinked lazily with no directory sync of its own.
		log = create_file("log-i");
		at = 0;
		RUN("I log: segment + ordering point + edit + full sync", NF, {
			snprintf(name, sizeof name, "segi-%d.sx", i);
			int s = create_file(name);
			write_all(s, seg, sizeof seg, 0);
			xfsync(s);
			xfsync(dirfd_g);
			xbarrier(dirfd_g);
			close(s);
			write_all(log, edit, sizeof edit, at);
			at += sizeof edit;
			xfull(log);
			if (i >= 2) {
				snprintf(old, sizeof old, "segi-%d.sx", i - 2);
				unlink_name(old);
			}
		});
		// J: an edit with no new file (deletes, metadata, fields, pins): the append alone.
		RUN("J log: edit alone + full sync", NF, { write_all(log, edit, sizeof edit, at); at += sizeof edit; xfull(log); });
		close(log);

		// K: a turn-log-like append into preallocated, zero-written space (no size change).
		{
			int t = create_file("turns");
			static char zero[1 << 20];
			for (int k = 0; k < 4; k++) write_all(t, zero, sizeof zero, (off_t)k << 20);
			xfull(t);
			at = 0;
			RUN("K turn-like: overwrite 150 B prealloc + fsync", N, { write_all(t, edit, sizeof edit, at); at += 150; xfsync(t); });
			RUN("K' turn-like: overwrite 150 B prealloc + F_FULLFSYNC", NF, { write_all(t, edit, sizeof edit, at); at += 150; xfull(t); });
			close(t);
		}

		// L: an open reading a whole log: open, fstat, pread, close.
		{
			int fd = create_file("read-4k");
			write_all(fd, snap64, 4096, 0);
			close(fd);
			fd = create_file("read-64k");
			write_all(fd, snap64, sizeof snap64, 0);
			close(fd);
			static char buf[65536];
			char p4[1200], p64[1200];
			path(p4, sizeof p4, "read-4k");
			path(p64, sizeof p64, "read-64k");
			RUN("L open + fstat + read 4 KiB + close", N, {
				int f = open(p4, O_RDONLY | O_CLOEXEC);
				struct stat st;
				fstat(f, &st);
				if (pread(f, buf, st.st_size, 0) != st.st_size) die("pread");
				close(f);
			});
			RUN("L open + fstat + read 64 KiB + close", N, {
				int f = open(p64, O_RDONLY | O_CLOEXEC);
				struct stat st;
				fstat(f, &st);
				if (pread(f, buf, st.st_size, 0) != st.st_size) die("pread");
				close(f);
			});
		}

		// M: the disk a tiny file takes.
		{
			int fd = create_file("tiny");
			write_all(fd, "MANIFEST-000001\n", 16, 0);
			xfsync(fd);
			struct stat st;
			fstat(fd, &st);
			printf("%-46s %8lld bytes allocated for a %lld-byte file\n", "M tiny file", (long long)st.st_blocks * 512, (long long)st.st_size);
			close(fd);
		}
		close(dirfd_g);
	}
	return 0;
}
