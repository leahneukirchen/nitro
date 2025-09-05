#if !defined(RUNDIR)
#if defined(__linux__) || defined(__NetBSD__)
// NetBSD doesn't have /run nor a writable filesystem before init(8) starts, on
// the other hand, /var could be later mounted so we can't use /var/run at this
// stage, same goes for /tmp. So let's use a previously created /run dedicated
// to nitro.
//
// Linux has /run nowadays by default.
#define RUNDIR "/run"
#else
#define RUNDIR "/var/run"
#endif
#endif

static char default_sock[] = RUNDIR "/nitro/nitro.sock";

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
