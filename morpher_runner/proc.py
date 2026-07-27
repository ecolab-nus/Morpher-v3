"""Shared process-group lifecycle management for subprocess fleets.

Every long-running runner in this repo (``run_morpher.py``, the benchmark
runners, the arch sweeps) spawns children with ``start_new_session=True`` so
each child leads its own process group. They register those children here so a
single SIGINT/SIGTERM at the top level can deterministically tear down the whole
fleet -- including grandchildren (clang, opt, the mapper) that would otherwise
be orphaned and keep burning CPU.

This module is the single implementation. Previously ``morpher_runner/exec.py``
and three scripts under ``scripts/`` each carried their own copy, in two
different strengths:

  * a pgid-only variant that signalled ``os.getpgid(child)`` and nothing else,
  * a descendant-walking variant that used ``pgrep`` to find grandchildren.

The descendant-walking variant is the one kept here, because the pgid-only
version misses any grandchild that called ``setsid()`` and left the group.
Consolidating also means one shared registry, so a signal arriving while both
``exec.run_command`` and a script-level pool have children in flight cleans up
both rather than only whichever installed its handler first.
"""

from __future__ import annotations

import concurrent.futures
import os
import signal
import subprocess
import sys
import threading
from typing import Callable, List, Optional

_ACTIVE_PROCESSES: set = set()
_ACTIVE_PROCESSES_LOCK = threading.Lock()

_SHUTDOWN_CALLBACKS: List[Callable[[], None]] = []
_SHUTDOWN_CALLBACKS_LOCK = threading.Lock()
_SIGNAL_CLEANUP_INSTALLED = False

# Set once a shutdown signal has been seen. Runners poll this to avoid
# launching new work into a dying process tree.
SHUTDOWN_REQUESTED = threading.Event()


def is_shutdown_requested() -> bool:
  return SHUTDOWN_REQUESTED.is_set()


def child_pids(pid: int) -> List[int]:
  """Direct children of ``pid`` per ``pgrep``; empty if pgrep is unavailable."""
  try:
    output = subprocess.check_output(["pgrep", "-P", str(pid)], text=True)
  except (subprocess.CalledProcessError, FileNotFoundError):
    return []
  return [int(line) for line in output.splitlines() if line.strip().isdigit()]


def descendant_pids(pid: int) -> List[int]:
  """Transitive descendants of ``pid``, discovered breadth-first."""
  out: List[int] = []
  stack = child_pids(pid)
  while stack:
    child = stack.pop()
    out.append(child)
    stack.extend(child_pids(child))
  return out


def signal_pid_or_group(pid: int, sig: int) -> None:
  """Signal ``pid``'s process group and the pid itself, ignoring races.

  Both are attempted because a descendant may have escaped into its own
  session, in which case killing the parent's group alone would miss it.
  """
  try:
    pgid = os.getpgid(pid)
  except ProcessLookupError:
    return
  try:
    os.killpg(pgid, sig)
  except ProcessLookupError:
    pass
  try:
    os.kill(pid, sig)
  except ProcessLookupError:
    pass


def terminate_process_group(process, grace_sec: float = 3.0) -> None:
  """SIGTERM the process tree, then SIGKILL whatever survives ``grace_sec``."""
  descendants = descendant_pids(process.pid)
  for pid in sorted(descendants, reverse=True):
    signal_pid_or_group(pid, signal.SIGTERM)

  signal_pid_or_group(process.pid, signal.SIGTERM)

  try:
    process.wait(timeout=grace_sec)
    return
  except subprocess.TimeoutExpired:
    pass

  # Re-walk: the tree may have changed while we waited out the grace period.
  descendants = descendant_pids(process.pid)
  for pid in sorted(descendants, reverse=True):
    signal_pid_or_group(pid, signal.SIGKILL)
  signal_pid_or_group(process.pid, signal.SIGKILL)
  process.wait()


def register_process(process) -> None:
  with _ACTIVE_PROCESSES_LOCK:
    _ACTIVE_PROCESSES.add(process)


def unregister_process(process) -> None:
  with _ACTIVE_PROCESSES_LOCK:
    _ACTIVE_PROCESSES.discard(process)


def terminate_active_process_groups() -> None:
  with _ACTIVE_PROCESSES_LOCK:
    processes = list(_ACTIVE_PROCESSES)
  for process in processes:
    terminate_process_group(process)


def signal_active_process_groups(signum: int) -> None:
  """Signal every registered tree without waiting for exit."""
  with _ACTIVE_PROCESSES_LOCK:
    processes = list(_ACTIVE_PROCESSES)
  for process in processes:
    signal_pid_or_group(process.pid, signum)


def shutdown_executor(
    executor: concurrent.futures.Executor,
    *,
    wait: bool,
    cancel_futures: bool,
) -> None:
  """``Executor.shutdown`` with a fallback for Python < 3.9."""
  try:
    executor.shutdown(wait=wait, cancel_futures=cancel_futures)
  except TypeError:
    executor.shutdown(wait=wait)


def install_signal_cleanup(on_shutdown: Optional[Callable[[], None]] = None) -> None:
  """Install idempotent SIGINT/SIGTERM handlers that reap registered children.

  ``on_shutdown`` runs before the child teardown -- callers use it to cancel
  their own work queues (e.g. shutting down a thread pool). Callbacks always
  accumulate even when the handler is already installed, so a caller that
  registers late still gets invoked; only the ``signal.signal`` wiring is
  one-shot.
  """
  global _SIGNAL_CLEANUP_INSTALLED

  if on_shutdown is not None:
    with _SHUTDOWN_CALLBACKS_LOCK:
      if on_shutdown not in _SHUTDOWN_CALLBACKS:
        _SHUTDOWN_CALLBACKS.append(on_shutdown)

  if _SIGNAL_CLEANUP_INSTALLED:
    return

  def handle_signal(signum, _frame):
    SHUTDOWN_REQUESTED.set()
    with _SHUTDOWN_CALLBACKS_LOCK:
      callbacks = list(_SHUTDOWN_CALLBACKS)
    for callback in callbacks:
      # A failing callback must not prevent child teardown, but stays visible.
      try:
        callback()
      except Exception as exc:  # noqa: BLE001 - best-effort shutdown hook
        print("warning: shutdown callback failed: %s" % exc, file=sys.stderr)
    terminate_active_process_groups()
    raise SystemExit(128 + signum)

  try:
    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)
  except ValueError:
    # Not on the main thread; the parent context owns signal handling.
    return
  _SIGNAL_CLEANUP_INSTALLED = True
