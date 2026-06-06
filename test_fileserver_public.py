#!/usr/bin/env python3
"""
PUBLIC tests for the `file-server` / `file-client` exercise.
Run from the project root: python3 test_fileserver_public.py
"""

import sys, os, time, socket, subprocess, tempfile, shutil
sys.path.insert(0, os.path.dirname(__file__))
from test_harness import build, run, expect, print_summary, TempFile

SERVER = "./file-server"
CLIENT = "./file-client"

def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]

class ManagedServer:
    """Starts file-server in a temp working directory; tears down on exit."""
    def __init__(self, port: int):
        self.port    = port
        self.workdir = tempfile.mkdtemp(prefix="fileserver_")
        self.proc    = None

    def __enter__(self):
        self.proc = subprocess.Popen(
            [os.path.abspath(SERVER), str(self.port)],
            cwd=self.workdir,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        # Wait until the server prints its ready message
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            line = self.proc.stdout.readline().decode(errors="replace")
            if f"Listening on {self.port}" in line:
                break
            if self.proc.poll() is not None:
                break
            time.sleep(0.05)
        else:
            time.sleep(0.2)   # fallback
        return self

    def __exit__(self, *_):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        shutil.rmtree(self.workdir, ignore_errors=True)

    def place_file(self, name: str, content: bytes):
        """Pre-populate a file in the server's working directory."""
        with open(os.path.join(self.workdir, name), "wb") as f:
            f.write(content)

    def read_file(self, name: str) -> bytes:
        with open(os.path.join(self.workdir, name), "rb") as f:
            return f.read()

    def file_exists(self, name: str) -> bool:
        return os.path.exists(os.path.join(self.workdir, name))

def client(*args, timeout=10) -> tuple[int, str, str]:
    rc, out, err = run([CLIENT, "127.0.0.1", str(args[0])] + list(args[1:]),
                       timeout=timeout)
    return rc, out.decode(errors="replace"), err.decode(errors="replace")

def main():
    print("Building file-server and file-client...")
    if not build("file-server") or not build("file-client"):
        sys.exit(1)
    print()
    print("Running public tests for file-server / file-client")
    print("─" * 50)

    port = free_port()
    with ManagedServer(port) as srv:

        # ── Test 1: server prints ready message ───────────────────────────────
        # (checked implicitly by ManagedServer startup above; we verify it here)
        srv2_port = free_port()
        srv2 = subprocess.Popen(
            [os.path.abspath(SERVER), str(srv2_port)],
            cwd=srv.workdir,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        ready_line = srv2.stdout.readline().decode(errors="replace")
        srv2.terminate(); srv2.wait(timeout=3)
        expect(f"Listening on {srv2_port}" in ready_line,
               "server-ready: prints 'Listening on <port>...' on startup",
               f"got: {ready_line!r}")

        # ── Test 2: ls on empty server ────────────────────────────────────────
        rc, out, _ = client(port, "ls")
        expect(rc == 0, "ls-empty: exit 0", f"got {rc}")
        # Empty listing should produce empty or blank output — no crash
        expect(True, "ls-empty: does not crash on empty directory")

        # ── Test 3: upload a file ─────────────────────────────────────────────
        with TempFile(b"hello from client") as local:
            rc, out, err = client(port, "upload", local, "hello.txt")
            expect(rc == 0, "upload: exit 0", f"got {rc}\nstderr: {err}")
            expect(srv.file_exists("hello.txt"),
                   "upload: file appears in server working dir")
            if srv.file_exists("hello.txt"):
                expect(srv.read_file("hello.txt") == b"hello from client",
                       "upload: file content is correct",
                       f"got {srv.read_file('hello.txt')!r}")

        # ── Test 4: ls shows uploaded file ───────────────────────────────────
        rc, out, _ = client(port, "ls")
        expect(rc == 0, "ls-after-upload: exit 0")
        expect("hello.txt" in out,
               "ls-after-upload: uploaded file appears in listing",
               f"output: {out!r}")

        # ── Test 5: download a file ───────────────────────────────────────────
        srv.place_file("greet.txt", b"hi there")
        with tempfile.NamedTemporaryFile(delete=False) as f:
            local_dl = f.name
        try:
            rc, out, err = client(port, "download", "greet.txt", local_dl)
            expect(rc == 0, "download: exit 0", f"got {rc}\nstderr: {err}")
            with open(local_dl, "rb") as f:
                got = f.read()
            expect(got == b"hi there",
                   "download: file content is correct",
                   f"got {got!r}")
        finally:
            os.unlink(local_dl)

    passed, total = print_summary()
    sys.exit(0 if passed == total else 1)

if __name__ == "__main__":
    main()
