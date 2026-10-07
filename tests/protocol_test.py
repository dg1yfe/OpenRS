#!/usr/bin/env python3
"""Protocol tests for OpenRS.

Plays the TNC side of the host link protocol over a pseudo terminal and checks
OpenRS' replies and the effect on the file system. Only the Python standard
library is used; runs on Linux and macOS.

Usage: protocol_test.py <openrs binary> [runner ...]
       e.g. protocol_test.py ./openrs qemu-aarch64-static
            protocol_test.py ./openrs.exe wine

Under wine, a COM port of the wine prefix is linked to the pseudo terminal and
OpenRS is given that port name.
"""

import os
import pty
import select
import shutil
import subprocess
import sys
import tempfile
import termios
import time

STX, ETX, DLE = 0x02, 0x03, 0x10
FOPEN, FREAD, FWRITE, FCLOSE, FGETC = 0x00, 0x01, 0x02, 0x03, 0x04
FINDFIRST, FINDNEXT, REMOVE, FSEEK, UNGETC = 0x08, 0x09, 0x0A, 0x0D, 0x0E

COMMAND = sys.argv[2:] + [os.path.abspath(sys.argv[1])]
WINE = any(os.path.basename(a).startswith("wine") for a in sys.argv[2:3])
WINE_COM = "COM9"
STARTUP = 2.0 if WINE else 0.5     # seconds until OpenRS has opened the port


def esc(data):
    return b"".join(bytes([DLE, b]) if b in (STX, ETX, DLE) else bytes([b]) for b in data)


def unesc(data):
    out, i = bytearray(), 0
    while i < len(data):
        if data[i] == DLE and i + 1 < len(data):
            i += 1
        out.append(data[i])
        i += 1
    return bytes(out)


def L(v):
    return esc((v & 0xFFFFFFFF).to_bytes(4, "big"))


def W(v):
    return esc((v & 0xFFFF).to_bytes(2, "big"))


def S(text):
    return esc(text.encode()) + bytes([ETX])


def request(cmd, fields=b""):
    return bytes([STX]) + esc(bytes([cmd])) + fields


class OpenRS:
    """One OpenRS process on a pty, running in its own temporary directory."""

    def __init__(self, files=None, args=()):
        self.dir = tempfile.mkdtemp()
        for name, content in (files or {}).items():
            path = os.path.join(self.dir, name)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(content)
        self.master, self.slave = pty.openpty()
        port = os.ttyname(self.slave)
        if WINE:
            prefix = os.environ.get("WINEPREFIX", os.path.expanduser("~/.wine"))
            link = os.path.join(prefix, "dosdevices", WINE_COM.lower())
            if os.path.lexists(link):
                os.remove(link)
            os.symlink(port, link)
            port = WINE_COM
        self.proc = subprocess.Popen(
            COMMAND + [a.replace("TTY", port) for a in (args or ["TTY"])],
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            cwd=self.dir)
        time.sleep(STARTUP)

    def read(self, first_timeout=5.0, idle=0.3):
        out, timeout = b"", first_timeout
        while select.select([self.master], [], [], timeout)[0]:
            out += os.read(self.master, 65536)
            timeout = idle
        return out

    def call(self, data):
        """Send a request, return the unescaped reply without the ack."""
        os.write(self.master, data)
        raw = self.read()
        assert raw[:1] == bytes([ETX]), f"no ack, got {raw.hex(' ')}"
        return raw[1:]

    def fopen(self, path, mode):
        reply = unesc(self.call(request(FOPEN, S(path) + S(mode))))
        return int.from_bytes(reply, "big")

    def stop(self):
        """End OpenRS with CTRL-C on the keyboard, which works on every platform."""
        if self.proc.poll() is not None:
            return self.proc.returncode
        try:
            self.proc.stdin.write(b"\x03")
            self.proc.stdin.flush()
            return self.proc.wait(timeout=10)
        except (OSError, ValueError, subprocess.TimeoutExpired):
            self.proc.kill()
            self.proc.wait()
            return "did not exit on CTRL-C"

    def close(self):
        code = self.stop()
        os.close(self.master)
        os.close(self.slave)
        shutil.rmtree(self.dir)
        return code

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        code = self.close()
        if exc[0] is None:
            assert code == 0, f"exit status {code}"


def run(args=()):
    """Run OpenRS to completion; returns (exit status, stdout).

    Output goes through a file, not a pipe: under wine the first program
    started may launch background services that inherit a pipe and keep it
    open, so reading a pipe to its end would wait for them as well."""
    with tempfile.TemporaryFile() as out:
        code = subprocess.run(COMMAND + list(args), stdin=subprocess.DEVNULL, stdout=out,
                              stderr=subprocess.DEVNULL, timeout=30).returncode
        out.seek(0)
        return code, out.read()


def test_usage():
    code, out = run()
    assert code == 0, code
    assert b"Usage: openrs" in out, out


def test_upload_pc_to_tnc():
    # cp c:\x r:x -- one ETX after a short read, clean fclose and exit
    with OpenRS({"hello.txt": b"hello"}) as t:
        h = t.fopen("c:\\hello.txt", "rb")
        assert h != 0
        assert t.call(request(FREAD, L(1024) + L(h))) == b"hello" + bytes([ETX])
        assert t.call(request(FCLOSE, L(h))) == b"\0\0"


def test_fread_exact_then_eof():
    with OpenRS({"hello.txt": b"hello"}) as t:
        h = t.fopen("c:\\hello.txt", "rb")
        assert t.call(request(FREAD, L(5) + L(h))) == b"hello"
        assert t.call(request(FREAD, L(5) + L(h))) == bytes([ETX])


def test_download_tnc_to_pc():
    data = bytes([0x01, STX, ETX, DLE, 0xFF])
    with OpenRS() as t:
        h = t.fopen("c:\\New.Txt", "wb")
        assert h != 0
        os.write(t.master, request(FWRITE, L(h) + esc(data) + bytes([ETX])))
        assert t.read() == bytes([ETX])
        assert t.call(request(FCLOSE, L(h))) == b"\0\0"
        assert os.listdir(t.dir) == ["New.Txt"]
        with open(os.path.join(t.dir, "New.Txt"), "rb") as f:
            assert f.read() == data
        assert t.fopen("c:\\NEW.TXT", "wb") == 0, "existing file overwritten"


def test_paths_and_case():
    with OpenRS({"README.TXT": b"r", "sub/x.txt": b"x"}) as t:
        for path in ("c:\\README.TXT", "c:\\readme.txt", "c:README.TXT", "c:\\sub\\readme.txt"):
            assert t.fopen(path, "rb") != 0, path
        assert t.fopen("c:\\missing.txt", "rb") == 0


def list_dir(t, pattern):
    names = []
    reply = unesc(t.call(request(FINDFIRST, S(pattern) + W(0x16))))
    while reply[:2] == b"\0\0":
        assert len(reply) == 26, reply.hex(" ")
        assert 0 in reply[12:26], "file name not NUL terminated"
        names.append(reply[12:26].split(b"\0")[0].decode())
        reply = unesc(t.call(request(FINDNEXT)))
    assert reply == b"\xff\xff", reply.hex(" ")
    return sorted(names)


def test_findfirst():
    files = {"prog.apl": b"", "Tool.APL": b"", "dip1.scr": b"", "dip10.scr": b"",
             "a_very_long_file_name.txt": b"", "sub/x.apl": b""}
    with OpenRS(files) as t:
        assert (got := list_dir(t, "c:\\*.apl")) == ["Tool.APL", "prog.apl"], got
        assert (got := list_dir(t, "c:\\DIP?.SCR")) == ["dip1.scr"], got
        assert (got := list_dir(t, "c:\\sub\\*.*")) == [".", "..", "x.apl"], got
        assert "a_very_long_f" in list_dir(t, "c:\\*.*")
        assert (got := list_dir(t, "c:\\tool.apl")) == ["Tool.APL"], got
        assert (got := list_dir(t, "c:\\*.xyz")) == [], got
        # a further findnext after the end must not crash
        assert t.call(request(FINDNEXT)) == b"\xff\xff"


def test_remove():
    with OpenRS({"work.txt": b"", "sub/keep.txt": b""}) as t:
        assert t.call(request(REMOVE, S("c:\\WORK.TXT"))) == b"\0\0"
        for path in ("c:\\missing.txt", "c:\\sub", "c:\\..", "c:\\sub\\keep.txt"):
            assert t.call(request(REMOVE, S(path))) == b"\xff\xff", path
        assert os.listdir(t.dir) == ["sub"]
        assert os.listdir(os.path.join(t.dir, "sub")) == ["keep.txt"]


def test_fseek_ungetc():
    with OpenRS({"f.txt": b"hello"}) as t:
        h = t.fopen("c:\\f.txt", "rb")
        assert t.call(request(FSEEK, L(h) + L(0) + W(2))) == b"\0\0"
        assert t.call(request(FSEEK, L(h) + L(-2) + W(1))) == b"\0\0"
        assert t.call(request(FGETC, L(h))) == b"\0l"
        assert t.call(request(UNGETC, W(ord("A")) + L(h))) == b"\0A"
        assert t.call(request(FGETC, L(h))) == b"\0A"


def test_invalid_handles():
    with OpenRS() as t:
        for h in (0, 999):
            assert unesc(t.call(request(FCLOSE, L(h)))) == b"\xff\xff"
            assert t.call(request(FREAD, L(16) + L(h))) == bytes([ETX])


def test_command_sent():
    # the TNC command from the command line is typed on the TNC, escaped
    with OpenRS(args=["TTY", "19200", "cp", "c:\\x\x10y", "r:x"]) as t:
        assert t.read() == b"cp c:\\x" + bytes([DLE, DLE]) + b"y r:x\r"


def test_close_after():
    with OpenRS({"f.txt": b"x"}, args=["-c", "3", "TTY", "19200"]) as t:
        # still serving before the time is up
        assert t.fopen("c:\\f.txt", "rb") != 0
        assert t.proc.wait(timeout=15) == 0


def test_option_errors():
    for args in (["-c"], ["-c", "abc", "x"], ["--close-after=0", "x"], ["-x", "x"]):
        code, _ = run(args)
        assert code == 1, (args, code)


def test_serves_without_keyboard():
    # stdin at its end (e.g. openrs ... </dev/null) must not stop serving the TNC
    t = OpenRS({"f.txt": b"x"})
    try:
        t.proc.stdin.close()
        time.sleep(0.5)
        h = t.fopen("c:\\f.txt", "rb")
        assert h != 0
        assert t.call(request(FREAD, L(16) + L(h))) == b"x" + bytes([ETX])
    finally:
        t.close()


def test_keystrokes_escaped():
    with OpenRS() as t:
        t.proc.stdin.write(bytes([STX, DLE]) + b"ab\x7f")
        t.proc.stdin.flush()
        assert t.read() == bytes([DLE, STX, DLE, DLE]) + b"ab\x08"


def test_bitrate_and_flow_control():
    for args, speed, rtscts in ((["TTY", "9600"], termios.B9600, False),
                                (["-r", "TTY", "38400"], termios.B38400, True)):
        with OpenRS(args=args) as t:
            attr = termios.tcgetattr(t.slave)
            assert attr[4] == speed and attr[5] == speed, attr[4:6]
            assert bool(attr[2] & termios.CRTSCTS) == rtscts


def main():
    tests = [(n, f) for n, f in globals().items() if n.startswith("test_")]
    failed = 0
    for name, func in tests:
        try:
            func()
            print(f"ok    {name}")
        except Exception as e:  # report every failure, then fail the run
            failed += 1
            print(f"FAIL  {name}: {type(e).__name__}: {e}")
    print(f"{len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
