import os
import signal
import subprocess
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


def run_command(command):
  process = subprocess.Popen(
    command.argv,
    cwd=command.cwd,
    stdout=subprocess.PIPE if command.capture_output else command.stdout,
    stderr=subprocess.PIPE if command.capture_output else command.stderr,
    text=command.text,
    env=command.env,
    start_new_session=True,
  )
  try:
    stdout, stderr = process.communicate()
  except KeyboardInterrupt:
    _terminate_process_group(process)
    raise
  except BaseException:
    _terminate_process_group(process)
    raise

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
