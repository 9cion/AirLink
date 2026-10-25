"""AirLink's socket-to-PCM boundary.

The native process supplies interleaved little-endian signed 16-bit stereo at
44100 Hz. pyatv's raw-source interface consumes the sample bytes in network
order. No container decoder, disk file, encoder patch, or global hook is used.
"""
import asyncio
from dataclasses import dataclass, field

from pyatv.interface import MediaMetadata
from pyatv.protocols.raop.audio_source import AudioSource


@dataclass
class PcmInput(AudioSource):
    reader: asyncio.StreamReader
    writer: asyncio.StreamWriter
    started: asyncio.Event = field(default_factory=asyncio.Event)
    closed: bool = False

    sample_rate = 44100
    channels = 2
    sample_size = 2
    duration = 0

    @classmethod
    async def attach(cls, port: int, token: str):
        if len(token) != 64 or any(c not in "0123456789abcdef" for c in token):
            raise ValueError("Invalid PCM session token")
        if not 0 < port < 65536:
            raise ValueError("Invalid PCM port")
        reader, writer = await asyncio.wait_for(
            asyncio.open_connection("127.0.0.1", port, limit=16384), 5
        )
        try:
            writer.write((token + "\n").encode("ascii"))
            await asyncio.wait_for(writer.drain(), 5)
        except BaseException:
            writer.close()
            await writer.wait_closed()
            raise
        return cls(reader, writer)

    async def readframes(self, nframes):
        if nframes < 0:
            raise ValueError("A frame count cannot be negative")
        if self.closed or nframes == 0:
            return b""
        try:
            block = await asyncio.wait_for(self.reader.readexactly(nframes * 4), 5)
        except asyncio.IncompleteReadError as end:
            block = end.partial
            self.closed = True
        if len(block) % 4:
            raise ValueError("PCM input ended within a stereo frame")
        if not block:
            return b""
        # This callback is reached by the sender only after RECORD and volume.
        self.started.set()
        network_order = bytearray(len(block))
        network_order[::2], network_order[1::2] = block[1::2], block[::2]
        return bytes(network_order)

    async def get_metadata(self):
        return MediaMetadata(title="System audio", artist="AirLink", duration=0)

    async def close(self):
        self.closed = True
        self.writer.close()
        await self.writer.wait_closed()

