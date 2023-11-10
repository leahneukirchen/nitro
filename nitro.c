#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/un.h>

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef int64_t deadline;		/* milliseconds since boot */

deadline time_now()
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

enum global_state {
	GLBL_UP,
	GLBL_WANT_SHUTDOWN,
	GLBL_WANT_REBOOT,
} global_state;

enum process_state {
	PROC_DOWN = 1,
	PROC_STARTING,
	PROC_UP,
	PROC_DEAD,
	PROC_SHUTDOWN,
	PROC_RESTART,
	PROC_FATAL,
	PROC_DELAY,
};

enum process_events {
	EVNT_TIMEOUT = 1,
	EVNT_WANT_UP,
	EVNT_WANT_DOWN,
	EVNT_WANT_RESTART,
	EVNT_EXITED,
	// EVNT_DIED,   health check failed
};

struct service {
	char name[64];
	deadline startstop;
	deadline deadline;
	int timeout;
	pid_t pid;
	int logpipe[2];
	enum process_state state;
	char seen;
	char islog;
} services[512];

int max_service = 0;
int controlsock;
int nullfd;
int selfpipe[2];
int globallog[2];

volatile sig_atomic_t want_rescan;
volatile sig_atomic_t want_shutdown;

char *
steprn(char *dst, char *end, const char *fmt, ...)
{
	if (dst >= end)
		return end;

	va_list ap;
	va_start(ap, fmt);
	int r = vsnprintf(dst, end - dst, fmt, ap);
	va_end(ap);

	if (r < 0) {
		/* snprintf only fails for silly reasons:
		   truncate what was written, behave as noop.  */
		*dst = 0;
		return dst;
	}

	return r > end - dst ? end : dst + r;
}

void
proc_launch(int i)
{
	unsigned char status;
	int alivepipefd[2];
	if (pipe(alivepipefd) < 0)
		abort();
	fcntl(alivepipefd[0], F_SETFD, FD_CLOEXEC);
	fcntl(alivepipefd[1], F_SETFD, FD_CLOEXEC);

	pid_t child = fork();
	if (child == 0) {
		chdir(services[i].name);

		if (strcmp(services[i].name, "LOG") == 0) {
			dup2(globallog[0], 0);
			dup2(1, 2);
		} else if (services[i].islog) {
			dup2(services[i].logpipe[0], 0);
		} else {
			dup2(nullfd, 0);
			if (services[i].logpipe[1] != -1)
				dup2(services[i].logpipe[1], 1);
			else if (globallog[1] != -1)
				dup2(globallog[1], 1);
		}

		setsid();

		execl("run", "run", (char *)0);

		status = (errno == ENOENT ? 127 : 126);
		write(alivepipefd[1], &status, 1);
		_exit(status);
	} else if (child < 0) {
		abort();	/* XXX handle retry */
	}

	close(alivepipefd[1]);
	if (read(alivepipefd[0], &status, 1) == 1) {
		printf("exec failed with status %d\n", status);
		close(alivepipefd[0]);

		services[i].state = PROC_FATAL;
		services[i].pid = 0;
		services[i].startstop = time_now();
		services[i].timeout = 0;
		services[i].deadline = 0;
		return;
	}
	close(alivepipefd[0]);

	services[i].pid = child;
	services[i].startstop = time_now();
	services[i].state = PROC_STARTING;
	services[i].timeout = 2000;
	services[i].deadline = 0;
}

void
proc_shutdown(int i)
{
	if (services[i].pid)
		kill(services[i].pid, SIGTERM);

	if (services[i].state != PROC_SHUTDOWN &&
	    services[i].state != PROC_RESTART) {
		services[i].state = PROC_SHUTDOWN;
		services[i].timeout = 7000;
		services[i].deadline = 0;
	}
}

void
proc_kill(int i)
{
	if (services[i].pid)
		kill(services[i].pid, SIGKILL);

	assert(services[i].state == PROC_SHUTDOWN ||
	    services[i].state == PROC_RESTART);
}

void
proc_cleanup(int i)
{
	services[i].pid = 0;
	services[i].timeout = 0;
	services[i].deadline = 0;
	services[i].state = PROC_DOWN;
	services[i].startstop = time_now();

	if (global_state != GLBL_UP) {
		if (services[i].logpipe[0] > 0)
			close(services[i].logpipe[0]);
		if (services[i].logpipe[1] > 0)
			close(services[i].logpipe[1]);
	}
}

void
proc_zap(int i) {
	// XXX clean up log pipe?

	if (!services[i].seen) {
		printf("can garbage-collect %s\n", services[i].name);
		services[i] = services[--max_service];
	}
}


void
process_step(int i, enum process_events ev)
{
	printf("process %s[%d] state %d step %d\n",
	    services[i].name, services[i].pid,
	    services[i].state, ev);

	switch (ev) {
	case EVNT_WANT_UP:
		if (global_state != GLBL_UP)
			break;
		switch (services[i].state) {
		case PROC_STARTING:
		case PROC_UP:
		case PROC_DEAD:
		case PROC_RESTART:
			/* ignore, is up */
			break;

		case PROC_SHUTDOWN:
			services[i].state = PROC_RESTART;
			break;

		case PROC_DOWN:
		case PROC_FATAL:
		case PROC_DELAY:
			proc_launch(i);
			break;
		}
		break;

	case EVNT_WANT_DOWN:
		switch (services[i].state) {
		case PROC_STARTING:
		case PROC_UP:
		case PROC_DEAD:
		case PROC_RESTART:
		case PROC_SHUTDOWN:
			proc_shutdown(i);
			break;

		case PROC_FATAL:
		case PROC_DELAY:
			services[i].state = PROC_DOWN;
			services[i].timeout = 0;
			services[i].deadline = 0;

		case PROC_DOWN:
			/* ignore, is down */
			break;
		}
		break;

	case EVNT_WANT_RESTART:
		if (global_state != GLBL_UP)
			break;
		switch (services[i].state) {
		case PROC_STARTING:
		case PROC_UP:
		case PROC_DEAD:
		case PROC_RESTART:
		case PROC_SHUTDOWN:
			proc_shutdown(i);
			services[i].state = PROC_RESTART;
			break;

		case PROC_DOWN:
		case PROC_FATAL:
		case PROC_DELAY:
			proc_launch(i);
			break;
		}
		break;

	case EVNT_EXITED:
		services[i].timeout = 0;
		services[i].deadline = 0;
		switch (services[i].state) {
		case PROC_STARTING:
			proc_cleanup(i);
			if (global_state != GLBL_UP)
				break;
			services[i].state = PROC_DELAY;
			services[i].timeout = 1000;
			services[i].deadline = 0;
			break;

		case PROC_UP:
		case PROC_DEAD:
		case PROC_RESTART:
			proc_cleanup(i);
			if (global_state != GLBL_UP)
				break;
			proc_launch(i);
			break;

		case PROC_SHUTDOWN:
			proc_cleanup(i);
			proc_zap(i);
			break;

		case PROC_DOWN:                /* can't happen */
		case PROC_FATAL:                  /* can't happen */
		case PROC_DELAY:                  /* can't happen */
			assert(!"invalid state transition");
			break;
		}
		break;


	case EVNT_TIMEOUT:
		services[i].timeout = 0;
		services[i].deadline = 0;
		switch (services[i].state) {
		case PROC_DELAY:
			proc_launch(i);
			break;

		case PROC_STARTING:
			services[i].state = PROC_UP;
			break;

		case PROC_DEAD:
			proc_shutdown(i);
			break;

		case PROC_RESTART:
		case PROC_SHUTDOWN:
			proc_kill(i);
			break;

		case PROC_UP:
		case PROC_DOWN:
		case PROC_FATAL:
			assert(!"invalid timeout handler");
			break;
		}
		break;
	}
}


void
on_signal(int sig)
{
	int old_errno = errno;

	switch (sig) {
	case SIGCHLD:
		write(selfpipe[1], "", 1);
		break;
	case SIGINT:		/* XXX for debugging */
	case SIGTERM:
		want_shutdown = 1;
		break;
	case SIGHUP:
		want_rescan = 1;
		break;
	}

	errno = old_errno;
}

int
find_service(char *name)
{
	for (int i = 0; i < max_service; i++)
		if (strcmp(services[i].name, name) == 0)
			return i;

	return -1;
}

int
add_service(char *name)
{
	int i;
	for (i = 0; i < max_service; i++)
		if (strcmp(services[i].name, name) == 0)
			break;

	if (i == max_service) {
		max_service++;

		strcpy(services[i].name, name);
		services[i].pid = 0;
		services[i].logpipe[0] = -1;
		services[i].logpipe[1] = -1;
		services[i].state = PROC_DELAY;
		services[i].timeout = 1;
		services[i].deadline = 0;
		services[i].islog = 0;
	}

	services[i].seen = 1;
	return i;
}

void
rescan(int first)
{
	int i;
	for (i = 0; i < max_service; i++)
		services[i].seen = 0;

	DIR *dir = opendir(".");
	if (!dir)
	       abort();

	struct dirent *ent;
	while ((ent = readdir(dir))) {
		char *name = ent->d_name;
		struct stat st;

		if (name[0] == '.')
			continue;
		if (stat(name, &st) < 0)
			continue;
		if (!S_ISDIR(st.st_mode))
			continue;

		printf("SCAN %s\n", name);

		int i = add_service(name);

		if (strcmp(name, "LOG") == 0 && globallog[0] == -1) {
			printf("making global pipe\n");
			pipe(globallog);
			fcntl(globallog[1], F_SETFL, O_NONBLOCK);
			fcntl(globallog[0], F_SETFD, FD_CLOEXEC);
			fcntl(globallog[1], F_SETFD, FD_CLOEXEC);
		}

		char buf[PATH_MAX];

		if (first) {
			snprintf(buf, sizeof buf, "%s/down", name);
			if (stat(buf, &st) == 0) {
				services[i].state = PROC_DOWN;
				services[i].timeout = 0;
			}
		}

		snprintf(buf, sizeof buf, "%s/log", name);
		printf("buf=%s\n" ,buf);

		if (stat(buf, &st) == 0 && S_ISDIR(st.st_mode)) {
			printf("SCAN %s\n", buf);
			int j = add_service(buf);
			services[j].islog = 1;
			if (services[j].logpipe[0] == -1) {
				pipe(services[i].logpipe);
				fcntl(services[i].logpipe[0], F_SETFD, FD_CLOEXEC);
				fcntl(services[i].logpipe[1], F_SETFD, FD_CLOEXEC);
				services[j].logpipe[0] = services[i].logpipe[0];
				services[j].logpipe[1] = services[i].logpipe[1];
			}
		}
	}
	closedir(dir);

	for (i = 0; i < max_service; i++)
		if (!services[i].seen)
			process_step(i, EVNT_WANT_DOWN);
}

void
do_shutdown(int state)
{
	global_state = state;
	for (int i = 0; i < max_service; i++) {
		if (services[i].islog)
			continue;
		if (strcmp(services[i].name, "LOG") == 0)
			continue;

		printf("want down %d %d\n", i, services[i].state);
		process_step(i, EVNT_WANT_DOWN);
	}
}

void
open_control_socket() {
	static const char default_sock[] = "/run/nitro/nitro.sock";
	const char *path = getenv("NITRO_SOCK");
	if (!path || !*path)
		path = default_sock;

	char *last_slash = strrchr(path, '/');
	if (last_slash) {
		char dir[PATH_MAX];
		memcpy(dir, path, last_slash - path);
		dir[last_slash - path] = 0;
		mkdir(dir, 0700);
		// ignore errors
	}

	struct sockaddr_un addr = { 0 };
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof addr.sun_path - 1);

	controlsock = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (controlsock < 0) {
		perror("nitro: socket");
		exit(111);
	}
	fcntl(controlsock, F_SETFD, FD_CLOEXEC);

	unlink(path);
	mode_t mask = umask(0077);
	int r = bind(controlsock, (struct sockaddr *)&addr, sizeof addr);
	umask(mask);
	if (r < 0) {
		perror("nitro: bind");
		exit(111);
	}
}

int
charsig(char c)
{
	switch (c) {
	case 'p': return SIGSTOP;
	case 'c': return SIGCONT;
	case 'h': return SIGHUP;
	case 'a': return SIGALRM;
	case 'i': return SIGINT;
	case 'q': return SIGQUIT;
	case '1': return SIGUSR1;
	case '2': return SIGUSR2;
	case 't': return SIGTERM;
	case 'k': return SIGKILL;
	default: return 0;
	}
}

const char *
proc_state_str(enum process_state state)
{
	switch (state) {
	case PROC_DOWN: return "DOWN";
	case PROC_STARTING: return "STARTING";
	case PROC_UP: return "UP";
	case PROC_DEAD: return "DEAD";
	case PROC_SHUTDOWN: return "SHUTDOWN";
	case PROC_RESTART: return "RESTART";
	case PROC_FATAL: return "FATAL";
	case PROC_DELAY: return "DELAY";
	}

	assert(!"unreachable");
};

void
handle_control_sock() {
	char buf[256];
	struct sockaddr_un src;
	socklen_t srclen = sizeof src;
	ssize_t r = recvfrom(controlsock, buf, sizeof buf,
	    MSG_DONTWAIT, (struct sockaddr *)&src, &srclen);

	if (r == sizeof buf) {
		printf("message truncated");
	}

	if (r < 0) {
		if (errno == EAGAIN)
			return;
		printf("callback error: %m\n");
		return;
	}

	printf(">>> got %ld from %d\n", r, srclen);

	if (r == 0)
		return;

	buf[r] = 0;
	// chop trailing newline
	if (buf[r - 1] == '\n')
		buf[r - 1] = 0;

	switch (buf[0]) {
	case 'l':
	{
		char replybuf[4096];
		char *replyend = replybuf + sizeof replybuf;
		char *reply = replybuf;
		deadline now = time_now();

		for (int i = 0; i < max_service; i++) {
			if (services[i].pid)
				reply = steprn(reply, replyend, "%s %s (pid %d) %ds\n",
				    proc_state_str(services[i].state),
				    services[i].name,
				    services[i].pid,
				    (now - services[i].startstop) / 1000);
			else
				reply = steprn(reply, replyend, "%s %s %ds\n",
				    proc_state_str(services[i].state),
				    services[i].name,
				    (now - services[i].startstop) / 1000);
		}

		sendto(controlsock, replybuf, reply - replybuf,
		    MSG_DONTWAIT, (struct sockaddr *)&src, srclen);
		return;
	}
	case 's':
		want_rescan = 1;
		goto ok;
	case 'u':
	case 'd':
	case 'r':
	{
		int i = find_service(buf + 1);
		if (i < 0)
			goto fail;

		if (buf[0] == 'u')
			process_step(i, EVNT_WANT_UP);
		else if (buf[0] == 'd')
			process_step(i, EVNT_WANT_DOWN);
		else if (buf[0] == 'r')
			process_step(i, EVNT_WANT_RESTART);
		goto ok;
	}
	case 'S':
		want_shutdown = 1;
		goto ok;
	case 'R':
		do_shutdown(GLBL_WANT_REBOOT);
		goto ok;
	default:
		if (!charsig(buf[0]))
			goto fail;
		int i = find_service(buf + 1);
		printf("%s is %d\n", buf+1, i);
		if (i >= 0 && services[i].pid) {
			kill(services[i].pid, charsig(buf[0]));
			goto ok;
		}
		goto fail;
	}

ok:
	if (srclen > 0) {
		char reply[] = "ok\n";
		sendto(controlsock, reply, sizeof reply,
		    MSG_DONTWAIT, (struct sockaddr *)&src, srclen);
	}
	return;
fail:
	if (srclen > 0) {
		char reply[] = "error\n";
		sendto(controlsock, reply, sizeof reply,
		    MSG_DONTWAIT, (struct sockaddr *)&src, srclen);
	}
}


#define CHLD 0
#define CTRL 1

void
has_died(pid_t pid, int status)
{
	for (int i = 0; i < max_service; i++) {
		if (services[i].pid == pid) {
			printf("service %s[%d] has died with status %d\n",
			    services[i].name, pid, status);
			process_step(i, EVNT_EXITED);
			return;
		}

		// XXX handle logger?
	}
}

int
main(int argc, char *argv[])
{
	int i;

	chdir(argv[1]);

	nullfd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	if (nullfd < 0) {
		perror("nitro: open /dev/null");

		// use a closed pipe instead
		int fd[2];
		pipe(fd);
		nullfd = fd[0];
		close(fd[1]);
	}

	pipe(selfpipe);
	fcntl(selfpipe[0], F_SETFL, O_NONBLOCK);
	fcntl(selfpipe[1], F_SETFL, O_NONBLOCK);
	fcntl(selfpipe[0], F_SETFD, FD_CLOEXEC);
	fcntl(selfpipe[1], F_SETFD, FD_CLOEXEC);

	globallog[0] = -1;
	globallog[1] = -1;

	sigset_t allset;
	sigfillset(&allset);
	struct sigaction sa = {
		.sa_handler = on_signal,
		.sa_mask = allset,
		.sa_flags = SA_NOCLDSTOP | SA_RESTART,
	};
	sigaction(SIGCHLD, &sa, 0);
	sigaction(SIGHUP, &sa, 0);
	sigaction(SIGINT, &sa, 0);
/* for debugging
	sigaction(SIGTERM, &sa, 0);
*/

	open_control_socket();

	global_state = GLBL_UP;

	rescan(1);

	struct pollfd fds[2];
	fds[CHLD].fd = selfpipe[0];
	fds[CHLD].events = POLLIN;
	fds[CTRL].fd = controlsock;
	fds[CTRL].events = POLLIN;

	while (1) {
		deadline now = time_now();

		int timeout = -1;

		printf("\nnow = %ld\n", now);
		for (i = 0; i < max_service; i++) {
			printf("-> %s[%d] %d %d %ld\n", services[i].name,
			    services[i].pid, services[i].state,
			    services[i].timeout,
			    services[i].deadline
			);
		}

		for (i = 0; i < max_service; i++) {
printf("TO %s %d\n", services[i].name, services[i].timeout);
			if (services[i].timeout <= 0)
				continue;

			if (services[i].deadline == 0)
				services[i].deadline = now + services[i].timeout;

			if (services[i].deadline <= now) {
				printf("timeout for %d\n", i);
				process_step(i, EVNT_TIMEOUT);
			}

			if (services[i].timeout <= 0)
				continue;

			if (services[i].deadline == 0)
				services[i].deadline = now + services[i].timeout;

			int64_t wait_for = services[i].deadline - now;
			if (wait_for > 0) {
				if (timeout == -1 || (wait_for < timeout))
					timeout = wait_for;
			}
		}

		printf("poll(timeout=%d)\n", timeout);

		poll(fds, 2, timeout);

		if (fds[CHLD].revents & POLLIN) {
			char ch;

			printf("data on self pipe\n");

			while (read(selfpipe[0], &ch, 1) == 1)
				;
			errno = 0;
		}

		while (1) {
			int wstatus = 0;
			int r = waitpid(-1, &wstatus, WNOHANG);
			if (r == 0 || (r < 0 && errno == ECHILD))
				break;
			if (r < 0)
				abort();
			has_died(r, wstatus);
		}

		if (fds[CTRL].revents & POLLIN) {
			handle_control_sock();
		}

		if (want_rescan) {
			rescan(0);
			want_rescan = 0;
		}

		if (want_shutdown) {
			do_shutdown(GLBL_WANT_SHUTDOWN);
		}

		if (global_state != GLBL_UP) {
			int up = 0;
			int uplog = 0;
			for (i = 0; i < max_service; i++) {
				printf("DBG %s %d %d\n", services[i].name, services[i].pid, services[i].state);
				if (!(services[i].state == PROC_DOWN ||
				    services[i].state == PROC_FATAL)) {
					up++;
					if (services[i].islog)
						uplog++;
					if (strcmp(services[i].name, "LOG"))
						uplog++;
				}
			}
			if (up) {
				printf("shutdown waiting for %d processes\n", up);
				if (up == uplog) {
					printf("signalling %d log processes\n", uplog);
					for (int i = 0; i < max_service; i++)
						if (services[i].islog || strcmp(services[i].name, "LOG") == 0)
							process_step(i, EVNT_WANT_DOWN);
				}
			} else {
				break;
			}
		}
	}
}
