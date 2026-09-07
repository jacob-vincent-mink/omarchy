#!/usr/bin/python

import json
import os
import socket
import struct
import subprocess
import sys

NETWORK_CONTRACT = "24d172b971ac832c86d4e28c224bfd7e912aa1ea3de4517aa09af2c02d362a20"
MEDIA_CONTRACT = "f060bfec526a4355bd3431d39310bf96e61907f26be0e0dea6e6e8c2d28a415c"
NETWORK_SCOPE = '{"methods":["GET"],"origins":["https://fixture.invalid"]}'
MEDIA_SCOPE = '{"controls":["pause","stop","mute","volume","status"],"sourceHandles":["network.fetch"]}'


def text(value):
    encoded = value.encode()
    return struct.pack(">H", len(encoded)) + encoded


def frame(correlation, adapter, contract, operation, scope, payload):
    encoded = json.dumps(payload, separators=(",", ":")).encode()
    body = (
        text(adapter)
        + text(contract)
        + struct.pack(">I", 1)
        + text(operation)
        + text(scope)
        + struct.pack(">I", len(encoded))
        + encoded
    )
    return struct.pack(">IBBBBQI", 0x4F505256, 1, 1, 0, 0, correlation, len(body)) + body


def start(path):
    parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    process = subprocess.Popen([path], stdin=child)
    child.close()
    return process, parent


def call(channel, correlation, adapter, contract, operation, scope, payload):
    channel.send(frame(correlation, adapter, contract, operation, scope, payload))
    response = channel.recv(65557)
    magic, version, kind, zero_a, zero_b, observed, length = struct.unpack(
        ">IBBBBQI", response[:20]
    )
    assert (magic, version, kind, zero_a, zero_b, observed) == (
        0x4F505256,
        1,
        2,
        0,
        0,
        correlation,
    )
    assert length == len(response) - 20 and response[20] == 0
    return json.loads(response[21:])


def fetch(channel, correlation, path="/stations", origin="https://fixture.invalid"):
    return call(
        channel,
        correlation,
        "bounded-network-fetch",
        NETWORK_CONTRACT,
        "fetch",
        NETWORK_SCOPE,
        {
            "method": "GET",
            "origin": origin,
            "path": path,
            "responseType": "json",
            "mediaJsonPointers": ["/*/url"],
        },
    )


def media(channel, correlation, operation, payload):
  return call(channel, correlation, "activation-media-stream", MEDIA_CONTRACT,
              operation, MEDIA_SCOPE, payload)


def main():
    valid = frame(1, "activation-media-stream", MEDIA_CONTRACT, "control",
                  MEDIA_SCOPE, {"control": "status"})
    for malformed in (b"x" * 65557, valid[:-1]):
        rejected, channel = start(sys.argv[1])
        channel.send(malformed)
        assert rejected.wait(timeout=2) == 65
        assert channel.recv(65557) == b""
        channel.close()
    rejected, channel = start(sys.argv[1])
    with open(os.devnull, "rb") as descriptor:
        channel.sendmsg([valid], [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                                  struct.pack("i", descriptor.fileno()))])
    assert rejected.wait(timeout=2) == 65
    assert channel.recv(65557) == b""
    channel.close()

    process, channel = start(sys.argv[1])
    result = fetch(channel, 1)
    assert result["ok"] is True and result["status"] == 200
    handle = result["sourceHandles"]["/0/url"]
    cleartext_handle = result["sourceHandles"]["/1/url"]
    assert len(handle) == 32 and len(cleartext_handle) == 32
    assert result["json"][0]["url"] == "https://stream.example/audio"
    assert result["json"][1]["url"] == "http://stream.example:8000/audio"

    denied = fetch(channel, 2, origin="https://example.com")
    assert denied == {"error": "outside-scope", "ok": False}
    redirected = fetch(channel, 3, path="/redirect")
    assert redirected == {"error": "redirect-rejected", "ok": False}
    private = fetch(channel, 4, path="/private-source")
    assert private["ok"] is True and private["sourceHandles"] == {}

    fractional = media(channel, 5, "play", {"handle": handle, "volume": 42.5})
    assert fractional == {"error": "invalid-request", "ok": False}

    playing = media(channel, 6, "play", {"handle": cleartext_handle, "volume": 42})
    assert playing["ok"] is True and playing["running"] is True and playing["volume"] == 42
    for correlation, command, value, expected in [
        (7, "pause", None, {"ok": True, "paused": True}),
        (20, "pause", None, {"ok": True, "paused": False}),
        (21, "mute", None, {"ok": True, "muted": True}),
        (22, "mute", None, {"ok": True, "muted": False}),
        (23, "volume", 0, {"ok": True, "volume": 0}),
        (24, "volume", 100, {"ok": True, "volume": 100}),
        (25, "volume", 42, {"ok": True, "volume": 42}),
        (8, "stop", None, {"ok": True, "running": False}),
        (26, "status", None, {"ok": True, "running": False}),
        (27, "pause", None, {"ok": False, "error": "provider-failed"}),
        (28, "mute", None, {"ok": False, "error": "provider-failed"}),
        (29, "volume", 42, {"ok": False, "error": "provider-failed"}),
        (30, "stop", None, {"ok": False, "error": "provider-failed"}),
    ]:
        payload = {"control": command}
        if value is not None:
            payload["value"] = value
        result = media(channel, correlation, "control", payload)
        assert all((result[key] is wanted if isinstance(wanted, bool) else result[key] == wanted)
                   for key, wanted in expected.items())
        if not expected["ok"]:
            assert result == expected
    channel.close()
    assert process.wait(timeout=2) == 0

    other, other_channel = start(sys.argv[1])
    stale = media(other_channel, 9, "play", {"handle": handle})
    assert stale == {"error": "invalid-handle", "ok": False}
    other_channel.close()
    assert other.wait(timeout=2) == 0


if __name__ == "__main__":
    main()
