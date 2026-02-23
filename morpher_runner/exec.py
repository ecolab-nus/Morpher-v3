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


def run_command(command):
  return subprocess.run(
    command.argv,
    cwd=command.cwd,
    check=command.check,
    capture_output=command.capture_output,
    text=command.text,
  )


def run_commands(commands):
  results = []
  for command in commands:
    results.append(run_command(command))
  return results

