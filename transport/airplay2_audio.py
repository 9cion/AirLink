"""Session-local AirPlay 2 ALAC support; no global library modifications.

Some receivers accept a PCM SETUP but discard its audio. Negotiate ALAC and
serialize each payload before the library performs authenticated encryption.
The library still owns authentication, event channels, timing and RTP state.
"""
from pyatv.protocols.raop.protocols.airplayv2 import (
    AirPlayV2, EVENTS_SALT, EVENTS_WRITE_INFO, EVENTS_READ_INFO,
)
from pyatv.support.chacha20 import Chacha20Cipher8byteNonce
from pyatv.support.http import decode_bplist_from_body
from alac import encode_packet


class AlacAirPlay2(AirPlayV2):
    async def setup_audio_stream(self, control_client_port):
        if self._verifier is None:
            raise RuntimeError("AirPlay authentication has not completed")
        channel_keys = self._verifier.encryption_keys(
            EVENTS_SALT, EVENTS_WRITE_INFO, EVENTS_READ_INFO
        )
        key = channel_keys[0][:32]
        description = {
            "type": 96,
            "ct": 2,
            "audioFormat": 262144,
            "sr": 44100,
            "spf": 352,
            "controlPort": control_client_port,
            "shk": key,
            "isMedia": True,
            "audioMode": "default",
            "latencyMin": 11025,
            "latencyMax": 88200,
            "streamConnectionID": self.rtsp.session_id,
            "supportsDynamicStreamID": False,
        }
        response = await self.rtsp.setup(body={"streams": [description]})
        result = decode_bplist_from_body(response)["streams"][0]
        self.context.server_port = int(result["dataPort"])
        self.context.control_port = int(result["controlPort"])
        self._cipher = Chacha20Cipher8byteNonce(key, key)

    async def send_audio_packet(self, transport, rtp_header, audio):
        return await super().send_audio_packet(transport, rtp_header, encode_packet(audio))
