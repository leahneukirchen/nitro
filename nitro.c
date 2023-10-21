#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <uv.h>

uv_loop_t *loop;

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
	// EVNT_DIED,   health check failed
};

struct process {
	char name[64];
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

void disarm_timeout(struct process *p);
void process_step(struct process *p, enum process_events ev);

uv_file globallogfd[2];
uv_pipe_t log_input;

void
callback_exit(uv_process_t *proc, int64_t exit_status, int term_signal)
{
	struct process *p = proc->data;

	disarm_timeout(p);

	printf("pid %d exited with status %d and signal %d\n",
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
		printf("LOG ERROR: %s\n", strerror(-nread));
		return;
	}
	if (nread == UV_EOF)
		return;

	uv_buf_t b[] = {
		{ .base = "Wrote: ", .len = 7 },
		{ .base = buf->base, .len = nread },
	};

	printf("isref: %d\n", uv_has_ref((uv_handle_t *)&p->wr_handle));

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
	options.args = (char *[]){"./slowexit.rb", "20", (char*)0};
	options.file = options.args[0];

	int r = uv_spawn(loop, &p->main, &options);
	if (r) {
		fprintf(stderr, "uv_spawn: %s\n", uv_strerror(r));
		p->state = PROC_FATAL;
		return;
	}

	p->start = uv_now(loop);
	p->state = PROC_STARTING;
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
//      uv_close((uv_handle_t *)&p->log_pipe, 0);
//	uv_close((uv_handle_t *)&p->timer, 0);
	close(p->logfd[0]);
	close(p->logfd[1]);
}

void
process_step(struct process *p, enum process_events ev)
{
	printf("%d[%d] got %d\n", p->state, p->main.pid, ev);

	switch (ev) {
	case EVNT_WANT_UP:
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

		case PROC_STOPPED:                /* can't happen */
		case PROC_FATAL:                  /* can't happen */
		case PROC_DELAY:                  /* can't happen */
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

struct process mainproc;

void
callback_signal(uv_signal_t *handle, int signum)
{
	switch (signum) {
	case SIGINT:
		uv_signal_stop(handle);
		uv_stop(loop);
		break;

	case SIGUSR1:
		process_step(&mainproc, EVNT_WANT_UP);
		break;
	case SIGUSR2:
		process_step(&mainproc, EVNT_WANT_DOWN);
		break;
	case SIGWINCH:
		process_step(&mainproc, EVNT_WANT_RESTART);
		break;

	default:
		/* ignore */
	}
}

void
fixed_log_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
	static char buffer[4096];
	buf->base = buffer;
	buf->len = sizeof buffer;
}

void
read_log(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
	if (nread < 0) {
		printf("LOG ERROR: %s\n", strerror(-nread));
		uv_read_stop(stream);
		return;
	}
	if (nread == UV_EOF)
		return;

	printf("LOG READ %d\n", nread);
	printf("LOG: %.*s\n", (int)nread, buf->base);
}

void
alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
	buf->base = malloc(256);
	buf->len = 256;
}

void
write_done(uv_write_t *req, int status)
{
	free(req->data);
	free(req);
}

void
handle_command(uv_stream_t *client, ssize_t nread, const uv_buf_t *buf)
{
	printf("read %d: %.*s\n", nread, (int)nread, buf->base);

	uv_write_t *req = (uv_write_t *)malloc(sizeof(uv_write_t));
	req->data = strdup("gotem\n");
	const uv_buf_t b = uv_buf_init(req->data, 6);
	uv_write((uv_write_t *)req, client, &b, 1, write_done);

	free(buf->base);
	uv_close((uv_handle_t *)client, free);
	return;
}

void
callback_new_connection(uv_stream_t *server, int status)
{
	if (status < 0) {
		fprintf(stderr, "New connection error %s\n", uv_strerror(status));
		return;
	}

	printf("new connection\n");

	uv_pipe_t *client = (uv_pipe_t *)malloc(sizeof (uv_pipe_t));
	uv_pipe_init(loop, client, 0);
	int r = uv_accept(server, (uv_stream_t *)client);
	assert(r == 0);

	uv_read_start((uv_stream_t *)client, alloc_buffer, handle_command);
}

int
main()
{
	signal(SIGPIPE, SIG_IGN);

	loop = uv_default_loop();

	uv_disable_stdio_inheritance();

	uv_pipe_t server;
	uv_pipe_init(loop, &server, 0);

	int r;
	unlink("/tmp/nitro.sock");
	if ((r = uv_pipe_bind(&server, "/tmp/nitro.sock"))) {
		fprintf(stderr, "Bind error %s\n", uv_err_name(r));
		return 1;
	}
	if ((r = uv_listen((uv_stream_t *)&server, 128, callback_new_connection))) {
		fprintf(stderr, "Listen error %s\n", uv_err_name(r));
		return 2;
	}

	uv_signal_t sigusr1;
	uv_signal_init(loop, &sigusr1);
	uv_signal_start(&sigusr1, callback_signal, SIGUSR1);

	uv_signal_t sigusr2;
	uv_signal_init(loop, &sigusr2);
	uv_signal_start(&sigusr2, callback_signal, SIGUSR2);

	uv_signal_t sigwinch;
	uv_signal_init(loop, &sigwinch);
	uv_signal_start(&sigwinch, callback_signal, SIGWINCH);

	uv_signal_t sigint;
	uv_signal_init(loop, &sigint);
	uv_signal_start(&sigint, callback_signal, SIGINT);

	uv_pipe(globallogfd, UV_NONBLOCK_PIPE, UV_NONBLOCK_PIPE);
	uv_pipe_t log_pipe;
	uv_pipe_init(loop, &log_pipe, 0);
	uv_pipe_open(&log_pipe, globallogfd[0]);
	uv_pipe_init(loop, &log_input, 0);
	uv_pipe_open(&log_input, globallogfd[1]);
	uv_read_start((uv_stream_t *)&log_pipe, fixed_log_buffer, read_log);

	proc_launch(&mainproc);

	printf("nitro up at %d\n", getpid());

	uv_run(loop, UV_RUN_DEFAULT);

	uv_loop_close(loop);
	return 0;
}
