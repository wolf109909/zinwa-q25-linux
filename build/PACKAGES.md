# Image package defaults

ARM packages use CERNET's HTTPS redirector first, followed by TUNA and the
upstream Arch Linux ARM mirror. `rootfs/etc/pacman.d/mirrorlist` is the shipped
source of truth; configure installs that file rather than regenerating a
separate list. `build/pacman-build.conf` uses the same ordered mirrors.

The `archlinuxcn` repository also uses CERNET's HTTPS redirector. Configure
bootstraps **only archlinuxcn-keyring** with a temporary config containing only
that repository and a scoped `SigLevel=Never` exception. This is an explicit
trust-on-first-use dependency on the HTTPS mirror, not cryptographic validation
of that first keyring. The file is removed even on error, and
`pacman-key --populate archlinuxcn` must succeed. If the keyring is already
installed, configure only repopulates it.

The shipped `pacman.conf` always uses `Required DatabaseOptional` signatures;
there is no repository signature bypass in the image. Host-side bootstrap
retains its existing build-only signature exception. Kernel/firmware and
modemmanager/libmm-glib IgnorePkg holds are unchanged.

`paru` is installed as a signed **aarch64 binary from archlinuxcn**, not built
from AUR. The image also includes base-devel and git. Its stock `/etc/paru.conf`
only enables `BottomUp`. Run paru as the normal user (it uses sudo for pacman),
review AUR build files before building, and keep the kernel/modem ABI holds.

The full stock `/etc/security/faillock.conf` changes only `deny = 10` and
`unlock_time = 120`. This tolerates more touchscreen PIN mistakes than Arch's
3-attempt / 600-second defaults, without disabling lockout or changing the
900-second failure interval. It also permits more PIN guesses; choose a strong
credential for an exposed device. Actual lock-screen usability is a device test.

Offline regression check: `python3 build/test-package-config.py -v`.
