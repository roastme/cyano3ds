// SPDX-License-Identifier: GPL-2.0
/*
 * androidtest.c - smoke-test the kernel interfaces Android needs
 *
 * Android's `init` segfaulted (kernel panic "Attempted to kill init!
 * exitcode=0x0000000b" = SIGSEGV in PID 1) and produced no log output before
 * dying, so guessing which interface upset it was wasteful.  This tool checks
 * each interface the port provides, in the same order the Android userspace
 * touches them, and prints one line per check - to the console and (because the
 * init redirects it) to the SD card.
 *
 * The interesting ones:
 *   /dev/ashmem   - libcutils' ashmem_create_region(), then mmap + write.
 *                   Android's init mmaps a 32 KiB property area and *writes* to
 *                   it; if SET_SIZE works but the mapping is not backed, that
 *                   write is a segfault in PID 1 - which is exactly the panic
 *                   we saw.  So: set name, set size, mmap, write every page,
 *                   read back.
 *   /dev/binder   - open + BINDER_VERSION (prints the protocol version the
 *                   kernel offers: mainline binder says 8, Android's libbinder is
 *                   from the v7 era, so this number matters) + mmap.
 *   /dev/log/main - liblog's record format, end to end: if this works,
 *                   `logdump` will show the line we wrote.
 *   /dev/kmsg     - Android's init logs through this, which is how we see how far
 *                   it gets.
 *   /dev/graphics/fb0 and /dev/fb1 - what EGLDisplaySurface will open.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <linux/fb.h>

/*
 * NOTE: the ashmem ioctl *type* is 0x77 (__ASHMEMIOC), not 'a'!
 * Getting this wrong makes every ashmem ioctl fail with ENOTTY and looks
 * exactly like a broken driver - it cost one hardware boot to notice.
 * Values taken from the vendored uapi header (include/uapi/linux/ashmem.h).
 */
#ifndef ASHMEM_NAME_LEN
#define ASHMEM_NAME_LEN 256
#endif
#define __ASHMEMIOC 0x77
#define ASHMEM_SET_NAME _IOW(__ASHMEMIOC, 1, char[ASHMEM_NAME_LEN])
#define ASHMEM_SET_SIZE _IOW(__ASHMEMIOC, 3, size_t)
#define ASHMEM_GET_SIZE _IO(__ASHMEMIOC, 4)

#define BINDER_VERSION _IOWR('b', 9, struct binder_version)
struct binder_version { int protocol_version; };

/* 32-bit ABI: 6 x __u32 = 24 bytes.  mainline without BINDER_IPC_32BIT
 * makes these __u64, which changes the ioctl numbers -> ENOTTY. */
#define BINDER_WRITE_READ _IOWR('b', 1, struct binder_write_read)
struct binder_write_read {
	unsigned int write_size;
	unsigned int write_consumed;
	unsigned int write_buffer;
	unsigned int read_size;
	unsigned int read_consumed;
	unsigned int read_buffer;
};

struct logger_entry {
	unsigned short len;
	unsigned short __pad;
	int pid;
	int tid;
	int sec;
	int nsec;
	char msg[0];
} __attribute__((packed));

static void ok(const char *what, const char *fmt, ...);
static void bad(const char *what, const char *fmt, ...);

#include <stdarg.h>

static void emit(const char *tag, const char *what, const char *fmt, va_list ap)
{
	printf("%-8s %-14s ", tag, what);
	vprintf(fmt, ap);
	printf("\n");
	fflush(stdout);
}

static void ok(const char *what, const char *fmt, ...)
{
	va_list ap; va_start(ap, fmt); emit("OK", what, fmt, ap); va_end(ap);
}
static void bad(const char *what, const char *fmt, ...)
{
	va_list ap; va_start(ap, fmt); emit("FAIL", what, fmt, ap); va_end(ap);
}

/* ------------------------------------------------------------------ tests */

/* Report whether a driver is at least present in sysfs, so "device node
 * missing" can be told apart from "driver broken". */
static void note_sysfs(const char *what, const char *name)
{
	char path[128];
	FILE *f;

	snprintf(path, sizeof(path), "/sys/class/misc/%s/dev", name);
	f = fopen(path, "r");
	if (!f) {
		bad(what, "no device node and no %s (driver not registered?)", path);
		return;
	}
	{
		char dev[64] = "?";

		if (fgets(dev, sizeof(dev), f)) {
			char *nl = strchr(dev, '\n');

			if (nl)
				*nl = 0;
		}
		bad(what, "no device node, but %s exists (%s)", path, dev);
	}
	fclose(f);
}

/*
 * mmap the ashmem region, write a NON-ZERO pattern to every page and read it
 * back.  The first version of this test wrote (unsigned char)i at i = 0, 4096,
 * ... - which is always 0 - so it would have passed even with a completely
 * broken mapping.  Non-zero values force real page faults.
 * Returns 0 ok, -1 mmap failed, -2 verify failed.
 */
static int ashmem_map_check(int fd, size_t size, int shared)
{
	unsigned char *p;
	int i, rc = 0;

	p = mmap(NULL, size, PROT_READ | PROT_WRITE,
		 shared ? MAP_SHARED : MAP_PRIVATE, fd, 0);
	if (p == MAP_FAILED)
		return -1;
	for (i = 0; i < (int)size; i += 4096)
		p[i] = (unsigned char)(0xa0 + (i / 4096));
	for (i = 0; i < (int)size; i += 4096) {
		if (p[i] != (unsigned char)(0xa0 + (i / 4096))) {
			rc = -2;
			break;
		}
	}
	munmap(p, size);
	return rc;
}

/*
 * The kuser "software TLS" slot.
 *
 * Android's bionic (API 10) does not read the hardware TLS register: __get_tls()
 * loads its thread pointer from the kernel's kuser helper page at 0xffff0ff0.
 * A kernel built for CONFIG_CPU_V6 keeps the TLS value in the hardware register
 * only (asm/tls.h: tls_emu == 0) and never writes that slot, so every static
 * Android binary segfaults on its first TLS access - which is precisely how
 * init died ("fault address=0x8" one instruction after __ARM_NR_set_tls).
 *
 * This test sets a TLS value through the syscall and reads the slot back, so
 * the requirement is verified on the device instead of inferred from a crash.
 */
#define KUSER_TLS_SLOT 0xffff0ff0UL
#define __ARM_NR_set_tls 0x0f0005

static void test_kuser_tls(void)
{
	unsigned long wanted = 0x12345000UL;
	register unsigned long r0 asm("r0") = wanted;
	register long r7 asm("r7") = __ARM_NR_set_tls;
	volatile unsigned long *slot = (volatile unsigned long *)KUSER_TLS_SLOT;
	unsigned long got;

	asm volatile("svc 0\n" : "+r"(r0) : "r"(r7) : "memory");
	got = *slot;
	if (got == wanted)
		ok("kuser-tls", "kernel maintains the software TLS slot (%#lx)", got);
	else
		bad("kuser-tls", "slot reads %#lx, want %#lx - bionic will fault "
		    "on its first TLS access (CONFIG_TLS_REG_EMUL?)", got, wanted);
}

static void test_ashmem_big(void)
{
	/* The exact allocation dalvik's GC makes (vm/alloc/MarkSweep.c:
	 * ashmem_create_region("dalvik-heap-markstack", 700416)).  It works
	 * for the VM heap but fails at zygote's first GC, so isolate whether
	 * it is the size or the process state. */
	const size_t size = 700416;
	char name[ASHMEM_NAME_LEN] = "dalvik-heap-markstack";
	int fd = open("/dev/ashmem", O_RDWR);

	if (fd < 0) {
		bad("ashmem-big", "open: %s", strerror(errno));
		return;
	}
	if (ioctl(fd, ASHMEM_SET_NAME, name) < 0) {
		bad("ashmem-big", "SET_NAME: %s", strerror(errno));
	} else if (ioctl(fd, ASHMEM_SET_SIZE, size) < 0) {
		bad("ashmem-big", "SET_SIZE(%zu): %s", size, strerror(errno));
	} else if (ashmem_map_check(fd, size, 1) == 0) {
		ok("ashmem-big", "%zu-byte mark-stack-sized region OK", size);
	} else {
		bad("ashmem-big", "mmap/write %zu: %s", size, strerror(errno));
	}
	close(fd);
}

static void test_ashmem(void)
{
	const size_t size = 32 * 1024;	/* Android's property area is 32 KiB */
	char name[ASHMEM_NAME_LEN] = "android-3ds-test";
	int i, fd, r;

	fd = open("/dev/ashmem", O_RDWR);
	if (fd < 0) {
		note_sysfs("ashmem", "ashmem");
		return;
	}
	if (ioctl(fd, ASHMEM_SET_NAME, name) < 0) {
		bad("ashmem", "SET_NAME: %s", strerror(errno));
		goto out;
	}
	if (ioctl(fd, ASHMEM_SET_SIZE, size) < 0) {
		bad("ashmem", "SET_SIZE(%zu): %s", size, strerror(errno));
		goto out;
	}
	/* ASHMEM_GET_SIZE returns the size as the ioctl *return value* (it does
	 * not fill a pointer), which the first version of this test got wrong. */
	r = ioctl(fd, ASHMEM_GET_SIZE, 0);
	if (r < 0)
		bad("ashmem", "GET_SIZE: %s", strerror(errno));
	else if ((size_t)r != size)
		bad("ashmem", "GET_SIZE returned %d, want %zu", r, size);
	else
		ok("ashmem", "SET_SIZE/GET_SIZE round trip = %d bytes", r);

	for (i = 0; i < 2; i++) {
		r = ashmem_map_check(fd, size, i == 0);
		if (r == 0)
			ok("ashmem", "mmap %s, wrote+verified %d non-zero pages",
			   i == 0 ? "MAP_SHARED " : "MAP_PRIVATE",
			   (int)(size / 4096));
		else if (r == -1)
			bad("ashmem", "mmap %s: %s",
			    i == 0 ? "MAP_SHARED " : "MAP_PRIVATE",
			    strerror(errno));
		else
			bad("ashmem", "%s write/read mismatch (mapping not backed!)",
			    i == 0 ? "MAP_SHARED " : "MAP_PRIVATE");
	}
out:
	close(fd);
}

/* Plain "does this device exist and open" check for the nodes Android needs. */
static void test_dev(const char *path)
{
	int fd = open(path, O_RDWR);

	if (fd < 0)
		bad(path, "open: %s", strerror(errno));
	else {
		ok(path, "open ok");
		close(fd);
	}
}

static void test_binder(void)
{
	struct binder_version ver = { 0 };
	void *m;
	int fd = open("/dev/binder", O_RDWR);

	if (fd < 0) {
		note_sysfs("binder", "binder");
		return;
	}
	if (ioctl(fd, BINDER_VERSION, &ver) < 0) {
		bad("binder", "BINDER_VERSION: %s", strerror(errno));
	} else {
		/* Android's libbinder is from the protocol-7 era; if it hard-checks
		 * this, a mismatch is fatal for servicemanager/zygote. */
		printf("%-8s %-14s protocol version %d%s\n", "OK", "binder",
		       ver.protocol_version,
		       ver.protocol_version == 7 ? " (matches CM7 libbinder)" : " (CM7 expects 7)");
		fflush(stdout);
	}
	m = mmap(NULL, 128 * 1024, PROT_READ, MAP_PRIVATE, fd, 0);
	if (m == MAP_FAILED)
		bad("binder", "mmap: %s", strerror(errno));
	else {
		ok("binder", "mmap ok");
		munmap(m, 128 * 1024);
	}
	/* binder_loop()'s ioctl: this is where a 32-vs-64-bit ABI mismatch
	 * shows up as ENOTTY (the command encodes the struct size) */
	{
		struct binder_write_read bwr;
		int r;

		memset(&bwr, 0, sizeof(bwr));
		errno = 0;
		r = ioctl(fd, BINDER_WRITE_READ, &bwr);
		printf("%-8s %-14s BINDER_WRITE_READ -> %d errno %d (%s)\n",
		       r < 0 ? "BAD" : "OK", "binder", r, errno,
		       strerror(errno));
		fflush(stdout);
	}
	close(fd);
}

static void test_logger(void)
{
	static const char tag[] = "androidtest";
	static const char text[] = "logger write test from the bring-up initramfs";
	char buf[256];
	int fd, len;

	fd = open("/dev/log/main", O_WRONLY);
	if (fd < 0) {
		bad("logger", "open /dev/log/main: %s", strerror(errno));
		return;
	}
	/* Classic 2009 ABI: write ONLY [priority][tag NUL][text]; the kernel
	 * adds the logger_entry header.  Sending a header too is what made the
	 * (then-buggy) driver drop the first 20 bytes of every Android line. */
	len = 1 + sizeof(tag) + sizeof(text);
	memset(buf, 0, sizeof(buf));
	buf[0] = '4';			/* ANDROID_LOG_INFO */
	memcpy(buf + 1, tag, sizeof(tag));
	memcpy(buf + 1 + sizeof(tag), text, sizeof(text));
	if (write(fd, buf, len) != (ssize_t)len) {
		bad("logger", "write: %s", strerror(errno));
	} else {
		ok("logger", "wrote one record to /dev/log/main");
	}
	close(fd);
}

/*
 * The mark-stack failure in zygote looked like a permission problem on
 * /dev/ashmem, so check that a non-root process can open the kernel interfaces
 * at all.  uid 1000 (system) is what the framework's system_server runs as;
 * this is run while still root from the bring-up initramfs.
 */
static void test_nonroot_access(void)
{
	pid_t pid = fork();
	int st = 0;

	if (pid < 0) {
		bad("nonroot", "fork: %s", strerror(errno));
		return;
	}
	if (pid == 0) {
		int fd;

		if (setgid(1000) != 0 || setuid(1000) != 0) {
			printf("FAIL     nonroot         setuid(1000): %s\n",
			       strerror(errno));
			fflush(stdout);
			_exit(1);
		}
		fd = open("/dev/ashmem", O_RDWR);
		if (fd < 0)
			printf("FAIL     nonroot         uid1000 open /dev/ashmem: %s\n",
			       strerror(errno));
		else {
			printf("OK       nonroot         uid1000 open /dev/ashmem\n");
			close(fd);
		}
		fd = open("/dev/binder", O_RDWR);
		if (fd < 0)
			printf("FAIL     nonroot         uid1000 open /dev/binder: %s\n",
			       strerror(errno));
		else {
			printf("OK       nonroot         uid1000 open /dev/binder\n");
			close(fd);
		}
		fflush(stdout);
		_exit(0);
	}
	waitpid(pid, &st, 0);
}

static void test_kmsg(void)
{
	int fd = open("/dev/kmsg", O_WRONLY);

	if (fd < 0) {
		bad("kmsg", "open: %s", strerror(errno));
		return;
	}
	if (write(fd, "<6>androidtest: kmsg write test\n", 32) != 32)
		bad("kmsg", "write: %s", strerror(errno));
	else
		ok("kmsg", "wrote a line (Android's init logs through this)");
	close(fd);
}

static void test_fb(const char *path)
{
	struct fb_var_screeninfo var;
	int fd = open(path, O_RDWR);

	if (fd < 0) {
		bad(path, "open: %s", strerror(errno));
		return;
	}
	if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0)
		bad(path, "FBIOGET_VSCREENINFO: %s", strerror(errno));
	else
		ok(path, "%ux%u %ubpp line_length=? (what EGLDisplaySurface opens)",
		   var.xres, var.yres, var.bits_per_pixel);
	close(fd);
}

/*
 * cacheflush(2) - what libpixelflinger does right after JIT-ing a scanline.
 *
 * The framework hangs with SurfaceFlinger parked on the cacheflush syscall
 * return address (libc.so+0xd35c) right after PixelFlinger logs its first
 * generated scanline, so exercise the syscall directly here, before Android
 * starts.  Print before *and* after every call so a hang names the exact case.
 */
static void test_cacheflush(void)
{
	const long NR_cacheflush = 0x0f0002;	/* __ARM_NR_cacheflush */
	void *page, *heap;
	char stackbuf[256];
	long r;
	int i;

	page = mmap(NULL, 64 * 1024, PROT_READ | PROT_WRITE | PROT_EXEC,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	heap = malloc(256);
	if (heap)
		memset(heap, 0, 256);

	printf("--- cacheflush ---\n");
	fflush(stdout);

#define CF(label, a, b) \
	do { \
		printf("cacheflush: %-8s %08lx..%08lx ... ", label, \
		       (unsigned long)(a), (unsigned long)(b)); \
		fflush(stdout); \
		errno = 0; \
		r = syscall(NR_cacheflush, (long)(a), (long)(b), 0); \
		printf("ret=%ld errno=%d\n", r, errno); \
		fflush(stdout); \
	} while (0)

	/* a freshly mmap'ed executable page (the PixelFlinger-like case) */
	if (page != MAP_FAILED) {
		CF("mmap256", page, (char *)page + 256);
		CF("mmap4k", page, (char *)page + 4096);
		CF("mmap64k", page, (char *)page + 65536);
	} else {
		printf("cacheflush: mmap failed: %s\n", strerror(errno));
	}

	/* a malloc'ed (heap) block, like Android's Assembly */
	if (heap)
		CF("heap", heap, (char *)heap + 256);

	/* same buffer repeatedly (PixelFlinger caches many assemblies) */
	for (i = 0; i < 3; i++)
		CF("again", heap ? heap : stackbuf,
		   (char *)(heap ? heap : stackbuf) + 256);

	/* the stack */
	CF("stack", stackbuf, stackbuf + sizeof(stackbuf));

	/* an unmapped address must return -EFAULT, not hang */
	CF("unmapped", 0x50000000UL, 0x50000100UL);

	/*
	 * The real thing: write a tiny function (mov r0,#42; bx lr) into the
	 * executable page, flush the caches, then execute it.  This is exactly
	 * what PixelFlinger does with its generated scanline.  If the flush is
	 * ineffective the CPU runs stale I-cache data (hang/garbage); if the
	 * syscall hangs we never reach the printf.
	 */
	if (page != MAP_FAILED) {
		volatile unsigned int *code = (unsigned int *)page;
		int v;

		code[0] = 0xe3a0002a;	/* mov r0, #42 */
		code[1] = 0xe12fff1e;	/* bx  lr     */
		__sync_synchronize();
		printf("cacheflush: jit-flush ... ");
		fflush(stdout);
		errno = 0;
		r = syscall(NR_cacheflush, (long)code, (long)code + 8, 0);
		printf("ret=%ld errno=%d\n", r, errno);
		fflush(stdout);
		printf("cacheflush: jit-exec ... ");
		fflush(stdout);
		v = ((int (*)(void))code)();
		printf("returned %d (want 42)\n", v);
		fflush(stdout);
	}

#undef CF
	printf("--- end cacheflush ---\n");
	fflush(stdout);
}

int main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	printf("=== android interface smoke test ===\n");
	fflush(stdout);

	/* binder first: if a later test wedges us, this is the important one */
	test_binder();
	test_ashmem();
	test_ashmem_big();
	/* NOTE: test_kuser_tls() is deliberately NOT called.  It does a real
	 * set_tls(0x12345000), which this glibc binary cannot survive: the
	 * kernel also loads that fake pointer into TPIDRURO and glibc's next
	 * TLS access faults.  Android's own (bionic) init exercises the same
	 * path, so it is covered where it matters. */
	test_logger();
	test_kmsg();
	test_nonroot_access();
	test_fb("/dev/graphics/fb0");
	test_fb("/dev/graphics/fb1");
	test_fb("/dev/fb0");
	test_fb("/dev/fb1");
	test_dev("/dev/null");
	test_dev("/dev/zero");
	test_dev("/dev/console");
	test_dev("/dev/tty0");
	test_dev("/dev/urandom");
	test_dev("/dev/input/event0");
	test_dev("/dev/input/event1");
	test_cacheflush();

	printf("=== end of smoke test ===\n");
	fflush(stdout);
	return 0;
}
