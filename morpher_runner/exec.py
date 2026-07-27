import subprocess
from dataclasses import dataclass
from typing import List, Optional

# Process-group lifecycle lives in morpher_runner.proc so the runners and the
# scripts/ entry points all share one registry and one signal handler.
from morpher_runner.proc import install_signal_cleanup
from morpher_runner.proc import register_process as _register_process
from morpher_runner.proc import signal_active_process_groups
from morpher_runner.proc import terminate_active_process_groups
from morpher_runner.proc import terminate_process_group as _terminate_process_group
from morpher_runner.proc import unregister_process as _unregister_process

__all__ = [
  "Command",
  "run_command",
  "run_commands",
  "install_signal_cleanup",
  "signal_active_process_groups",
  "terminate_active_process_groups",
]


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
