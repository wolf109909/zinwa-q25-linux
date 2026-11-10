# Pkgrel 2: native SMS state reconciliation and adjacent call deletion race

**Lifecycle follow-up:** [SMS-LIFECYCLE.md](SMS-LIFECYCLE.md) describes pkgrel 3,
which retires terminal observers automatically and handles authenticated owner
loss. This page preserves the pkgrel 2 source audit and baseline evidence.

Two additional, sequential patches; no runtime/dependency/pin changes:

* `0005-reconcile-native-sms-state.patch`: SMS state observation and Create
  Variant validation. No native queue, replay, cancel, retry or history reader.
* `0006-idempotent-call-delete.patch`: shared idempotent local call removal and
  string-Variant validation for CreateCall.number. A native CallRemoved during
  the awaited Hangup no longer causes a second local removal/error; unrelated
  calls remain exported and no second Hangup is issued.

## Native source facts, not inferred delivery

Read-only source audit: Furi oFono
`45d1d49ac2ec3aa011761e46b0c53cc1933cf8ad`, under the authorized
`ims-reuse/deps/ofono/ofono`. The four inspected working files were compared
byte-for-byte with pinned git blobs; evidence is in
`native-message-evidence/SOURCE-PIN.txt`. No native-workspace writes occurred.

* `src/message.c:43-56,182-200` exposes State strings and emits
  `org.ofono.Message.PropertyChanged("State", variant<string>)`.
  Documented states are pending/sent/failed; implementation also emits cancelled.
* `src/sms.c:2269-2335` creates/registers the message, queues it, schedules initial
  transmission after 100 ms when registered, calls the queued callback (which
  sends the SendMessage path reply at `:983-993`), then emits MessageAdded.
* `src/sms.c:671-720` emits the final State, immediately emits MessageRemoved and
  unregisters the object. A subsequent GetProperties may legitimately find no
  object. **Removal does not identify whether it was sent, failed or cancelled.**
* `src/sms.c:769-789` marks sent after successful submission of all PDUs; its
  history status is SUBMITTED (`:687-691`). This is **not recipient delivery**.

## Mapping and races

| Native evidence | MM SMS State | DeliveryState |
|---|---|---|
| pending | SENDING (4) | UNKNOWN (0x100) |
| positive sent property/signal for the returned path | SENT (5) | UNKNOWN (0x100) |
| failed / cancelled | UNKNOWN (0); MM has no FAILED SMS state | UNKNOWN (0x100) |
| missing object, unknown/malformed state, removal without terminal evidence | UNKNOWN (0) | UNKNOWN (0x100) |
| SendMessage return alone | never proof of SENT | UNKNOWN (0x100) |

The unknown delivery enum was checked in the locally cached libmm-glib 1.24.2
ModemManager-enums.h (UNKNOWN=0x100), not guessed from native sent.

Before the single SendMessage attempt, the observer resolves org.ofono's unique
owner and **awaits AddMatch acknowledgements** for State PropertyChanged and
modem-scoped MessageAdded/MessageRemoved. Existing queues are never enumerated.
Before the reply, it buffers only State metadata for objects newly announced
within this Send window; signals for unrelated pre-existing paths are ignored.
After receiving the native path it discards all other paths, reads GetProperties
**only for that path**, and ignores unrelated subsequent events. No SMS body or
number is observed by this handler. Modem-scoped signal rules can receive other
messages' state metadata, but never trigger reads of their properties or queues.

This pre-arming protects fast signals delivered before the awaiting coroutine
resumes, even though pinned native code queues the method reply before MessageAdded.
A revision counter prevents a stale GetProperties snapshot overwriting a newer
signal. Captured positive sent survives subsequent removal/UnknownObject; a
missing terminal signal is **not reconstructed from removal or guessed history**.
A lost Send reply leaves no trustworthy identity: cached signals are discarded,
state becomes UNKNOWN, handlers detach, and the attempt latch prevents a retry.
Subscription-setup failure also aborts without sending.

Local Delete first marks the object deleted, disconnects its handler/removes bus
match rules, then unexports it. Races with AddMatch, SendMessage or GetProperties
cannot reattach a deleted object, issue a new property query after deletion, or
transmit again.
This cleanup **never calls org.ofono.Message.Cancel**. Pkgrel 2 retained terminal
observation until local deletion; pkgrel 3 replaces that behavior with automatic
retirement while preserving final properties and the terminal-state latch.

## Evidence and limits

**34/34 real private-bus tests pass** against the working tree, fresh six-patch
source, and packaged tree. Synthetic native org.ofono.Message/MessageManager
services exercise actual dbus-fast methods and signals. Coverage includes:

* pending to sent/failed plus removal; cancelled/unknown native state;
* terminal signal before Send reply; removed-before-read/unknown object;
* stale initial properties after a newer terminal signal;
* uncertain reply with an uncorrelated sent signal;
* unrelated newly announced message never queried and its cache discarded;
* concurrent local deletion during Send reply and initial state query;
* malformed number/text/delivery-report Variants rejected before allocation/send;
* CallRemoved delivered on the private bus **before Hangup returns**, exactly one
  Hangup and one local CallDeleted, with an unrelated call preserved.

The prior 21-test report and package log are retained as `pkgrel1-*-results.txt`;
pkgrel 1 source/binary bundles remain alongside pkgrel 2. No deployment/phone test.
The private daemon has no service directories, both bus addresses are isolated,
and no network, host system/session bus, Binder or real queue was accessed.

Remaining limits: no delivery-report reconciliation, native-history recovery,
durable cross-restart deduplication, new native queue or automatic retry. If the
native service tears down without a final State/MessageRemoved after an observed
pending state, this bounded observer does not invent an outcome; silent per-modem lifecycle reconciliation remains an integration check;
pkgrel 3 now handles authenticated service owner disappearance/replacement. Native pre-start
queue replay safety and explicit MM-mutator authorization are still separate
requirements; diagnostic mode is not a firewall or deployment authorization.
