/* SPDX-License-Identifier: 0BSD */

#if !defined(RUNDIR)
#if defined(__linux__)
// Linux systems use /run nowadays by default.
#define RUNDIR "/run"
#else
#define RUNDIR "/var/run"
#endif
#endif

static char default_sock[256] = RUNDIR "/nitro/nitro.sock";

/* max fd usage: 500 services (250 loggers) = 1000 fd for log pipes + const. */
#ifndef MAXSV
#define MAXSV 500
#endif

enum process_state {
	PROC_DOWN     = 1,
	PROC_SETUP    = 2,
	PROC_STARTING = 3,
	PROC_UP       = 4,
	PROC_ONESHOT  = 5,
	PROC_SHUTDOWN = 6,
	PROC_RESTART  = 7,
	PROC_FATAL    = 8,
	PROC_DELAY    = 9,
};

// must not overlap with process_state
enum tags {
	// enum process_state // payload: service name
	T_OK              = 80,
	T_ESRCH           = 81,
	T_ENOSYS          = 82,
	T_SERVICE         = 100, // framing for service metadata
	T_NAME            = 101, // payload: service name
	T_STATE           = 102, // payload: state
	T_PID             = 103, // payload: u32
	T_WSTATUS         = 104, // payload: i32
	T_UPTIME          = 105, // payload: u32 [secs]
	T_NITRO_PID       = 106, // payload: u32
	T_MAX_SERVICE     = 107, // payload: u32
	T_TOTAL_REAPS     = 108, // payload: u32
	T_TOTAL_SV_REAPS  = 109, // payload: u32
	T_CMD_UP          = 120, // payload: service name
	T_CMD_DOWN        = 121, // payload: service name
	T_CMD_RESTART     = 122, // payload: service name
	T_CMD_INFO        = 123,
	T_CMD_LIST        = 124,
	T_CMD_QUERY       = 125, // payload: service name
	T_CMD_RESCAN      = 126,
	T_CMD_SHUTDOWN    = 127,
	T_CMD_REBOOT      = 128,
	T_CMD_SIGNAL      = 129,
	T_CMD_READY       = 130,
};

enum internal_commands {
	// not used on the wire
	T_WAIT_UP         = 230,
	T_WAIT_DOWN       = 231,
	T_WAIT_STARTING   = 232,
};

static char *
control_socket()
{
	char *path = getenv("NITRO_SOCK");
	if (path && *path)
		return path;

	ssize_t r = readlink("/etc/nitro.sock",
	    default_sock, sizeof default_sock - 1);
	if (r > 0 && (size_t)r <= sizeof default_sock - 1)
		default_sock[r] = 0;

	return default_sock;
}

static int
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
