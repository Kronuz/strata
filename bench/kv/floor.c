// M4 (standalone-turn-log): the append-and-sync floor K', a turn's minimum. One pwrite of BYTES
// into a preallocated, synced file, then completion's File::sync_data (F_FULLFSYNC on macOS,
// fdatasync elsewhere), N times; prints the latency percentiles and the CPU per iteration.
//   cc -O2 bench/kv/floor.c -o floor && ./floor DIR [N=2000] [BYTES=4096]
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

static double now_us(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}
static double cpu_us(void) {
	struct rusage u;
	getrusage(RUSAGE_SELF, &u);
	return (u.ru_utime.tv_sec + u.ru_stime.tv_sec) * 1e6 + u.ru_utime.tv_usec + u.ru_stime.tv_usec;
}
static int sync_data(int fd) {
#ifdef F_FULLFSYNC
	return fcntl(fd, F_FULLFSYNC);
#else
	return fdatasync(fd);
#endif
}
static int cmp(const void* a, const void* b) {
	double x = *(const double*)a, y = *(const double*)b;
	return x < y ? -1 : x > y;
}

int main(int argc, char** argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: floor DIR [N] [BYTES]\n");
		return 2;
	}
	const long n = argc > 2 ? atol(argv[2]) : 2000;
	const size_t bytes = argc > 3 ? (size_t)atol(argv[3]) : 4096;
	char path[4096];
	snprintf(path, sizeof path, "%s/floor.%d", argv[1], (int)getpid());
	const int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	char* buf = malloc(bytes);
	memset(buf, 'x', bytes);
	// Preallocated and synced: the loop writes no metadata, as a turn log's segment.
	for (long i = 0; i < n; ++i) {
		if (pwrite(fd, buf, bytes, (off_t)(i * bytes)) != (ssize_t)bytes) {
			perror("pwrite");
			return 1;
		}
	}
	if (fsync(fd) || sync_data(fd)) {
		perror("sync");
		return 1;
	}
	double* lat = malloc(sizeof(double) * (size_t)n);
	const double c0 = cpu_us(), w0 = now_us();
	for (long i = 0; i < n; ++i) {
		const double t = now_us();
		buf[0] = (char)i;
		if (pwrite(fd, buf, bytes, (off_t)(i * bytes)) != (ssize_t)bytes || sync_data(fd)) {
			perror("append");
			return 1;
		}
		lat[i] = now_us() - t;
	}
	const double wall = now_us() - w0, cpu = cpu_us() - c0;
	qsort(lat, (size_t)n, sizeof(double), cmp);
	printf("{\"n\":%ld,\"bytes\":%zu,\"per_sec\":%.1f,\"p50_us\":%.1f,\"p99_us\":%.1f,\"p999_us\":%.1f,\"cpu_us_per_op\":%.2f}\n", n, bytes, n / (wall / 1e6),
		lat[n / 2], lat[n * 99 / 100], lat[n * 999 / 1000], cpu / n);
	close(fd);
	unlink(path);
	return 0;
}
