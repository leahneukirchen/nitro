#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#ifdef __linux__
#include <sys/mount.h>
#include <sys/reboot.h>
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <paths.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define DELAY_SPAWN_ERROR 2000   /* ms to wait when fork failed */
#define DELAY_STARTING 2000      /* ms until s service is considered up */
#define DELAY_RESPAWN 1000       /* ms to wait if a serviced crashed when starting */
#define TIMEOUT_SHUTDOWN 7000    /* ms before killing a service */
#define TIMEOUT_FINISH 7000      /* ms before killing the service finish script */
#define TIMEOUT_SIGTERM 7000     /* max wait after SIGTERM */
#define TIMEOUT_SIGKILL 7000     /* max wait after SIGKILL */
#define TIMEOUT_SYS_FINISH 30000 /* ms before killing SYS/finish */
#define TIMEOUT_SYS_FINAL 30000  /* ms before killing SYS/final */

/* no stdio */
#ifdef DEBUG
#define assert(x)                                                           \
	do { if (!(x)) { prn(2, "%s:%d: %s: error: assertion failed: %s\n", \
	     __FILE__, __LINE__, __func__, #x); abort(); } } while (0)
#else
#define assert(x) do { if (!(x)) abort(); } while (0)
#endif

extern char **environ;
char **child_environ;
#define ENVSIZE 64
char *envbuf[ENVSIZE+1];

typedef int64_t deadline;               /* milliseconds since boot */

deadline time_now()
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

enum global_state {
	GLBL_UP = 0,
	GLBL_WAIT_FINISH,
	GLBL_SHUTDOWN,
	GLBL_WAIT_TERM,
	GLBL_WAIT_KILL,
	GLBL_FINAL,
} global_state;

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

enum process_events {
	EVNT_TIMEOUT = 1,
	EVNT_WANT_UP,
	EVNT_WANT_DOWN,
	EVNT_WANT_RESTART,
	EVNT_SETUP,             /* setup script exited */
	EVNT_EXITED,
	EVNT_FINISHED,          /* finish script exited */
};

/* max fd usage: 500 services (250 loggers) = 1000 fd for logpipes + const. */
#ifndef MAXSV
#define MAXSV 500
#endif

struct service {
	char name[64];
	deadline startstop;
	deadline deadline;
	int timeout;
	pid_t pid;
	pid_t setuppid;
	pid_t finishpid;
	int wstatus;
	int logpipe[2];
#ifdef DEBUG
	enum process_state state;
#else
	char /* enum process_state */ state;
#endif
	char seen;
	char islog;
} services[MAXSV];

int max_service;
int controlsock;
int nullfd;
int selfpipe[2];
int globallog[2];
DIR *cwd;
DIR *notifydir;
char notifypath[256];
const char *control_socket_path;

int pid1;
int real_pid1;

volatile sig_atomic_t want_rescan;
volatile sig_atomic_t want_shutdown;
volatile sig_atomic_t want_reboot;

static ssize_t
safe_write(int fd, const char *buf, size_t len)
{
	size_t off = 0;
	while (off < len) {
		ssize_t r = write(fd, buf + off, len - off);
		if (r == -1 && errno == EINTR)
			continue;
		if (r == -1)
			return -1;
		if (r == 0) {  /* can't happen */
			errno = EIO;
			return -1;
		}
		off += r;
	}
	return off;
}

static char *
stecpe(char *dst, const char *end, const char *src, const char *srcend)
{
	if (dst >= end)
		return dst;

	ptrdiff_t l = end - dst - 1;
	size_t t = 1;
	if (srcend - src < l) {
		l = srcend - src;
		t = 0;
	}

	memcpy(dst, src, l);
	dst[l] = 0;

	return dst + l + t;
}

static char *
stecpy(char *dst, char *end, const char *src)
{
	if (dst >= end)
		return dst;

	while (dst < end && (*dst = *src))
		src++, dst++;

	if (dst == end)
		dst[-1] = 0;

	return dst;
}

static char *
steprl(char *dst, char *end, long n)
{
	if (dst >= end)
		return end;

	char buf[24];
	char *bufend = buf + sizeof buf;
	char *s = bufend;

	unsigned long u = n < 0 ? -n : n;

	do {
		*--s = '0' + (u % 10);
		u /= 10;
	} while (u);

	if (n < 0)
		*--s = '-';

	return stecpe(dst, end, s, bufend);
}

size_t
sprn(char *out, char *oute, const char *fmt, ...)
{
	char *outs = out;
	va_list ap;

	va_start(ap, fmt);

	for (const char *s = fmt; *s && out < oute; s++) {
		if (*s == '%') {
			s++;
			if (*s == 'd') {
				out = steprl(out, oute, (int)va_arg(ap, int));
			} else if (*s == 's') {
				out = stecpy(out, oute, va_arg(ap, char *));
			} else if (*s == 'c') {
				*out++ = va_arg(ap, int);
			} else {
				abort();
			}
		} else {
			*out++ = *s;
		}
	}

	*out = 0;

	va_end(ap);

	return out - outs;
}

int
prn(int fd, const char *fmt, ...)
{
	char buf[2048];
	char *out = buf;
	char *oute = buf + sizeof buf;
	va_list ap;

	va_start(ap, fmt);

	for (const char *s = fmt; *s && out < oute; s++) {
		if (*s == '%') {
			s++;
			if (*s == 'd') {
				out = steprl(out, oute, (int)va_arg(ap, int));
			} else if (*s == 's') {
				out = stecpy(out, oute, va_arg(ap, char *));
			} else {
				abort();
			}
		} else {
			*out++ = *s;
		}
	}

	va_end(ap);

	return safe_write(fd, buf, out - buf);
}

#define fatal(...) do { prn(2, "- nitro: " __VA_ARGS__); exit(111); } while (0)
#ifdef DEBUG
#define dprn(...) prn(2, __VA_ARGS__)
#else
#define dprn(...) /**/
#endif

int
stat_slash(const char *dir, const char *name, struct stat *st)
{
	char buf[PATH_MAX];
	sprn(buf, buf + sizeof buf, "%s/%s", dir, name);
	return stat(buf, st);
}

int
stat_slash_to_at(const char *dir, const char *name, struct stat *st)
{
	char buf[PATH_MAX];
	char *instance = strchr(dir, '@');
	if (instance)
		*instance = 0;
	sprn(buf, buf + sizeof buf, "%s%s/%s", dir, (instance ? "@" : ""), name);
	if (instance)
		*instance = '@';
	return stat(buf, st);
}

static char *
chdir_at(char *dir)
{
	char *instance = strchr(dir, '@');
	char s = 0;
	if (instance) {
		instance++;
		s = *instance;
		*instance = 0;
	}
	chdir(dir);
	if (instance)
		*instance = s;
	return instance;
}

void process_step(int i, enum process_events ev);
void notify(int);
void slayall();

void
proc_launch(int i)
{
	services[i].setuppid = 0;

	struct stat st;
	if (stat_slash_to_at(services[i].name, "run", &st) < 0 && errno == ENOENT) {
		services[i].pid = 0;
		services[i].startstop = time_now();
		services[i].state = PROC_ONESHOT;
		services[i].timeout = 0;
		services[i].deadline = 0;

		return;
	}

	unsigned char status;
	int alivepipefd[2];
	if (pipe(alivepipefd) < 0) {
		/* pipe failed, delay */
		prn(2, "- nitro: can't create status pipe: errno=%d\n", errno);
		services[i].state = PROC_DELAY;
		services[i].timeout = DELAY_SPAWN_ERROR;
		services[i].deadline = 0;
		return;
	}
	fcntl(alivepipefd[0], F_SETFD, FD_CLOEXEC);
	fcntl(alivepipefd[1], F_SETFD, FD_CLOEXEC);

	pid_t child = fork();
	if (child == 0) {
		char *instance = chdir_at(services[i].name);

		setsid();

		if (strcmp(services[i].name, "LOG") == 0) {
			dup2(globallog[0], 0);
			dup2(1, 2);
		} else if (services[i].islog) {
			dup2(services[i].logpipe[0], 0);
		} else {
			dup2(nullfd, 0);
			if (services[i].logpipe[1] != -1)
				dup2(services[i].logpipe[1], 1);
			else if (globallog[1] > 0)
				dup2(globallog[1], 1);
			// else keep fd 1 to /dev/console
		}

		if (instance)
			execle("run", "run", instance, (char *)0, child_environ);
		else
			execle("run", "run", (char *)0, child_environ);

		status = (errno == ENOENT ? 127 : 126);
		write(alivepipefd[1], &status, 1);
		_exit(status);
	} else if (child < 0) {
		/* fork failed, delay */
		close(alivepipefd[0]);
		close(alivepipefd[1]);
		services[i].state = PROC_DELAY;
		services[i].timeout = DELAY_SPAWN_ERROR;
		services[i].deadline = 0;
		return;
	}

	close(alivepipefd[1]);
	if (read(alivepipefd[0], &status, 1) == 1) {
		dprn("exec failed with status %d\n", status);
		close(alivepipefd[0]);

		services[i].state = PROC_FATAL;
		services[i].wstatus = -1;
		services[i].pid = 0;
		services[i].startstop = time_now();
		services[i].timeout = 0;
		services[i].deadline = 0;

		process_step(i, EVNT_EXITED);

		return;
	}
	close(alivepipefd[0]);

	if (strcmp(services[i].name, "LOG") == 0)
		globallog[1] = -globallog[1];

	services[i].pid = child;
	services[i].startstop = time_now();
	services[i].state = PROC_STARTING;
	services[i].timeout = DELAY_STARTING;
	services[i].deadline = 0;
}

void
proc_setup(int i)
{
	struct stat st;
	if (stat_slash_to_at(services[i].name, "setup", &st) < 0 && errno == ENOENT) {
		services[i].state = PROC_SETUP;
		process_step(i, EVNT_SETUP);
		return;
	}

	pid_t child = fork();
	if (child == 0) {
		char *instance = chdir_at(services[i].name);

		setsid();

		if (strcmp(services[i].name, "SYS") == 0) {
			// keep fd connected to console, acquire controlling tty
			// only works after setsid!
			ioctl(0, TIOCSCTTY, 1);
		} else {
			dup2(nullfd, 0);
		}

		if (services[i].logpipe[1] != -1)
			dup2(services[i].logpipe[1], 1);
		else if (globallog[1] > 0)
			dup2(globallog[1], 1);
		// else keep fd 1 to /dev/console

		if (instance)
			execle("setup", "setup", instance, (char *)0, child_environ);
		else
			execle("setup", "setup", (char *)0, child_environ);
		_exit(127);
	} else if (child < 0) {
		/* fork failed, delay */
		services[i].state = PROC_DELAY;
		services[i].timeout = DELAY_SPAWN_ERROR;
		services[i].deadline = 0;
		return;
	}

	// XXX use alivepipe?

	services[i].setuppid = child;
	services[i].startstop = time_now();
	services[i].state = PROC_SETUP;
	services[i].timeout = 0;
	services[i].deadline = 0;
}

void
proc_finish(int i)
{
	if (services[i].finishpid)
		return;

	struct stat st;
	if (stat_slash_to_at(services[i].name, "finish", &st) < 0 && errno == ENOENT) {
		process_step(i, EVNT_FINISHED);
		return;
	}

	int wstatus = services[i].wstatus;
	int status;
	int signal;
	char run_status[16];
	char run_signal[16];
	if (wstatus == -1) {
		// exec failed;
		status = 111;
		signal = 0;
	} else if (WIFSIGNALED(wstatus)) {
		status = -1;
		signal = WTERMSIG(wstatus);
	} else {
		status = WEXITSTATUS(wstatus);
		signal = 0;
	}
	steprl(run_status, run_status + sizeof run_status, status);
	steprl(run_signal, run_signal + sizeof run_signal, signal);

	pid_t child = fork();
	if (child == 0) {
		char *instance = chdir_at(services[i].name);

		dup2(nullfd, 0);
		if (services[i].logpipe[1] != -1)
			dup2(services[i].logpipe[1], 1);
		else if (globallog[1] > 0)
			dup2(globallog[1], 1);
		// else keep fd 1 to /dev/console

		setsid();

		if (strcmp(services[i].name, "SYS") == 0) {
			if (want_reboot)
				instance = (char *)"reboot";
			else
				instance = (char *)"shutdown";
		}

		if (instance)
			execle("finish", "finish", run_status, run_signal, instance, (char *)0, child_environ);
		else
			execle("finish", "finish", run_status, run_signal, (char *)0, child_environ);
		_exit(127);
	} else if (child < 0) {
		/* fork failed, skip over the finish script */
		process_step(i, EVNT_FINISHED);
		return;
	}

	services[i].finishpid = child;
	services[i].timeout = TIMEOUT_FINISH;
	services[i].deadline = 0;
}

void
proc_shutdown(int i)
{
	if (services[i].setuppid) {
		kill(services[i].setuppid, SIGTERM);
		kill(services[i].setuppid, SIGCONT);
	}

	if (services[i].pid) {
		kill(services[i].pid, SIGTERM);
		kill(services[i].pid, SIGCONT);
	}

	if (strcmp(services[i].name, "LOG") == 0)
		globallog[1] = -globallog[1];

	if (services[i].state != PROC_SHUTDOWN &&
	    services[i].state != PROC_RESTART) {
		services[i].state = PROC_SHUTDOWN;
		services[i].timeout = TIMEOUT_SHUTDOWN;
		services[i].deadline = 0;
	}
}

void
proc_kill(int i)
{
	assert(services[i].state == PROC_SHUTDOWN ||
	    services[i].state == PROC_RESTART ||
	    services[i].state == PROC_ONESHOT);

	if (services[i].setuppid)
		kill(services[i].setuppid, SIGKILL);

	if (services[i].pid)
		kill(services[i].pid, SIGKILL);

	if (services[i].finishpid)
		kill(services[i].finishpid, SIGKILL);
}

void
proc_cleanup(int i)
{
	services[i].pid = 0;
	services[i].setuppid = 0;
	services[i].finishpid = 0;
	services[i].timeout = 0;
	services[i].deadline = 0;
	services[i].state = PROC_DOWN;
	services[i].startstop = time_now();

	if (global_state != GLBL_UP) {
		if (services[i].logpipe[0] > 0)
			close(services[i].logpipe[0]);
		if (services[i].logpipe[1] > 0)
			close(services[i].logpipe[1]);
		services[i].logpipe[0] = -1;
		services[i].logpipe[1] = -1;
	}

	notify(i);
}

void
proc_zap(int i) {
	/* close the logpipe and remove all references to it */
	if (services[i].islog) {
		int fd;

		fd = services[i].logpipe[0];
		if (fd > 0) {
			for (int i = 0; i < max_service; i++)
				if (services[i].logpipe[0] == fd)
					services[i].logpipe[0] = -1;
			close(fd);
		}

		fd = services[i].logpipe[1];
		if (fd > 0) {
			for (int i = 0; i < max_service; i++)
				if (services[i].logpipe[1] == fd)
					services[i].logpipe[1] = -1;
			close(fd);
		}
	}

	if (!services[i].seen) {
		dprn("can garbage-collect %s\n", services[i].name);
		if (max_service > 0) {
			services[i] = services[--max_service];
		} else {
			assert(i == 0);
			services[i] = (struct service) { 0 };
		}
	}
}

void
process_step(int i, enum process_events ev)
{
	dprn("process %s[%d] state %d step %d\n",
	    services[i].name, services[i].pid,
	    services[i].state, ev);

	switch (ev) {
	case EVNT_WANT_UP:
		if (global_state != GLBL_UP)
			break;
		switch (services[i].state) {
		case PROC_ONESHOT:
		case PROC_SETUP:
		case PROC_STARTING:
		case PROC_UP:
		case PROC_RESTART:
			/* ignore, is up */
			break;

		case PROC_SHUTDOWN:
			services[i].state = PROC_RESTART;
			break;

		case PROC_DOWN:
		case PROC_FATAL:
		case PROC_DELAY:
			proc_setup(i);
			break;
		}
		break;

	case EVNT_WANT_DOWN:
		switch (services[i].state) {
		case PROC_SETUP:
		case PROC_STARTING:
		case PROC_UP:
		case PROC_RESTART:
		case PROC_SHUTDOWN:
			proc_shutdown(i);
			break;

		case PROC_ONESHOT:
			proc_finish(i);
			break;

		case PROC_FATAL:
		case PROC_DELAY:
		case PROC_DOWN:
			services[i].state = PROC_DOWN;
			services[i].timeout = 0;
			services[i].deadline = 0;
			break;
		}
		break;

	case EVNT_WANT_RESTART:
		if (global_state != GLBL_UP)
			break;
		switch (services[i].state) {
		case PROC_SETUP:
		case PROC_STARTING:
		case PROC_UP:
		case PROC_RESTART:
		case PROC_SHUTDOWN:
			proc_shutdown(i);
			services[i].state = PROC_RESTART;
			break;

		case PROC_ONESHOT:
			proc_finish(i);
			services[i].state = PROC_RESTART;
			break;

		case PROC_DOWN:
		case PROC_FATAL:
		case PROC_DELAY:
			proc_setup(i);
			break;
		}
		break;

	case EVNT_SETUP:
		switch (services[i].state) {
		case PROC_ONESHOT:
		case PROC_STARTING:
		case PROC_UP:
		case PROC_RESTART:
		case PROC_SHUTDOWN:
		case PROC_FATAL:
		case PROC_DELAY:
		case PROC_DOWN:
			assert(!"invalid state transition");

		case PROC_SETUP:
			if (global_state == GLBL_UP)
				proc_launch(i);
			else
				proc_cleanup(i);
			break;
		}
		break;

	case EVNT_EXITED:
		services[i].timeout = 0;
		services[i].deadline = 0;
		switch (services[i].state) {
		case PROC_UP:
			services[i].state = PROC_RESTART;
			proc_finish(i);
			break;

		case PROC_STARTING:
		case PROC_RESTART:
		case PROC_SHUTDOWN:
		case PROC_FATAL:
			proc_finish(i);
			break;

		case PROC_SETUP:               /* can't happen */
		case PROC_DOWN:                /* can't happen */
		case PROC_DELAY:               /* can't happen */
		case PROC_ONESHOT:             /* can't happen */
			assert(!"invalid state transition");
			break;
		}
		break;

	case EVNT_FINISHED:
		services[i].timeout = 0;
		services[i].deadline = 0;
		switch (services[i].state) {
		case PROC_STARTING:
			proc_cleanup(i);
			if (global_state != GLBL_UP)
				break;
			services[i].state = PROC_DELAY;
			services[i].timeout = DELAY_RESPAWN;
			services[i].deadline = 0;
			break;

		case PROC_UP:
		case PROC_RESTART:
			proc_cleanup(i);
			if (global_state != GLBL_UP)
				break;
			proc_setup(i);
			break;

		case PROC_ONESHOT:
		case PROC_SHUTDOWN:
			proc_cleanup(i);
			proc_zap(i);
			break;

		case PROC_FATAL:
			proc_cleanup(i);
			services[i].state = PROC_FATAL;
			notify(i);
			break;

		case PROC_SETUP:               /* can't happen */
		case PROC_DOWN:                /* can't happen */
		case PROC_DELAY:               /* can't happen */
			assert(!"invalid state transition");
			break;
		}
		break;

	case EVNT_TIMEOUT:
		services[i].timeout = 0;
		services[i].deadline = 0;
		switch (services[i].state) {
		case PROC_DELAY:
			if (global_state == GLBL_WAIT_TERM) {
				slayall();
			} else if (global_state == GLBL_WAIT_KILL) {
				global_state = GLBL_FINAL;
			} else {
				proc_setup(i);
			}
			break;

		case PROC_STARTING:
			services[i].state = PROC_UP;
			notify(i);
			break;

		case PROC_RESTART:
		case PROC_SHUTDOWN:
		case PROC_ONESHOT:
			proc_kill(i);
			break;

		case PROC_UP:
		case PROC_DOWN:
		case PROC_FATAL:
		case PROC_SETUP:
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
	case SIGPIPE:           /* ignore, but don't use SIG_IGN */
		return;
	case SIGINT:
		if (real_pid1)
			want_reboot = 1;    /* Linux Ctrl-Alt-Delete */
		else
			want_shutdown = 1;
		break;
	case SIGTERM:
		want_shutdown = 1;
		break;
	case SIGHUP:
		want_rescan = 1;
		break;
	case SIGCHLD:           /* just selfpipe */
		break;
	}

	ssize_t r;
	do {
		r = write(selfpipe[1], "", 1);
	} while (r == -1 && errno == EINTR);

	errno = old_errno;
}

int
find_service(const char *name)
{
	for (int i = 0; i < max_service; i++)
		if (strcmp(services[i].name, name) == 0)
			return i;

	return -1;
}

int
add_service(const char *name)
{
	int i;
	for (i = 0; i < max_service; i++)
		if (strcmp(services[i].name, name) == 0)
			goto refresh_log;

	struct stat st;
	if (stat_slash_to_at(name, "run", &st) != 0 &&
	    stat_slash_to_at(name, "setup", &st) != 0) {
		prn(2, "- nitro: no such service: %s\n", name);
		return -1;
	}

	/* else set up a new service */

	if (strlen(name) >= sizeof (services[i].name)) {
		return -1;
	} else if (max_service >= MAXSV - 1) {
		prn(2, "- nitro: too many services, limit=%d\n", MAXSV);
		return -1;
	}

	max_service++;

	strcpy(services[i].name, name);
	services[i].pid = 0;
	services[i].state = PROC_DELAY;
	services[i].startstop = time_now();
	services[i].timeout = 1;
	services[i].deadline = 0;
	services[i].islog = 0;
	if (strcmp(services[i].name, "LOG") == 0)
		services[i].islog = 1;

refresh_log:
	if (services[i].islog)
		return i;

	char log_target[PATH_MAX];
	char log_link[PATH_MAX];
	sprn(log_link, log_link + sizeof log_link, "%s/log", name);

	ssize_t r = readlink(log_link, log_target, sizeof log_target - 1);
	if (r < 0 || (size_t)r >= sizeof log_target - 1) {
		if (errno == EINVAL)
			prn(2, "warning: ignoring log, it is not a symlink: %s\n", name);
		services[i].logpipe[0] = -1;
		services[i].logpipe[1] = -1;
	} else {
		/* just interpret the last path segment as service name */
		log_target[r] = 0;
		char *target_name = strrchr(log_target, '/');
		if (target_name)
			target_name++;
		else
			target_name = log_target;

		int j = add_service(target_name);
		if (j < 0) {
			services[i].logpipe[0] = -1;
			services[i].logpipe[1] = -1;
			return i;
		}

		int waslog = services[j].islog;
		services[j].islog = 1;
		services[j].seen = 1;
		if (services[j].logpipe[0] == -1) {
			if (pipe(services[j].logpipe) < 0) {
				prn(2, "- nitro: can't create log pipe: errno=%d\n", errno);
				services[j].logpipe[0] = -1;
				services[j].logpipe[1] = -1;
			} else {
				fcntl(services[j].logpipe[0], F_SETFD, FD_CLOEXEC);
				fcntl(services[j].logpipe[1], F_SETFD, FD_CLOEXEC);
			}
		}

		services[i].logpipe[0] = services[j].logpipe[0];
		services[i].logpipe[1] = services[j].logpipe[1];

		if (!waslog)
			process_step(j, EVNT_WANT_RESTART);
	}

	return i;
}

void
rescan(int first)
{
	int i;
	for (i = 0; i < max_service; i++)
		if (!strchr(services[i].name, '@'))
			services[i].seen = 0;

	struct dirent *ent;
	rewinddir(cwd);
	while ((ent = readdir(cwd))) {
		char *name = ent->d_name;
		struct stat st;

		if (name[0] == '.')
			continue;
		if (stat(name, &st) < 0)
			continue;
		if (!S_ISDIR(st.st_mode))
			continue;

		// ignore parametrized services
		if (name[strlen(name) - 1] == '@')
			continue;

		// ignore magic bootup/shutdown service
		if (strcmp(name, "SYS") == 0)
			continue;

		int i = add_service(name);
		if (i < 0)
			continue;

		services[i].seen = 1;

		if (first && stat_slash(name, "down", &st) == 0 &&
		    !(services[i].state == PROC_UP ||
		    services[i].state == PROC_STARTING)) {
			services[i].state = PROC_DOWN;
			services[i].timeout = 0;
		}
	}

	for (i = 0; i < max_service; i++)
		if (!services[i].seen) {
			process_step(i, EVNT_WANT_DOWN);
			if (services[i].state == PROC_DOWN)
				proc_zap(i);
		}
}

void
own_console()
{
	int ttyfd = open("/dev/console", O_RDWR);
	if (ttyfd < 0)
		return;
	dup2(ttyfd, 0);
	dup2(ttyfd, 1);
	dup2(ttyfd, 2);
	if (ttyfd > 2)
		close(ttyfd);
	ioctl(0, TIOCSCTTY, 1);
}

void
do_stop_services() {
	global_state = GLBL_SHUTDOWN;

	int up = 0;
	for (int i = 0; i < max_service; i++) {
		if (services[i].islog)
			continue;

		process_step(i, EVNT_WANT_DOWN);

		if (!(services[i].state == PROC_DOWN ||
		    services[i].state == PROC_FATAL))
			up++;
	}

	if (up)
		prn(2, "- nitro: waiting for %d services to finish", up);
}

void
do_shutdown()
{
	if (global_state == GLBL_UP) {
		global_state = GLBL_WAIT_FINISH;

		if (want_reboot)
			prn(2, "- nitro: rebooting\n");
		else
			prn(2, "- nitro: shutting down\n");

		if (pid1)
			own_console();
#ifdef __linux__
		if (real_pid1)
			reboot(RB_ENABLE_CAD);
#endif

		struct stat st;
		if (stat("SYS/finish", &st) == 0) {
			int b = add_service("SYS");
			services[b].state = PROC_ONESHOT;
			process_step(b, EVNT_WANT_DOWN);
			/* got zapped or is down */
			if (strcmp(services[b].name, "SYS") != 0 ||
			    services[b].state == PROC_DOWN)
				do_stop_services();
			else
				services[b].timeout = TIMEOUT_SYS_FINISH;
		} else {
			do_stop_services();
		}
	}
}

void
open_control_socket() {
#ifdef __linux__
	static const char default_sock[] = "/run/nitro/nitro.sock";
#else
	static const char default_sock[] = "/var/run/nitro/nitro.sock";
#endif
	control_socket_path = getenv("NITRO_SOCK");
	if (!control_socket_path || !*control_socket_path)
		control_socket_path = default_sock;

	char *last_slash = strrchr(control_socket_path, '/');
	if (last_slash) {
		char dir[PATH_MAX];
		memcpy(dir, control_socket_path, last_slash - control_socket_path);
		dir[last_slash - control_socket_path] = 0;
		mkdir(dir, 0700);
		// ignore errors

		strcpy(notifypath, dir);
		strcat(notifypath, "/notify/");
		mkdir(notifypath, 0700);
		// ignore errors

		notifydir = opendir(notifypath);
		if (!notifydir)
			fatal("could not create notify dir %s: errno=%d\n", notifypath, errno);
	}

	struct sockaddr_un addr = { 0 };
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, control_socket_path, sizeof addr.sun_path - 1);

	controlsock = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (controlsock < 0)
		fatal("socket");
	fcntl(controlsock, F_SETFD, FD_CLOEXEC);

	mode_t mask = umask(0077);
	int r = bind(controlsock, (struct sockaddr *)&addr, sizeof addr);
	umask(mask);
	if (r < 0)
		fatal("could not bind control socket: errno=%d\n", errno);
}

int
notifyprefix(const char *name, const char *file)
{
	if (strncmp("ALL,", file, 4) == 0)
		return 1;

	// slashes are encoded as comma in the notify file name
	while (*name && (*name == *file || (*name == '/' && *file == ',')))
		name++, file++;

	return *name == 0 && *file == ',';
}

void
notify(int i)
{
	char notifybuf[128];
	sprn(notifybuf, notifybuf + sizeof notifybuf, "%c%s\n",
	    64 + services[i].state, services[i].name);

	struct dirent *ent;
	rewinddir(notifydir);
	while ((ent = readdir(notifydir))) {
		char *name = ent->d_name;

		if (name[0] == '.')
			continue;

		if (notifyprefix(services[i].name, name)) {
			struct sockaddr_un addr = { 0 };
			addr.sun_family = AF_UNIX;
			strncpy(addr.sun_path, notifypath, sizeof addr.sun_path - 1);
			strcat(addr.sun_path, name);

			sendto(controlsock, notifybuf, strlen(notifybuf),
			    MSG_DONTWAIT, (struct sockaddr *)&addr, sizeof addr);
		}
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

void
handle_control_sock() {
	char buf[256];
	struct sockaddr_un src;
	socklen_t srclen = sizeof src;
	ssize_t r = recvfrom(controlsock, buf, sizeof buf,
	    MSG_DONTWAIT, (struct sockaddr *)&src, &srclen);

	if (r < 0) {
		if (errno == EAGAIN)
			return;
		dprn("callback error: errno=%d\n", errno);
		return;
	}

	if (r == 0)
		return;

	buf[r] = 0;
	// chop trailing newline
	if (buf[r - 1] == '\n')
		buf[r - 1] = 0;

	switch (buf[0]) {
	case 'l':
	{
		if (srclen == 0)
			return;

		char replybuf[4096];
		char *replyend = replybuf + sizeof replybuf;
		char *reply = replybuf;
		deadline now = time_now();

		for (int i = 0; i < max_service; i++) {
			reply += sprn(reply, replyend, "%s %d %d %d %d\n",
			    services[i].name,
			    services[i].state,
			    services[i].pid,
			    services[i].wstatus,
			    (now - services[i].startstop) / 1000);
		}

		sendto(controlsock, replybuf, reply - replybuf,
		    MSG_DONTWAIT, (struct sockaddr *)&src, srclen);
		return;
	}
	case '?':
	{
		if (srclen == 0)
			return;

		int i = find_service(buf + 1);
		if (i < 0)
			goto fail;
		char replybuf[3] = "?\n";
		replybuf[0] = 64 + services[i].state;
		sendto(controlsock, replybuf, sizeof replybuf - 1,
		    MSG_DONTWAIT, (struct sockaddr *)&src, srclen);
		return;
	}
	case 'u':
	case 'd':
	case 'r':
	{
		struct stat st;

		if (!buf[1])
			goto fail;
		int i = find_service(buf + 1);
		if (stat_slash_to_at(buf + 1, ".", &st) == 0)
			i = add_service(buf + 1);
		if (i < 0)
			goto fail;

		if (buf[0] == 'u')
			process_step(i, EVNT_WANT_UP);
		else if (buf[0] == 'd')
			process_step(i, EVNT_WANT_DOWN);
		else if (buf[0] == 'r')
			process_step(i, EVNT_WANT_RESTART);

		notify(i);

		goto ok;
	}
	case 's':
		want_rescan = 1;
		goto ok;
	case 'S':
		want_shutdown = 1;
		goto ok;
	case 'R':
		want_reboot = 1;
		goto ok;
	default:
		if (!charsig(buf[0]))
			goto fail;
		int i = find_service(buf + 1);
		if (i >= 0 && services[i].pid) {
			kill(services[i].pid, charsig(buf[0]));
			goto ok;
		}
		goto fail;
	}

ok:
	if (srclen > 0) {
		char reply[] = "ok\n";
		sendto(controlsock, reply, sizeof reply - 1,
		    MSG_DONTWAIT, (struct sockaddr *)&src, srclen);
	}
	return;
fail:
	if (srclen > 0) {
		char reply[] = "error\n";
		sendto(controlsock, reply, sizeof reply - 1,
		    MSG_DONTWAIT, (struct sockaddr *)&src, srclen);
	}
}

void
has_died(pid_t pid, int status)
{
	for (int i = 0; i < max_service; i++) {
		if (services[i].setuppid == pid) {
			dprn("setup %s[%d] has died with status %d\n",
			    services[i].name, pid, status);

			services[i].setuppid = 0;

			if (WEXITSTATUS(status) == 0) {
				process_step(i, EVNT_SETUP);
			} else if (WEXITSTATUS(status) == 111) {
				services[i].state = PROC_FATAL;
				services[i].wstatus = -1;
				notify(i);
			} else {
				services[i].state = PROC_DELAY;
				services[i].timeout = DELAY_RESPAWN;
				services[i].deadline = 0;
			}

			if (strcmp(services[i].name, "SYS") == 0 &&
			    global_state == GLBL_UP) { /* C-A-D during SYS */
				services[i].seen = 0;
				proc_cleanup(i);
				proc_zap(i);

				prn(2, "- nitro: SYS/setup finished with status %d\n", status);

				// bring up rest of the services
				rescan(1);
			}

			return;
		}

		if (services[i].pid == pid) {
			dprn("service %s[%d] has died with status %d\n",
			    services[i].name, pid, status);
			services[i].pid = 0;
			services[i].wstatus = status;
			process_step(i, EVNT_EXITED);
			return;
		}

		if (services[i].finishpid == pid) {
			dprn("finish script %s[%d] has died with status %d\n",
			    services[i].name, pid, status);
			services[i].finishpid = 0;
			process_step(i, EVNT_FINISHED);

			if (strcmp(services[i].name, "SYS") == 0) {
				prn(2, "- nitro: SYS/finish finished\n");
				do_stop_services();
			}

			return;
		}
	}

	dprn("reaping unknown child %d\n", pid);
}

#ifdef __linux__
static int
mounted(const char *dir)
{
	struct stat rootst;
	struct stat dirst;

	/* only detects toplevel mount points */
	stat("/", &rootst);
	if (stat(dir, &dirst) < 0)
		return 1;  /* can't mount if mountpoint doesn't exist */

	return rootst.st_dev != dirst.st_dev;
}

void
init_mount()
{
	if (access("/dev/null", F_OK) == 0 && !mounted("/dev"))
		mount("dev", "/dev", "devtmpfs", MS_NOSUID, "mode=0755");
	if (!mounted("/run"))
		mount("run", "/run", "tmpfs", MS_NOSUID|MS_NODEV, "mode=0755");
}
#endif

void
killall()
{
	prn(2, "- nitro: sending SIGTERM to all processes\n");
	kill(-1, SIGTERM);
	kill(-1, SIGCONT);
	global_state = GLBL_WAIT_TERM;

	int i = add_service(".SHUTDOWN");
	services[i].state = PROC_DELAY;
	services[i].timeout = TIMEOUT_SIGTERM;
	services[i].deadline = 0;
}

void
slayall()
{
	prn(2, "- nitro: sending SIGKILL to all processes\n");

#ifdef __linux__
	/* On Linux, kill(-1, SIGKILL) can block indefinitely and hang the
	   current process when a process is stuck in state D, so fork and only
	   block the child.  */
	pid_t child = fork();
	if (child == 0) {
		kill(-1, SIGKILL);
		_exit(0);
	}
#else
	kill(-1, SIGKILL);
#endif

	global_state = GLBL_WAIT_KILL;

	int i = add_service(".SHUTDOWN");
	services[i].state = PROC_DELAY;
	services[i].timeout = TIMEOUT_SIGKILL;
	services[i].deadline = 0;
}

#undef CTRL

#define CHLD 0
#define CTRL 1

int
main(int argc, char *argv[])
{
	int i;

	pid1 = real_pid1 = (getpid() == 1);
	if (pid1) {
		umask(0022);
#ifdef __linux__
		init_mount();
		if (reboot(RB_DISABLE_CAD) < 0)
			real_pid1 = 0;  /* we are in a container */
#endif
		own_console();
	}

	// can't use putenv, which pulls in realloc
	if (!getenv("PATH")) {
		envbuf[0] = (char *)"PATH=" _PATH_DEFPATH;
		for (i = 1; i < ENVSIZE && environ[i - 1]; i++)
			envbuf[i] = environ[i - 1];
		envbuf[i] = 0;
		child_environ = envbuf;
	} else {
		child_environ = environ;
	}

	nullfd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	if (nullfd < 0) {
		// use a closed pipe instead
		int fd[2];
		if (pipe(fd) < 0)
			fatal("pipe: errno=%d\n", errno);
		nullfd = fd[0];
		close(fd[1]);
	}
	if (nullfd <= 2) {      /* fd 0,1,2 aren't open */
		dup2(nullfd, 3);
		nullfd = 3;
		fcntl(nullfd, F_SETFD, FD_CLOEXEC);
		dup2(nullfd, 0);

		int voidfd = open("/dev/null", O_WRONLY); /* for screaming into */
		if (voidfd < 0) {
			// use a process that reads from a pipe instead
			int fd[2];
			if (pipe(fd) < 0)
				fatal("voidfd pipe: errno=%d\n", errno);
			pid_t child = fork();
			if (child < 0) {
				fatal("voidfd fork: errno=%d\n", errno);
			} else if (child == 0) {
				close(0);
				close(nullfd);
				close(fd[1]);

				char buf[1024];
				while (read(fd[0], buf, sizeof buf) != 0)
					;

				_exit(0);
			}
			voidfd = fd[1];
			close(fd[0]);
		}
		dup2(voidfd, 1);
		dup2(voidfd, 2);
		if (voidfd > 2)
			close(voidfd);
	}

	const char *dir = "/etc/nitro";
	if (argc == 2)
		dir = argv[1];

	if (chdir(dir) < 0)
		fatal("chdir to '%s': errno=%d\n", dir, errno);

	cwd = opendir(".");
	if (!cwd)
		fatal("opendir '%s': errno=%d\n", dir, errno);

	if (pipe(selfpipe) < 0)
		fatal("selfpipe pipe: errno=%d\n", errno);
	fcntl(selfpipe[0], F_SETFL, O_NONBLOCK);
	fcntl(selfpipe[1], F_SETFL, O_NONBLOCK);
	fcntl(selfpipe[0], F_SETFD, FD_CLOEXEC);
	fcntl(selfpipe[1], F_SETFD, FD_CLOEXEC);

	if (pipe(globallog) < 0)
		fatal("globallog pipe: errno=%d\n", errno);
	/* keep globallog[0] blocking */
	fcntl(globallog[1], F_SETFL, O_NONBLOCK);
	fcntl(globallog[0], F_SETFD, FD_CLOEXEC);
	fcntl(globallog[1], F_SETFD, FD_CLOEXEC);
	globallog[1] = -globallog[1]; /* made active when LOG is started */

	sigset_t allset;
	sigfillset(&allset);
	struct sigaction sa = {
		.sa_handler = on_signal,
		.sa_mask = allset,
		.sa_flags = SA_NOCLDSTOP | SA_RESTART,
	};
	sigaction(SIGPIPE, &sa, 0);
	sigaction(SIGCHLD, &sa, 0);
	sigaction(SIGHUP, &sa, 0);
	sigaction(SIGINT, &sa, 0);
	if (!real_pid1)         // only standalone and in containers
		sigaction(SIGTERM, &sa, 0);

	open_control_socket();

	global_state = GLBL_UP;

	prn(2, "- nitro: booting\n");

	struct stat st;
	if (stat("SYS", &st) == 0) {
		int b = add_service("SYS");
		process_step(b, EVNT_WANT_UP);
	} else {
		rescan(1);
	}

	struct pollfd fds[2];
	fds[CHLD].fd = selfpipe[0];
	fds[CHLD].events = POLLIN;
	fds[CTRL].fd = controlsock;
	fds[CTRL].events = POLLIN;

	while (1) {
		deadline now = time_now();

		int timeout = -1;

		for (i = 0; i < max_service; i++) {
			if (services[i].timeout <= 0)
				continue;

			if (services[i].deadline == 0)
				services[i].deadline = now + services[i].timeout;

			if (services[i].deadline <= now)
				process_step(i, EVNT_TIMEOUT);

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

		if (global_state == GLBL_FINAL)
			break;

		dprn("poll(timeout=%d) %d\n", timeout, global_state);

		int r = 0;
		do {
			r = poll(fds, sizeof fds / sizeof fds[0], timeout);
		} while (r == -1 && errno == EINTR);

		if (fds[CHLD].revents & POLLIN) {
			char ch;
			while (read(selfpipe[0], &ch, 1) == 1)
				;
			errno = 0;
		}

		while (1) {
			int wstatus = 0;
			pid_t r = waitpid(-1, &wstatus, WNOHANG);
			if (r == 0)
				break;
			if (r < 0) {
				if (errno != ECHILD)
					prn(2, "- nitro: mysterious waitpid error: %d\n", errno);
				if (global_state >= GLBL_WAIT_TERM && errno == ECHILD)
					global_state = GLBL_FINAL;
				break;
			}
			has_died(r, wstatus);
		}

		if (fds[CTRL].revents & POLLIN) {
			handle_control_sock();
		}

		if (want_rescan) {
			rescan(0);
			want_rescan = 0;
		}

		if (want_shutdown || want_reboot) {
			do_shutdown();
		}

		if (global_state == GLBL_SHUTDOWN) {
			int up = 0;
			int uplog = 0;
			for (i = 0; i < max_service; i++) {
				if (!(services[i].state == PROC_DOWN ||
				    services[i].state == PROC_FATAL)) {
					up++;
					if (services[i].islog)
						uplog++;
				}
			}
			if (up) {
				prn(2, ".");
				if (up == uplog) {
					dprn("signalling %d log processes\n", uplog);
					for (int i = 0; i < max_service; i++)
						if (services[i].islog)
							process_step(i, EVNT_WANT_DOWN);
				}
			} else {
				prn(2, " done.\n");
				if (!pid1)
					break;
				killall();
			}
		}
	}

	close(controlsock);
	unlink(control_socket_path);

#ifdef __linux__
	if (real_pid1) {
		if (access("SYS/final", X_OK) == 0) {
			prn(2, "- nitro: SYS/final starting\n");
			pid_t child = fork();
			if (child < 0) {
				prn(2, "- nitro: SYS/final failed to exec: errno=%d\n", errno);
			} else if (child == 0) {
				execle("SYS/final", "final",
				    want_reboot ? "reboot" : "shutdown",
				    (char *)0, child_environ);
				_exit(127);
			} else {
				int wstatus = 0;
				char ch;
				while (read(selfpipe[0], &ch, 1) == 1)
					;
				poll(fds, 1, TIMEOUT_SYS_FINAL);
				if (waitpid(child, &wstatus, WNOHANG) == child) {
					prn(2, "- nitro: SYS/final finished with status %d\n", WEXITSTATUS(wstatus));
				} else {
					kill(child, SIGKILL);
					waitpid(child, &wstatus, 0);
					prn(2, "- nitro: SYS/final terminated after timeout\n");
				}
			}
		}

		if (mount("/", "/", "", MS_REMOUNT | MS_RDONLY, "") < 0)
			prn(2, "- nitro: could not remount / read-only: errno=%d\n", errno);
		else
			prn(2, "- nitro: remounted / read-only\n");

		sync();

		prn(2, "- nitro: system %s\n", want_reboot ? "reboot" : "halt");
		sleep(1);

		if (want_reboot) {
			reboot(RB_AUTOBOOT);
		} else {
			// falls back to RB_HALT_SYSTEM if not possible
			reboot(RB_POWER_OFF);
		}
	}
#endif

	if (want_reboot) {
		prn(2, "- nitro: re-execing\n");
		execvp(argv[0], argv);
		fatal("could not re-exec '%s': errno=%d\n", argv[0], errno);
	}

	prn(2, "- nitro: finished\n");

	return 0;
}
