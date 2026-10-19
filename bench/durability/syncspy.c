// Counts fsync and fcntl(F_FULLFSYNC) calls (macOS, DYLD_INSERT_LIBRARIES).
#include <fcntl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <unistd.h>

static atomic_ulong n_fsync, n_fullfsync, n_barrierfsync;

static int spy_fsync(int fd) {
	atomic_fetch_add(&n_fsync, 1);
	return fsync(fd);
}

static int spy_fcntl(int fd, int cmd, ...) {
	va_list ap;
	va_start(ap, cmd);
	void* arg = va_arg(ap, void*);
	va_end(ap);
	if (cmd == F_FULLFSYNC) { atomic_fetch_add(&n_fullfsync, 1); }
#ifdef F_BARRIERFSYNC
	if (cmd == F_BARRIERFSYNC) { atomic_fetch_add(&n_barrierfsync, 1); }
#endif
	return fcntl(fd, cmd, arg);
}

__attribute__((used)) static const struct { const void* replacement; const void* replacee; } interposers[]
	__attribute__((section("__DATA,__interpose"))) = {
		{(const void*)spy_fsync, (const void*)fsync},
		{(const void*)spy_fcntl, (const void*)fcntl},
};

void syncspy_counts(unsigned long* fsyncs, unsigned long* fullfsyncs, unsigned long* barriers) {
	*fsyncs = atomic_load(&n_fsync);
	*fullfsyncs = atomic_load(&n_fullfsync);
	*barriers = atomic_load(&n_barrierfsync);
}
