import os
import signal
import subprocess
import threading
from dataclasses import dataclass
from typing import List, Optional


@dataclass
class Command:
  argv: List[str]
  cwd: Optional[str] = None
  check: bool = True
  capture_output: bool = False
  text: bool = True
  env: Optional[dict] = None
  stdout: Optional[object] = None
  stderr: Optional[object] = None


_ACTIVE_PROCESSES = set()
_ACTIVE_PROCESSES_LOCK = threading.Lock()
_SIGNAL_CLEANUP_INSTALLED = False


def _terminate_process_group(process):
  try:
    pgid = os.getpgid(process.pid)
  except ProcessLookupError:
    return

  try:
    os.killpg(pgid, signal.SIGTERM)
  except ProcessLookupError:
    return

  try:
    process.wait(timeout=3)
    return
  except subprocess.TimeoutExpired:
    pass

  try:
    os.killpg(pgid, signal.SIGKILL)
  except ProcessLookupError:
    return
  process.wait()


def _register_process(process):
  with _ACTIVE_PROCESSES_LOCK:
    _ACTIVE_PROCESSES.add(process)


def _unregister_process(process):
  with _ACTIVE_PROCESSES_LOCK:
    _ACTIVE_PROCESSES.discard(process)


def terminate_active_process_groups():
  with _ACTIVE_PROCESSES_LOCK:
    processes = list(_ACTIVE_PROCESSES)
  for process in processes:
    _terminate_process_group(process)


def signal_active_process_groups(signum):
  with _ACTIVE_PROCESSES_LOCK:
    processes = list(_ACTIVE_PROCESSES)
  for process in processes:
    try:
      pgid = os.getpgid(process.pid)
    except ProcessLookupError:
      continue
    try:
      os.killpg(pgid, signum)
    except ProcessLookupError:
      continue


def install_signal_cleanup():
  global _SIGNAL_CLEANUP_INSTALLED
  if _SIGNAL_CLEANUP_INSTALLED:
    return

  def handle_signal(signum, _frame):
    signal_active_process_groups(signal.SIGTERM)
    raise SystemExit(128 + signum)

  try:
    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)
  except ValueError:
    return
  _SIGNAL_CLEANUP_INSTALLED = True


def run_command(command):
  install_signal_cleanup()
  process = subprocess.Popen(
    command.argv,
    cwd=command.cwd,
    stdout=subprocess.PIPE if command.capture_output else command.stdout,
    stderr=subprocess.PIPE if command.capture_output else command.stderr,
    text=command.text,
    env=command.env,
    start_new_session=True,
  )
  _register_process(process)
  try:
    stdout, stderr = process.communicate()
  except KeyboardInterrupt:
    _terminate_process_group(process)
    raise
  except BaseException:
    _terminate_process_group(process)
    raise
  finally:
    _unregister_process(process)

  if command.check and process.returncode != 0:
    raise subprocess.CalledProcessError(
      process.returncode,
      command.argv,
      output=stdout,
      stderr=stderr,
    )

  return subprocess.CompletedProcess(
    command.argv,
    process.returncode,
    stdout,
    stderr,
  )


def run_commands(commands):
  results = []
  for command in commands:
    results.append(run_command(command))
  return results
