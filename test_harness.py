"""
Shared test harness for C coursework grading scripts.
"""

import subprocess
import os
import sys
import signal
import time
import tempfile
import shutil
from typing import Optional

# ── ANSI colours (disabled when not a tty) ───────────────────────────────────
_USE_COLOUR = sys.stdout.isatty()
GREEN  = "\033[32m" if _USE_COLOUR else ""
RED    = "\033[31m" if _USE_COLOUR else ""
YELLOW = "\033[33m" if _USE_COLOUR else ""
BOLD   = "\033[1m"  if _USE_COLOUR else ""
RESET  = "\033[0m"  if _USE_COLOUR else ""

# ── Result accumulator ────────────────────────────────────────────────────────
_results: list[tuple[bool, str, Optional[str]]] = []   # (passed, name, reason)

def _record(passed: bool, name: str, reason: Optional[str] = None):
    _results.append((passed, name, reason))
    tag  = f"{GREEN}PASS{RESET}" if passed else f"{RED}FAIL{RESET}"
    line = f"  [{tag}] {name}"
    if not passed and reason:
        # indent multi-line reasons
        indented = reason.replace("\n", "\n         ")
        line += f"\n         {RED}{indented}{RESET}"
    print(line)

def expect(condition: bool, name: str, reason: str = ""):
    _record(condition, name, reason if not condition else None)

def print_summary():
    total  = len(_results)
    passed = sum(1 for p, *_ in _results if p)
    bar    = f"{BOLD}{passed}/{total}{RESET}"
    colour = GREEN if passed == total else (YELLOW if passed > 0 else RED)
    print()
    print(f"{colour}{BOLD}{'─'*50}{RESET}")
    print(f"  Result: {bar} tests passed")
    print(f"{colour}{BOLD}{'─'*50}{RESET}")
    return passed, total

# ── Build helper ──────────────────────────────────────────────────────────────
def build(target: str) -> bool:
    """Run `make <target>` in cwd; return True on success."""
    result = subprocess.run(
        ["make", target],
        capture_output=True, text=True
    )
    if result.returncode != 0:
        print(f"{RED}Build failed for target '{target}':{RESET}")
        print(result.stderr or result.stdout)
        return False
    return True

# ── Process helpers ───────────────────────────────────────────────────────────
def run(cmd: list[str], stdin: bytes = b"", timeout: float = 5.0,
        input_file: Optional[str] = None) -> tuple[int, bytes, bytes]:
    """
    Run *cmd*, return (returncode, stdout_bytes, stderr_bytes).
    stdin bytes are fed to the process's stdin.
    Raises RuntimeError on timeout.
    """
    try:
        proc = subprocess.run(
            cmd,
            input=stdin,
            capture_output=True,
            timeout=timeout,
        )
        return proc.returncode, proc.stdout, proc.stderr
    except subprocess.TimeoutExpired:
        raise RuntimeError(f"Process timed out after {timeout}s: {cmd}")

def run_with_file(cmd_factory, content: bytes, timeout: float = 5.0
                  ) -> tuple[int, bytes, bytes]:
    """
    Write *content* to a temp file, call cmd_factory(path) → list[str],
    run it, clean up, return (rc, stdout, stderr).
    """
    fd, path = tempfile.mkstemp()
    try:
        os.write(fd, content)
        os.close(fd)
        return run(cmd_factory(path), timeout=timeout)
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass

# ── Temp-file context manager ─────────────────────────────────────────────────
class TempFile:
    """Context manager: creates a named temp file, optionally pre-filled."""
    def __init__(self, content: bytes = b"", suffix: str = ""):
        self.content = content
        self.suffix  = suffix
        self.path    = None

    def __enter__(self) -> str:
        fd, self.path = tempfile.mkstemp(suffix=self.suffix)
        os.write(fd, self.content)
        os.close(fd)
        return self.path

    def __exit__(self, *_):
        try:
            os.unlink(self.path)
        except OSError:
            pass
