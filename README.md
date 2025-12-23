# nitro, a tiny but flexible init system and process supervisor

## Overview

nitro is a tiny process supervisor that also can be used as pid 1 on
Linux and NetBSD.

These are the main applications nitro is designed for:
- As init for a Linux or NetBSD machine for embedded, desktop or
  server purposes
- As init for a Linux initramfs
- As init for a Linux container (Docker/Podman/LXC/Kubernetes)
- As unprivileged supervision daemon on generic POSIX systems

nitro is configured by a directory of scripts, defaulting to
`/etc/nitro` (or the first command line argument).

## Requirements

- Kernel support for Unix sockets (SOCK_DGRAM)
- `tmpfs` or writable `/run` on another fs

## Benefits over other systems

- All state is kept in RAM, works without tricks on read-only root file systems.
- Efficient event-driven, polling free operation.
- Zero memory allocations during runtime.
- No unbounded file descriptor usage during runtime.
- One single self-contained binary, plus one optional binary to
  control the system.
- No configuration compilation steps needed, services are simple
  directories containing scripts.
- Supports reliable restarting of services.
- Reliable logging mechanisms per service or as default.
- Support for logging chains spread over several services.
- Works independently of properly set system clock.
- Can be run on FreeBSD from /etc/ttys (sets up file descriptors 0, 1, 2).
- Tiny static binary when using musl libc.

## Services

Every directory inside `/etc/nitro` (or your custom service directory)
can contain several files:

- `setup`, an optional executable file that is run before the service starts.
  It must exit with status 0 to continue.
- `run`, an optional executable file that runs the service;
  it must not exit as long as the service is considered running.
  If there is no `run` script, the service is considered a "one shot",
  and stays "up" until it's explicitly taken "down".
- `finish`, an optional executable file that is run after the `run`
  process finished.  It is passed two arguments, the exit status
  of the `run` process (or -1 if it was killed by a signal)
  and the signal that killed it (or 0, if it exited regularly).
- `log`, a symlink to another service directory.
  The standard output of `run` is connected to the standard input of the
  service under `log` by a pipe.  You can chain these for reliable and
  supervised log processing.
- `down`, an optional file that causes nitro to not bring up this
  service by default.
- Service directories ending with '@' are ignored; they can be used
  for parameterized services.
- Service names must be shorter than 64 chars, and not contain `/`,
  `,` or newlines.

You may find runit's `chpst` useful when writing `run` scripts.

## Special services

- `LOG`: this service is used as a logging service for all services
  that don't have a `log` symlink.
- `SYS`: `SYS/setup` is run before other services are brought up.
  You can already use `nitroctl` in `SYS/setup` to bring up services
  in a certain order.
  `SYS/finish` is run before all remaining services are killed and the
  system is brought down.
  After all processes are terminated, `SYS/final` is run.
  The program `SYS/fatal`, if it exists, is run instead of exiting
  when an unrecoverable, fatal error happens.
  The program `SYS/reincarnate`, if it exists, is executed into
  instead of a shutdown.  This can be used to implement an initramfs,
  for example.

## Parametrized services

Service directories ending in `@` are considered service templates,
and are ignored as services, however you can instantiate them into
parametrized services by using symlinks with a parameter after the `@`
(either in the service directory or as a `log` symlink), or start them
manually using `nitroctl`.

The part after the `@`, the parameter, is passed to the scripts as
first argument.

For example, if you have a script `agetty@/run` and a symlink
`agetty@tty1` -> `agetty@`, nitro will spawn `agetty@/run tty1`.  Upon
running `nitroctl up agetty@tty2`, nitro will spawn `agetty@/run
tty2`, even if it does not exist in the service directory.
Likewise, a symlink `log` -> `../mylogger@foo` will spawn
`mylogger@/run foo` and use it as a logger.

Additionally, if `log` directly points to a service template, it is
instantiated with the parameter of the service, or the name of the
service.  For example, `sv_a/log` -> `../mylogger@` will use
`mylogger@sv_a` as a logger, and `sv@/log` -> `../mylogger@' will use
`mylogger@foo` when you start `sv@foo`.

Parametrized services are removed on `rescan` if they are DOWN and not
referred to by other services.

## Modes of operation

The lifecycle of a machine/container/session using nitro consists of
three phases.

First, the system is brought up.  If there is a special service
`SYS`, its `setup` script is run first.  After it finishes, all
services not marked `down` are brought up.

When a service exits, it's being restarted, potentially waiting for
two seconds if the last restart happened too quickly.

By using `nitroctl Reboot` or `nitroctl Shutdown`, the system can be
brought down.  If it exists, `SYS/finish` will be run.  After this,
nitro will send a SIGTERM signal to all running services and waits for
up to 7 seconds for the service to exit.  Otherwise, a SIGKILL is
sent.  After all processes are terminated, `SYS/final` is run.

Finally, nitro reboots or shuts down the system; or just exits when it
was used as a container init or unprivileged supervisor.  (When a
reboot was requested, it re-execs itself.  This requires being called
with absolute path for the binary and the service directory.)

## Service states

There are 9 possible states a service can be in:

- `DOWN`: the service is not running and is not supposed to.
- `SETUP`: the service is running the `./setup` script.
- `STARTING`: the service is running the `./run` script, but is not
  considered ready yet.
- `UP`: the service is running.
- `ONESHOT`: it's a "one shot" service and `./setup` has finished.
- `SHUTDOWN`: the service is being brought down, or it has exited
  already and `./finish` is running.  It will be `DOWN` next.
- `RESTART`: the service is being brought down, or it has exited
  already and `./finish` is running.  It will be restarted next.
- `FATAL`: the service is down.  An error has happened that will not
  fix itself.  Investigate and restart the service manually.
- `DELAY`: the service is down.  An error has happened that will
  potentially fix itself.  The service will be restarted automatically.

## Readiness notification

If a service directory has a file `notification-fd` which contains a
number, the service will be started having the file descriptor with
this number connected to a pipe.  Once the service is ready, it should
write a newline to the pipe (and ideally close it).  Other data can be
written but is ignored, only the newline is relevant.  Then nitro
considers the service UP.

If the file `notification-fd` does not exist, nitro considers
services to be up after 2 seconds.

This mechanism is compatible with s6 and dinit.

## Control socket configuration

nitro uses a single Unix socket for control.  The socket path is
determined in the following way:
1. The environment variable NITRO_SOCK, if it is set.
2. The target of the symlink /etc/nitro.sock, if that link exists.
3. On Linux, /run/nitro/nitro.sock.
4. On other operating systems, /var/run/nitro/nitro.sock.

Note that the socket needs to be on a writable file system.
When used as pid 1, nitro mounts `/run` (on Linux) or the target of
`/etc/nitro.sock` (on NetBSD) as a tmpfs.

## Controlling nitro with nitroctl

You can remote control a running nitro instance using the tool
`nitroctl`.

Usage: `nitroctl [COMMAND] [SERVICE]`

Where COMMAND is one of:

- list: show a list of services and their state, pid, uptime and last
  exit status.
- up: start SERVICE
- down: stop SERVICE (sending SIGTERM or the first letter of `./down-signal`)
- start: start SERVICE, waiting for success
- restart: restart SERVICE, waiting for success
- stop: stop SERVICE, waiting for success
- p: send signal SIGSTOP to SERVICE
- c: send signal SIGCONT to SERVICE
- h: send signal SIGHUP to SERVICE
- a: send signal SIGALRM to SERVICE
- i: send signal SIGINT to SERVICE
- q: send signal SIGQUIT to SERVICE
- 1: send signal SIGUSR1 to SERVICE
- 2: send signal SIGUSR2 to SERVICE
- t: send signal SIGTERM to SERVICE
- k: send signal SIGKILL to SERVICE
- pidof: print the PID of the SERVICE, or return 1 if it's not up
- rescan: re-read `/etc/nitro`, start added daemons, stop removed daemons
- Shutdown: shutdown (poweroff) the system
- Reboot: reboot the system

## Controlling nitro by signals

rescan can also be triggered by sending `SIGHUP` to nitro.

reboot can also be triggered by sending `SIGINT` to nitro.

shutdown can also be triggered by sending `SIGTERM` to nitro, unless
nitro is used as Linux pid 1.

## nitro as `init` for Linux

nitro is self-contained and can be booted directly as pid 1.
It will mount `/dev` and `/run` when required, everything else
should be done with `SYS/setup`.

When receiving Ctrl-Alt-Delete, nitro triggers an orderly reboot.

## nitro as init for a Docker container

nitro is compiled statically, so you can copy it into your container easily:

	COPY ./nitro /bin/
	COPY ./nitroctl /bin/
	CMD ["/bin/nitro"]

Note that `/run` must exist in the container if you want to use the
default control socket name.

You can put the control socket onto a bind mount and remote control
`nitro` using `nitroctl` from the outside by pointing `NITRO_SOCK` to
the appropriate target.

## nitro on FreeBSD

You can add this line to `/etc/ttys` to run `nitro` supervised by
FreeBSD `init`:

	/etc/nitro "/usr/local/sbin/nitro" "" on

## Periodic jobs

Instead of cron, you can use snooze to run jobs periodically from a
supervisor: https://git.vuxu.org/snooze/about/

## Authors

Leah Neukirchen <leah@vuxu.org>

## Thanks

I'm standing on the shoulder of giants; this software would not have
been possible without detailed study of prior systems such as
daemontools, freedt, runit, perp, and s6.

## Copying 

nitro is licensed under the 0BSD license, see LICENSE for details.
