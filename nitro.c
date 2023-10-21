#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <uv.h>

uv_loop_t *loop;
int controlsock;

enum global_state {
	GLBL_UP,
	GLBL_WANT_SHUTDOWN,
	GLBL_WANT_REBOOT,
} global_state;

enum process_state {
	PROC_STOPPED = 1,
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
	// EVNT_DIED,	health check failed
};

struct process {
	struct process *next;

	char name[64];
	char tag[64];
	uv_timer_t timer;
	uv_process_t main;
//	uv_process_t log;
	uv_pipe_t log_pipe;
	uv_file logfd[2];
	uint64_t start;
	enum process_state state;

	uv_write_t wr_handle;
	char log_buffer[4096];
};

struct process *process_head;

void disarm_timeout(struct process *p);
void process_step(struct process *p, enum process_events ev);

uv_file globallogfd[2];
uv_pipe_t log_input;

void
callback_exit(uv_process_t *proc, int64_t exit_status, int term_signal)
{
	struct process *p = proc->data;

	disarm_timeout(p);

	printf("pid %d exited with status %ld and signal %d\n",
	    proc->pid, exit_status, term_signal);

	// uv_close((uv_handle_t *)proc, 0);

	p->main.pid = 0;

	process_step(p, EVNT_EXITED);
}

void
callback_timeout(uv_timer_t *handle)
{
	struct process *p = handle->data;
	process_step(p, EVNT_TIMEOUT);
}

void
arm_timeout(struct process *p, uint64_t timeout)
{
	uv_timer_init(loop, &p->timer);
	p->timer.data = p;
	uv_timer_start(&p->timer, callback_timeout, timeout, 0);
}

void
disarm_timeout(struct process *p)
{
	uv_timer_stop(&p->timer);
}

void
fixed_stream_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
	struct process *p = handle->data;

	buf->base = p->log_buffer;
	buf->len = 4096;
}

void
done(uv_write_t *req, int status)
{
}

void
read_proc_log(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
	struct process *p = stream->data;

	if (nread < 0) {
		printf("LOG ERROR: %s\n", uv_strerror(nread));
		return;
	}
	if (nread == UV_EOF)
		return;

	uv_buf_t b[] = {
		{ .base = p->tag, .len = strlen(p->tag) },
		{ .base = buf->base, .len = nread },
	};

	/*
	printf("isref: %d\n", uv_has_ref((uv_handle_t *)&p->wr_handle));
	printf("got log: %.*s\n", nread, buf->base);
	*/

	uv_write(&p->wr_handle, (uv_stream_t *)&log_input, b, 2, done);
}

void
proc_launch(struct process *p)
{
	p->main = (uv_process_t){ 0 };
	p->main.data = p;

	uv_pipe(p->logfd, 0, 0);

	uv_stdio_container_t child_stdio[3];
	child_stdio[0].flags = UV_INHERIT_FD;
	child_stdio[0].data.fd = 0;
	child_stdio[1].flags = UV_INHERIT_FD;
	child_stdio[1].data.fd = p->logfd[1];
	child_stdio[2].flags = UV_INHERIT_FD;
	child_stdio[2].data.fd = 2;

	uv_pipe_init(loop, &p->log_pipe, 0);
	p->log_pipe.data = p;

	uv_pipe_open(&p->log_pipe, p->logfd[0]);
	uv_read_start((uv_stream_t *)&p->log_pipe, fixed_stream_buffer, read_proc_log);

	uv_process_options_t options = { 0 };
	options.stdio_count = 3;
	options.stdio = child_stdio;
	options.exit_cb = callback_exit;

	char path[1024];
	snprintf(path, sizeof path, "%s/run", p->name);

	options.args = (char *[]){path, 0};
	options.file = path;

	int r = uv_spawn(loop, &p->main, &options);
	if (r) {
		fprintf(stderr, "uv_spawn: %s\n", uv_strerror(r));
		p->state = PROC_FATAL;
		return;
	}

	p->start = uv_now(loop);
	p->state = PROC_STARTING;
	snprintf(p->tag, sizeof p->tag, "%s[%d]: ", p->name, p->main.pid);

	arm_timeout(p, 100);
}

void
proc_shutdown(struct process *p)
{
	if (p->main.pid)
		kill(p->main.pid, SIGTERM);
	p->state = PROC_SHUTDOWN;
	arm_timeout(p, 15000);
}

void
proc_kill(struct process *p)
{
	if (p->main.pid)
		kill(p->main.pid, SIGKILL);

	assert(p->state == PROC_SHUTDOWN || p->state == PROC_RESTART);
}

void
proc_cleanup(struct process *p)
{
//	uv_close((uv_handle_t *)&p->main, 0);
//	uv_close((uv_handle_t *)&p->log_pipe, 0);
//	uv_close((uv_handle_t *)&p->timer, 0);
	close(p->logfd[0]);
	close(p->logfd[1]);
}

void
process_step(struct process *p, enum process_events ev)
{
	printf("%d[%d] got %d %d\n", p->state, p->main.pid, ev, global_state);

	switch (ev) {
	case EVNT_WANT_UP:
		if (global_state != GLBL_UP)
			break;
		switch (p->state) {
		case PROC_STARTING:
		case PROC_UP:
		case PROC_DEAD:
		case PROC_RESTART:
			/* ignore, is up */
			break;

		case PROC_SHUTDOWN:
			p->state = PROC_RESTART;
			break;

		case PROC_STOPPED:
		case PROC_FATAL:
		case PROC_DELAY:
			proc_launch(p);
			break;
		}
		break;

	case EVNT_WANT_DOWN:
		switch (p->state) {
		case PROC_STARTING:
		case PROC_UP:
		case PROC_DEAD:
		case PROC_RESTART:
		case PROC_SHUTDOWN:
			proc_shutdown(p);
			break;

		case PROC_FATAL:
		case PROC_DELAY:
			p->state = PROC_STOPPED;

		case PROC_STOPPED:
			/* ignore, is down */
			break;
		}
		break;

	case EVNT_WANT_RESTART:
		if (global_state != GLBL_UP)
			break;
		switch (p->state) {
		case PROC_STARTING:
		case PROC_UP:
		case PROC_DEAD:
		case PROC_RESTART:
		case PROC_SHUTDOWN:
			proc_shutdown(p);
			p->state = PROC_RESTART;
			break;

		case PROC_STOPPED:
		case PROC_FATAL:
		case PROC_DELAY:
			proc_launch(p);
			break;
		}
		break;

	case EVNT_EXITED:
		switch (p->state) {
		case PROC_STARTING:
		case PROC_UP:
		case PROC_DEAD:
		case PROC_RESTART:
			proc_cleanup(p);

			// XXX check too many restarts
			uint64_t now = uv_now(loop);
			if (now - p->start > 2000) {
				proc_launch(p);
			} else {
				p->state = PROC_DELAY;
				arm_timeout(p, 1000);
			}
			break;

		case PROC_SHUTDOWN:
			proc_cleanup(p);
			p->state = PROC_STOPPED;
			break;

		case PROC_STOPPED:		  /* can't happen */
		case PROC_FATAL:		  /* can't happen */
		case PROC_DELAY:		  /* can't happen */
			assert(!"invalid state transition");
			break;
		}
		break;

	case EVNT_TIMEOUT:
		disarm_timeout(p);
		switch (p->state) {
		case PROC_DELAY:
			proc_launch(p);
			break;

		case PROC_STARTING:
			/* detect failed start */
			p->state = PROC_UP;
			break;

		case PROC_DEAD:
			proc_shutdown(p);
			break;

		case PROC_RESTART:
		case PROC_SHUTDOWN:
			proc_kill(p);
			break;

		case PROC_UP:
		case PROC_STOPPED:
		case PROC_FATAL:
			assert(!"invalid timeout handler");
			break;
		}
		break;
	}
}

void
do_shutdown(int state)
{
	global_state = state;
	for (struct process *p = process_head; p; p = p->next)
		process_step(p, EVNT_WANT_DOWN);
}

void
callback_signal(uv_signal_t *handle, int signum)
{
	switch (signum) {
	case SIGINT:
		do_shutdown(GLBL_WANT_SHUTDOWN);
		break;

	case SIGTERM:
		uv_signal_stop(handle);
		uv_stop(loop);
		break;
	}
}

void
fixed_log_buffer(uv_handle_t *, size_t suggested_size, uv_buf_t *buf)
{
	static char buffer[4096];
	buf->base = buffer;
	buf->len = sizeof buffer;
}

void
read_log(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
	if (nread < 0) {
		fprintf(stderr, "log error: %s\n", uv_strerror(nread));
		uv_read_stop(stream);
		return;
	}
	if (nread == UV_EOF)
		return;

	printf("LOG: %.*s", (int)nread, buf->base);
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
	unlink(path);
	mode_t mask = umask(0077);
	int r = bind(controlsock, (struct sockaddr *)&addr, sizeof addr);
	umask(mask);
	if (r < 0) {
		perror("nitro: bind");
		exit(111);
	}
}

struct process *
new_service(char *name)
{
	struct stat st;
	if (name[0] == '.')
		return 0;
	if (stat(name, &st) < 0)	     /* we are in SVDIR */
		return 0;
	if (!S_ISDIR(st.st_mode))
		return 0;

	struct process *p = calloc(sizeof (struct process), 1);
	strcpy(p->name, name);
	p->state = PROC_DELAY;

	p->next = process_head;
	process_head = p;

	return p;
}

struct process *
find_service(char *name)
{
	for (struct process *p = process_head; p; p = p->next)
		if (strcmp(p->name, name) == 0)
			return p;

	return new_service(name);
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
	}

	return 0;
}

void
callback_control_socket(uv_poll_t *handle, int status, int events)
{
	if (status < 0) {
		fprintf(stderr, "poll error: %s\n", uv_strerror(status));
		return;
	}

	char buf[256];
	struct sockaddr_un src;
	socklen_t srclen;
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

	printf("got %ld from %d [%d %d]\n", r, srclen, status, events);

	if (r == 0)
		return;

	// chop trailing newline
	if (buf[strlen(buf) - 1] == '\n')
		buf[strlen(buf) - 1] = 0;

	switch (buf[0]) {
	case 'l':
		for (struct process *p = process_head; p; p = p->next) {
			printf("%s[%d] %d\n", p->name, p->main.pid, p->state);
		}
		goto ok;
	case 'u':
	case 'd':
	case 'r':
	{
		struct process *p = find_service(buf + 1);
		if (p) {
			if (buf[0] == 'u')
			    process_step(p, EVNT_WANT_UP);
			else if (buf[0] == 'd')
			    process_step(p, EVNT_WANT_DOWN);
			else if (buf[0] == 'r')
			    process_step(p, EVNT_WANT_RESTART);
			goto ok;
		}
		goto fail;
	}
	case 'S':
		do_shutdown(GLBL_WANT_SHUTDOWN);
		goto ok;
	case 'R':
		do_shutdown(GLBL_WANT_REBOOT);
		goto ok;
	default:
		if (charsig(buf[0])) {
			struct process *p = find_service(buf + 1);
			if (p && p->main.pid) {
				kill(p->main.pid, charsig(buf[0]));
				goto ok;
			}
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

void
load_services()
{
	DIR *dir = opendir(".");
	if (!dir)
		abort();

	struct dirent *ent;
	while ((ent = readdir(dir)))
		new_service(ent->d_name);

	closedir(dir);
}

void callback_stop_check(uv_check_t *handle)
{
	if (global_state == GLBL_UP)
		return;

	int up = 0;
	for (struct process *p = process_head; p; p = p->next) {
		printf("DBG %s %d %d\n", p->name, p->main.pid, p->state);
		if (!(p->state == PROC_STOPPED || p->state == PROC_FATAL))
			up++;
	}
	if (up) {
		printf("shutdown waiting for %d processes\n", up);
		return;
	}

	uv_stop(loop);
}

int
main(int argc, char *argv[])
{
	if (argc < 1)
		return 111;

	loop = uv_default_loop();
	uv_disable_stdio_inheritance();

	const char *dir = "/var/service";
	if (argc == 2)
		dir = argv[1];

	if (chdir(dir) < 0) {
		perror("nitro: chdir");
		return 111;
	}

	signal(SIGPIPE, SIG_IGN);

	uv_signal_t sigterm;
	uv_signal_init(loop, &sigterm);
	uv_signal_start(&sigterm, callback_signal, SIGTERM);

	uv_signal_t sigint;
	uv_signal_init(loop, &sigint);
	uv_signal_start(&sigint, callback_signal, SIGINT);

	open_control_socket();

	/* we use plain poll access for the control socket, so we can
	   handle requests without any memory overhead.  */
	uv_poll_t control_poll;
	uv_poll_init(loop, &control_poll, controlsock);

	uv_poll_start(&control_poll, UV_READABLE, callback_control_socket);

	uv_pipe(globallogfd, UV_NONBLOCK_PIPE, UV_NONBLOCK_PIPE);
	uv_pipe_t log_pipe;
	uv_pipe_init(loop, &log_pipe, 0);
	uv_pipe_open(&log_pipe, globallogfd[0]);
	uv_pipe_init(loop, &log_input, 0);
	uv_pipe_open(&log_input, globallogfd[1]);
	uv_read_start((uv_stream_t *)&log_pipe, fixed_log_buffer, read_log);

	global_state = GLBL_UP;

	load_services();

	printf("nitro up at %d\n", getpid());

	for (struct process *p = process_head; p; p = p->next) {
		char path[1024];
		struct stat st;
		snprintf(path, sizeof path, "%s/down", p->name);
		if (stat(path, &st))
			process_step(p, EVNT_WANT_DOWN);
		else
			process_step(p, EVNT_WANT_UP);
	}

	uv_check_t stop_check;
	uv_check_init(loop, &stop_check);
	uv_check_start(&stop_check, callback_stop_check);

	uv_run(loop, UV_RUN_DEFAULT);

	uv_loop_close(loop);
	return 0;
}
