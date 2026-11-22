# Q25 keyboard and Phosh state indicators

## Mapping and compatibility

The deployed source is `rootfs/etc/keyd/default.conf`; the keyboard driver is
unchanged. Green pickup is tap-Tab / hold-function: the printed number positions
`W E R S D F Z X C 0` produce F1–F10 and `V B` produce F11/F12. Releasing either
member of the chord first does not leak Tab. Other useful layers remain intact.

Red hangup taps Super+D, explicitly configured as **Phosh home/overview**, not
minimize-all or a separate launcher. An uninterrupted 600 ms hold sends one
Alt+F4 pulse, with no repeat or release action. Another key before the deadline
resolves the tap branch and cancels close. `layerm(close_once, A-f4)` is deliberate:
it executes once, unlike a held F4 or an active repeating macro. The existing
RightShift+red Alt+F4 and Control+Alt+red VT chord remain separate overrides.
Berry remains a real Super key; the stock overview accepts application search.

Back emits Alt+Left. Forward is typing Shift+Back **or printed left Shift+Back**.
The printed Alt key is the existing typing Shift; printed left Shift is the
existing Control. Both approved forward aliases consume their source modifier:
no Shift+Alt+Right or Control+Alt+Right leaks. Other Control shortcuts are unchanged.
Typing Shift+`$` emits `|`; unshifted `$` still emits `$` (US key symbols).

### Sym must not steal editing/navigation

Held, one-shot and locked Sym all preserve Backspace and Up/Down/Left/Right/Home.
The previous `backspace = delete` and navigation-to-punctuation bindings are gone.
Punctuation remains on letters (`Y )`, `G /`, `H :`, `J ;`, `B !`, etc.). One-shot
Sym is consumed by a navigation/erase key just as by any ordinary key; held Sym
stays held and locked Sym stays locked. This is not an input-event inference.

`drivers/keyboard/config.h` selects the IRQ path. Its `bbqX0kbd_work_fnc` passes
Sym+letter events intact; the **legacy, disabled polling path** has modifier
transformations and must not be confused with the active path. The keymap's lower
case/number scancodes are documented as synthetic/legacy aliases, not current
firmware's ordinary letter output. A real KEY_HOME is navigation, never colon.
Trackpad center is scancode `0x05`: Enter in arrow mode, BTN_LEFT in mouse mode,
not Home. RightShift+T toggles these modes in the driver and suppresses T; normal
relative mouse motion/arrow generation is unchanged. Existing Sym+mouse right-click
mapping is retained. No kernel or firmware remapping was added.

## Authoritative state, not a key logger

`packages/keyd-q25` pins upstream keyd 2.6.0 and adds one daemon patch plus a small
state publisher. `packages/phosh-q25-keyboard` uses the upstream PhoshStatusIcon
extension, pinned to Phosh 0.59.0. It does not replace/fork the shell, run an overlay,
read input devices, invoke shell commands, or join the privileged `keyd` group.

The separate Unix socket `/run/keyd-q25-state.sock` is root-created in root-owned
`/run`, mode 0666. This permits **read-only state subscription**, not daemon control.
The ordinary privileged `/run/keyd.socket` is unchanged and is never used by the
plugin. A client that writes anything is disconnected on the next publisher or
accept pass; there is no command decoder, injection operation or raw event API.
The plugin checks Linux SO_PEERCRED and only trusts a root-owned server.

Every connection immediately receives the latest **complete four-byte snapshot**:

| Byte | Value |
| --- | --- |
| 0 | ASCII `Q` |
| 1 | protocol version `1` |
| 2 | Shift flags: bit 0 held, bit 1 one-shot, bit 2 toggled; bit 3 real CapsLock LED |
| 3 | Sym flags: bit 0 held, bit 1 one-shot, bit 2 toggled |

No timestamps, device identities, keycodes, text, or incremental key/layer events
are sent. Only changed snapshots are broadcast. At most eight clients, one accept
per event-loop pass, nonblocking IO and bounded kernel send buffers prevent slow
clients from holding up input; backpressure/partial writes disconnect the client.
The plugin tolerates stream fragmentation, rejects bad versions/flags, clears
immediately on EOF/error, and reconnects after about one second. Disconnected or
unknown state is hidden, never guessed. Local users can exhaust subscriber slots
(availability limitation), but cannot inject keys or block the keyboard via this API.

Snapshots come **after completed keyd event processing**, because upstream layer
callbacks occur before all one-shot/toggle bookkeeping is finished. A matched Q25
input removal releases its output keys and reconstructs its keyboard engine;
reload resets layers and invalidates the old active-keyboard pointer. Every
reconnection gets the current state, so restarting only Phosh retains real locks,
whereas restarting/reloading keyd clears keyd layers. A real compositor CapsLock LED
is distinct and is not falsely cleared by a keyd config reload. No Q25 device means
no Q25 status. Ordinary upstream behavior is retained for non-Q25 configs.

### Display semantics and placement

The Shift outline arrow and Sym `#` appear immediately before battery in the stock
LTR top panel. No badge means held, `1` means next-key, `•` means locked. True
CapsLock has a solid arrow/bar; the tooltip/accessibility name explicitly says
Caps Lock. Momentary Shift is never mislabeled Caps. Shift lock is accurately
called Shift lock (it also shifts digits), not conflated with CapsLock. Combined
flags are retained in the tooltip/accessibility description.

Phosh 0.57 inserts equal-priority status icons newest-first. Battery is its first
built-in priority-10 icon; the priority-10 Q25 plugin therefore precedes battery.
Real upstream widget allocation is tested. The shipped plugin list contains only
Q25; additional same-priority plugins/reordering or a future ABI need revalidation.
The plugin also refuses Phosh's initial revealer/show-all request until actual
state is active; absence of the daemon cannot fabricate a lit icon.

### Lock-screen limitation

Close uses the existing compositor policy, not a second lock-state IPC or keyd
lock-screen detector. Reviewed Phoc 0.57 `handle_close()` calls
`phoc_seat_get_focus_view()`, which returns NULL when `has_focus` is false;
`phoc_seat_set_focus_layer()` clears that flag when the lock layer takes focus.
Thus an established lock layer has no app view to close. **This is source review,
not live lock/unlock/race validation.** Physical testing with an app behind the lock
screen remains a deployment gate; no stronger live safety claim is made here.

## Reproducible isolated validation

Required host tools: gcc, patch, Python 3, pkg-config, GTK3/GIO development files,
glib-compile-resources, Xvfb/xauth, bsdtar and vercmp. Obtain the pinned release
archives using the URLs and SHA-256 values in the PKGBUILDs, then extract them to
private scratch directories. `KEYD_SOURCE` must be **pristine**: tests copy and patch
it in a temporary directory. No tests open `/dev/input`, `/dev/uinput`, the running
keyd control socket, or a physical display/session.

```sh
KEYD_SOURCE=/private/keyd-2.6.0 \
PHOSH_SOURCE=/private/phosh-0.59.0 \
python3 -m unittest discover -s tests -v
bash -n build/build-packages.sh packages/{keyd-q25,phosh-q25-keyboard}/PKGBUILD
```

Without source variables, optional engine/widget tests are explicitly skipped,
not counted as successful runtime coverage. Engine tests exercise real daemon
callbacks and keyd processing. The driver harness compiles the actual IRQ worker
and keymap, substitutes FIFO/relative movement, and feeds its output into keyd;
it covers genuine Home versus Sym+H and center Enter, plus mouse/arrow mode
round-trip. GTK tests use real upstream status widgets, allocation and module
loading, testing startup, unknown/off, held/one-shot/locked, true CapsLock,
fragmentation, malformed data, spoofed peer, daemon/Phosh restart and teardown.
The loader harness applies Phosh's actual exported-symbol whitelist. A header
alone is not an ABI guarantee: Phosh 0.57 declares but does not export the
status-icon priority setter, so the plugin uses its public GObject property.
The ARM package also checks every `phosh_*` import against the installed shell.
The keyd upstream `make test-io` suite also runs in the package's `check()`.

Use an **isolated** buildroot/output (never the shared staging root while another
lane is building):

```sh
sudo env BUILDROOT=/private/keyboard-buildroot OUT=/private/keyboard-out \
  bash build/build-packages.sh packages/phosh-q25-keyboard packages/keyd-q25
```

The deliberate reversed argument order verifies keyd is built first. The builder
resolves needed local dependencies from archive `.PKGINFO` name/version/arch/provides,
rejects missing required keyd/ambiguous/stale/incompatible candidates, then lets
pacman resolve remaining repo dependencies. It uses normal provides/conflicts to
replace stock keyd, without `--nodeps`/overwrite tricks. Unrelated package order
is unchanged. The default build-all/package/image flow includes both packages;
no build-rootfs change or shared rootfs mutation is needed.

## Deployment / rollback — requires separate approval

Tests/builds do not install onto the phone, restart sessions, capture input or
reboot. For an approved live installation, first take read-only root **and home**
checkpoints (`/home` is a separate subvolume), retain independent root SSH, and save:

- `/etc/keyd/default.conf`, the Phosh override file and old package versions;
- `gsettings get org.gnome.shell.keybindings toggle-overview` and
  `gsettings get mobi.phosh.shell.plugins status-icons` from the real Phosh user;
- an explicit note of any user dconf override (defaults alone do not replace it).

Install **both** reviewed aarch64 packages with pacman (allow the declared stock
keyd replacement); deploy the keyd config and `90-q25-phosh.gschema.override`, then
compile schemas. Existing sessions with user settings must explicitly adopt
Super+D for `toggle-overview` and append `q25-keyboard` to their existing
`status-icons` list rather than clobber other plugins. Restart keyd and the Phosh
session only with permission; a reboot is unnecessary. Do not add the desktop
user to the keyd group. Verify the ABI/version and independent SSH first.
`keyd.service` is Type=simple: an active unit does not yet guarantee its socket
exists. Validate a complete four-byte snapshot as the desktop user after startup.
After restarting Phosh, check its live memory map for the plugin and its journal
for missing-symbol errors; successful package installation alone is insufficient.

Live acceptance checklist: all twelve function chords and both release orders;
Back/forward in browser and Files; `$`/pipe; red tap versus 600 ms hold (no repeated
close) including behind a locked screen; Berry search; Sym punctuation followed
by Backspace/trackpad navigation in held/one-shot/locked modes; center/mouse toggle;
icons next to battery; one-shot consumption and lock/unlock; keyd reload/restart,
Phosh restart and input hotplug without stale indicators. Hardware-dependent
behavior, current firmware and lock-screen races remain unverified until then.

Rollback: restore saved config/override/settings, remove the plugin package,
replace keyd-q25 with the saved stock keyd package using pacman's normal conflict
handling, compile schemas, then restart the two services/session with approval.
Restoring only the old config is also a mapping-only rollback; the read-only state
patch does not require the new mappings. Keep the independent SSH path throughout.
