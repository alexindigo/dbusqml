#!/usr/bin/env python3
"""Wire fixture: many h references, exactly one SCM_RIGHTS descriptor.

Qt/libdbus writers attach a descriptor for every append. Build the wire body
directly so this tests references, independently of the daemon's attached-fd cap.
The parent owns stdin; EOF ends the fixture, including on parent death.
"""
import array
import os
import select
import socket
import struct
import sys
from urllib.parse import unquote


class Body:
    def __init__(self):
        self.data = bytearray()

    def align(self, n):
        self.data.extend(b"\0" * (-len(self.data) % n))

    def uint(self, n):
        self.align(4)
        self.data.extend(struct.pack("<I", n))

    def string(self, s):
        b = s.encode()
        self.uint(len(b))
        self.data.extend(b + b"\0")

    def signature(self, s):
        b = s.encode()
        self.data.extend(bytes([len(b)]) + b + b"\0")

    def fd_array(self, count):
        self.uint(count * 4)
        self.data.extend(b"\0" * (count * 4))


def message(kind, serial, fields, body):
    header = Body()
    for code, sig, value in fields:
        header.align(8)
        header.data.append(code)
        header.signature(sig)
        if sig == "u":
            header.uint(value)
        elif sig == "g":
            header.signature(value)
        else:
            header.string(value)
    fixed = struct.pack("<BBBBIII", ord("l"), kind, 0, 1,
                        len(body), serial, len(header.data))
    header.align(8)
    return fixed + header.data + body


def receive(sock):
    def exact(n):
        result = b""
        while len(result) < n:
            part = sock.recv(n - len(result))
            if not part:
                raise EOFError
            result += part
        return result

    fixed = exact(16)
    _, kind, _, _, length, serial, hlen = struct.unpack("<BBBBIII", fixed)
    header = exact((hlen + 7) & ~7)
    fields = {}
    pos = 0
    while pos < hlen:
        pos = (pos + 7) & ~7
        code = header[pos]
        slen = header[pos + 1]
        sig = header[pos + 2:pos + 2 + slen]
        pos += 3 + slen
        if sig == b"g":
            size = header[pos]
            fields[code] = header[pos + 1:pos + 1 + size].decode()
            pos += size + 2
        else:
            pos = (pos + 3) & ~3
            size = struct.unpack_from("<I", header, pos)[0]
            pos += 4
            if sig == b"u":
                fields[code] = size
            else:
                fields[code] = header[pos:pos + size].decode()
                pos += size + 1
    return kind, serial, fields, exact(length)


def main():
    count, shape = int(sys.argv[1]), sys.argv[2]
    address = os.environ["DBUS_SESSION_BUS_ADDRESS"].split(";")[0]
    options = dict(item.split("=", 1) for item in address[5:].split(","))
    path = unquote(options.get("path", ""))
    if "abstract" in options:
        path = "\0" + unquote(options["abstract"])
    with socket.socket(socket.AF_UNIX) as sock:
        sock.connect(path)
        sock.sendall(b"\0AUTH EXTERNAL " + str(os.getuid()).encode().hex().encode() + b"\r\n")
        def line():
            result = b""
            while not result.endswith(b"\r\n"):
                result += sock.recv(1)
            return result
        assert line().startswith(b"OK ")
        sock.sendall(b"NEGOTIATE_UNIX_FD\r\n")
        assert line() == b"AGREE_UNIX_FD\r\n"
        sock.sendall(b"BEGIN\r\n")
        sock.sendall(message(1, 1, [(1, "o", "/org/freedesktop/DBus"),
                                  (2, "s", "org.freedesktop.DBus"),
                                  (3, "s", "Hello"), (6, "s", "org.freedesktop.DBus")], b""))
        kind, _, _, body = receive(sock)
        assert kind == 2
        name = body[4:-1].decode()
        print(name, flush=True)
        serial = 2
        with open("/dev/null", "rb") as fd:
            while True:
                ready, _, _ = select.select([sock, sys.stdin], [], [])
                if sys.stdin in ready and not os.read(sys.stdin.fileno(), 1):
                    return
                if sock not in ready:
                    continue
                kind, reply_serial, fields, _ = receive(sock)
                if kind != 1:
                    continue
                payload = Body()
                signature = "ah"
                if shape == "variant":
                    signature = "(ahv)"
                    payload.fd_array(count - 1)
                    payload.signature("h")
                    payload.uint(0)
                elif shape == "mixed":
                    # Native scalar, raw struct, dict value, variant payload,
                    # nested array and trailing ah all share the same budget.
                    signature = "h(h)a{sh}vaahahh"
                    payload.uint(0)
                    payload.align(8)
                    payload.uint(0)
                    payload.uint(12)  # a{sh}: one 8-aligned entry, s + h
                    payload.align(8)
                    payload.string("key")
                    payload.uint(0)
                    payload.signature("h")
                    payload.uint(0)
                    payload.uint(8)  # aah: one inner array with one h
                    payload.fd_array(1)
                    payload.fd_array(count - 6)
                    payload.uint(0)
                else:
                    payload.fd_array(count)
                packet = message(2, serial, [(5, "u", reply_serial),
                                             (6, "s", fields[7]),
                                             (8, "g", signature), (9, "u", 1)], payload.data)
                sock.sendmsg([packet], [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                                        array.array("i", [fd.fileno()]))])
                serial += 1


if __name__ == "__main__":
    main()
