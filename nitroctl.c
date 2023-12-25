#include <sys/socket.h>
#include <sys/un.h>

#include <ctype.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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

	while(!timed_out) {
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

int
main(int argc, char *argv[])
{
	if (argc < 2 || (
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
	    strcmp(argv[1], "stop") != 0 &&
	    strcmp(argv[1], "Reboot") != 0 &&
	    strcmp(argv[1], "Shutdown") != 0)) {
		dprintf(2, "usage: nitroctl COMMAND [SERVICE]\n");
		exit(2);
	}

	static const char default_sock[] = "/run/nitro/nitro.sock";
	sockpath = getenv("NITRO_SOCK");
	if (!sockpath || !*sockpath)
		sockpath = default_sock;

	connfd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (connfd < 0) {
		perror("socket");
		exit(111);
	}

	char cmd = argv[1][0];

	if (strcmp(argv[1], "start") == 0 && argv[2])
		return send_and_wait('u', argv[2]);
	else if (strcmp(argv[1], "stop") == 0 && argv[2])
		return send_and_wait('d', argv[2]);
	else if (strcmp(argv[1], "restart") == 0 && argv[2])
		return send_and_wait('r', argv[2]);
	else if (strcmp(argv[1], "check") == 0 && argv[2])
		cmd = '?';

	notifysock("");

	dprintf(connfd, "%c%s", cmd, argv[2] ? argv[2] : "");

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
