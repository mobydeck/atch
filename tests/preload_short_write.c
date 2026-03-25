#include <stdlib.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

static int did_inject;

static ssize_t real_write(int fd, const void *buf, size_t count)
{
	return syscall(SYS_write, fd, buf, count);
}

static int should_inject(int fd, size_t count)
{
	struct stat st;

	if (did_inject || count <= 1)
		return 0;
	if (!getenv("ATCH_FAULT_SHORT_WRITE_ONCE"))
		return 0;
	if (fstat(fd, &st) < 0)
		return 0;
	return S_ISSOCK(st.st_mode);
}

static ssize_t short_write_impl(int fd, const void *buf, size_t count)
{
	if (should_inject(fd, count)) {
		did_inject = 1;
		return real_write(fd, buf, 1);
	}
	return real_write(fd, buf, count);
}

#ifdef __APPLE__
#define DYLD_INTERPOSE(_replacement, _replacee) \
	__attribute__((used)) static struct { \
		const void *replacement; \
		const void *replacee; \
	} _interpose_##_replacee \
	__attribute__((section("__DATA,__interpose"))) = { \
		(const void *)(unsigned long)&_replacement, \
		(const void *)(unsigned long)&_replacee \
	}

ssize_t interposed_write(int fd, const void *buf, size_t count)
{
	return short_write_impl(fd, buf, count);
}

DYLD_INTERPOSE(interposed_write, write);
#else
ssize_t write(int fd, const void *buf, size_t count)
{
	return short_write_impl(fd, buf, count);
}
#endif
