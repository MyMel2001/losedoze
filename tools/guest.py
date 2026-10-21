#!/usr/bin/env python3
"""Drive the Losedoze guest serial console: log in, run a shell command.

Usage: guest.py 'command1; command2'   (reads script from argv[1] or stdin)
"""
import os, socket, subprocess, sys, time

QEMU = [
    "qemu-system-x86_64", "-enable-kvm", "-m", "3072", "-smp", "2",
    "-cdrom", "build/losedoze.iso", "-boot", "d", "-vga", "std",
    "-display", "none", "-no-reboot",
    "-serial", "unix:/tmp/loseterm-guest.sock,server,nowait",
]
SOCK = "/tmp/loseterm-guest.sock"


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else sys.stdin.read()
    os.path.exists("build/losedoze.iso") or sys.exit("run from repo root; missing ISO")
    try:
        os.unlink(SOCK)
    except OSError:
        pass
    proc = subprocess.Popen(QEMU, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        for _ in range(60):
            try:
                s.connect(SOCK)
                break
            except OSError:
                time.sleep(0.5)
        else:
            sys.exit("serial connect failed")
        buf = b""

        def read_until(needle, timeout):
            nonlocal buf
            t0 = time.time()
            while needle not in buf:
                if time.time() - t0 > timeout:
                    sys.stdout.write(buf.decode(errors="replace"))
                    buf = b""
                    sys.exit("\nTIMEOUT waiting for %r" % needle)
                try:
                    s.settimeout(2)
                    chunk = s.recv(4096)
                except socket.timeout:
                    continue
                if not chunk:
                    sys.exit("serial closed")
                buf += chunk
            out, buf = buf, b""
            return out.decode(errors="replace")

        boot = read_until(b"login:", 300)
        print(boot[-800:])
        s.sendall(b"root\n")
        read_until(b"# ", 60)
        time.sleep(0.5)
        buf = b""
        s.sendall(cmd.encode() + b"\n")
        # read until the console goes quiet (marker echoes confuse naive matching)
        deadline, last, collected = time.time() + 150, time.time(), b""
        while time.time() < deadline and time.time() - last < 8:
            try:
                s.settimeout(2)
                chunk = s.recv(8192)
            except socket.timeout:
                continue
            if chunk:
                collected += chunk
                last = time.time()
        print(collected.decode(errors="replace"))
        open("/tmp/guest-raw.log", "wb").write(collected)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()


main()
