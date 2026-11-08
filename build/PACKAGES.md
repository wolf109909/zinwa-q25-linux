# Image package defaults

ARM packages use CERNET's HTTPS redirector first, followed by TUNA and the
upstream Arch Linux ARM mirror. `rootfs/etc/pacman.d/mirrorlist` is the shipped
source of truth; configure installs that file rather than regenerating a
separate list. `build/pacman-build.conf` uses the same ordered mirrors.

Offline regression check: `python3 build/test-package-config.py -v`.
