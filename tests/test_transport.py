"""Exercise real live PCM framing and connection lifecycle without a speaker."""
import asyncio
import contextlib
import io
import struct
import sys
import unittest
from pathlib import Path
from unittest.mock import AsyncMock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "transport"))
from pcm_input import PcmInput
from raop_session import send_live
from host import Host, clean


class LiveSourceTests(unittest.IsolatedAsyncioTestCase):
    async def test_invalid_token_cannot_open_a_connection(self):
        with patch("pcm_input.asyncio.open_connection", new=AsyncMock()) as connect:
            for token in ("short", "a" * 63 + "\n", "z" * 64):
                with self.assertRaises(ValueError):
                    await PcmInput.attach(12345, token)
            connect.assert_not_called()

    async def test_truncated_stereo_frame_is_rejected(self):
        async def serve(reader, writer):
            await reader.readline()
            writer.write(b"\x01\x02\x03")
            await writer.drain()
            writer.close()
            await writer.wait_closed()

        server = await asyncio.start_server(serve, "127.0.0.1", 0)
        async with server:
            source = await PcmInput.attach(server.sockets[0].getsockname()[1], "a" * 64)
            try:
                with self.assertRaises(ValueError):
                    await source.readframes(1)
                self.assertFalse(source.started.is_set())
            finally:
                await source.close()

    async def test_fragmented_pcm_and_authentication(self):
        authenticated = asyncio.Event()
        pcm = struct.pack("<hhhh", 100, -100, 200, -200)

        async def serve(reader, writer):
            self.assertEqual(await reader.readline(), b"a" * 64 + b"\n")
            authenticated.set()
            for chunk in (pcm[:1], pcm[1:5], pcm[5:]):
                writer.write(chunk)
                await writer.drain()
                await asyncio.sleep(0)
            writer.close()
            await writer.wait_closed()

        server = await asyncio.start_server(serve, "127.0.0.1", 0)
        async with server:
            source = await PcmInput.attach(server.sockets[0].getsockname()[1], "a" * 64)
            await asyncio.wait_for(authenticated.wait(), 1)
            self.assertFalse(source.started.is_set())
            self.assertEqual(await source.readframes(2), struct.pack(">hhhh", 100, -100, 200, -200))
            self.assertTrue(source.started.is_set())
            self.assertEqual(await source.readframes(2), b"")
            await source.close()

    async def test_cancel_pending_read(self):
        connection_closed = asyncio.Event()

        async def serve(reader, writer):
            await reader.readline()
            await reader.read()
            writer.close()
            await writer.wait_closed()
            connection_closed.set()

        server = await asyncio.start_server(serve, "127.0.0.1", 0)
        async with server:
            source = await PcmInput.attach(server.sockets[0].getsockname()[1], "a" * 64)
            task = asyncio.create_task(source.readframes(352))
            await asyncio.sleep(0.02)
            task.cancel()
            with self.assertRaises(asyncio.CancelledError):
                await task
            await source.close()
            await asyncio.wait_for(connection_closed.wait(), 1)

    async def test_stop_cancels_reconnect(self):
        host = Host()
        with patch("host.pyatv.connect", new=AsyncMock(side_effect=OSError)), patch("host.emit") as events, patch("host.logging.exception"):
            host.task = asyncio.create_task(host.stream(object(), 12345, "secret"))
            await asyncio.sleep(0.05)
            await host.stop()
            self.assertIsNone(host.task)
            self.assertIsNone(host.atv)
            self.assertIn(unittest.mock.call("STATE", "reconnecting"), events.call_args_list)

    async def test_streaming_only_after_receiver_ready(self):
        host = Host()
        fake_atv = unittest.mock.Mock()
        fake_atv.close.return_value = set()
        fake_atv.audio.set_volume = AsyncMock()
        ready_gate = asyncio.Event()

        async def playback(device, service, source, volume):
            await ready_gate.wait()
            source.started.set()
            await asyncio.Event().wait()

        fake_source = unittest.mock.Mock()
        fake_source.started = asyncio.Event()
        fake_source.close = AsyncMock()
        with patch("host.pyatv.connect", new=AsyncMock(return_value=fake_atv)), \
             patch("host.PcmInput.attach", new=AsyncMock(return_value=fake_source)), \
             patch("host.send_live", side_effect=playback), \
             patch("host.emit") as events:
            host.task = asyncio.create_task(host.stream(unittest.mock.Mock(), 12345, "a" * 64))
            await asyncio.sleep(0.02)
            self.assertNotIn(unittest.mock.call("STATE", "streaming"), events.call_args_list)
            ready_gate.set()
            await asyncio.sleep(0.02)
            self.assertIn(unittest.mock.call("STATE", "streaming"), events.call_args_list)
            await host.stop()
            fake_source.close.assert_awaited_once()
            fake_atv.close.assert_called_once()

    async def test_native_gate_waits_for_volume_acknowledgement(self):
        host = Host()
        host.volume = 0
        volume_requested = asyncio.Event()
        volume_ack = asyncio.Event()
        streaming = asyncio.Event()
        fake_atv = unittest.mock.Mock()
        fake_atv.close.return_value = set()
        fake_source = unittest.mock.Mock()
        fake_source.started = asyncio.Event()
        fake_source.close = AsyncMock()

        async def volume(level):
            volume_requested.set()
            await volume_ack.wait()

        async def playback(*args):
            fake_source.started.set()
            await asyncio.Event().wait()

        def event(*fields):
            if fields == ("STATE", "streaming"):
                streaming.set()

        fake_atv.audio.set_volume = AsyncMock(side_effect=volume)
        with patch("host.pyatv.connect", new=AsyncMock(return_value=fake_atv)), \
             patch("host.PcmInput.attach", new=AsyncMock(return_value=fake_source)), \
             patch("host.send_live", side_effect=playback), patch("host.emit", side_effect=event):
            host.task = asyncio.create_task(host.stream(unittest.mock.Mock(), 12345, "a" * 64))
            try:
                await asyncio.wait_for(volume_requested.wait(), 1)
                self.assertFalse(host.ready)
                self.assertFalse(streaming.is_set())
                host.volume = 37  # Slider moved while the first command was pending.
                volume_ack.set()
                await asyncio.wait_for(streaming.wait(), 1)
                self.assertEqual(fake_atv.audio.set_volume.await_args_list,
                                 [unittest.mock.call(0.0), unittest.mock.call(37.0)])
            finally:
                await host.stop()


class ProtocolTests(unittest.TestCase):
    def test_device_names_cannot_inject_protocol_messages(self):
        self.assertEqual(clean("Room\nSTATE\tstreaming\r"), "Room STATE streaming ")
        self.assertEqual(len(clean("x"*1000)), 200)


if __name__ == "__main__":
    unittest.main()
