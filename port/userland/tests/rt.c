// SPDX-License-Identifier: GPL-2.0
/*
 * rt.c - run a command at SCHED_FIFO realtime priority
 *
 * Why this exists
 * ---------------
 * The machine freezes at ~90 s (right when the framework starts its first app)
 * with *no* report at all: no hung-task report, no softlockup report, and the
 * /dev/kmsg heartbeat - which touches no storage - stops too.  One explanation
 * that fits every observation is a high-priority (SCHED_FIFO) userspace thread
 * spinning: it would starve every SCHED_OTHER task (our heartbeat, powerkey,
 * logdump, kmsgdump) *and* the softlockup/hung-task detector kthreads, while
 * leaving the kernel itself alive.
 *
 * A watchdog that runs at a higher realtime priority than the spinner would
 * still be scheduled, so /init starts /bin/heartbeat through this helper.  If
 * the heartbeat keeps counting after everything else stops, the kernel is
 * alive and something in userspace is starving the CPU; the heartbeat then
 * also reports the top CPU-consuming thread, which names the culprit.
 *
 * Usage: rt <priority> <command> [args...]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <errno.h>

int main(int argc, char **argv)
{
	struct sched_param sp;
	int prio;

	if (argc < 3) {
		fprintf(stderr, "usage: rt <priority> <command> [args...]\n");
		return 2;
	}

	prio = atoi(argv[1]);
	memset(&sp, 0, sizeof(sp));
	sp.sched_priority = prio;

	if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0)
		fprintf(stderr, "rt: sched_setscheduler(SCHED_FIFO, %d): %s\n",
			prio, strerror(errno));
	else
		printf("rt: %s now SCHED_FIFO priority %d\n", argv[2], prio);

	execv(argv[2], &argv[2]);
	fprintf(stderr, "rt: exec %s: %s\n", argv[2], strerror(errno));
	return 127;
}
