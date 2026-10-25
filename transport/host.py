"""AirLink's single-receiver transport. UTF-8 tab-separated local IPC, no shell.

Only DEVICE names are free-form; control characters are removed before output.
Network addresses and pairing material stay inside this process.
"""
from __future__ import annotations

import asyncio
import contextlib
import hashlib
import logging
import sys

import pyatv
from pyatv.const import PairingRequirement, Protocol
from pcm_input import PcmInput
from raop_session import send_live


def clean(value: object) -> str:
    return "".join(c if c >= " " and c != "\x7f" else " " for c in str(value))[:200]


def emit(kind: str, *fields: object) -> None:
    print("\t".join([kind, *(clean(field) for field in fields)]), flush=True)


class Host:
    def __init__(self) -> None:
        self.devices: dict = {}
        self.task: asyncio.Task | None = None
        self.atv = None
        self.volume = 40
        self.ready = False
        self.volume_task: asyncio.Task | None = None

    async def discover(self) -> None:
        emit("SCAN_BEGIN")
        try:
            configs = await pyatv.scan(asyncio.get_running_loop(), timeout=5)
            self.devices.clear()
            for config in sorted(configs, key=lambda c: c.name.casefold()):
                service = config.get_service(Protocol.RAOP)
                if service is None:
                    continue
                identity = str(service.identifier or config.identifier or config.address)
                key = hashlib.sha256(identity.encode()).hexdigest()
                self.devices[key] = config
                locked = bool(service.requires_password) or service.pairing == PairingRequirement.Mandatory
                emit("DEVICE", key, config.name, int(locked))
        except Exception:
            logging.exception("Discovery failed")
            emit("ERROR", "Lautsprechersuche fehlgeschlagen. Netzwerk prüfen.")
        finally:
            emit("SCAN_END", len(self.devices))

    async def start(self, key: str, port: str, token: str, volume: str) -> None:
        await self.stop()
        config = self.devices.get(key)
        if config is None:
            emit("ERROR", "Lautsprecher nicht mehr verfügbar. Bitte erneut suchen.")
            return
        service = config.get_service(Protocol.RAOP)
        if service.requires_password or service.pairing == PairingRequirement.Mandatory:
            emit("ERROR", "Dieser Lautsprecher benötigt eine Kopplung. Diese Version unterstützt offene AirPlay-Empfänger.")
            return
        self.volume = max(0, min(100, int(volume)))
        self.task = asyncio.create_task(self.stream(config, int(port), token))

    async def stream(self, config, port: int, token: str) -> None:
        attempt = 0
        while True:
            source = None
            playback = None
            waiter = None
            try:
                self.ready = False
                emit("STATE", "connecting" if attempt == 0 else "reconnecting")
                self.atv = await asyncio.wait_for(
                    pyatv.connect(config, asyncio.get_running_loop(), protocol=Protocol.RAOP), timeout=15
                )
                source = await PcmInput.attach(port, token)
                ready = source.started
                playback = asyncio.create_task(send_live(
                    self.atv, config.get_service(Protocol.RAOP), source, self.volume
                ))
                waiter = asyncio.create_task(ready.wait())
                done, _ = await asyncio.wait({playback, waiter}, timeout=20,
                                             return_when=asyncio.FIRST_COMPLETED)
                if playback in done:
                    await playback
                    raise RuntimeError("Playback ended before ready")
                if waiter not in done:
                    raise TimeoutError("Receiver handshake timeout")
                # Keep the native PCM gate on silence until the receiver has
                # accepted the final slider value, including an exact mute.
                while True:
                    requested_volume = self.volume
                    await asyncio.wait_for(self.atv.audio.set_volume(float(requested_volume)), 5)
                    if requested_volume == self.volume:
                        break
                self.ready = True
                attempt = 0
                emit("STATE", "streaming")
                await playback
                raise RuntimeError("Playback ended")
            except asyncio.CancelledError:
                raise
            except Exception:
                logging.exception("AirPlay connection failed")
                self.ready = False
                emit("STATE", "reconnecting")
            finally:
                self.ready = False
                for task in (playback, waiter):
                    if task:
                        task.cancel()
                await asyncio.gather(*(t for t in (playback, waiter) if t), return_exceptions=True)
                if source:
                    with contextlib.suppress(Exception):
                        await source.close()
                if self.atv:
                    tasks = self.atv.close()
                    self.atv = None
                    if tasks:
                        with contextlib.suppress(TimeoutError):
                            await asyncio.wait_for(asyncio.gather(*tasks, return_exceptions=True), 3)
            await asyncio.sleep((1, 2, 5, 10, 30)[min(attempt, 4)])
            attempt += 1

    async def apply_volume(self) -> None:
        await asyncio.sleep(0.12)
        if self.atv and self.ready:
            try:
                await asyncio.wait_for(self.atv.audio.set_volume(float(self.volume)), 5)
            except Exception:
                logging.exception("Volume update failed")
                emit("NOTICE", "Lautstärke konnte nicht geändert werden.")

    async def stop(self) -> None:
        if self.volume_task:
            self.volume_task.cancel()
            await asyncio.gather(self.volume_task, return_exceptions=True)
            self.volume_task = None
        if self.task:
            self.task.cancel()
            await asyncio.gather(self.task, return_exceptions=True)
            self.task = None
        emit("STATE", "idle")


async def main() -> None:
    host = Host()
    emit("READY", "1")
    try:
        while line := await asyncio.to_thread(sys.stdin.readline):
            fields = line.rstrip("\r\n").split("\t")
            try:
                if fields == ["DISCOVER"]:
                    if host.task is None:
                        await host.discover()
                elif fields[0] == "START" and len(fields) == 5:
                    await host.start(*fields[1:])
                elif fields == ["STOP"]:
                    await host.stop()
                elif fields[0] == "VOLUME" and len(fields) == 2:
                    host.volume = max(0, min(100, int(fields[1])))
                    if host.volume_task:
                        host.volume_task.cancel()
                        await asyncio.gather(host.volume_task, return_exceptions=True)
                    host.volume_task = asyncio.create_task(host.apply_volume())
                elif fields == ["QUIT"]:
                    break
                else:
                    emit("ERROR", "Ungültiger interner Befehl.")
            except Exception:
                logging.exception("Command failed")
                emit("ERROR", "AirPlay-Befehl fehlgeschlagen.")
    finally:
        await host.stop()


if __name__ == "__main__":
    sys.stdin.reconfigure(encoding="utf-8")
    sys.stdout.reconfigure(encoding="utf-8")
    logging.basicConfig(stream=sys.stderr, level=logging.WARNING)
    asyncio.run(main())
