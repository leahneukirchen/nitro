# nitro, a tiny but flexible init system and process supervisior

## Overview

Nitro is a tiny process supervisor that also can be used as pid 1 on Linux.

There are three main applications it is designed for:
- As init for a Linux machine for embedded, desktop or server purposes
- As init for a Linux container (Docker/Podman/LXC/Kubernetes)
- As unprivileged supervision daemon on POSIX systems

Nitro is configured by a directory of scripts, defaulting to
`/etc/nitro` (or the first command line argument).

## Requirements

- Kernel support for Unix sockets
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

Service directories ending in `@` are ignored, however you can refer
to parametrized services by symlinks (either in the service directory
or as a `log` symlink), or start them manually using `nitroctl`.

The part after the `@`, the parameter, is passed to the scripts as
first argument.

For example, given you have a script `agetty@/run` and a symlink
`agetty@tty1` -> `agetty@`, nitro will spawn `agetty@/run tty1`.  Upon
running `nitroctl up agetty@tty2`, nitro will spawn `agetty@/run
tty2`, even if it does not exist in the service directory.

## Modes of operation

The lifecycle of a machine/container/session using nitro consists of
three phases.

First, the system is brought up.  If there is a special service
g`SYS`, its `setup` script is run first.  After it finishes, all
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

## Nitro as `init` for Linux

Nitro is self-contained and can be booted directly as pid 1.
It will mount `/dev` and `/run` when required, everything else
should be done with `SYS/setup`.

When receiving Ctrl-Alt-Delete, nitro triggers an orderly reboot.

## Nitro as init for a Docker container

Nitro is compiled statically, so you can copy it into your container easily:

	COPY ./nitro /bin/
	COPY ./nitroctl /bin/
	CMD ["/bin/nitro"]

Note that `/run` must exist in the container if you want to use the
default control socket name.

You can put the control socket onto a bind mount and remote control
`nitro` using `nitroctl` from the outside by pointing `NITRO_SOCK` to
the appropriate target.

## Nitro on FreeBSD

You can add this line to `/etc/ttys` to run `nitro` supervised by
FreeBSD `init`:

	/etc/nitro "/usr/local/sbin/nitro" "" on

## Authors

Leah Neukirchen <leah@vuxu.org>

## Thanks

I'm standing on the shoulder of giants; this software would not have
been possible without detailed study of prior systems such as
daemontools, freedt, runit, perp, and s6.

## Copying 

nitro is licensed under the 0BSD license, see LICENSE for details.
