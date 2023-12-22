#include <sys/socket.h>
#include <sys/un.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int connfd;
char template[256] = "/tmp/nitroctl.XXXXXX";

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
cleanup()
{
	unlink(template);
	*strrchr(template, '/') = 0;
	rmdir(template);
}

enum process_state
check(char *name)
{
	dprintf(connfd, "?%s", name);

	ssize_t rd;
	char buf[64];
	rd = read(connfd, buf, sizeof buf);
	if (rd < 0) {
		perror("read");
		exit(111);
	}
	if (buf[0] >= 'A' && buf[0] <= 'Z')
		return buf[0] - 64;
	return -1;
}

int
main(int argc, char *argv[])
{
	if (argc < 2 || (
	    strcmp(argv[1], "l") != 0 && strcmp(argv[1], "level") != 0 &&
	    strcmp(argv[1], "s") != 0 && strcmp(argv[1], "status") != 0 &&
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
	    strcmp(argv[1], "stop") != 0 &&
	    strcmp(argv[1], "Reboot") != 0 &&
	    strcmp(argv[1], "Shutdown") != 0)) {
		dprintf(2, "usage: nitroctl COMMAND [SERVICE]\n");
		exit(2);
	}

	static const char default_sock[] = "/run/nitro/nitro.sock";
	const char *path = getenv("NITRO_SOCK");
	if (!path || !*path)
		path = default_sock;

	struct sockaddr_un addr = { 0 };
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof addr.sun_path - 1);
	connfd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (connfd < 0) {
		perror("socket");
		exit(111);
	}

	struct sockaddr_un my_addr = { 0 };
	my_addr.sun_family = AF_UNIX;
	if (!mkdtemp(template)) {
		perror("mkdtemp");
		exit(111);
	}
	strcat(template, "/sock");
	strncpy(my_addr.sun_path, template, sizeof my_addr.sun_path - 1);
	if (bind(connfd, (struct sockaddr *)&my_addr, sizeof my_addr) < 0) {
		perror("bind");
		exit(111);
	}
	atexit(cleanup);

	if (connect(connfd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		perror("connect");
		exit(111);
	}

	char cmd = argv[1][0];

	if (strcmp(argv[1], "start") == 0 && argv[2])
		cmd = 'u';
	else if (strcmp(argv[1], "stop") == 0 && argv[2])
		cmd = 'd';
	else if (strcmp(argv[1], "check") == 0 && argv[2])
		cmd = '?';

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

	if (strcmp(argv[1], "start") == 0 && argv[2]) {
		int checks = 0;
		while (1) {
			switch (check(argv[2])) {
			case PROC_DOWN:
			case PROC_FATAL:
				fprintf(stderr, "start failed\n");
				return 1;
			case PROC_UP:
			case PROC_ONESHOT:
				return 0;
			default:
				if (checks > 10) {
					fprintf(stderr, "start timed out\n");
					return 1;
				}
				nanosleep(&(struct timespec){0, 250000000}, 0);
				checks++;
			}
		}
	} else if (strcmp(argv[1], "stop") == 0 && argv[2]) {
		int checks = 0;
		while (1) {
			switch (check(argv[2])) {
			case PROC_DOWN:
			case PROC_FATAL: /* ? */
				return 0;
			default:
				if (checks > 10) {
					fprintf(stderr, "stop timed out\n");
					return 1;
				}
				nanosleep(&(struct timespec){0, 250000000}, 0);
				checks++;
			}
		}
	}

	return status;
}
