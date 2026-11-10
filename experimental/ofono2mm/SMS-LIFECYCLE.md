# Pkgrel 3 — bounded SMS observer lifecycle

`0007-sms-observer-lifecycle.patch` changes only `ofono2mm/mm_sms.py` on top of
pkgrel 2. Previous source/binary bundles and the 34-test report/package log remain
available. Native C, other workspaces and the phone were untouched.

## Automatic retirement

A correlated native `sent`, `failed` or `cancelled`, a MessageRemoved without
terminal evidence, or a failed initial GetProperties now closes the observer.
Final MM properties, native path and the one-attempt latch remain available in
local history. Positive native sent remains SENT, not delivered; failures/removal
without positive evidence remain UNKNOWN. Fast terminal evidence before the Send
reply is correlated first, then cleanup runs **without a GetProperties query**.
No native retry, cancel, queue enumeration, history recovery or replay was added.

Teardown has one shared task and a permanent closed latch. It waits on a small
per-object lock until outstanding AddMatch acknowledgements have been accounted
for, then removes every installed rule. Local Delete awaits that same task via
shield, so overlapping Delete/automatic cleanup cannot remove twice, interrupt
shared cleanup or re-arm observation. The handler is removed outside signal
iteration: otherwise removing it from inside dispatch could skip the next SMS's
owner-loss callback. Three simultaneous pending messages exercise this detail.

## Authenticated owner lifetime

While pending there are four owned match registrations: the previous three
message-state/lifetime rules and exactly:

```
type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.ofono'
```

The handler independently checks daemon sender, path, interface, member,
signature, service name and old/new owner. A directly addressed forged signal
from another connection cannot trigger cleanup. Disappearance or replacement
of the captured owner changes pending SMS to UNKNOWN and detaches; it never
follows a replacement service or reads its queue.

The owner is resolved, the owner-change rule is armed first, all message matches
are acknowledged, and the owner is revalidated before transmission. This covers
the initial owner-query/AddMatch gap. Loss during prearm prevents SendMessage.
**SendMessage itself is addressed to the captured unique owner**, not the movable
well-known name, so replacement cannot receive the old operation. This uses the
same existing `org.ofono.MessageManager.SendMessage(ss)->o` API through dbus-fast;
there is no protocol change. Constructor/Create still do not transmit.

An owner change after SendMessage was dispatched cannot unsend it. The result
becomes UNKNOWN, no subsequent state query is made, and the existing latch blocks
another attempt. A previously correlated terminal result is retained, not erased
by later owner changes.

## Actual resource checks

**46/46 tests pass** in the working tree, clean seven-patch source build, and
packaged tree, using compiled dbus-fast 5.2.0 and the private non-activating daemon.
The tests query the daemon's real
`org.freedesktop.DBus.Debug.Stats.GetConnectionStats` MatchRules value and inspect
the real service connection's registered Python handler list—not just SMS State
or the adapter's bookkeeping fields.

Measured stress result, without any local Delete:

```
48 completed SMS retained in local history
broker MatchRules: 1 -> 1
Python user handlers: 0 -> 0
owned SMS match registrations: 0
owned SMS handlers: 0
```

The one baseline rule belongs to dbus-fast's name-owner machinery, not an SMS.
Every live pending observer adds four rules; after **each** sent/failed/cancelled
completion the counts return to baseline. Additional tests verify:

* all three simultaneous pending observers close on service-name loss;
* actual native connection disappearance and name replacement yield UNKNOWN;
* owner loss during AddMatch and replacement before the owner match prevent send;
* fast terminal reply skips the dead-object query and leaves zero resources;
* MissingObject and removed/unknown cases leave zero resources;
* Delete during AddMatch acknowledgement and duplicate Delete during paused
  RemoveMatch cleanup are idempotent, with real broker rules removed;
* a forged owner-change signal is rejected;
* loss after dispatch does not unsend, query a replacement or retry.

Race injection delays real private-bus method replies only. Initial attempts to
patch dbus-fast's compiled read-only `MessageBus.call` attribute were rejected;
tests instead use an SMS-specific delegating adapter while retaining actual bus
I/O. No fake AddMatch/RemoveMatch replies or fake broker counts are used.

## Artifact / remaining boundaries

Source: `arch/ofono2mm-furilabs-inert-1.24.0.r82d43ea-3.src.tar.gz`.
Binary built but **not installed**; still 33 non-executable source/license files,
no launcher, activation/configuration, service, policy or install hook. Upstream
pin and dependencies are unchanged. Checksums and actual logs are retained here.

No phone, host system/session bus, network or native queue was accessed. These
results are not standard-app/device, IMS/audio or ABI acceptance. Native pre-start
queue safety, delivery reports, durable deduplication and explicit-mutator guards
remain separate. Silent per-modem teardown without State/MessageRemoved or owner
change is not polled/recovered by this patch. Diagnostic behavior and metadata-only
logging are unchanged. Next step is independent review, not deployment.
