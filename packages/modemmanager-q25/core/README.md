# ModemManager core: CPMS query whitespace

This optional recipe builds `modemmanager 1.24.2-2.1` for aarch64. It fixes the
current-storage parser in the daemon; rebuilding only the adjacent Q25 plugin
cannot change that parser. The existing plugin, APN/data path, package holds and
rootfs build integration are deliberately unchanged.

## Scope

MOLY can return `+CPMS: "ME", 0, 50, "SM", 0, 40, "SM", 0, 40`.
The original parser rejects its separator whitespace. The patch accepts spaces
and tabs, bounds quoted names, and requires a complete nine-field response.
It preserves unknown-name handling and the unused receive-storage field behavior.
Upstream helper tests cover compact responses, observed spacing, mixed whitespace,
multi-digit counts, unknown storage names and malformed/truncated/extra fields.

This repairs **current-storage reply parsing only**. It does not explain the
separate `No network` send error or establish SMS delivery, voice registration,
call audio, or an IMS requirement. It adds no SMS commands, transport selectors,
message access, dial operations, or proprietary initialization.

The live current-storage initialization result is private and logged at DEBUG.
Neither `SupportedStorages`/`DefaultStorage` nor silence at INFO proves that
initialization succeeded. Parser regressions and normal service recovery must
not be reported as a live initialization or SMS-transport acceptance test.

## Packaging and build

The baseline is Arch's `modemmanager` recipe at tag `1.24.2-2`. Its gtk-doc and
Fibocom fixes are already present in upstream release commit
`f2b9ab1ad78d322f32134a444b5b54c6e8160e19`; `prepare()` verifies their exact
hunks without reapplying them. The archive is the same pinned upstream source
used by the adjacent plugin recipe. Runtime defaults, all stock
plugins, protocol support, permissive polkit policy, default D-Bus AT-command
restriction, unit files and installation paths are preserved.

Only gtk-doc, Vala bindings and GIR/typelib generation are disabled; those outputs
belong to excluded docs/development split packages. The recipe does **not** ship
or replace libmm-glib, its headers, typelib or GIR. It explicitly depends on the
installed distro `libmm-glib=1.24.2-2`, rather than inventing a dependency on a
locally rebuilt `2.1` library.

Build with `makepkg` in a dedicated aarch64 build environment containing that
library version. `check()` runs the upstream modem helpers (generic/QMI/MBIM) and
error helpers, with synthetic fixtures only. Run the adjacent Q25 helper tests
as well. Before deployment, compare the candidate and original core archives'
file lists, non-ELF assets, dependency metadata and ELF ABI requirements.

Do not install into a shared buildroot or overwrite live binaries manually.
A device trial needs a verified original core package, current checkpoints,
blocked transmit/retry clients, a tested recovery timer, and an in-place package
transaction while ModemManager is stopped. Preserve the working library/plugin
and require LTE plus DNS/HTTPS recovery. No successful parser test authorizes
sending a message or thawing queued clients.
