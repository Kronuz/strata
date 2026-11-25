// Probe helper (rocksdb-parity design, macOS): DYLD-inserted, turns fsync, fdatasync and
// fcntl(F_FULLFSYNC, F_BARRIERFSYNC) into counted no-ops, so a probe measures CPU and bytes, not
// device time. nosync_count() returns the count.
#include <fcntl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <unistd.h>

#define DYLD_INTERPOSE(_new, _old) \
	__attribute__((used)) static struct { const void* n; const void* o; } _interpose_##_old __attribute__((section("__DATA,__interpose"))) = {(const void*)(unsigned long)&_new, (const void*)(unsigned long)&_old};

static atomic_ullong skipped;
unsigned long long nosync_count(void) { return skipped; }
static int my_fsync(int fd) { (void)fd; ++skipped; return 0; }
static int my_fcntl(int fd, int cmd, ...) {
	va_list ap;
	va_start(ap, cmd);
	void* arg = va_arg(ap, void*);
	va_end(ap);
	if (cmd == F_FULLFSYNC || cmd == F_BARRIERFSYNC) { ++skipped; return 0; }
	return fcntl(fd, cmd, arg);
}
DYLD_INTERPOSE(my_fsync, fsync)
DYLD_INTERPOSE(my_fcntl, fcntl)
