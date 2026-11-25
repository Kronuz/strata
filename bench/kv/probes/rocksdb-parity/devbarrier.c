// Probe E (rocksdb-parity design, macOS): T threads, each committing 4 KiB records durably in a
// closed loop, three ways:
//   perlog  each thread its own preallocated file and its own F_FULLFSYNC (one log per loop)
//   shared  one preallocated file, group commit: a flush covers every write before it started
//   device  each thread its own file and F_BARRIERFSYNC (data to the device, ordered), then group
//           commit on one F_FULLFSYNC per device (per-loop logs, one barrier per device)
// Prints commits/s, flushes/s and the commit latency p50/p99.
//   cc -O2 devbarrier.c -o devbarrier -lpthread && ./devbarrier DIR SECONDS [COHORT_US=1000]
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>

enum { rec = 4096, file_bytes = 16 << 20, max_threads = 8, max_lat = 1 << 20 };
static const char* dir;
static int mode, threads, fds[max_threads], shared_fd;
static volatile int stop;
static atomic_ullong commits, flushes, shared_off;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static unsigned long long started_gen, done_gen;  // flushes started, completed
static int flushing;
static double lat[max_lat];
static atomic_long nlat;

static double now(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}
static int prealloc(const char* name) {
	char p[1024];
	snprintf(p, sizeof p, "%s/%s", dir, name);
	int fd = open(p, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) { perror(p); exit(1); }
	static char z[1 << 20];
	for (long o = 0; o < file_bytes; o += sizeof z) {
		if (pwrite(fd, z, sizeof z, o) != (ssize_t)sizeof z) { perror("pwrite"); exit(1); }
	}
	fcntl(fd, F_FULLFSYNC);
	return fd;
}
// Group commit with a cohort wait (the note's D5): the leader waits up to `cohort_us` for every
// writer to arrive, then one flush covers every write that arrived before it started.
static int cohort_us;
static int pending;
static void group_flush(int fd) {
	pthread_mutex_lock(&mu);
	++pending;
	const unsigned long long need = started_gen + 1;  // the next flush to start covers this write
	if (pending == threads) { pthread_cond_broadcast(&cv); }
	struct timespec deadline = {0, 0};
	for (;;) {
		if (done_gen >= need) { break; }
		if (!flushing) {
			if (pending < threads && cohort_us > 0) {
				struct timeval tv;
				if (!deadline.tv_sec) {
					gettimeofday(&tv, NULL);
					long ns = (tv.tv_usec + cohort_us) * 1000L;
					deadline.tv_sec = tv.tv_sec + ns / 1000000000L;
					deadline.tv_nsec = ns % 1000000000L;
				}
				if (pthread_cond_timedwait(&cv, &mu, &deadline) == 0) { continue; }
				gettimeofday(&tv, NULL);
				if (tv.tv_sec < deadline.tv_sec || (tv.tv_sec == deadline.tv_sec && tv.tv_usec * 1000L < deadline.tv_nsec)) { continue; }
			}
			if (done_gen >= need || flushing) { continue; }
			flushing = 1;
			pending = 0;
			const unsigned long long mine = ++started_gen;
			pthread_mutex_unlock(&mu);
			fcntl(fd, F_FULLFSYNC);
			atomic_fetch_add(&flushes, 1);
			pthread_mutex_lock(&mu);
			flushing = 0;
			done_gen = mine;
			pthread_cond_broadcast(&cv);
			continue;
		}
		pthread_cond_wait(&cv, &mu);
	}
	pthread_mutex_unlock(&mu);
}
static void* run(void* arg) {
	const int id = (int)(long)arg;
	char buf[rec];
	memset(buf, 'a' + id, sizeof buf);
	long off = 0;
	while (!stop) {
		const double t0 = now();
		if (mode == 1) {
			const unsigned long long o = atomic_fetch_add(&shared_off, rec) % file_bytes;
			pwrite(shared_fd, buf, rec, (off_t)o);
			group_flush(shared_fd);
		} else {
			pwrite(fds[id], buf, rec, off);
			off = (off + rec) % file_bytes;
			if (mode == 0) {
				fcntl(fds[id], F_FULLFSYNC);
				atomic_fetch_add(&flushes, 1);
			} else {
				fcntl(fds[id], F_BARRIERFSYNC);
				group_flush(fds[id]);
			}
		}
		atomic_fetch_add(&commits, 1);
		const long i = atomic_fetch_add(&nlat, 1);
		if (i < max_lat) { lat[i] = now() - t0; }
	}
	return NULL;
}
static int cmp(const void* a, const void* b) {
	const double x = *(const double*)a, y = *(const double*)b;
	return x < y ? -1 : x > y;
}
int main(int argc, char** argv) {
	if (argc < 3) { fprintf(stderr, "devbarrier DIR SECONDS [COHORT_US]\n"); return 2; }
	dir = argv[1];
	const double secs = atof(argv[2]);
	for (int i = 0; i < max_threads; ++i) {
		char n[32];
		snprintf(n, sizeof n, "log-%02d", i);
		fds[i] = prealloc(n);
	}
	shared_fd = fds[0];
	cohort_us = argc > 3 ? atoi(argv[3]) : 1000;
	const char* names[] = {"perlog", "shared", "device"};
	for (int t = 1; t <= 8; t *= 2) {
		for (mode = 0; mode < 3; ++mode) {
			threads = t;
			stop = 0;
			commits = flushes = 0;
			nlat = 0;
			started_gen = done_gen = 0;
			pending = 0;
			pthread_t th[max_threads];
			const double t0 = now();
			for (long i = 0; i < t; ++i) { pthread_create(&th[i], NULL, run, (void*)i); }
			while (now() - t0 < secs) { usleep(10000); }
			stop = 1;
			for (int i = 0; i < t; ++i) { pthread_join(th[i], NULL); }
			const double el = now() - t0;
			long n = nlat < max_lat ? nlat : max_lat;
			qsort(lat, (size_t)n, sizeof lat[0], cmp);
			printf("cohort=%dus T=%d %-7s commits/s %7.1f flushes/s %6.1f commits/flush %5.2f p50 %6.2f ms p99 %6.2f ms\n", cohort_us, t, names[mode], commits / el, flushes / el,
				flushes ? (double)commits / flushes : 0.0, n ? lat[n / 2] * 1e3 : 0, n ? lat[n * 99 / 100] * 1e3 : 0);
			fflush(stdout);
		}
	}
	for (int i = 0; i < max_threads; ++i) { close(fds[i]); }
	return 0;
}
