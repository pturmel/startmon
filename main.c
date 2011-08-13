/*
 * Process/Thread Start Monitor
 * Copyright (C) 2011 Philip J. Turmel <philip@turmel.org>
 *
 * Inspired by a blog entry by Scott James Remnant:
 * http://netsplit.com/2011/02/09/the-proc-connector-and-socket-filters/
 *
 * Maintained at http://github.com/pturmel/startmon
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <linux/netlink.h>
#include <linux/connector.h>
#include <linux/cn_proc.h>

void usage(char *arg0) {
	fprintf(stderr, "Usage:\n  %s [-eft] [--exec] [--fork] [--thread]\n\n"
		"Specify at least '--exec' or '--fork'\n",
		arg0);
	exit(1);
}

const char my_short_opts[] = "efht";
struct option my_long_opts[] = {
	{"exec", 0, NULL, 'e'},
	{"fork", 0, NULL, 'f'},
	{"help", 0, NULL, 'h'},
	{"thread", 0, NULL, 't'},
	{}
};

int execflag   = 0;
int forkflag   = 0;
int threadflag = 0;

char procname[50];
char cmdlinebuf[4096];
ssize_t bytes;

char *get_cmdline(pid_t id) {
	int fd;
	ssize_t remain;
	char *c;

	sprintf(procname, "/proc/%d/cmdline", id);
	fd = open(procname, O_RDONLY);
	if (fd != -1) {
		bytes = read(fd, cmdlinebuf, 4096);
		close(fd);
		for (c=cmdlinebuf, remain=bytes; remain>0; c++, remain--)
			if (*c < 32)
				*c = ' ';
		*c = 0;
	} else
		bytes = sprintf(cmdlinebuf, "<N/A>");
	return cmdlinebuf;
}

/* Given a single connection message, evaluate and process
 * the enclosed proc message.
 */
void dispatch_nl_cn(struct cn_msg *hdr) {
	if (hdr->id.idx != CN_IDX_PROC || hdr->id.val != CN_VAL_PROC)
		return;
	struct proc_event *pe = (struct proc_event *)hdr->data;
	switch (pe->what) {
		case PROC_EVENT_FORK:
			if (!forkflag)
				break;
			if (pe->event_data.fork.child_pid == pe->event_data.fork.child_tgid) {
				/* Regular fork, parent process is the originator */
				printf("Fork %d %d %s\n",
					pe->event_data.fork.parent_pid,
					pe->event_data.fork.child_pid,
					get_cmdline(pe->event_data.fork.child_tgid));
			} else {
				/* Thread fork, thread group leader is the originator */
				if (threadflag)
					printf("Thread %d %d %s\n",
						pe->event_data.fork.child_tgid,
						pe->event_data.fork.child_pid,
						get_cmdline(pe->event_data.fork.child_tgid));
			}
			break;
		case PROC_EVENT_EXEC:
			if (!execflag)
				break;
			if (pe->event_data.exec.process_pid == pe->event_data.exec.process_tgid) {
				/* Thread group leader did an exec */
				printf("Exec - %d %s\n",
					pe->event_data.exec.process_pid,
					get_cmdline(pe->event_data.exec.process_tgid));
			} else {
				/* Subordinate thread did an exec */
				if (threadflag)
					printf("Exec %d %d %s\n",
						pe->event_data.exec.process_tgid,
						pe->event_data.exec.process_pid,
						get_cmdline(pe->event_data.exec.process_tgid));
			}
			break;
	}
}

/* Given a single netlink message, evaluate and process the
 * enclosed container message.
 */
void dispatch_nl(struct nlmsghdr *nlhdr) {
	switch (nlhdr->nlmsg_type) {
		case NLMSG_ERROR:
		case NLMSG_NOOP:
			return;
		case NLMSG_OVERRUN:
			printf("overrun\n");
			return;
		default:
			dispatch_nl_cn(NLMSG_DATA(nlhdr));
	}
}

int main(int argc, char **argv) {
	int opt, longidx, nlsock, rc;
	struct sockaddr_nl nladdr = {AF_NETLINK};
	socklen_t          nlalen;
	pid_t mypid;
	ssize_t bcount;
	char *called;
	void *rcvbuf;

	/* Isolate the base name of the program as invoked. */
	called = strrchr(argv[0], '/');
	if (!called)
		called = argv[0];

	/* Parse the given options, looking for the filtering mode. */
	while ((opt = getopt_long(argc, argv, my_short_opts, my_long_opts, &longidx)) != -1) {
		switch (opt) {
			case 'e':
				execflag = 1;
				break;
			case 'f':
				forkflag = 1;
				break;
			case 't':
				threadflag = 1;
				break;
			default:
				if (opt != 'h')
					fprintf(stderr, "%s: Invalid option '%c' !\n", called, opt);
				usage(called);
		}
	}

	/* If no filtering mode, bail out. */
	if (!(execflag || forkflag)) {
		fprintf(stderr, "%s: Missing required mode option!\n", called);
		usage(called);
	}

	/* Create the netlink socket */
	nlsock = socket(PF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_CONNECTOR);
	if (nlsock == -1) {
		perror("Unable to open a netlink socket!");
		exit(1);
	}

	/* Attach to the process connector group */
	{
		nladdr.nl_pid = mypid = getpid();
		nladdr.nl_groups = CN_IDX_PROC;
		if (bind(nlsock, (struct sockaddr *)&nladdr, sizeof(nladdr))) {
			perror("Unable to bind to the process connector!");
			exit(1);
		}
	}

	/* Request the process messages */
	{
		enum proc_cn_mcast_op cnop  = PROC_CN_MCAST_LISTEN;
		struct cn_msg         cnmsg = {{CN_IDX_PROC, CN_VAL_PROC}, 0, 0, sizeof(cnop), 0};
		struct nlmsghdr       nlmsg = {NLMSG_LENGTH(sizeof cnmsg + sizeof cnop), NLMSG_DONE};
		char padding[16];
		struct iovec iov[4] = {
			{&nlmsg, sizeof(nlmsg)},
			{padding, NLMSG_LENGTH(0) - sizeof(nlmsg)},
			{&cnmsg, sizeof(cnmsg)},
			{&cnop, sizeof(cnop)}
		};
		nlmsg.nlmsg_pid = mypid;
		if ((bcount = writev(nlsock, iov, 4)) == -1) {
			perror("Unable to listen to the process connector!");
			exit(1);
		}
	}

	/* Receive messages forever ... */
	rcvbuf = malloc(4096+CONNECTOR_MAX_MSG_SIZE);
	if (!rcvbuf) {
		perror("Unable to allocate a receive buffer!");
		exit(1);
	}
	setbuf(stdout, NULL);
	while (1) {
		nlalen = sizeof(nladdr);
		bcount = recvfrom(nlsock, rcvbuf, 4096+CONNECTOR_MAX_MSG_SIZE,
			0, (struct sockaddr *)&nladdr, &nlalen);
		if (nladdr.nl_pid == 0) {
			struct nlmsghdr *hdr = rcvbuf;
			for (hdr=rcvbuf; NLMSG_OK(hdr, bcount); hdr=NLMSG_NEXT(hdr, bcount))
				dispatch_nl(hdr);
		}
	}
}
