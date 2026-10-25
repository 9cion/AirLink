"""Loopback RTSP/RTP integration against the installed, unmodified pyatv.

This receiver checks the wire, not mocked playback events. It models an open
AirPlay 1 receiver; encrypted AirPlay 2 hardware still needs a separate test.
"""
import asyncio
import contextlib
from ipaddress import IPv4Address
import plistlib
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "transport"))
import pyatv
from pyatv.conf import AppleTV, ManualService
from pyatv.const import Protocol
from pcm_input import PcmInput
from raop_session import send_live


class PacketInbox(asyncio.DatagramProtocol):
    def __init__(self):
        self.packets = asyncio.Queue()

    def datagram_received(self, data, address):
        self.packets.put_nowait(data)


class RaopWireTests(unittest.IsolatedAsyncioTestCase):
    async def test_audio_and_volume_after_record_without_library_patches(self):
        await self.check_wire(20)

    async def test_mute_does_not_fall_back_to_receiver_volume(self):
        await self.check_wire(0)

    async def check_wire(self, initial_volume):
        loop = asyncio.get_running_loop()
        receiver_ports = []
        udp = []
        inboxes = []
        for _ in range(3):
            transport, inbox = await loop.create_datagram_endpoint(
                PacketInbox, local_addr=("127.0.0.1", 0)
            )
            udp.append(transport)
            inboxes.append(inbox)
            receiver_ports.append(transport.get_extra_info("sockname")[1])
        requests = []
        teardown = asyncio.Event()
        pcm_closed = asyncio.Event()
        sample = struct.pack("<hh", 1234, -4321)

        async def rtsp(reader, writer):
            try:
                while header := await reader.readuntil(b"\r\n\r\n"):
                    lines = header.decode().split("\r\n")
                    method, uri, protocol = lines[0].split()
                    headers = dict(line.split(": ", 1) for line in lines[1:] if ": " in line)
                    body = await reader.readexactly(int(headers.get("Content-Length", "0")))
                    requests.append((method, body))
                    payload = plistlib.dumps({}) if uri == "/info" else b""
                    response = [f"{protocol} 200 OK", f"CSeq: {headers['CSeq']}",
                                f"Content-Length: {len(payload)}"]
                    if method == "SETUP":
                        response += ["Session: 42", "Transport: RTP/AVP/UDP;unicast;"
                                     f"server_port={receiver_ports[0]};control_port={receiver_ports[1]};"
                                     f"timing_port={receiver_ports[2]}"]
                    writer.write(("\r\n".join(response) + "\r\n\r\n").encode() + payload)
                    await writer.drain()
                    if method == "TEARDOWN":
                        teardown.set()
            except (asyncio.IncompleteReadError, ConnectionError):
                pass
            finally:
                writer.close()
                await writer.wait_closed()

        async def native_pcm(reader, writer):
            try:
                self.assertEqual(await reader.readline(), b"b" * 64 + b"\n")
                while True:
                    writer.write(sample * 882)
                    await writer.drain()
                    await asyncio.sleep(0.02)
            except ConnectionError:
                pass
            finally:
                writer.close()
                with contextlib.suppress(ConnectionError):
                    await writer.wait_closed()
                pcm_closed.set()

        server = await asyncio.start_server(rtsp, "127.0.0.1", 0)
        pcm_server = await asyncio.start_server(native_pcm, "127.0.0.1", 0)
        source = device = playback = None
        try:
            config = AppleTV(IPv4Address("127.0.0.1"), "Loopback receiver")
            service = ManualService("loopback", Protocol.RAOP, server.sockets[0].getsockname()[1],
                                    {"sr": "44100", "ss": "16", "ch": "2", "et": "0", "md": "0"})
            config.add_service(service)
            device = await pyatv.connect(config, loop, protocol=Protocol.RAOP)
            source = await PcmInput.attach(pcm_server.sockets[0].getsockname()[1], "b" * 64)
            playback = asyncio.create_task(send_live(device, service, source, initial_volume))
            ready = asyncio.create_task(source.started.wait())
            done, _ = await asyncio.wait({playback, ready}, timeout=5, return_when=asyncio.FIRST_COMPLETED)
            if playback in done:
                await playback
            self.assertTrue(source.started.is_set(), "First PCM request never reached the sender")
            ready.cancel()
            await asyncio.gather(ready, return_exceptions=True)
            packet = await asyncio.wait_for(inboxes[0].packets.get(), 2)
            self.assertEqual(packet[12:16], struct.pack(">hh", 1234, -4321))
            methods = [method for method, _ in requests]
            first_volume = next(i for i, (method, body) in enumerate(requests)
                                if method == "SET_PARAMETER" and body.startswith(b"volume:"))
            self.assertLess(methods.index("RECORD"), first_volume)
            dbfs = float(requests[first_volume][1].decode().split(":", 1)[1])
            level = initial_volume if initial_volume else 0.001
            self.assertAlmostEqual(dbfs, -30.0 + 0.3 * level)
            await device.audio.set_volume(35)
            self.assertEqual(sum(m == "SET_PARAMETER" and b.startswith(b"volume:")
                                 for m, b in requests), 2)
            playback.cancel()
            with self.assertRaises(asyncio.CancelledError):
                await playback
            await asyncio.wait_for(teardown.wait(), 2)
        finally:
            if playback and not playback.done():
                playback.cancel()
                await asyncio.gather(playback, return_exceptions=True)
            if source:
                await source.close()
                await asyncio.wait_for(pcm_closed.wait(), 2)
            if device:
                await asyncio.gather(*device.close(), return_exceptions=True)
            server.close()
            pcm_server.close()
            await server.wait_closed()
            await pcm_server.wait_closed()
            for transport in udp:
                transport.close()


if __name__ == "__main__":
    unittest.main()
