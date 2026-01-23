/* nitroctl - control and manage services monitored by nitro */
/* SPDX-License-Identifier: 0BSD */

#ifdef __linux__
#define INIT_SYSTEM
#endif

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/utsname.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
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

const char *sockpath;
struct sockaddr_un sockaddr;

struct request {
	unsigned char cmd;
	int wait;
	char *service;
	unsigned char signal;
	char notifypath[128];
};

struct request *reqs;
struct pollfd *fds;
int maxreq;
int vflag;

typedef int64_t deadline;               /* milliseconds since boot */

deadline
time_now()
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (deadline)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int
max(int a, int b)
{
	return a > b ? a : b;
}

volatile sig_atomic_t got_sig;

static void
on_sigint(int sig)
{
	got_sig = sig;
}

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
abspath(char *s, char *output, size_t maxlen)
{
	char *t = output;
	if (*s != '/') {
		char *pwd = getenv("PWD");  // use $PWD if it resolves to .
		struct stat st1, st2;
		if (pwd && *pwd &&
		    stat(pwd, &st1) == 0 && stat(".", &st2) == 0 &&
		    st1.st_dev == st2.st_dev && st1.st_ino == st2.st_ino) {
			size_t l = strlen(pwd);
			memcpy(output, pwd, l < maxlen - 1 ? l : maxlen - 1);
			t = output + l;
			*t++ = '/';
		} else if (getcwd(output, maxlen - 1)) {
			t = output + strlen(output);
			*t++ = '/';
		}
	}

	while (*s && t < output + maxlen) {
		if ((t == output || t[-1] == '/') &&
		    s[0] == '.' && (s[1] == '/' || s[1] == 0)) {
			s += 1 + !!s[1];
		} else if ((t == output || t[-1] == '/') &&
		    s[0] == '.' && s[1] == '.' && (s[2] == '/' || s[2] == 0)) {
			do {
				t--;
			} while (t > output && t[-1] != '/');
			if (t == output)
				*t++ = '/';
			s += 2 + !!s[2];
		} else if ((t > output && t[-1] == '/') && s[0] == '/') {
			s++;
		} else {
			*t++ = *s++;
		}
	}

	while (t > output + 1 && t[-1] == '/')
		t--;

	*t = 0;
}

char *
normalize(char *service)
{
	char buf[PATH_MAX];
	abspath(service, buf, sizeof buf);

	char *tail = strrchr(buf, '/');
	if (!tail)
		return service;
	if (access(buf, F_OK) == 0)
		return strdup(tail + 1);
	if (!strchr(service, '/'))
		return service;

	fprintf(stderr, "nitroctl: no such service: %s: %s\n",
	    buf, strerror(errno));
	exit(111);
}

static void
close_sock(int i)
{
	if (fds[i].fd >= 0)
		close(fds[i].fd);
	fds[i].fd = -1;
	if (*reqs[i].notifypath)
		unlink(reqs[i].notifypath);
	*reqs[i].notifypath = 0;
}

int
notifysock(const char *service, int i, char *notifypath)
{
	int connfd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (connfd < 0) {
		perror("socket");
		exit(111);
	}

	char *sockpath2 = strdup(sockpath);
	char *path = sockpath2;
	if (!path || !*path)
		path = default_sock;
	path = dirname(path);

	snprintf(notifypath, sizeof (struct request){ 0 }.notifypath,
	    "%s/notify/%s,%ld_%d", path, service, (long)getpid(), i);

	free(sockpath2);

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
		if (errno == EACCES) {
			*notifypath = 0;
		} else {
			exit(111);
		}
	}

	return connfd;
}

struct service {
	char name[64];
	uint32_t pid, state, wstatus, uptime;
} services[MAXSV];

int
svnamecmp(const void *a, const void *b)
{
	return strcmp(((struct service *)a)->name, ((struct service *)b)->name);
}

#define SPAT_OPEN (-2)
#define SPAT_CLOSE (-1)
static int
spat_len(unsigned char *buf)
{
	uint16_t len = buf[0] | (buf[1] << 8);
	if (len == 0xffff)
		return SPAT_OPEN;
	if (len == 0xfffe)
		return SPAT_CLOSE;
	return len;
}

static uint8_t
spat_tag(unsigned char *buf)
{
	return buf[2];
}

static unsigned char *
spat_skip(unsigned char *buf)
{
	int len = spat_len(buf);
	return buf + 3 + (len >= 0 ? len : 0);
}

static unsigned char *
spat_skip_to_end(unsigned char *buf, unsigned char *bufe)
{
	uint8_t tag = spat_tag(buf);
	while (buf < bufe) {
		if (spat_len(buf) == SPAT_CLOSE && spat_tag(buf) == tag)
			return spat_skip(buf);
		buf = spat_skip(buf);
	}
	return bufe;
}

static int
spat_decode_u32(unsigned char *buf, unsigned char tag, uint32_t *dst)
{
	if (spat_tag(buf) != tag || spat_len(buf) != 4)
		return 0;

	buf += 3;

	*dst = ((uint32_t)buf[0] << 0) |
               ((uint32_t)buf[1] << 8) |
               ((uint32_t)buf[2] << 16) |
               ((uint32_t)buf[3] << 24);
	return 1;
}


void
list(unsigned char *buf,  unsigned char *bufe)
{
	int max_service = 0;

	while (buf < bufe && max_service < MAXSV) {
		if (spat_tag(buf) != T_SERVICE && spat_len(buf) != SPAT_OPEN) {
			buf = spat_skip_to_end(buf, bufe);
			continue;
		}
		buf = spat_skip(buf);

		while (buf < bufe) {
			if (spat_decode_u32(buf, T_PID, &services[max_service].pid) ||
			    spat_decode_u32(buf, T_WSTATUS, &services[max_service].wstatus) ||
			    spat_decode_u32(buf, T_UPTIME, &services[max_service].uptime))
				;
			else if (spat_tag(buf) == T_STATE && spat_len(buf) == 1)
				services[max_service].state = buf[3];
			else if (spat_tag(buf) == T_NAME && spat_len(buf) < 64) {
				memcpy(services[max_service].name, buf + 3, spat_len(buf));
				services[max_service].name[spat_len(buf)] = 0;
			} else if (spat_tag(buf) == T_SERVICE && spat_len(buf) == SPAT_CLOSE) {
				max_service++;
				break;
			}
			buf = spat_skip(buf);
		}
	}

	qsort(services, max_service, sizeof services[0], svnamecmp);

	for (int i = 0; i < max_service; i++) {
		printf("%s %s",
		    proc_state_str(services[i].state), services[i].name);
		if (services[i].pid)
			printf(" (pid %d)", services[i].pid);
		printf(" (wstatus %d) %ds\n",
		    services[i].wstatus, services[i].uptime);
	}
}

int
handle_request(int i)
{
	const char *sv = reqs[i].service ? reqs[i].service : "";

	char buffer[256];
	char *buf = buffer;

	if (strlen(sv) > 64) {
		fprintf(stderr, "nitroctl: service name too long: %s\n", sv);
		return 111;
	}

	int len = strlen(sv);
	*buf++ = len + !!(reqs[i].cmd == T_CMD_SIGNAL);
	*buf++ = 0;
	switch (reqs[i].cmd) {
	case T_WAIT_UP:
	case T_WAIT_DOWN:
	case T_WAIT_STARTING:
		*buf++ = T_CMD_QUERY;
		break;
	default:
		*buf++ = reqs[i].cmd;
	}
	if (reqs[i].cmd == T_CMD_SIGNAL)
		*buf++ = reqs[i].signal;
	memcpy(buf, sv, len);
	buf += len;

	int r = sendto(fds[i].fd, buffer, buf - buffer,
	    MSG_DONTWAIT, (struct sockaddr *)&sockaddr, sizeof sockaddr);

	if (r < 0) {
		// On BSD, UNIX DGRAM sockets can be full, but have POLLOUT.
		// On Linux, DGRAM sockets can block/EAGAIN, but have POLLOUT.
		if (errno == ENOBUFS || errno == EAGAIN) {
			return -1;
		} else {
			fprintf(stderr, "sendto to %s: %s\n",
			    sockpath, strerror(errno));
			close_sock(i);
			return 4;
		}
	}

	fds[i].events = POLLIN;
	return 0;
}

int
handle_response(int i)
{
	ssize_t rd;
	unsigned char buffer[4096];
	rd = read(fds[i].fd, buffer, sizeof buffer);
	if (rd < 0) {
		perror("read");
		return 111;
	}
	unsigned char *buf = buffer;
	unsigned char *bufe = buffer + rd;

	if (spat_tag(buf) == T_ESRCH) {
		fprintf(stderr, "nitroctl: no such service '%s'\n",
		    reqs[i].service);
		return 111;
	} else if (spat_tag(buf) == T_ENOSYS) {
		fprintf(stderr, "nitroctl: command not implemented in server\n");
		return 111;
	}

	if (spat_tag(buf) == T_OK)
		buf = spat_skip(buf);

	int state = 0;
	if (spat_tag(buf) >= PROC_DOWN && spat_tag(buf) <= PROC_DELAY) {
		state = spat_tag(buf);

		if (vflag)
			switch (reqs[i].cmd) {
			case T_CMD_UP:
			case T_CMD_DOWN:
			case T_CMD_RESTART:
			case T_WAIT_UP:
			case T_WAIT_DOWN:
				printf("%s %s\n", proc_state_str(state),
				    reqs[i].service);
			}
	} else if (spat_tag(buf) == T_STATE && spat_len(buf) == 1) {
		state = buf[3];

		if (vflag)
			switch (reqs[i].cmd) {
			case T_WAIT_UP:
			case T_WAIT_DOWN:
				printf("%s %s\n", proc_state_str(state),
				    reqs[i].service);
			}
	}

	switch (reqs[i].cmd) {
	case T_CMD_LIST:
		list(buf, buf + rd);
		return 0;
	case T_CMD_INFO: ;
		uint32_t u;
		while (buf < bufe) {
			if (spat_decode_u32(buf, T_NITRO_PID, &u))
				printf("nitro_pid %d\n", u);
			else if (spat_decode_u32(buf, T_MAX_SERVICE, &u))
				printf("max_service %d\n", u);
			else if (spat_decode_u32(buf, T_TOTAL_REAPS, &u))
				printf("total_reaps %d\n", u);
			else if (spat_decode_u32(buf, T_TOTAL_SV_REAPS, &u))
				printf("total_sv_reaps %d\n", u);
			else
				printf("# unknown tag 0x%02x\n", spat_tag(buf));

			buf = spat_skip(buf);
		}
		return 0;
	case T_WAIT_STARTING:
		if (state == PROC_STARTING ||
		    state == PROC_UP ||
		    state == PROC_ONESHOT)
			return 0;
		return -1;
	case T_WAIT_UP:
		if (state == PROC_FATAL)
			return -1;
		/* fallthrough */
	case T_CMD_UP:
	case T_CMD_RESTART:
		if (reqs[i].wait < 0)
			return 0;
		if (reqs[i].wait == 0 && state == PROC_STARTING)
			return 0;
		if (state == PROC_UP || state == PROC_ONESHOT)
			return 0;
		if (state == PROC_FATAL) {
			fprintf(stderr,
			    "nitroctl: failed to %sstart '%s'\n",
			    reqs[i].cmd == T_CMD_UP ? "" : "re", reqs[i].service);
			return 1;
		}
		break;
	case T_WAIT_DOWN:
	case T_CMD_DOWN:
		if (!reqs[i].wait && state == PROC_SHUTDOWN)
			return 0;
		if (state == PROC_DOWN || state == PROC_FATAL)
			return 0;
		break;
	case T_CMD_QUERY: ;
		uint32_t pid = 0, wstatus = 0, uptime = 0;

		while (buf < bufe) {
			if (spat_decode_u32(buf, T_PID, &pid) ||
			    spat_decode_u32(buf, T_WSTATUS, &wstatus) ||
			    spat_decode_u32(buf, T_UPTIME, &uptime))
				;
			else if (spat_tag(buf) == T_STATE && spat_len(buf) == 1)
				state = buf[3];

			buf = spat_skip(buf);
		}

		if (reqs[i].wait == 2) {
			printf("%s %s", proc_state_str(state), reqs[i].service);
			if (pid)
				printf(" (pid %d)", pid);
			printf(" (wstatus %d) %ds\n", (int)wstatus, uptime);
		} else if (reqs[i].wait == 1 && pid) {
			printf("%d\n", pid);
		} else if (!pid) {
			return 1;
		}
		return 0;
	default:
		return 0;
	}

	return -1;  // again
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

	deadline timeout = 0;
	int c;
	while ((c = getopt(argc, argv, "t:v")) != -1)
		switch (c) {
		case 't': {
			errno = 0;
			char *rest = 0;
			double secs = strtod(optarg, &rest);
			if (secs < 0.0 || *rest || errno != 0) {
				fprintf(stderr, "nitroctl: invalid timeout\n");
				exit(2);
			}
			if (isfinite(secs))
				timeout = time_now() + secs * 1000;

			break;
		}
		case 'v': vflag++; break;
		default:
			goto usage;
		}

	argc -= optind;
	argv += optind;

	if (cmd) {
		argc = 1;       // INIT_SYSTEM
	} else if (argc == 0) {
		cmd = "list";   // default command
		argc = 1;
	} else {
		cmd = argv[0];
	}

	fds = calloc(argc, sizeof fds[0]);
	reqs = calloc(argc, sizeof reqs[0]);
	if (!fds || !reqs) {
		perror("malloc");
		exit(111);
	}

	sockpath = control_socket();
	sockaddr.sun_family = AF_UNIX;
	strncpy(sockaddr.sun_path, sockpath, sizeof sockaddr.sun_path - 1);

        sigset_t allset;
        sigfillset(&allset);
        struct sigaction sa = {
                .sa_handler = on_sigint,
                .sa_mask = allset,
                .sa_flags = 0,
        };
	sigaction(SIGINT, &sa, 0);

	if (streq1(cmd, "list") && argc == 1)
		reqs[maxreq++] = (struct request){ .cmd = T_CMD_LIST };
	else if (streq(cmd, "info"))
		reqs[maxreq++] = (struct request){ .cmd = T_CMD_INFO };
	else if (streq1(cmd, "scan") || streq(cmd, "rescan"))
		reqs[maxreq++] = (struct request){ .cmd = T_CMD_RESCAN };
	else if (streq(cmd, "Reboot"))
		reqs[maxreq++] = (struct request){ .cmd = T_CMD_REBOOT };
	else if (streq(cmd, "Shutdown"))
		reqs[maxreq++] = (struct request){ .cmd = T_CMD_SHUTDOWN };
	else if (argc > 1 && (
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
		for (int i = 1; i < argc; i++)
			reqs[maxreq++] = (struct request){
				.cmd = T_CMD_SIGNAL,
				.signal = charsig(cmd[0]),
				.service = normalize(argv[i])
			};
	} else if (argc > 1) {
		for (int i = 1; i < argc; i++) {
			char *service = normalize(argv[i]);
			if (streq1(cmd, "up"))
				reqs[maxreq++] = (struct request){ .cmd = T_CMD_UP, .service = service, .wait = -1 };
			else if (streq1(cmd, "down"))
				reqs[maxreq++] = (struct request){ .cmd = T_CMD_DOWN, .service = service, .wait = -1 };
			else if (streq(cmd, "start"))
				reqs[maxreq++] = (struct request){ .cmd = T_CMD_UP, .service = service, .wait = 1 };
			else if (streq(cmd, "fast-start"))
				reqs[maxreq++] = (struct request){ .cmd = T_CMD_UP, .service = service };
			else if (streq(cmd, "stop"))
				reqs[maxreq++] = (struct request){ .cmd = T_CMD_DOWN, .service = service, .wait = 1 };
			else if (streq(cmd, "fast-stop"))
				reqs[maxreq++] = (struct request){ .cmd = T_CMD_DOWN, .service = service };
			else if (streq(cmd, "restart"))
				reqs[maxreq++] = (struct request){ .cmd = T_CMD_RESTART, .service = service, .wait = 1 };
			else if (streq(cmd, "fast-restart") || streq(cmd, "r"))
				reqs[maxreq++] = (struct request){ .cmd = T_CMD_RESTART, .service = service };
			else if (streq(cmd, "pidof"))
				reqs[maxreq++] = (struct request){ .cmd = T_CMD_QUERY, .service = service, .wait = 1 };
			else if (streq(cmd, "list"))
				reqs[maxreq++] = (struct request){ .cmd = T_CMD_QUERY, .service = service, .wait = 2 };
			else if (streq(cmd, "check"))
				reqs[maxreq++] = (struct request){ .cmd = T_CMD_QUERY, .service = service };
			else if (streq(cmd, "wait-up"))
				reqs[maxreq++] = (struct request){ .cmd = T_WAIT_UP, .service = service, .wait = 1 };
			else if (streq(cmd, "wait-down"))
				reqs[maxreq++] = (struct request){ .cmd = T_WAIT_DOWN, .service = service, .wait = 1 };
			else if (streq(cmd, "wait-starting"))
				reqs[maxreq++] = (struct request){ .cmd = T_WAIT_STARTING, .service = service, .wait = 1 };
			else if (streq(cmd, "ready"))
				reqs[maxreq++] = (struct request){ .cmd = T_CMD_READY, .service = service, .wait = -1 };
			else
				goto usage;
		}
	} else {
usage:
		fprintf(stderr, "usage: nitroctl COMMAND [SERVICE ...]\n");
		exit(2);
	}

	for (int i = 0; i < maxreq; i++) {
		const char *sv = reqs[i].service ? reqs[i].service : "";
		fds[i].fd = notifysock(sv, i, reqs[i].notifypath);
		fds[i].events = POLLOUT;
		if (!*reqs[i].notifypath)
			close_sock(i);
	}

	int err = 0;

	while (!got_sig) {
		for (int i = 0; i < maxreq; i++)
			if (fds[i].fd > 0)
				goto go;
		break;
go: ;
		int max_wait = -1;
		if (timeout)
			max_wait = max(0, timeout - time_now());

		int n = poll(fds, maxreq, max_wait);
		if (n < 0) {
			if (errno == EINTR)
				err = 128 + got_sig;
			else
				perror("poll");
			break;
		} else if (n == 0) {
			fprintf(stderr, "nitroctl: action timed out\n");
			err = 3;
			break;
		}

		for (int i = 0; i < maxreq; i++) {
			if (fds[i].revents & POLLOUT) {
				int r = handle_request(i);
				if (r == -1) {
					struct timespec ts = {
						.tv_sec = 0,
						.tv_nsec = 10000000L
					};
					nanosleep(&ts, 0);
					break;
				} else if (r > 0) {
					err = max(err, r);
					close_sock(i);
				}
			} else if (fds[i].revents & POLLIN) {
				int r = handle_response(i);
				if (r != -1) {
					err = max(err, r);
					close_sock(i);
				}
			}
		}
	}

	for (int i = 0; i < maxreq; i++)
		close_sock(i);

	free(fds);
	free(reqs);

	return err;
}
