"""Validate ALAC using an independent decoder when FFmpeg is available."""
import asyncio
import plistlib
import shutil
import struct
import subprocess
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import AsyncMock, Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "transport"))
from alac import encode_packet
from airplay2_audio import AlacAirPlay2
from pyatv.protocols.raop.protocols import StreamContext
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305


def caf_file(packet, frames):
    def chunk(name, body):
        return name + struct.pack(">q", len(body)) + body

    desc = struct.pack(">d4sIIIII", 44100, b"alac", 0, 0, 352, 2, 0)
    cookie = struct.pack(">IBBBBBBHIII", 352, 0, 16, 40, 10, 14, 2, 255, 0, 0, 44100)
    length = len(packet)
    groups = [length & 127]
    while length >> 7:
        length >>= 7
        groups.insert(0, (length & 127) | 128)
    table = struct.pack(">qqii", 1, frames, 0, 0) + bytes(groups)
    return (b"caff\x00\x01\x00\x00" + chunk(b"desc", desc) + chunk(b"kuki", cookie)
            + chunk(b"pakt", table) + chunk(b"data", b"\x00" * 4 + packet))


class CodecTests(unittest.TestCase):
    def test_invalid_frame_sizes(self):
        for block in (b"", b"\x00", b"\x00" * 3, b"\x00" * 1412):
            with self.assertRaises(ValueError):
                encode_packet(block)

    @unittest.skipUnless(shutil.which("ffmpeg"), "Optional independent FFmpeg decoder is not installed")
    def test_ffmpeg_decodes_samples_exactly(self):
        for frames in (1, 2, 351, 352):
            with self.subTest(frames=frames):
                samples = [((n * 9973) % 65536) - 32768 for n in range(frames * 2)]
                network = struct.pack(">" + "h" * len(samples), *samples)
                expected = struct.pack("<" + "h" * len(samples), *samples)
                result = subprocess.run(
                    [shutil.which("ffmpeg"), "-v", "error", "-f", "caf", "-i", "pipe:0",
                     "-f", "s16le", "-acodec", "pcm_s16le", "pipe:1"],
                    input=caf_file(encode_packet(network), frames), capture_output=True, timeout=10,
                )
                self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
                self.assertEqual(result.stdout, expected)


class AirPlay2Tests(unittest.IsolatedAsyncioTestCase):
    async def test_negotiation_and_encrypted_wire_payload(self):
        key = bytes(range(32))
        context = StreamContext()
        rtsp = SimpleNamespace(session_id=42)
        rtsp.setup = AsyncMock(return_value=SimpleNamespace(body=plistlib.dumps(
            {"streams": [{"dataPort": 9000, "controlPort": 9001}]}, fmt=plistlib.FMT_BINARY)))
        protocol = AlacAirPlay2(context, rtsp)
        protocol._verifier = Mock()
        protocol._verifier.encryption_keys.return_value = (key, key)
        await protocol.setup_audio_stream(8100)
        request = rtsp.setup.call_args.kwargs["body"]["streams"][0]
        self.assertEqual((request["ct"], request["audioFormat"], request["spf"]), (2, 262144, 352))
        self.assertEqual((context.server_port, context.control_port), (9000, 9001))
        header = struct.pack(">BBHII", 128, 224, 17, 123456, 42)
        audio = struct.pack(">hh", 3000, -3000) * 352
        transport = Mock()
        for _ in range(2):
            _, packet = await protocol.send_audio_packet(transport, header, audio)
            self.assertEqual(packet[:12], header)
            plaintext = ChaCha20Poly1305(key).decrypt(b"\x00" * 4 + packet[-8:], packet[12:-8], header[4:12])
            self.assertEqual(plaintext, encode_packet(audio))
        calls = transport.sendto.call_args_list
        self.assertNotEqual(calls[0].args[0][-8:], calls[1].args[0][-8:])


if __name__ == "__main__":
    unittest.main()
