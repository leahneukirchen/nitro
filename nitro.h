#if !defined(RUNDIR)
#if defined(__linux__)
// Linux systems use /run nowadays by default.
#define RUNDIR "/run"
#else
#define RUNDIR "/var/run"
#endif
#endif

static char default_sock[256] = RUNDIR "/nitro/nitro.sock";

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
