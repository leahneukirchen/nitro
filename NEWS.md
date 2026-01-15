## 0.7 (2026-01-15)

* N.B.: for this release you need to update nitro and nitroctl in
  lockstep: the new nitroctl will *not* be able to shut down a pre-0.7
  nitro.  You can kill PID 1 with SIGINT to trigger a reboot.
* feature: nitro and nitroctl now use an extensible binary protocol
  which will allow for forward-compatibility (see SPAT.md)
* feature: nitroctl is more robust in case of rare socket errors
* feature: log can be a symlink to a service template which is then
  instantiated with the parameter or the name of the service
* documentation improvements

## 0.6 (2025-12-12)

* feature: nitroctl: sort "list" output by service name
* feature: improvements to output on shutdown
* feature: panic handler is not run when socket is in use
  (accidentally running nitro on a booted system)
* bugfix: changes to log links are now picked up on rescan and restart

## 0.5 (2025-10-17)

* feature: nitroctl now sends commands in parallel
* feature: nitroctl now has a verbose mode (-v)
* feature: nitroctl now uses fractional seconds for -t
* feature: nitroctl now works when no reply socket can be created
* feature: nitroctl list can only show services asked for
* permit creation of down services in SYS/setup
* service startup is retried after temporary exec(2) errors
* contrib: zsh completion
* small bugfixes
* more robust test suite

## 0.4.1 (2025-09-23)

* feature: print execution errors for run scripts
* bugfix: avoid hang after SYS/finish

## 0.4 (2025-09-19)

* feature: support for pid 1 on NetBSD
* feature: configurable NITRO_SOCK via /etc/nitro.sock symlink
* feature: pipe-based, s6-compatible readiness notification
* feature: optional reopening of service directory
  (compile with -DREOPEN_USE_CLOSEDIR or -DREOPEN_USE_DUP_HACK)
* regression bugfix: SYS/finish wasn't executed properly (since v0.3)
* small bugfixes
* test suite added

## 0.3 (2025-08-29)

* feature: add single-user mode by adding "S" or "single" to the
  kernel command line
* feature: nitroctl accepts multiple arguments for "start" etc.
* bugfix: properly shutdown, slaying could hang
* bugfix: respect 'down' file for services added later
* bugfix: allow spaces in the instance string
* documentation improvements

## 0.2 (2025-08-22)

* initial public release
* add NEWS.md file
