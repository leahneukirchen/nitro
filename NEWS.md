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
