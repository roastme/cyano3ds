// SPDX-License-Identifier: GPL-2.0
/*
 * trace.c - a very small ptrace syscall tracer, for the 3DS bring-up
 *
 * Android's `init` segfaults on this port before it manages to log
 * anything (no kernel message, no /dev/log entry, and its klog path needs a
 * mknod that only works once the port gives it a tmpfs /dev).  Guessing which
 * syscall upsets it had already cost several boots, so this is the tool that
 * removes the guessing: it runs the program under ptrace and prints every
 * syscall, then the faulting PC/LR/SP and the fault address.
 *
 * ARMs are the same architecture here (host == guest == ARM EABI), so the
 * trace is exact, and because the traced process runs normally (we only stop
 * and resume it) the addresses are the real ones.
 *
 * Usage: trace <program> [args...]
 *
 * Output goes to stdout, which the init redirects into the SD-card log.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/user.h>

#define MAX_LINES 4000

struct name {
	int nr;
	const char *name;
};

/* Only the calls that matter for bring-up; anything else prints as a number,
 * which is easy enough to look up.  #ifdef'd so this always compiles. */
static const struct name names[] = {
#ifdef __NR_exit
	{ __NR_exit, "exit" },
#endif
#ifdef __NR_read
	{ __NR_read, "read" },
#endif
#ifdef __NR_write
	{ __NR_write, "write" },
#endif
#ifdef __NR_writev
	{ __NR_writev, "writev" },
#endif
#ifdef __NR_open
	{ __NR_open, "open" },
#endif
#ifdef __NR_close
	{ __NR_close, "close" },
#endif
#ifdef __NR_creat
	{ __NR_creat, "creat" },
#endif
#ifdef __NR_unlink
	{ __NR_unlink, "unlink" },
#endif
#ifdef __NR_execve
	{ __NR_execve, "execve" },
#endif
#ifdef __NR_chdir
	{ __NR_chdir, "chdir" },
#endif
#ifdef __NR_mknod
	{ __NR_mknod, "mknod" },
#endif
#ifdef __NR_chmod
	{ __NR_chmod, "chmod" },
#endif
#ifdef __NR_lseek
	{ __NR_lseek, "lseek" },
#endif
#ifdef __NR__llseek
	{ __NR__llseek, "_llseek" },
#endif
#ifdef __NR_getpid
	{ __NR_getpid, "getpid" },
#endif
#ifdef __NR_gettid
	{ __NR_gettid, "gettid" },
#endif
#ifdef __NR_mount
	{ __NR_mount, "mount" },
#endif
#ifdef __NR_umount2
	{ __NR_umount2, "umount2" },
#endif
#ifdef __NR_access
	{ __NR_access, "access" },
#endif
#ifdef __NR_mkdir
	{ __NR_mkdir, "mkdir" },
#endif
#ifdef __NR_rmdir
	{ __NR_rmdir, "rmdir" },
#endif
#ifdef __NR_dup
	{ __NR_dup, "dup" },
#endif
#ifdef __NR_dup2
	{ __NR_dup2, "dup2" },
#endif
#ifdef __NR_ioctl
	{ __NR_ioctl, "ioctl" },
#endif
#ifdef __NR_brk
	{ __NR_brk, "brk" },
#endif
#ifdef __NR_mmap2
	{ __NR_mmap2, "mmap2" },
#endif
#ifdef __NR_munmap
	{ __NR_munmap, "munmap" },
#endif
#ifdef __NR_mprotect
	{ __NR_mprotect, "mprotect" },
#endif
#ifdef __NR_stat64
	{ __NR_stat64, "stat64" },
#endif
#ifdef __NR_lstat64
	{ __NR_lstat64, "lstat64" },
#endif
#ifdef __NR_fstat64
	{ __NR_fstat64, "fstat64" },
#endif
#ifdef __NR_uname
	{ __NR_uname, "uname" },
#endif
#ifdef __NR_clone
	{ __NR_clone, "clone" },
#endif
#ifdef __NR_wait4
	{ __NR_wait4, "wait4" },
#endif
#ifdef __NR_rt_sigaction
	{ __NR_rt_sigaction, "rt_sigaction" },
#endif
#ifdef __NR_rt_sigprocmask
	{ __NR_rt_sigprocmask, "rt_sigprocmask" },
#endif
#ifdef __NR_getuid32
	{ __NR_getuid32, "getuid32" },
#endif
#ifdef __NR_setuid32
	{ __NR_setuid32, "setuid32" },
#endif
#ifdef __NR_umask
	{ __NR_umask, "umask" },
#endif
#ifdef __NR_fcntl64
	{ __NR_fcntl64, "fcntl64" },
#endif
#ifdef __NR_nanosleep
	{ __NR_nanosleep, "nanosleep" },
#endif
#ifdef __NR_poll
	{ __NR_poll, "poll" },
#endif
#ifdef __NR_ppoll
	{ __NR_ppoll, "ppoll" },
#endif
#ifdef __NR_select
	{ __NR_select, "select" },
#endif
#ifdef __NR_clock_gettime
	{ __NR_clock_gettime, "clock_gettime" },
#endif
#ifdef __NR_gettimeofday
	{ __NR_gettimeofday, "gettimeofday" },
#endif
#ifdef __NR_futex
	{ __NR_futex, "futex" },
#endif
#ifdef __NR_exit_group
	{ __NR_exit_group, "exit_group" },
#endif
#ifdef __NR_getcwd
	{ __NR_getcwd, "getcwd" },
#endif
#ifdef __NR_readlink
	{ __NR_readlink, "readlink" },
#endif
#ifdef __NR_getdents64
	{ __NR_getdents64, "getdents64" },
#endif
#ifdef __NR_openat
	{ __NR_openat, "openat" },
#endif
#ifdef __NR_fstatat64
	{ __NR_fstatat64, "fstatat64" },
#endif
#ifdef __NR_unlinkat
	{ __NR_unlinkat, "unlinkat" },
#endif
#ifdef __NR_set_tls
	{ __NR_set_tls, "set_tls" },
#endif
#ifdef __NR_prctl
	{ __NR_prctl, "prctl" },
#endif
#ifdef __NR_kill
	{ __NR_kill, "kill" },
#endif
#ifdef __NR_tgkill
	{ __NR_tgkill, "tgkill" },
#endif
#ifdef __NR_socket
	{ __NR_socket, "socket" },
#endif
#ifdef __NR_connect
	{ __NR_socket, "connect" },
#endif
};

static const char *sysname(long nr)
{
	static char buf[24];
	unsigned i;

	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (names[i].nr == nr)
			return names[i].name;
	snprintf(buf, sizeof(buf), "sys#%ld", nr);
	return buf;
}

int main(int argc, char **argv)
{
	pid_t child;
	int status, deliver = 0, entering = 1;
	long nlines = 0;
	struct user_regs regs;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <program> [args...]\n", argv[0]);
		return 2;
	}

	printf("trace: %s\n", argv[1]);
	fflush(stdout);

	child = fork();
	if (child < 0) {
		perror("fork");
		return 1;
	}
	if (child == 0) {
		ptrace(PTRACE_TRACEME, 0, 0, 0);
		execv(argv[1], &argv[1]);
		perror("execv");
		_exit(127);
	}

	if (waitpid(child, &status, 0) < 0) {
		perror("waitpid");
		return 1;
	}
	ptrace(PTRACE_SETOPTIONS, child, 0, PTRACE_O_TRACESYSGOOD);

	for (;;) {
		if (ptrace(PTRACE_SYSCALL, child, 0, deliver) < 0) {
			perror("PTRACE_SYSCALL");
			break;
		}
		deliver = 0;
		if (waitpid(child, &status, 0) < 0) {
			perror("waitpid");
			break;
		}
		if (WIFEXITED(status)) {
			printf("trace: exited with %d\n", WEXITSTATUS(status));
			break;
		}
		if (WIFSIGNALED(status)) {
			printf("trace: killed by signal %d (%s)\n",
			       WTERMSIG(status), strsignal(WTERMSIG(status)));
			break;
		}
		if (!WIFSTOPPED(status))
			continue;

		{
			int sig = WSTOPSIG(status);

			if (sig == (SIGTRAP | 0x80)) {	/* syscall stop */
				ptrace(PTRACE_GETREGS, child, 0, &regs);
				if (entering) {
					long nr = (long)regs.uregs[7];

					if (nlines < MAX_LINES)
						printf("%5ld %-16s(%#lx, %#lx, %#lx, %#lx)\n",
						       nr, sysname(nr),
						       (unsigned long)regs.uregs[0],
						       (unsigned long)regs.uregs[1],
						       (unsigned long)regs.uregs[2],
						       (unsigned long)regs.uregs[3]);
					nlines++;
				} else if (nlines < MAX_LINES) {
					printf("      = %ld\n",
					       (long)regs.uregs[0]);
				}
				entering = !entering;
				continue;
			}

			if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL ||
			    sig == SIGFPE || sig == SIGABRT) {
				siginfo_t si;

				memset(&si, 0, sizeof(si));
				ptrace(PTRACE_GETREGS, child, 0, &regs);
				ptrace(PTRACE_GETSIGINFO, child, 0, &si);
				printf("\n*** FAULT: signal %d (%s)\n"
				       "***   pc=%#lx lr=%#lx sp=%#lx fp=%#lx\n"
				       "***   fault address=%#lx\n"
				       "***   r0..r3=%#lx %#lx %#lx %#lx\n",
				       sig, strsignal(sig),
				       (unsigned long)regs.uregs[15],
				       (unsigned long)regs.uregs[14],
				       (unsigned long)regs.uregs[13],
				       (unsigned long)regs.uregs[11],
				       (unsigned long)si.si_addr,
				       (unsigned long)regs.uregs[0],
				       (unsigned long)regs.uregs[1],
				       (unsigned long)regs.uregs[2],
				       (unsigned long)regs.uregs[3]);
				fflush(stdout);
				deliver = sig;	/* let it die as it would */
				continue;
			}

			deliver = sig;		/* forward anything else */
		}
	}

	fflush(stdout);
	return 0;
}
