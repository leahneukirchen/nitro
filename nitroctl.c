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

int connfd;
const char *sockpath;
char notifypath[PATH_MAX];
volatile sig_atomic_t timed_out;

enum process_state {
	PROC_DOWN = 1,
	PROC_SETUP,
	PROC_STARTING,
	PROC_UP,
	PROC_ONESHOT,
	PROC_SHUTDOWN,
	PROC_RESTART,
	PROC_FATAL,
	PROC_DELAY,
};

void
noop()
{
	timed_out = 1;
}

void
cleanup_notify()
{
	unlink(notifypath);
}

void
notifysock(const char *service)
{
	static char default_sock[] = "/run/nitro/nitro.sock";
	char *path = strdup(sockpath);
	if (!path || !*path)
		path = default_sock;

	snprintf(notifypath, sizeof notifypath,
	    "%s/notify/%s,%ld", dirname(path), service, (long)getpid());

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
		perror("bind");
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
}

int
send_and_wait(char cmd, const char *service)
{
	notifysock(service);

	dprintf(connfd, "%c%s", cmd, service);

	struct sigaction sa = {
		.sa_handler = noop,
		.sa_flags = 0,
	};
	sigaction(SIGALRM, &sa, 0);
	alarm(5);

	while (!timed_out) {
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
			if (state == PROC_UP || state == PROC_ONESHOT)
				return 0;
			if (state == PROC_FATAL) {
				fprintf(stderr,
				    "nitroctl: failed to %sstart '%s'\n", service, cmd == 'u' ? "" : "re");
				return 1;
			}
			break;
		case 'd':
			if (state == PROC_DOWN || state == PROC_FATAL)
				return 0;
			break;
		}
	}

	fprintf(stderr, "nitroctl: action timed out\n");
	return 2;
}

char *
normalize(char *service)
{
	if (!service || (service[0] != '/' && service[0] != '.'))
		return service;

	char *buf = realpath(service, 0);
	if (!buf) {
		fprintf(stderr, "nitroctl: no such service: %s: %m\n", service);
		exit(1);
	}

	char *end = strrchr(buf, '/');
	if (strcmp(end, "/log") == 0)
		while (end > buf && *--end != '/')
			;

	return end + 1;
}

#ifdef INIT_SYSTEM

#ifndef OUR_WTMP
#define OUR_WTMP "/var/log/wtmp"
#endif

#ifndef OUR_UTMP
#define OUR_UTMP "/run/utmp"
#endif

void write_wtmp(int boot) {
	int fd;

	if ((fd = open(OUR_WTMP, O_WRONLY|O_APPEND)) < 0)
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

	write(fd, (char *)&utmp, sizeof utmp);
	close(fd);

	if (boot) {
		if ((fd = open(OUR_UTMP, O_WRONLY|O_APPEND)) < 0)
			return;
		write(fd, (char *)&utmp, sizeof utmp);
		close(fd);
	}
}

#endif

int
suffix(const char *str, const char *suff)
{
	size_t a = strlen(str);
	size_t b = strlen(suff);
	return b <= a && strcmp(str + a - b, suff) == 0;
}

int
main(int argc, char *argv[])
{
#ifdef INIT_SYSTEM
	if (getpid() == 1) {
		execvp("nitro", argv);
		dprintf(2, "nitroctl: exec init failed: %s\n", strerror(errno));
		exit(111);
	}
#endif
	char cmd;

#ifdef INIT_SYSTEM
	if (suffix(argv[0], "init")) {
		if (argv[1] && strcmp(argv[1], "0") == 0) {
			cmd = 'S';
		} else if (argv[1] && strcmp(argv[1], "6") == 0) {
			cmd = 'R';
		} else if (argv[1] && strcmp(argv[1], "q") == 0) {
			cmd = 's';
		} else {
			dprintf(2, "usage: init [0|6|q]\n");
			exit(2);
		}
	} else if (suffix(argv[0], "halt")) {
		cmd = 'S';
	} else if (suffix(argv[0], "poweroff")) {
		cmd = 'S';
	} else if (suffix(argv[0], "reboot")) {
		cmd = 'R';
	} else
#endif
	if (argc > 1 && (
	    strcmp(argv[1], "l") != 0 && strcmp(argv[1], "list") != 0 &&
	    strcmp(argv[1], "d") != 0 && strcmp(argv[1], "down") != 0 &&
	    strcmp(argv[1], "u") != 0 && strcmp(argv[1], "up") != 0 &&
	    strcmp(argv[1], "p") != 0 && strcmp(argv[1], "pause") != 0 &&
	    strcmp(argv[1], "c") != 0 && strcmp(argv[1], "cont") != 0 &&
	    strcmp(argv[1], "h") != 0 && strcmp(argv[1], "hup") != 0 &&
	    strcmp(argv[1], "a") != 0 && strcmp(argv[1], "alarm") != 0 &&
	    strcmp(argv[1], "i") != 0 && strcmp(argv[1], "interrupt") != 0 &&
	    strcmp(argv[1], "q") != 0 && strcmp(argv[1], "quit") != 0 &&
	    strcmp(argv[1], "t") != 0 && strcmp(argv[1], "term") != 0 &&
	    strcmp(argv[1], "k") != 0 && strcmp(argv[1], "kill") != 0 &&
	    strcmp(argv[1], "1") != 0 &&
	    strcmp(argv[1], "2") != 0 &&
	    strcmp(argv[1], "check") != 0 &&
	    strcmp(argv[1], "start") != 0 &&
	    strcmp(argv[1], "r") != 0 && strcmp(argv[1], "restart") != 0 &&
	    strcmp(argv[1], "s") != 0 && strcmp(argv[1], "scan") != 0 &&
	    strcmp(argv[1], "stop") != 0 &&
	    strcmp(argv[1], "Reboot") != 0 &&
	    strcmp(argv[1], "Shutdown") != 0)) {
		dprintf(2, "usage: nitroctl COMMAND [SERVICE]\n");
		exit(2);
	} else if (argc == 1) {
		cmd = 'l';
	} else {
		cmd = argv[1][0];
	}

#ifdef INIT_SYSTEM
	if ((cmd == 'R' || cmd == 'S') && argv[1]) {
		if (strcmp(argv[1], "-f") == 0) {
			if (cmd == 'R') {
				reboot(RB_AUTOBOOT);
				dprintf(2, "nitroctl: force reboot failed: %s\n", strerror(errno));
				exit(111);
			} else if (cmd == 'S') {
				reboot(RB_POWER_OFF);
				dprintf(2, "nitroctl: force shutdown failed: %s\n", strerror(errno));
				exit(111);
			}
		}

		if (strcmp(argv[1], "-B") == 0) {
			write_wtmp(1);
			return 0;
		}

		if (strcmp(argv[1], "-w") == 0) {
			write_wtmp(0);
			return 0;
		}
	}
#endif

	static const char default_sock[] = "/run/nitro/nitro.sock";
	sockpath = getenv("NITRO_SOCK");
	if (!sockpath || !*sockpath)
		sockpath = default_sock;

	connfd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (connfd < 0) {
		perror("socket");
		exit(111);
	}

	char *service = argc > 2 ? normalize(argv[2]) : 0;

	if (argv[1]) {
		if (strcmp(argv[1], "start") == 0 && service)
			return send_and_wait('u', service);
		else if (strcmp(argv[1], "stop") == 0 && service)
			return send_and_wait('d', service);
		else if (strcmp(argv[1], "restart") == 0 && service)
			return send_and_wait('r', service);
		else if (strcmp(argv[1], "check") == 0 && service)
			cmd = '?';
	}

	notifysock("");

	dprintf(connfd, "%c%s", cmd, service ? service : "");

	int status = 1;

	ssize_t rd;
	char buf[4096];
	rd = read(connfd, buf, sizeof buf);
	if (rd < 0) {
		perror("read");
		exit(111);
	}
	if (rd > 0)
		status = 0;
	write(1, buf, rd);

	return status;
}
