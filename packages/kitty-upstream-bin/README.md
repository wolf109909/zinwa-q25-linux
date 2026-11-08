# Touch-capable Kitty for Q25

Repackages the unmodified official Kitty **0.50.0 ARM64** binary. The ARM mirror
was still at 0.49.2 when the user verified 0.50.0 touch under Phosh. The release
archive SHA256 is pinned in `PKGBUILD`; its self-contained runtime stays together
at `/opt/kitty`. Pacman owns the `kitty`/`kitten` commands and normal desktop entry.
It provides/replaces the stock Kitty, shell-integration and terminfo packages.

Build with `sudo build/build-packages.sh packages/kitty-upstream-bin`. The normal
image local-packages stage replaces stock Kitty; no Foot is in the image list.
Fish is installed by `build/packages.txt` and selected by the system Kitty config.
User `kitty.conf` files can override system defaults. Login shells are unchanged.

This is a pinned local package, not an automatic upstream updater. Review and
update the version/checksum here, or return to the repository package once it
contains the tested touch support. Do not overwrite the bundled runtime libraries
with system copies.

For the settled device: deploy with `pacman -U`, **never flash userdata**. Take
a root Snapper checkpoint plus a separate home snapshot before changing user
settings: the image has separate root and home subvolumes. Boot partitions are
not covered by either checkpoint.

Upstream: https://sw.kovidgoyal.net/kitty/binary/
Release: https://github.com/kovidgoyal/kitty/releases/tag/v0.50.0
