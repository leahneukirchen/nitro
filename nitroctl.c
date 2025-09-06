/* nitroctl - control and manage services monitored by nitro */
/* SPDX-License-Identifier: 0BSD */

#ifdef __linux__
#define INIT_SYSTEM
#endif

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/utsname.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef INIT_SYSTEM
#include <sys/reboot.h>
#include <utmp.h>
#endif

#include "nitro.h"

int connfd;
const char *sockpath;
char notifypath[PATH_MAX];
volatile sig_atomic_t got_signal;

static const char *
proc_state_str(enum process_state state)
{
       switch (state) {
       case PROC_DOWN: return "DOWN";
       case PROC_SETUP: return "SETUP";
       case PROC_STARTING: return "STARTING";
       case PROC_UP: return "UP";
       case PROC_ONESHOT: return "ONESHOT";
       case PROC_SHUTDOWN: return "SHUTDOWN";
       case PROC_RESTART: return "RESTART";
       case PROC_FATAL: return "FATAL";
       case PROC_DELAY: return "DELAY";
       default: return "???";
       }
}

static int
streq1(const char *a, const char *b)
{
	return (a[0] == b[0] && a[1] == 0) || strcmp(a, b) == 0;
}

static int
streq(const char *a, const char *b)
{
	return strcmp(a, b) == 0;
}

void
on_signal(int signal)
{
	got_signal = signal;
}

void
cleanup_notify()
{
	unlink(notifypath);
}

void
notifysock(const char *service)
{
	if (*notifypath)
		return;

	connfd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (connfd < 0) {
		perror("socket");
		exit(111);
	}

	char *sockpath2 = strdup(sockpath);
	char *path = sockpath2;
	if (!path || !*path)
		path = default_sock;
	path = dirname(path);

	snprintf(notifypath, sizeof notifypath,
	    "%s/notify/%s,%ld", path, service, (long)getpid());

	struct sockaddr_un my_addr = { 0 };
	my_addr.sun_family = AF_UNIX;
	strncpy(my_addr.sun_path, notifypath, sizeof my_addr.sun_path - 1);
again:
	if (bind(connfd, (struct sockaddr *)&my_addr, sizeof my_addr) < 0) {
		if (errno == EADDRINUSE) {
			// ok to delete this, only we can have the current pid.
			if (unlink(notifypath) == 0)
				goto again;
		}
		fprintf(stderr, "could not bind socket to '%s': %s\n",
		    notifypath, strerror(errno));
		exit(111);
	}
	atexit(cleanup_notify);

	struct sockaddr_un addr = { 0 };
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, sockpath, sizeof addr.sun_path - 1);
	if (connect(connfd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		fprintf(stderr, "nitroctl: could not connect to '%s', is nitro started?\n", sockpath);
		exit(111);
	}

	free(sockpath2);
}

int
send_and_wait(char cmd, const char *service, int fast)
{
	notifysock(service);

	dprintf(connfd, "%c%s", cmd, service);

	struct sigaction sa = {
		.sa_handler = on_signal,
		.sa_flags = 0,
	};
	sigaction(SIGALRM, &sa, 0);
	sigaction(SIGINT, &sa, 0);

	while (!got_signal) {
		ssize_t rd;
		char buf[64];

		rd = read(connfd, buf, sizeof buf); /* block */
		if (rd < 0) {
			if (errno == EINTR)
				continue;
			perror("read");
			exit(111);
		}

		buf[rd] = 0;

		int state = 0;
		if (buf[0] >= 'A' && buf[0] <= 'Z')
			state = buf[0] - 64;

		if (buf[0] == 'e') {
			fprintf(stderr, "nitroctl: no such service '%s'\n", service);
			return 111;
		}

		switch (cmd) {
		case 'u':
		case 'r':
			if (fast && state == PROC_STARTING)
				return 0;
			if (state == PROC_UP || state == PROC_ONESHOT)
				return 0;
			if (state == PROC_FATAL) {
				fprintf(stderr,
				    "nitroctl: failed to %sstart '%s'\n", cmd == 'u' ? "" : "re", service);
				return 1;
			}
			break;
		case 'd':
			if (fast && state == PROC_SHUTDOWN)
				return 0;
			if (state == PROC_DOWN || state == PROC_FATAL)
				return 0;
			break;
		case '?':
			if (state == PROC_STARTING || state == PROC_UP ||
			    state == PROC_SHUTDOWN || state == PROC_RESTART) {
				if (fast)
					printf("%s", buf + 1);
				return 0;
			}
			return 1;
		}
	}

	if (got_signal == SIGALRM)
		fprintf(stderr, "nitroctl: action timed out\n");
	return 2;
}

int
send_and_print(char cmd, const char *service)
{
	notifysock("");

	dprintf(connfd, "%c%s", cmd, service);

	int status = 1;

	ssize_t rd;
	char buf[4096];
	rd = read(connfd, buf, sizeof buf - 1);
	if (rd < 0) {
		perror("read");
		exit(111);
	}
	if (rd > 0 || cmd == 'l')
		status = 0;
	buf[rd] = 0;

	char *s = buf;
	char name[64];
	long pid, state, wstatus, uptime;
	int len;
	while (sscanf(s,  "%63[^#/,],%ld,%ld,%ld,%ld\n%n",
	    name, &state, &pid, &wstatus, &uptime, &len) == 5) {
		s += len;

		printf("%s %s", proc_state_str(state), name);
		if (pid)
			printf(" (pid %ld)", pid);
		printf(" (wstatus %ld) %lds\n", wstatus, uptime);
	}

	if (streq(s, "ok\n"))
		status = 0;
	else if (streq(s, "error\n"))
		status = 1;
	else if (s == buf)
		printf("%s", s);

	return status;
}

char *
normalize(char *service)
{
	if (!(service[0] == '/' || (service[0] == '.' && (!service[1] || service[1] == '/'))))
		return service;

	char *buf = realpath(service, 0);
	if (!buf) {
		fprintf(stderr, "nitroctl: no such service: %s: %s\n", service, strerror(errno));
		exit(1);
	}

	return strrchr(buf, '/') + 1;
}

#ifdef INIT_SYSTEM

#ifndef OUR_WTMP
#define OUR_WTMP "/var/log/wtmp"
#endif

#ifndef OUR_UTMP
#define OUR_UTMP "/run/utmp"
#endif

void write_wtmp(int boot)
{
	int fd;

	if ((fd = open(OUR_WTMP, O_WRONLY | O_APPEND)) < 0)
		return;

	struct utmp utmp = { 0 };
	struct utsname uname_buf;
	struct timeval tv;

	gettimeofday(&tv, 0);
	utmp.ut_tv.tv_sec = tv.tv_sec;
	utmp.ut_tv.tv_usec = tv.tv_usec;

	utmp.ut_type = boot ? BOOT_TIME : RUN_LVL;

	strncpy(utmp.ut_name, boot ? "reboot" : "shutdown", sizeof utmp.ut_name);
	strncpy(utmp.ut_id, "~~", sizeof utmp.ut_id);
	strncpy(utmp.ut_line, boot ? "~" : "~~", sizeof utmp.ut_line);
	if (uname(&uname_buf) == 0)
		strncpy(utmp.ut_host, uname_buf.release, sizeof utmp.ut_host);

	(void)! write(fd, (char *)&utmp, sizeof utmp);
	close(fd);

	if (boot) {
		if ((fd = open(OUR_UTMP, O_WRONLY | O_APPEND)) < 0)
			return;
		(void)! write(fd, (char *)&utmp, sizeof utmp);
		close(fd);
	}
}

static int
suffix(const char *str, const char *suff)
{
	size_t a = strlen(str);
	size_t b = strlen(suff);
	return b <= a && strcmp(str + a - b, suff) == 0;
}
#endif

static int
max(int a, int b)
{
	return a > b ? a : b;
}

int
main(int argc, char *argv[])
{
	const char *cmd = 0;

#ifdef INIT_SYSTEM
	if (suffix(argv[0], "init")) {
		if (getpid() == 1) {
			execvp("nitro", argv);
			dprintf(2, "nitroctl: exec init failed: %s\n", strerror(errno));

			if (chdir(argc == 2 ? argv[1] : "/etc/nitro") < 0)
				dprintf(2, "nitroctl: chdir failed: %s\n", strerror(errno));
			execl("SYS/fatal", "SYS/fatal", (char *)0);
			dprintf(2, "nitroctl: exec SYS/fatal failed: %s\n", strerror(errno));
			exit(111);
		}

		if (argc == 2) {
			if (streq(argv[1], "0"))
				cmd = "Shutdown";
			else if (streq(argv[1], "6") == 0)
				cmd = "Reboot";
			else if (streq(argv[1], "q") == 0)
				cmd = "scan";
			else
				goto init_usage;
		} else {
init_usage:
			dprintf(2, "usage: init [0|6|q]\n");
			exit(2);
		}
	} else if (suffix(argv[0], "halt") || suffix(argv[0], "poweroff")) {
		cmd = "Shutdown";
		if (argc > 1) {
			if (streq(argv[1], "-f")) {
				reboot(RB_POWER_OFF);
				dprintf(2, "nitroctl: force shutdown failed: %s\n", strerror(errno));
				exit(111);
			} else if (streq(argv[1], "-B")) {
				write_wtmp(1);
				return 0;
			} else if (streq(argv[1], "-w")) {
				write_wtmp(0);
				return 0;
			}
		}
	} else if (suffix(argv[0], "reboot")) {
		cmd = "Reboot";
		if (argc > 1 && streq(argv[1], "-f")) {
			reboot(RB_AUTOBOOT);
			dprintf(2, "nitroctl: force reboot failed: %s\n", strerror(errno));
			exit(111);
		}
	}
#endif

	int c;
	while ((c = getopt(argc, argv, "t:")) != -1)
		switch(c) {
		case 't': {
			errno = 0;
			char *rest = 0;
			int timeout = strtol(optarg, &rest, 10);
                        if (timeout < 0 || *rest || errno != 0) {
	                        dprintf(2, "nitroctl: invalid timeout\n");
	                        exit(2);
                        }
                        alarm(timeout);
                        break;
		}
		default:
			printf("c=%c\n", c);
 			goto usage;
		}

	argc -= optind;
        argv += optind;

        if (!cmd) {
	        if (argc == 0)
		        cmd = "list";
	        else
		        cmd = argv[0];
        }

	sockpath = control_socket();

	if (streq1(cmd, "list"))
		return send_and_print('l', "");
	else if (streq(cmd, "info"))
		return send_and_print('#', "");
	else if (streq1(cmd, "scan") || streq(cmd, "rescan"))
		return send_and_print('s', "");
	else if (streq(cmd, "Reboot"))
		return send_and_print('R', "");
	else if (streq(cmd, "Shutdown"))
		return send_and_print('S', "");
	else if (argc > 1 && (
	    streq1(cmd, "down") ||
	    streq1(cmd, "up") ||
	    streq1(cmd, "pause") ||
	    streq1(cmd, "cont") ||
	    streq1(cmd, "hup") ||
	    streq1(cmd, "alarm") ||
	    streq1(cmd, "interrupt") ||
	    streq1(cmd, "quit") ||
	    streq1(cmd, "term") ||
	    streq1(cmd, "kill") ||
	    streq1(cmd, "1") ||
	    streq1(cmd, "2"))) {
		int err = 0;
		for (int i = 1; i < argc; i++)
			if (send_and_print(cmd[0], argv[i]) != 0)
				err = 1;
		return err;
	} else if (argc > 1) {
		int err = 0;
		for (int i = 1; i < argc; i++) {
			if (i > 1) {
				unlink(notifypath);
				notifypath[0] = 0;
				close(connfd);
			}

			char *service = normalize(argv[i]);
			if (streq(cmd, "start"))
				err = max(err, send_and_wait('u', service, 0));
			else if (streq(cmd, "fast-start"))
				err = max(err, send_and_wait('u', service, 1));
			else if (streq(cmd, "stop"))
				err = max(err, send_and_wait('d', service, 0));
			else if (streq(cmd, "fast-stop"))
				err = max(err, send_and_wait('d', service, 1));
			else if (streq(cmd, "restart"))
				err = max(err, send_and_wait('r', service, 0));
			else if (streq(cmd, "fast-restart") || streq(cmd, "r"))
				err = max(err, send_and_wait('r', service, 1));
			else if (streq(cmd, "check"))
				err = max(err, send_and_wait('?', service, 0));
			else if (streq(cmd, "pidof"))
				err = max(err, send_and_wait('?', service, 1));
			else
				goto usage;
		}
		return err;
	}

usage:
	dprintf(2, "usage: nitroctl COMMAND [SERVICE ...]\n");
	exit(2);
}
