"""STREAM acknowledgement matching without Bluetooth hardware."""
import asyncio
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from cfw.image import FirmwareError
from g1flash import CFW_STREAM, StreamSender


class StreamTests(unittest.IsolatedAsyncioTestCase):
    def sender(self, replies, seq=0):
        queue = asyncio.Queue()
        for status, packet_seq, done in replies:
            queue.put_nowait(bytes([0xF0, CFW_STREAM, status, packet_seq, done]))
        writes = []

        async def write(_rx, packet, response):
            writes.append(packet)

        link = SimpleNamespace(replies=queue, rx=object(),
                               client=SimpleNamespace(write_gatt_char=write))
        sender = StreamSender(link)
        sender.seq = seq
        return sender, writes

    async def test_stale_success_and_error_do_not_finish_new_message(self):
        sender, _ = self.sender([(0, 6, 99), (6, 6, 0), (0, 7, 2)], seq=7)
        self.assertEqual(await sender.send(b'abc'), 2)

    async def test_only_final_packet_acknowledges_execution_across_wrap(self):
        sender, writes = self.sender([(0, 254, 99), (4, 255, 99), (0, 0, 3)], seq=254)
        self.assertEqual(await sender.send(bytes(600)), 3)
        self.assertEqual([p[2] for p in writes], [254, 255, 0])
        self.assertFalse(sender.deferred)

    async def test_error_on_earlier_packet_is_not_ignored(self):
        sender, _ = self.sender([(5, 255, 0)], seq=254)
        with self.assertRaisesRegex(FirmwareError, 'bad-seq at seq 255'):
            await sender.send(bytes(600))

    async def test_final_deferred_ack_is_accepted(self):
        sender, _ = self.sender([(4, 0, 2)])
        self.assertEqual(await sender.send(b'abc'), 2)
        self.assertTrue(sender.deferred)

    async def test_stale_traffic_does_not_restart_timeout(self):
        sender, _ = self.sender([(0, 255, 1)] * 5)
        # Deterministic elapsed time while the queue contains stale replies.
        loop = asyncio.get_running_loop()
        with patch.object(loop, 'time', side_effect=[10, 10.25, 10.75, 11.25]), \
                patch('g1flash.asyncio.wait_for') as wait:
            async def receive(pending, timeout):
                return await pending
            wait.side_effect = receive
            with self.assertRaises(TimeoutError):
                await sender.send(b'abc', timeout=1)
            self.assertEqual([c.args[1] for c in wait.call_args_list], [0.75, 0.25])
