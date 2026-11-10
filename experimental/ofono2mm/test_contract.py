"""FuriLabs pin contract tests: private D-Bus only, synthetic AsyncMock backend."""
import asyncio
import contextlib
import importlib
import io
import os
from pathlib import Path
import subprocess
import socket
import sys
import tempfile
import unittest
from unittest.mock import AsyncMock, Mock, patch
from types import SimpleNamespace

SOURCE = Path(os.environ.get('OFONO2MM_SOURCE', Path(__file__).resolve().parents[3] / '.offline-work/furilabs')).resolve()
sys.path.insert(0, str(SOURCE))
sys.dont_write_bytecode = True
sandbox = tempfile.TemporaryDirectory(prefix='private-bus-', dir=Path(__file__).resolve().parent)
SOCKET = f'{sandbox.name}/bus'
ADDRESS = f'unix:path={SOCKET}'
for name in ('DBUS_SYSTEM_BUS_ADDRESS', 'DBUS_SESSION_BUS_ADDRESS'):
    os.environ[name] = ADDRESS
for name in ('DISPLAY', 'DBUS_STARTER_ADDRESS', 'DBUS_STARTER_BUS_TYPE'):
    os.environ.pop(name, None)
config = Path(sandbox.name) / 'bus.conf'
config.write_text(f'''<busconfig><type>session</type><listen>{ADDRESS}</listen>
<auth>EXTERNAL</auth><policy context="default"><allow own="*"/>
<allow send_destination="*"/><allow eavesdrop="true"/></policy></busconfig>''')
# No service directories: this private daemon cannot activate host services.
DAEMON_ARGS = ['dbus-daemon', '--nofork', '--print-address=1', f'--config-file={config}']


def prohibit_external_io(event, args):
    if event == 'socket.connect' and args[1] != SOCKET:
        raise AssertionError('Only the private test bus may be connected')
    if event == 'subprocess.Popen' and list(args[1]) == DAEMON_ARGS:
        return
    if event in ('socket.getaddrinfo', 'subprocess.Popen', 'os.system'):
        raise AssertionError(f'External I/O forbidden: {event}')
    if event == 'open' and isinstance(args[0], (str, bytes)):
        path = os.fsdecode(args[0])
        if path.startswith(('/dev/binder', '/dev/hwbinder', '/dev/vndbinder', '/var/lib/ofono')):
            raise AssertionError('Hardware/private storage access forbidden')


sys.addaudithook(prohibit_external_io)
from dbus_fast import Variant, DBusError, Message
from dbus_fast.constants import MessageType, NameFlag
from dbus_fast.aio import MessageBus
from dbus_fast.service import ServiceInterface, method, signal
import ofono2mm
import main
from ofono2mm.mm_modem_messaging import MMModemMessagingInterface
from ofono2mm.mm_modem_voice import MMModemVoiceInterface
from ofono2mm.mm_modem import MMModemInterface
from ofono2mm.mm_bearer import MMBearerInterface
from ofono2mm.mm_modem_simple import MMModemSimpleInterface
from ofono2mm.mm_modem_cell_broadcast import MMModemCellBroadcastInterface
from ofono2mm import utils
utils.settings_dir = sandbox.name
utils.settings_file = str(Path(sandbox.name) / 'nonexistent-settings')
NUMBER = 'SYNTHETIC-NOT-A-NUMBER'
TEXT = 'SYNTHETIC-SECRET-BODY'
DTMF = 'SYNTHETIC-DTMF'


class RecordingLoop:
    def __init__(self):
        self.tasks = []

    def create_task(self, coroutine):
        self.tasks.append(coroutine.cr_code.co_name)
        coroutine.close()


class FakeBus:
    def __init__(self):
        self.exports = {}
        self.release_name = AsyncMock()
        self.request_name = AsyncMock()

    def export(self, path, interface):
        self.exports.setdefault(path, []).append(interface)

    def unexport(self, path):
        self.exports.pop(path, None)


class Backend(Mock):
    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.call_get_properties = AsyncMock(return_value={})
        self.call_get_contexts = AsyncMock(return_value=[])
        self.call_set_property = AsyncMock(side_effect=AssertionError('No implicit property mutation'))
        self.call_send_command = AsyncMock(side_effect=AssertionError('No implicit AT'))
        self.call_send_message = AsyncMock(side_effect=AssertionError('Send requires synthetic opt-in'))
        self.call_dial = AsyncMock(side_effect=AssertionError('Dial requires synthetic opt-in'))
        self.call_hangup_all = AsyncMock(side_effect=AssertionError('No guessed hangup'))
        self.call_hangup = AsyncMock()
        self.call_send_tones = AsyncMock()

    def _get_child_mock(self, **kwargs):
        return Mock(**kwargs)


class NativeMessage(ServiceInterface):
    def __init__(self):
        super().__init__('org.ofono.Message')
        self.state = Variant('s', 'pending')
        self.query = AsyncMock(side_effect=lambda: {'State': self.state})

    @method()
    async def GetProperties(self) -> 'a{sv}':
        return await self.query()

    @method()
    def Cancel(self):
        raise AssertionError('Native cancel is forbidden')

    @signal()
    def PropertyChanged(self, name, value) -> 'sv':
        return [name, value]

    def set_state(self, state):
        self.state = Variant('s', state)
        self.PropertyChanged('State', self.state)


class NativeMessageManager(ServiceInterface):
    def __init__(self):
        super().__init__('org.ofono.MessageManager')
        self.send = AsyncMock(return_value='/ril_0/message_1')

    @method()
    async def SendMessage(self, number: 's', text: 's') -> 'o':
        assert number == NUMBER and text == TEXT
        try:
            return await self.send(number, text)
        except DBusError:
            raise
        except Exception:
            raise DBusError('org.ofono.Error.Failed', 'Synthetic backend failure') from None

    @method()
    def GetMessages(self) -> 'a(oa{sv})':
        raise AssertionError('Unrelated native queue enumeration is forbidden')

    @signal()
    def MessageAdded(self, path, props) -> 'oa{sv}':
        return [path, props]

    @signal()
    def MessageRemoved(self, path) -> 'o':
        return path


class NativeVoiceManager(ServiceInterface):
    def __init__(self):
        super().__init__('org.ofono.VoiceCallManager')

    @signal()
    def CallAdded(self, path, props) -> 'oa{sv}':
        return [path, props]

    @signal()
    def CallRemoved(self, path) -> 'o':
        return path


class NativeCall(ServiceInterface):
    def __init__(self, manager, removed):
        super().__init__('org.ofono.VoiceCall')
        self.manager, self.removed = manager, removed
        self.hangups = 0

    @method()
    async def Hangup(self):
        self.hangups += 1
        self.manager.CallRemoved('/native_returned')
        await asyncio.wait_for(self.removed.wait(), 2)

    @signal()
    def PropertyChanged(self, name, value) -> 'sv':
        return [name, value]


class ContractTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.output = io.StringIO()
        self.capture = contextlib.redirect_stdout(self.output)
        self.capture.__enter__()
        self.server = await MessageBus(bus_address=ADDRESS).connect()
        self.client_bus = await MessageBus(bus_address=ADDRESS).connect()
        self.native_bus = await MessageBus(bus_address=ADDRESS).connect()
        await self.native_bus.request_name('org.ofono')
        self.native_message = NativeMessage()
        self.native_message_manager = NativeMessageManager()
        self.native_bus.export('/ril_0', self.native_message_manager)
        self.native_bus.export('/ril_0/message_1', self.native_message)
        self.backend = Backend()
        self.native_call = Backend()
        self.interfaces = {'org.ofono.MessageManager': self.backend, 'org.ofono.VoiceCallManager': self.backend}
        self.ofono_client = {'ofono_modem': {'/ril_0': self.interfaces,
                            '/native_returned': {'org.ofono.VoiceCall': self.native_call},
                            '/unrelated': {'org.ofono.VoiceCall': Backend()},
                            '/incoming': {'org.ofono.VoiceCall': Backend()}}}
        self.messaging = MMModemMessagingInterface(self.server, '/ril_0', self.interfaces, verbose=True)
        self.voice = MMModemVoiceInterface(self.server, self.ofono_client, '/ril_0', self.interfaces, {}, verbose=True)
        self.server.export('/modem', self.messaging)
        self.server.export('/modem', self.voice)
        self.sms_manager = await self.proxy('/modem', self.messaging.name)
        self.voice_manager = await self.proxy('/modem', self.voice.name)

    async def asyncTearDown(self):
        for sms in list(self.messaging.messages.values()):
            await sms.disconnect_native()
        self.native_bus.disconnect()
        await self.native_bus.wait_for_disconnect()
        self.client_bus.disconnect()
        self.server.disconnect()
        await self.client_bus.wait_for_disconnect()
        await self.server.wait_for_disconnect()
        self.capture.__exit__(None, None, None)
        for secret in (NUMBER, TEXT, DTMF):
            self.assertNotIn(secret, self.output.getvalue())

    async def proxy(self, path, interface):
        node = await self.client_bus.introspect(self.server.unique_name, path)
        return self.client_bus.get_proxy_object(self.server.unique_name, path, node).get_interface(interface)

    async def sms(self):
        path = await self.sms_manager.call_create({'number': Variant('s', NUMBER), 'text': Variant('s', TEXT)})
        return path, await self.proxy(path, 'org.freedesktop.ModemManager1.Sms')

    async def call(self):
        path = await self.voice_manager.call_create_call({'number': Variant('s', NUMBER)})
        return path, await self.proxy(path, 'org.freedesktop.ModemManager1.Call')

    def test_other_bus_connections_are_denied(self):
        with socket.socket(socket.AF_UNIX) as sock:
            with self.assertRaises(AssertionError):
                sock.connect(f'{sandbox.name}/not-the-test-bus')

    def test_real_imports_and_decorators(self):
        self.assertEqual(Path(ofono2mm.__file__).parent, SOURCE / 'ofono2mm')
        for file in (SOURCE / 'ofono2mm').glob('*.py'):
            importlib.import_module(f'ofono2mm.{file.stem}')
        method = next(m for m in ServiceInterface._get_methods(self.messaging) if m.name == 'Create')
        self.assertEqual((method.in_signature, method.out_signature), ('a{sv}', 'o'))
        method = next(m for m in ServiceInterface._get_methods(self.voice) if m.name == 'CreateCall')
        self.assertEqual((method.in_signature, method.out_signature), ('a{sv}', 'o'))

    async def test_create_sms_and_delete_unsent_do_not_send(self):
        path, sms = await self.sms()
        draft = self.messaging.messages[path]
        self.assertEqual(await sms.get_state(), 1)
        self.native_message_manager.send.assert_not_called()
        await self.sms_manager.call_delete(path)
        self.assertTrue(draft.deleted)
        with self.assertRaises(DBusError):
            await next(m.fn for m in ServiceInterface._get_methods(draft) if m.name == 'Send')(draft)
        self.native_message_manager.send.assert_not_called()

    async def test_explicit_send_submits_once_and_retains_native_path(self):
        self.native_message_manager.send = AsyncMock(return_value='/ril_0/message_1')
        path, sms = await self.sms()
        await sms.call_send()
        self.native_message_manager.send.assert_awaited_once_with(NUMBER, TEXT)
        self.assertEqual(self.messaging.messages[path].ofono_sms_path, '/ril_0/message_1')
        self.assertEqual(await sms.get_state(), 4) # Queued, not falsely delivered.
        with self.assertRaises(DBusError):
            await sms.call_send()
        self.assertEqual(self.native_message_manager.send.await_count, 1)

    async def test_send_failure_and_uncertain_reply_are_not_retried(self):
        self.native_message_manager.send = AsyncMock(side_effect=TimeoutError(TEXT + NUMBER))
        _, sms = await self.sms()
        for _ in range(2):
            with self.assertRaises(DBusError) as error:
                await sms.call_send()
            self.assertNotIn(TEXT, str(error.exception))
        self.assertEqual(self.native_message_manager.send.await_count, 1)

    async def test_concurrent_send_is_one_backend_request(self):
        entered, release = asyncio.Event(), asyncio.Event()
        async def send(*_):
            entered.set()
            await release.wait()
            return '/ril_0/message_1'
        self.native_message_manager.send = AsyncMock(side_effect=send)
        _, sms = await self.sms()
        first = asyncio.create_task(sms.call_send())
        await entered.wait()
        with self.assertRaises(DBusError):
            await sms.call_send()
        release.set()
        await first
        self.assertEqual(self.native_message_manager.send.await_count, 1)

    async def test_incoming_sms_metadata_logging_and_no_send(self):
        self.messaging.add_incoming_message(TEXT, {'Sender': Variant('s', NUMBER)})
        path = self.messaging.props['Messages'].value[0]
        sms = await self.proxy(path, 'org.freedesktop.ModemManager1.Sms')
        self.assertEqual(await sms.get_text(), TEXT)
        with self.assertRaises(DBusError):
            await sms.call_send()
        self.native_message_manager.send.assert_not_called()

    async def native_proxy(self, path, interface):
        node = await self.server.introspect('org.ofono', path)
        return self.server.get_proxy_object('org.ofono', path, node).get_interface(interface)

    async def use_native_sms(self):
        self.interfaces['org.ofono.MessageManager'] = await self.native_proxy('/ril_0', 'org.ofono.MessageManager')

    async def settle_native(self):
        # Two round trips let the private bus deliver emitted signals and handlers.
        await self.native_bus.introspect(self.server.unique_name, '/modem')
        await self.client_bus.introspect(self.server.unique_name, '/modem')

    async def test_native_pending_then_positive_sent_is_not_delivered(self):
        await self.use_native_sms()
        _, sms = await self.sms()
        await sms.call_send()
        self.assertEqual(await sms.get_state(), 4)
        self.assertEqual(await sms.get_delivery_state(), 0x100)
        self.native_message.set_state('sent')
        self.native_message_manager.MessageRemoved('/ril_0/message_1')
        self.native_bus.unexport('/ril_0/message_1')
        await self.settle_native()
        self.assertEqual(await sms.get_state(), 5)
        self.assertEqual(await sms.get_delivery_state(), 0x100)

    async def test_native_failed_cancelled_unknown_never_claim_sent(self):
        await self.use_native_sms()
        for state in ('failed', 'cancelled', 'unrecognized'):
            self.native_message.state = Variant('s', state)
            path, sms = await self.sms()
            await sms.call_send()
            self.assertEqual(await sms.get_state(), 0)
            self.assertEqual(await sms.get_delivery_state(), 0x100)
            await self.sms_manager.call_delete(path)

    async def test_native_failure_signal_then_removal_stays_unknown(self):
        await self.use_native_sms()
        _, sms = await self.sms()
        await sms.call_send()
        self.native_message.set_state('failed')
        self.native_message_manager.MessageRemoved('/ril_0/message_1')
        self.native_bus.unexport('/ril_0/message_1')
        await self.settle_native()
        self.assertEqual(await sms.get_state(), 0)
        self.assertEqual(await sms.get_delivery_state(), 0x100)

    async def test_terminal_state_before_send_reply_survives_removed_object(self):
        await self.use_native_sms()
        async def send(*_):
            self.native_message_manager.MessageAdded('/ril_0/message_1', {'State': Variant('s', 'pending')})
            self.native_message.set_state('sent')
            self.native_message_manager.MessageRemoved('/ril_0/message_1')
            self.native_bus.unexport('/ril_0/message_1')
            return '/ril_0/message_1'
        self.native_message_manager.send.side_effect = send
        _, sms = await self.sms()
        await sms.call_send()
        self.assertEqual(await sms.get_state(), 5)

    async def test_removed_without_positive_state_stays_unknown(self):
        await self.use_native_sms()
        self.native_bus.unexport('/ril_0/message_1')
        _, sms = await self.sms()
        await sms.call_send()
        self.assertEqual(await sms.get_state(), 0)

    async def test_removal_after_pending_does_not_infer_sent(self):
        await self.use_native_sms()
        _, sms = await self.sms()
        await sms.call_send()
        self.native_message_manager.MessageRemoved('/ril_0/message_1')
        await self.settle_native()
        self.assertEqual(await sms.get_state(), 0)

    async def test_stale_initial_properties_cannot_overwrite_terminal_signal(self):
        await self.use_native_sms()
        async def query():
            self.native_message.set_state('sent')
            return {'State': Variant('s', 'pending')}
        self.native_message.query.side_effect = query
        _, sms = await self.sms()
        await sms.call_send()
        self.assertEqual(await sms.get_state(), 5)

    async def test_lost_reply_with_uncorrelated_sent_signal_is_unknown(self):
        await self.use_native_sms()
        async def send(*_):
            self.native_message_manager.MessageAdded('/ril_0/message_1', {'State': Variant('s', 'pending')})
            self.native_message.set_state('sent')
            raise DBusError('org.ofono.Error.Failed', 'Synthetic uncertain reply')
        self.native_message_manager.send.side_effect = send
        path, sms = await self.sms()
        with self.assertRaises(DBusError):
            await sms.call_send()
        self.assertEqual(await sms.get_state(), 0)
        self.assertFalse(self.messaging.messages[path]._watching)

    async def test_unrelated_announced_message_state_is_discarded(self):
        await self.use_native_sms()
        other = NativeMessage()
        self.native_bus.export('/ril_0/message_other', other)
        async def send(*_):
            self.native_message_manager.MessageAdded('/ril_0/message_other', {'State': Variant('s', 'pending')})
            other.set_state('sent')
            return '/ril_0/message_1'
        self.native_message_manager.send.side_effect = send
        path, sms = await self.sms()
        await sms.call_send()
        self.assertEqual(await sms.get_state(), 4)
        other.query.assert_not_called()
        self.assertEqual(self.messaging.messages[path]._early_states, {})

    async def test_delete_during_initial_state_query_disconnects_observer(self):
        await self.use_native_sms()
        entered, release = asyncio.Event(), asyncio.Event()
        async def query():
            entered.set()
            await release.wait()
            return {'State': Variant('s', 'sent')}
        self.native_message.query.side_effect = query
        path, sms = await self.sms()
        draft = self.messaging.messages[path]
        pending = asyncio.create_task(sms.call_send())
        await asyncio.wait_for(entered.wait(), 2)
        await self.sms_manager.call_delete(path)
        self.assertFalse(draft._watching)
        self.assertEqual(draft._match_rules, [])
        release.set()
        await pending
        self.native_message.set_state('sent')
        await self.settle_native()
        self.assertNotEqual(draft.props['State'].value, 5)
        self.native_message_manager.send.assert_awaited_once()

    async def test_delete_during_send_reply_does_not_query_or_reconnect(self):
        await self.use_native_sms()
        entered, release = asyncio.Event(), asyncio.Event()
        async def send(*_):
            entered.set()
            await release.wait()
            return '/ril_0/message_1'
        self.native_message_manager.send.side_effect = send
        path, sms = await self.sms()
        draft = self.messaging.messages[path]
        pending = asyncio.create_task(sms.call_send())
        await asyncio.wait_for(entered.wait(), 2)
        await self.sms_manager.call_delete(path)
        release.set()
        await pending
        self.assertFalse(draft._watching)
        self.assertEqual(draft._match_rules, [])
        self.native_message.query.assert_not_called()
        self.native_message_manager.send.assert_awaited_once()

    async def test_malformed_create_variants_never_reach_backend(self):
        for props in ({'number': Variant('u', 1), 'text': Variant('s', TEXT)},
                      {'number': Variant('s', NUMBER), 'text': Variant('b', True)},
                      {'number': Variant('s', NUMBER)},
                      {'number': Variant('s', NUMBER), 'text': Variant('s', TEXT), 'delivery-report-request': Variant('s', 'true')}):
            with self.assertRaises(DBusError):
                await self.sms_manager.call_create(props)
        with self.assertRaises(DBusError):
            await self.voice_manager.call_create_call({'number': Variant('u', 1)})
        self.assertEqual(self.messaging.messages, {})
        self.assertEqual(self.voice.calls, {})
        self.native_message_manager.send.assert_not_called()
        self.backend.call_dial.assert_not_called()

    async def test_delete_call_handles_removed_signal_before_hangup_reply(self):
        native_manager = NativeVoiceManager()
        removed = asyncio.Event()
        native_call = NativeCall(native_manager, removed)
        self.native_bus.export('/ril_0', native_manager)
        self.native_bus.export('/native_returned', native_call)
        manager_proxy = await self.native_proxy('/ril_0', native_manager.name)
        call_proxy = await self.native_proxy('/native_returned', native_call.name)
        self.ofono_client['ofono_modem']['/native_returned']['org.ofono.VoiceCall'] = call_proxy
        def on_removed(path):
            self.voice.remove_call(path)
            removed.set()
        manager_proxy.on_call_removed(on_removed)
        self.backend.call_dial = AsyncMock(return_value='/native_returned')
        path, call = await self.call()
        await call.call_start()
        self.voice.add_call('/unrelated', {'State': Variant('s', 'incoming')})
        other = self.voice.call_path_map['/unrelated']
        deleted = []
        self.voice_manager.on_call_deleted(deleted.append)
        await self.voice_manager.call_delete_call(path)
        await self.settle_native()
        self.assertEqual(native_call.hangups, 1)
        self.assertNotIn(path, self.voice.calls)
        self.assertIn(other, self.voice.calls)
        self.assertEqual(deleted.count(path), 1)
        self.backend.call_hangup_all.assert_not_called()
        manager_proxy.off_call_removed(on_removed)

    def delayed_bus(self, call):
        # Keep real private-bus I/O; delay only reply delivery to this SMS.
        return SimpleNamespace(call=call, add_message_handler=self.server.add_message_handler,
                               remove_message_handler=self.server.remove_message_handler)

    async def broker_matches(self):
        reply = await self.client_bus.call(Message(destination='org.freedesktop.DBus',
            path='/org/freedesktop/DBus', interface='org.freedesktop.DBus.Debug.Stats',
            member='GetConnectionStats', signature='s', body=[self.server.unique_name]))
        self.assertEqual(reply.message_type, MessageType.METHOD_RETURN)
        return reply.body[0]['MatchRules'].value

    async def assert_observer_closed(self, draft, baseline_matches, baseline_handlers):
        self.assertIsNotNone(draft._cleanup_task)
        await asyncio.wait_for(asyncio.shield(draft._cleanup_task), 2)
        self.assertFalse(draft._watching)
        self.assertEqual(draft._match_rules, [])
        self.assertNotIn(draft._native_message_signal, self.server._user_message_handlers)
        self.assertEqual(len(self.server._user_message_handlers), baseline_handlers)
        self.assertEqual(await self.broker_matches(), baseline_matches)

    async def observer_baseline(self):
        await self.use_native_sms()
        await self.settle_native()
        return await self.broker_matches(), len(self.server._user_message_handlers)

    async def test_many_completed_sms_without_delete_leave_no_observer_resources(self):
        baseline = await self.observer_baseline()
        for index in range(48):
            self.native_message.state = Variant('s', 'pending')
            path, sms = await self.sms()
            await sms.call_send()
            self.assertEqual(await self.broker_matches(), baseline[0] + 4)
            terminal = ('sent', 'failed', 'cancelled')[index % 3]
            self.native_message.set_state(terminal)
            await self.settle_native()
            draft = self.messaging.messages[path]
            await self.assert_observer_closed(draft, *baseline)
            self.assertEqual(await sms.get_state(), 5 if terminal == 'sent' else 0)
            self.assertEqual(draft.ofono_sms_path, '/ril_0/message_1')
        self.assertEqual(len(self.messaging.messages), 48) # No local Delete at all.
        print(f'RESOURCE CHECK: 48 completed SMS retained; broker rules {baseline[0]} -> {await self.broker_matches()}, handlers {baseline[1]} -> {len(self.server._user_message_handlers)}; owned rules=0, owned handlers=0', file=sys.stderr)

    async def test_owner_loss_closes_every_pending_observer_unknown(self):
        baseline = await self.observer_baseline()
        pending = []
        for _ in range(3):
            path, sms = await self.sms()
            await sms.call_send()
            pending.append((self.messaging.messages[path], sms))
        await self.native_bus.release_name('org.ofono')
        await self.settle_native()
        for draft, sms in pending:
            await self.assert_observer_closed(draft, *baseline)
            self.assertEqual(await sms.get_state(), 0)
            self.assertTrue(draft._owner_lost)
            with self.assertRaises(DBusError):
                await sms.call_send()
        self.assertEqual(self.native_message_manager.send.await_count, 3)

    async def test_native_connection_disappearance_detaches_pending_observer(self):
        baseline = await self.observer_baseline()
        path, sms = await self.sms()
        await sms.call_send()
        self.native_bus.disconnect()
        await self.native_bus.wait_for_disconnect()
        await self.client_bus.introspect(self.server.unique_name, '/modem')
        await self.assert_observer_closed(self.messaging.messages[path], *baseline)
        self.assertEqual(await sms.get_state(), 0)
        self.native_message_manager.send.assert_awaited_once()

    async def test_replacement_owner_is_not_followed(self):
        baseline = await self.observer_baseline()
        await self.native_bus.request_name('org.ofono', NameFlag.ALLOW_REPLACEMENT)
        path, sms = await self.sms()
        await sms.call_send()
        await self.client_bus.request_name('org.ofono', NameFlag.REPLACE_EXISTING)
        await self.settle_native()
        draft = self.messaging.messages[path]
        await self.assert_observer_closed(draft, *baseline)
        self.assertEqual(await sms.get_state(), 0)
        self.assertEqual(draft._native_owner, self.native_bus.unique_name)
        self.native_message_manager.send.assert_awaited_once()
        await self.client_bus.release_name('org.ofono')

    async def test_fast_terminal_reply_detaches_without_properties_query(self):
        baseline = await self.observer_baseline()
        async def send(*_):
            self.native_message_manager.MessageAdded('/ril_0/message_1', {'State': Variant('s', 'pending')})
            self.native_message.set_state('sent')
            return '/ril_0/message_1'
        self.native_message_manager.send.side_effect = send
        path, sms = await self.sms()
        await sms.call_send()
        await self.assert_observer_closed(self.messaging.messages[path], *baseline)
        self.native_message.query.assert_not_called()
        self.assertEqual(await sms.get_state(), 5)

    async def test_missing_object_and_removed_unknown_detach(self):
        baseline = await self.observer_baseline()
        self.native_bus.unexport('/ril_0/message_1')
        path, sms = await self.sms()
        await sms.call_send()
        await self.assert_observer_closed(self.messaging.messages[path], *baseline)
        self.assertEqual(await sms.get_state(), 0)
        self.native_bus.export('/ril_0/message_1', self.native_message)
        path, sms = await self.sms()
        await sms.call_send()
        self.native_message_manager.MessageRemoved('/ril_0/message_1')
        await self.settle_native()
        await self.assert_observer_closed(self.messaging.messages[path], *baseline)
        self.assertEqual(await sms.get_state(), 0)

    async def test_owner_loss_during_addmatch_prevents_send_and_leaves_no_rules(self):
        baseline = await self.observer_baseline()
        call = self.server.call
        changed = False
        async def lose_owner(message):
            nonlocal changed
            reply = await call(message)
            if message.member == 'AddMatch' and not changed:
                changed = True
                await self.native_bus.release_name('org.ofono')
            return reply
        path, sms = await self.sms()
        with patch.object(self.messaging.messages[path], 'bus', self.delayed_bus(lose_owner)):
            with self.assertRaises(DBusError):
                await sms.call_send()
        await self.assert_observer_closed(self.messaging.messages[path], *baseline)
        self.assertEqual(await sms.get_state(), 0)
        self.native_message_manager.send.assert_not_called()

    async def test_owner_replaced_before_owner_match_is_caught_by_revalidation(self):
        baseline = await self.observer_baseline()
        replacement = NativeMessageManager()
        self.client_bus.export('/ril_0', replacement)
        call = self.server.call
        changed = False
        async def replace_before_watch(message):
            nonlocal changed
            reply = await call(message)
            if message.member == 'GetNameOwner' and not changed:
                changed = True
                await self.native_bus.release_name('org.ofono')
                await self.client_bus.request_name('org.ofono')
            return reply
        path, sms = await self.sms()
        draft = self.messaging.messages[path]
        with patch.object(draft, 'bus', self.delayed_bus(replace_before_watch)):
            with self.assertRaises(DBusError):
                await sms.call_send()
        await self.assert_observer_closed(draft, *baseline)
        self.assertEqual(await sms.get_state(), 0)
        self.native_message_manager.send.assert_not_called()
        replacement.send.assert_not_called()
        await self.client_bus.release_name('org.ofono')

    async def test_delete_during_addmatch_waits_for_install_then_removes_it(self):
        baseline = await self.observer_baseline()
        call = self.server.call
        installed, release = asyncio.Event(), asyncio.Event()
        async def pause_add(message):
            reply = await call(message)
            if message.member == 'AddMatch':
                installed.set()
                await release.wait()
            return reply
        path, sms = await self.sms()
        draft = self.messaging.messages[path]
        with patch.object(draft, 'bus', self.delayed_bus(pause_add)):
            sending = asyncio.create_task(sms.call_send())
            await asyncio.wait_for(installed.wait(), 2)
            deleting = asyncio.create_task(self.sms_manager.call_delete(path))
            await self.settle_native()
            self.assertTrue(draft.deleted)
            release.set()
            await asyncio.gather(sending, deleting)
        await self.assert_observer_closed(draft, *baseline)
        self.native_message_manager.send.assert_not_called()

    async def test_delete_is_idempotent_during_terminal_cleanup(self):
        baseline = await self.observer_baseline()
        path, sms = await self.sms()
        await sms.call_send()
        draft = self.messaging.messages[path]
        call = self.server.call
        removing, release = asyncio.Event(), asyncio.Event()
        async def pause_remove(message):
            if message.member == 'RemoveMatch':
                removing.set()
                await release.wait()
            return await call(message)
        with patch.object(draft, 'bus', self.delayed_bus(pause_remove)):
            self.native_message.set_state('sent')
            await asyncio.wait_for(removing.wait(), 2)
            first = asyncio.create_task(self.sms_manager.call_delete(path))
            await self.settle_native()
            await self.sms_manager.call_delete(path)
            release.set()
            await first
        await self.assert_observer_closed(draft, *baseline)
        self.assertEqual(draft.props['State'].value, 5)

    async def test_owner_loss_after_dispatch_does_not_unsend_or_retry(self):
        baseline = await self.observer_baseline()
        entered, release = asyncio.Event(), asyncio.Event()
        async def send(*_):
            entered.set()
            await release.wait()
            return '/ril_0/message_1'
        self.native_message_manager.send.side_effect = send
        path, sms = await self.sms()
        sending = asyncio.create_task(sms.call_send())
        await asyncio.wait_for(entered.wait(), 2)
        await self.native_bus.release_name('org.ofono')
        await self.settle_native()
        release.set()
        await sending
        await self.assert_observer_closed(self.messaging.messages[path], *baseline)
        self.assertEqual(await sms.get_state(), 0)
        self.native_message.query.assert_not_called()
        with self.assertRaises(DBusError):
            await sms.call_send()
        self.native_message_manager.send.assert_awaited_once()

    async def test_forged_owner_signal_cannot_end_pending_observation(self):
        baseline = await self.observer_baseline()
        path, sms = await self.sms()
        await sms.call_send()
        # A directly addressed signal bypasses broker match filtering but cannot
        # forge the daemon's sender name; the handler must authenticate it too.
        await self.native_bus.send(Message(message_type=MessageType.SIGNAL,
            destination=self.server.unique_name, path='/org/freedesktop/DBus',
            interface='org.freedesktop.DBus', member='NameOwnerChanged',
            signature='sss', body=['org.ofono', self.native_bus.unique_name, '']))
        await self.settle_native()
        draft = self.messaging.messages[path]
        self.assertFalse(draft._owner_lost)
        self.assertEqual(await sms.get_state(), 4)
        self.assertEqual(await self.broker_matches(), baseline[0] + 4)
        await self.sms_manager.call_delete(path)
        await self.assert_observer_closed(draft, *baseline)

    async def test_create_call_and_delete_unsent_do_not_dial_or_hangup(self):
        path, _ = await self.call()
        draft = self.voice.calls[path]
        self.backend.call_dial.assert_not_called()
        await self.voice_manager.call_delete_call(path)
        self.assertTrue(draft.deleted)
        self.backend.call_hangup_all.assert_not_called()

    async def test_start_uses_returned_path_and_late_signal_does_not_duplicate(self):
        self.backend.call_dial = AsyncMock(return_value='/native_returned')
        path, call = await self.call()
        await call.call_start()
        self.assertEqual(self.voice.call_path_map, {'/native_returned': path})
        self.assertEqual(await call.get_state(), 1)
        self.voice.add_call('/native_returned', {'State': Variant('s', 'alerting'), 'LineIdentification': Variant('s', NUMBER)})
        self.assertEqual(await call.get_state(), 2)
        self.assertEqual(len(self.voice.calls), 1)
        with self.assertRaises(DBusError):
            await call.call_start()
        self.backend.call_dial.assert_awaited_once_with(NUMBER, '')

    async def test_signal_before_dial_reply_correlates_exact_path_not_unrelated(self):
        async def dial(*_):
            for path in ('/unrelated', '/native_returned'):
                self.voice.add_call(path, {'State': Variant('s', 'alerting'), 'LineIdentification': Variant('s', NUMBER)})
            return '/native_returned'
        self.backend.call_dial = AsyncMock(side_effect=dial)
        path, call = await self.call()
        await call.call_start()
        self.assertEqual(self.voice.call_path_map['/native_returned'], path)
        self.assertNotEqual(self.voice.call_path_map['/unrelated'], path)
        self.assertEqual(len(self.voice.calls), 2)
        self.assertEqual(await call.get_state(), 2)

    async def test_dial_failure_is_not_retried(self):
        self.backend.call_dial = AsyncMock(side_effect=TimeoutError(NUMBER))
        _, call = await self.call()
        for _ in range(2):
            with self.assertRaises(DBusError):
                await call.call_start()
        self.assertEqual(self.backend.call_dial.await_count, 1)

    async def test_uncertain_dial_with_signal_retires_draft_without_duplicate(self):
        async def dial(*_):
            self.voice.add_call('/native_returned', {'State': Variant('s', 'dialing'), 'LineIdentification': Variant('s', NUMBER)})
            raise TimeoutError(NUMBER)
        self.backend.call_dial = AsyncMock(side_effect=dial)
        path, call = await self.call()
        with self.assertRaises(DBusError):
            await call.call_start()
        self.assertNotIn(path, self.voice.calls)
        self.assertEqual(len(self.voice.calls), 1)
        self.assertNotEqual(self.voice.call_path_map['/native_returned'], path)
        with self.assertRaises(DBusError):
            await call.call_start()
        self.assertEqual(self.backend.call_dial.await_count, 1)

    async def test_concurrent_start_is_one_dial(self):
        entered, release = asyncio.Event(), asyncio.Event()
        async def dial(*_):
            entered.set()
            await release.wait()
            return '/native_returned'
        self.backend.call_dial = AsyncMock(side_effect=dial)
        _, call = await self.call()
        first = asyncio.create_task(call.call_start())
        await entered.wait()
        with self.assertRaises(DBusError):
            await call.call_start()
        release.set()
        await first
        self.assertEqual(self.backend.call_dial.await_count, 1)

    async def test_call_removed_before_reply_is_not_resurrected(self):
        async def dial(*_):
            self.voice.add_call('/native_returned', {'State': Variant('s', 'dialing')})
            self.voice.remove_call('/native_returned')
            return '/native_returned'
        self.backend.call_dial = AsyncMock(side_effect=dial)
        _, call = await self.call()
        await call.call_start()
        self.assertEqual(await call.get_state(), 7)
        self.assertEqual(self.voice.call_path_map, {})
        await self.voice_manager.call_delete_call(self.voice.props['Calls'].value[0])
        self.backend.call_hangup_all.assert_not_called()

    async def test_call_number_and_dtmf_never_logged(self):
        self.voice.add_call('/incoming', {'State': Variant('s', 'incoming'), 'LineIdentification': Variant('s', NUMBER)})
        path = self.voice.call_path_map['/incoming']
        call = self.voice.calls[path]
        call.property_changed('LineIdentification', Variant('s', NUMBER))
        call.DtmfReceived(DTMF)
        proxy = await self.proxy(path, 'org.freedesktop.ModemManager1.Call')
        await proxy.call_send_dtmf(DTMF)
        self.backend.call_send_tones.assert_awaited_once_with(DTMF)

    def modem(self, powered=False, online=False):
        names = ('Modem', 'SimManager', 'ConnectionManager', 'CellBroadcast', 'FuriLabs.AT')
        interfaces = {f'org.ofono.{name}': Backend() for name in names}
        interfaces['org.ofono.Modem'].call_get_properties.return_value = {'Powered': Variant('b', powered), 'Online': Variant('b', online)}
        interfaces['org.ofono.SimManager'].call_get_properties.return_value = {'Present': Variant('b', True), 'PinRequired': Variant('s', 'none')}
        client = {'ofono_modem': {'/ril_0': interfaces}}
        loop = RecordingLoop()
        modem = MMModemInterface(loop, 0, FakeBus(), client, '/ril_0', verbose=True, diagnostic=True)
        for name, backend in interfaces.items():
            modem.ofono_interface_props[name].props = backend.call_get_properties.return_value
            modem.ofono_interfaces[name] = backend
        return modem, loop, interfaces, client

    async def test_diagnostic_discovery_and_constructors_do_not_mutate_backend(self):
        modem, loop, interfaces, client = self.modem()
        manager_backend = Backend()
        manager_backend.call_get_modems = AsyncMock(return_value=[['/ril_0', {'Powered': Variant('b', False), 'Online': Variant('b', False)}]])
        with patch.object(main, 'Ofono', return_value=client), patch.object(main, 'DBus', return_value={}):
            manager = main.MMInterface(loop, FakeBus(), verbose=True, diagnostic=True)
        manager.ofono_manager_interface = manager_backend
        await manager.find_ofono_modems()
        self.assertIn('/ril_0', manager.modems)
        self.assertNotIn('simple_set_apn', loop.tasks)
        for backend in interfaces.values():
            backend.call_set_property.assert_not_called()
            backend.call_send_message.assert_not_called()
            backend.call_dial.assert_not_called()
            backend.call_send_command.assert_not_called()

    async def test_diagnostic_state_updates_reflect_backend_without_enabling_it(self):
        modem, _, interfaces, _ = self.modem(powered=True, online=False)
        await modem.set_props()
        self.assertEqual(modem.props['PowerState'].value, 3)
        self.assertEqual(modem.props['State'].value, 3)
        modem.ofono_interface_props['org.ofono.Modem'].props['Online'] = Variant('b', True)
        await modem.set_props()
        self.assertEqual(modem.props['State'].value, 6)
        self.server.export('/diagnostic', modem)
        proxy = await self.proxy('/diagnostic', modem.name)
        self.assertEqual(await proxy.get_power_state(), 3)
        self.assertEqual(await proxy.get_state(), 6)
        await modem._restore_saved_bands()
        self.assertEqual(await modem.SupportedBands, modem.props['SupportedBands'].value)
        for backend in interfaces.values():
            backend.call_set_property.assert_not_called()
            backend.call_send_command.assert_not_called()

    async def test_diagnostic_nm_and_reconnect_and_cell_broadcast_are_passive(self):
        modem, _, interfaces, client = self.modem()
        simple = MMModemSimpleInterface(modem, '/ril_0', interfaces, modem.ofono_interface_props)
        with patch('ofono2mm.mm_modem_simple.SystemBus', side_effect=AssertionError('No NM bus')):
            self.assertTrue(await simple.network_manager_set_apn(force=True))
        bearer = MMBearerInterface(client, '/ril_0', interfaces, modem)
        bearer.props['Connected'] = Variant('b', True)
        with patch('asyncio.create_task', side_effect=AssertionError('No reconnect scheduling')):
            bearer.ofono_context_changed('Active', Variant('b', False))
        cbs = MMModemCellBroadcastInterface(FakeBus(), '/ril_0', interfaces, diagnostic=True)
        await cbs.init_cbs()
        interfaces['org.ofono.CellBroadcast'].call_set_property.assert_not_called()

    async def test_diagnostic_async_initialization_is_passive(self):
        modem, _, interfaces, client = self.modem(powered=True, online=False)
        for name in modem.used_interfaces:
            if name not in interfaces:
                interfaces[name] = Backend()
        await modem.init_ofono_interfaces()
        await modem.check_ofono_contexts()
        for backend in interfaces.values():
            backend.call_set_property.assert_not_called()
            backend.call_send_command.assert_not_called()
            backend.call_send_message.assert_not_called()
            backend.call_dial.assert_not_called()

    async def test_diagnostic_create_bearer_does_not_activate_existing_context(self):
        modem, _, interfaces, client = self.modem(powered=True, online=False)
        context = Backend()
        context.call_get_properties.return_value = {'Active': Variant('b', False), 'Protocol': Variant('s', 'ip')}
        client['ofono_context'] = {'/context': {'org.ofono.ConnectionContext': context}}
        interfaces['org.ofono.ConnectionManager'].call_get_contexts.return_value = [['/context', {'Type': Variant('s', 'internet')}]]
        path = await modem.doCreateBearer({'apn': Variant('s', 'synthetic.invalid')})
        self.assertEqual(modem.bearers[path].ofono_ctx, '/context')
        context.call_set_property.assert_not_called()

    async def test_diagnostic_explicit_enable_is_a_distinct_mutator(self):
        modem, _, interfaces, _ = self.modem()
        backend = interfaces['org.ofono.Modem']
        backend.call_set_property = AsyncMock()
        enable = next(m.fn for m in ServiceInterface._get_methods(modem) if m.name == 'Enable')
        await enable(modem, True)
        self.assertEqual([c.args[0] for c in backend.call_set_property.await_args_list], ['Powered', 'Online'])


if __name__ == '__main__':
    # Start only after imports succeed; always reap the test daemon on exit.
    daemon = subprocess.Popen(DAEMON_ARGS, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        assert daemon.stdout.readline().strip().startswith(ADDRESS)
        unittest.main(verbosity=2)
    finally:
        daemon.terminate()
        daemon.communicate(timeout=3)
        sandbox.cleanup()
