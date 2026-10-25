"""AirLink's live-session orchestration for an unmodified pyatv 0.18.0.

pyatv's file-oriented facade performs container decoding. The lower-level
session manager accepts its AudioSource interface directly, which is the
boundary we need for live PCM. This deliberately version-pinned integration
uses library methods and a session-local ALAC protocol implementation.
Authentication, RTP, RTSP, timing and encryption are implemented by pyatv.
"""
from pyatv.const import Protocol
from pyatv.protocols.airplay.auth import extract_credentials
from pyatv.protocols.raop.protocols.airplayv2 import AirPlayV2
from airplay2_audio import AlacAirPlay2


async def send_live(device, service, pcm, initial_volume):
    stream = device.stream.get(Protocol.RAOP)
    if stream is None:
        raise RuntimeError("No RAOP stream interface")
    manager = stream.playback_manager
    manager.acquire()
    try:
        sender, context = await manager.setup(service)
        if isinstance(sender._protocol, AirPlayV2):
            # Replace only this uninitialized session's protocol object.
            sender._protocol = AlacAirPlay2(context, sender.rtsp)
        context.credentials = extract_credentials(service)
        context.password = service.password
        await sender.initialize(service.properties)
        receiver_format = (context.sample_rate, context.channels, context.bytes_per_channel)
        if receiver_format != (pcm.sample_rate, pcm.channels, pcm.sample_size):
            raise ValueError(f"Receiver requires unsupported audio format: {receiver_format}")
        metadata = await pcm.get_metadata()
        # pyatv treats zero as 'no initial volume'. Use a minimal nonzero value
        # while the native source still supplies silence; Host then sets true 0.
        level = min(100.0, max(0.001, float(initial_volume)))
        await sender.send_audio(pcm, metadata, volume=level)
    finally:
        await manager.teardown()
