#!/usr/bin/env python3
"""JA2 co-op relay: pairs a hosting game with joining games by a join code
and forwards their messages. It knows nothing about JA2.

Frames, both ways: u32 little-endian length, then that many bytes; the first
byte is the type, the rest the payload.

  client -> relay
    1 HOST                      relay answers 2 CODE with a 6-letter code
    3 JOIN  <code ascii>        relay answers 4 JOINED <u8 seat> or 5 ERROR <text>
    6 SEND  <u8 to> <bytes>     forward to seat `to` (0 = host); 255 = everyone else
  relay -> client
    7 FROM  <u8 from> <bytes>   a forwarded message
    8 PEER_JOINED <u8 seat>     (to the host)
    9 PEER_LEFT   <u8 seat>     (to everyone left in the room)

Run:  python3 relay.py [--port 27960]
"""
import argparse
import asyncio
import secrets
import string
import struct

MAX_FRAME = 64 * 1024 * 1024  # saves are a few MB
MAX_SEATS = 4
CODE_CHARS = string.ascii_uppercase.replace("O", "").replace("I", "") + "23456789"

rooms = {}  # code -> {seat: writer}


async def send(writer, kind, payload=b""):
    data = bytes([kind]) + payload
    writer.write(struct.pack("<I", len(data)) + data)
    await writer.drain()


async def read_frame(reader):
    (n,) = struct.unpack("<I", await reader.readexactly(4))
    if n == 0 or n > MAX_FRAME:
        raise ValueError("bad frame length")
    data = await reader.readexactly(n)
    return data[0], data[1:]


async def handle(reader, writer):
    code = seat = None
    try:
        while True:
            kind, payload = await read_frame(reader)
            if kind == 1 and code is None:
                code = "".join(secrets.choice(CODE_CHARS) for _ in range(6))
                while code in rooms:
                    code = "".join(secrets.choice(CODE_CHARS) for _ in range(6))
                seat = 0
                rooms[code] = {0: writer}
                await send(writer, 2, code.encode())
            elif kind == 3 and code is None:
                want = payload.decode(errors="replace").strip().upper()
                room = rooms.get(want)
                free = [s for s in range(1, MAX_SEATS) if room is not None and s not in room]
                if not free:
                    await send(writer, 5, b"No such game, or it is full.")
                    continue
                code, seat = want, free[0]
                room[seat] = writer
                await send(writer, 4, bytes([seat]))
                await send(room[0], 8, bytes([seat]))
            elif kind == 6 and code is not None and payload:
                room = rooms.get(code, {})
                to, body = payload[0], payload[1:]
                targets = [w for s, w in room.items() if s != seat] if to == 255 else [room[to]] if to in room else []
                for w in targets:
                    await send(w, 7, bytes([seat]) + body)
    except (asyncio.IncompleteReadError, ConnectionError, ValueError):
        pass
    finally:
        room = rooms.get(code)
        if room is not None and room.get(seat) is writer:
            del room[seat]
            if seat == 0:  # host left: the game is over for everyone
                for w in room.values():
                    w.close()
                del rooms[code]
            else:
                for w in room.values():
                    try:
                        await send(w, 9, bytes([seat]))
                    except ConnectionError:
                        pass
        writer.close()


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=27960)
    args = ap.parse_args()
    server = await asyncio.start_server(handle, "0.0.0.0", args.port)
    print(f"JA2 co-op relay listening on {args.port}")
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
