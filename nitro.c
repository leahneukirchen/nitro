#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/un.h>
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
#include <time.h>
#include <unistd.h>

// no error message
#define assert(x) do { if(!(x)) abort(); } while(0);

extern char **environ;
char **child_environ;
char *envbuf[64];

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
	EVNT_SETUP,		/* setup script exited */
	EVNT_EXITED,
	EVNT_FINISHED,		/* finish script exited */
	// EVNT_DIED,   health check failed
};

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
	enum process_state state;
	char seen;
	char islog;
} services[512];

int max_service = 0;
int controlsock;
int nullfd;
int selfpipe[2];
int globallog[2];
DIR *cwd;
DIR *notifydir;
char notifypath[256];

int pid1;
int real_pid1;

volatile sig_atomic_t want_rescan;
volatile sig_atomic_t want_shutdown;
volatile sig_atomic_t want_reboot;

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
			} else if (*s == '%') {
				*out++ = '%';
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
			} else if (*s == 'S') {
				char *s = va_arg(ap, char *);
				char *se = va_arg(ap, char *);
				out = stecpe(out, oute, s, se);
//			} else if (*s == 'c') {
//				*out++ = va_arg(ap, int);
			} else if (*s == '%') {
				*out++ = '%';
			} else {
				abort();
			}
		} else {
			*out++ = *s;
		}
	}

	va_end(ap);

	return write(fd, buf, out - buf);  // XXX safe write
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
	sprn(buf, buf + sizeof buf, "%s/%s", dir, name);
	if (instance)
		*instance = '@';
	return stat(buf, st);
}

void process_step(int i, enum process_events ev);
void notify(int);

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
	if (pipe(alivepipefd) < 0)
		abort();
	fcntl(alivepipefd[0], F_SETFD, FD_CLOEXEC);
	fcntl(alivepipefd[1], F_SETFD, FD_CLOEXEC);

	pid_t child = fork();
	if (child == 0) {
		char *instance = strchr(services[i].name, '@');
		if (instance)
			*instance++ = 0;
		chdir(services[i].name);

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
		abort();	/* XXX handle retry */
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
	services[i].timeout = 2000;
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
		char *instance = strchr(services[i].name, '@');
		if (instance)
			*instance++ = 0;
		chdir(services[i].name);

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
		abort();	/* XXX handle retry */
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
		char *instance = strchr(services[i].name, '@');
		if (instance)
			*instance++ = 0;
		chdir(services[i].name);

		dup2(nullfd, 0);
		if (services[i].logpipe[1] != -1)
			dup2(services[i].logpipe[1], 1);
		else if (globallog[1] > 0)
			dup2(globallog[1], 1);
		// else keep fd 1 to /dev/console

		setsid();

		if (strcmp(services[i].name, "SYS") == 0) {
			if (global_state == GLBL_WANT_REBOOT)
				instance = (char *)"reboot";
			else if (global_state == GLBL_WANT_SHUTDOWN)
				instance = (char *)"shutdown";
		}

		if (instance)
			execle("finish", "finish", run_status, run_signal, instance, (char *)0, child_environ);
		else
			execle("finish", "finish", run_status, run_signal, (char *)0, child_environ);
		_exit(127);
	} else if (child < 0) {
		abort();	/* XXX handle retry */
	}

	services[i].finishpid = child;
	services[i].timeout = 7000;
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
		services[i].timeout = 7000;
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
	}

	notify(i);
}

void
proc_zap(int i) {
	// XXX clean up log pipe?

	if (!services[i].seen) {
		dprn("can garbage-collect %s\n", services[i].name);
		if (max_service > 0) {
			services[i] = services[--max_service];
		} else {
			assert(i == 0);
			services[i] = (struct service){ 0 };
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
		case PROC_STARTING:
		case PROC_UP:
		case PROC_RESTART:
		case PROC_SHUTDOWN:
		case PROC_FATAL:
			proc_finish(i);
			break;

		case PROC_SETUP:               /* can't happen */
		case PROC_DOWN:                /* can't happen */
		case PROC_DELAY:	       /* can't happen */
		case PROC_ONESHOT:	       /* can't happen */
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
			services[i].timeout = 1000;
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
		case PROC_DELAY:	       /* can't happen */
			assert(!"invalid state transition");
			break;
		}
		break;

	case EVNT_TIMEOUT:
		services[i].timeout = 0;
		services[i].deadline = 0;
		switch (services[i].state) {
		case PROC_DELAY:
			proc_setup(i);
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
	case SIGCHLD:		/* just selfpipe */
	case SIGALRM:		/* just for EINTR */
		break;
	}

	write(selfpipe[1], "", 1);

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
add_service(const char *name)
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

		// ignore magic bootup/shutdown service
		if (strcmp(name, "SYS") == 0)
			continue;

		int i = add_service(name);

		if (first && stat_slash(name, "down", &st) == 0) {
			services[i].state = PROC_DOWN;
			services[i].timeout = 0;
		}

		char buf[PATH_MAX];
		sprn(buf, buf + sizeof buf, "%s/log", name);

		if (stat_slash(name, "log", &st) == 0 && S_ISDIR(st.st_mode)) {
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

	for (i = 0; i < max_service; i++)
		if (!services[i].seen)
			process_step(i, EVNT_WANT_DOWN);
}

void
own_console()
{
	return;

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
	for (int i = 0; i < max_service; i++) {
		if (services[i].islog)
			continue;
		if (strcmp(services[i].name, "LOG") == 0)
			continue;

		process_step(i, EVNT_WANT_DOWN);
	}
}

void
do_shutdown(int state)
{
	if (global_state == GLBL_UP) {
		if (state == GLBL_WANT_REBOOT)
			prn(2, "- nitro: rebooting\n");
		else if (state == GLBL_WANT_SHUTDOWN)
			prn(2, "- nitro: shutting down\n");

		if (pid1)
			own_console();
#ifdef __linux__
		reboot(RB_ENABLE_CAD);
#endif

		global_state = state;


		struct stat st;
		if (stat("SYS", &st) == 0) {
			int b = add_service("SYS");
			services[b].state = PROC_ONESHOT;
			process_step(b, EVNT_WANT_DOWN);
			services[b].timeout = 30000;
		} else {
			do_stop_services();
		}
	}

	global_state = state;
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
	strncpy(addr.sun_path, path, sizeof addr.sun_path - 1);

	controlsock = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (controlsock < 0)
		fatal("socket");
	fcntl(controlsock, F_SETFD, FD_CLOEXEC);

	unlink(path);
	mode_t mask = umask(0077);
	int r = bind(controlsock, (struct sockaddr *)&addr, sizeof addr);
	umask(mask);
	if (r < 0)
		fatal("bind");
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

		if ((strncmp(name, services[i].name, strlen(services[i].name)) == 0 &&
		    name[strlen(services[i].name)] == ',') ||
		    (strncmp("ALL,", name, 4) == 0)) {

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

const char *
proc_state_str(enum process_state state)
{
	switch (state) {
	case PROC_DOWN: return "DOWN";
	case PROC_STARTING: return "STARTING";
	case PROC_UP: return "UP";
	case PROC_SHUTDOWN: return "SHUTDOWN";
	case PROC_RESTART: return "RESTART";
	case PROC_FATAL: return "FATAL";
	case PROC_DELAY: return "DELAY";
	case PROC_SETUP: return "SETUP";
	case PROC_ONESHOT: return "ONESHOT";
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
			if (services[i].pid)
				reply += sprn(reply, replyend, "%s %s (pid %d) (wstatus %d) %ds\n",
				    proc_state_str(services[i].state),
				    services[i].name,
				    services[i].pid,
				    services[i].wstatus,
				    (now - services[i].startstop) / 1000);
			else
				reply += sprn(reply, replyend, "%s %s (wstatus %d) %ds\n",
				    proc_state_str(services[i].state),
				    services[i].name,
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

			if (status == 0) {
				process_step(i, EVNT_SETUP);
			} else {
				services[i].state = PROC_DELAY;
				services[i].timeout = 1000;
				services[i].deadline = 0;
			}

			if (strcmp(services[i].name, "SYS") == 0 &&
			    global_state == GLBL_UP) { /* C-A-D during SYS */
				services[i].seen = 0;
				proc_cleanup(i);
				proc_zap(i);
				// bring up rest of the services

				prn(2, "- nitro: SYS/setup finished\n");

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

		// XXX handle logger?
	}
}

/* only detects toplevel mount points */
static int
mounted(const char *dir)
{
	struct stat rootst;
	struct stat dirst;

	stat("/", &rootst);
	if (stat(dir, &dirst) < 0)
		return 1;  /* can't mount if mountpoint doesn't exist */

	return rootst.st_dev != dirst.st_dev;
}

void
init_mount() {
#ifdef __linux__
	if (!access("/dev/null", F_OK) && !mounted("/dev"))
		mount("dev", "/dev", "devtmpfs", MS_NOSUID, "mode=0755");
	if (!mounted("/run"))
		mount("run", "/run", "tmpfs", MS_NOSUID|MS_NODEV, "mode=0755");
#endif
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
		init_mount();
		own_console();
#ifdef __linux__
		if (reboot(RB_DISABLE_CAD) < 0)
			real_pid1 = 0;  /* we are in a container */
#endif
	}

	// can't use putenv, which pulls in realloc
	if (!getenv("PATH")) {
		envbuf[0] = (char*) "PATH=" _PATH_DEFPATH;
		for (char **e = environ, **c = envbuf+1; *e; e++, c++)
			*c = *e;
		child_environ = envbuf;
	} else {
		child_environ = environ;
	}

	const char *dir = "/etc/nitro";
	if (argc == 2)
		dir = argv[1];

	if (chdir(dir) < 0)
		fatal("chdir to '%s': errno=%d\n", dir, errno);

	cwd = opendir(".");
	if (!cwd)
		abort();

	nullfd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	if (nullfd < 0) {
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

	pipe(globallog);
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
	sigaction(SIGCHLD, &sa, 0);
	sigaction(SIGHUP, &sa, 0);
	sigaction(SIGINT, &sa, 0);
	if (!real_pid1)		// only standalone and in containers
		sigaction(SIGTERM, &sa, 0);

	open_control_socket();

	global_state = GLBL_UP;

	prn(2, "- nitro: booting\n");

	{
		struct stat st;
		if (stat("SYS", &st) == 0) {
			int b = add_service("SYS");
			process_step(b, EVNT_WANT_UP);
		} else {
			rescan(1);
		}
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

		dprn("poll(timeout=%d)\n", timeout);

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

		if (want_reboot) {
			do_shutdown(GLBL_WANT_REBOOT);
		}

		if (global_state != GLBL_UP) {
			int up = 0;
			int uplog = 0;
			for (i = 0; i < max_service; i++) {
				if (!(services[i].state == PROC_DOWN ||
				    services[i].state == PROC_FATAL)) {
					up++;
					if (services[i].islog)
						uplog++;
					if (strcmp(services[i].name, "LOG") == 0)
						uplog++;
				}
			}
			if (up) {
				prn(2, "- nitro: waiting for %d processes to finish\n", up);
				if (up == uplog) {
					dprn("signalling %d log processes\n", uplog);
					for (int i = 0; i < max_service; i++)
						if (services[i].islog || strcmp(services[i].name, "LOG") == 0)
							process_step(i, EVNT_WANT_DOWN);
				}
			} else {
				break;
			}
		}
	}

#ifdef __linux__
	if (pid1) {
		prn(2, "- nitro: sending SIGTERM to all processes\n");
		kill(-1, SIGTERM);
		kill(-1, SIGCONT);

		struct sigaction sa = {
			.sa_handler = on_signal,
			.sa_mask = allset,
		};
		sigaction(SIGALRM, &sa, 0);
		alarm(3);

		while(1) {
			int r = waitpid(-1, 0, 0);
			if (r < 0)
				break;
		}

		prn(2, "- nitro: sending SIGKILL to all processes\n");
		kill(-1, SIGKILL);
	}
	if (real_pid1) {
		prn(2, "- nitro: system %s\n",
		    global_state == GLBL_WANT_REBOOT ? "reboot" : "halt");

		sync();
		sleep(1);

		if (global_state == GLBL_WANT_REBOOT) {
			reboot(RB_AUTOBOOT);
		} else {
			// falls back to RB_HALT_SYSTEM if not possible
			reboot(RB_POWER_OFF);
		}
	}
#endif

	prn(2, "- nitro: finished\n");

	return 0;
}
