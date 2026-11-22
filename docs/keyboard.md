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
