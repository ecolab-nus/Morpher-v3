"""Central logging setup for the morpher_runner package.

Module code under ``morpher_runner/`` used to call ``print()`` directly. That
made it impossible to silence, redirect, or level-filter runner diagnostics
without also swallowing subprocess output.

The formatter is deliberately bare ``%(message)s`` on **stdout**, so emitted
text is byte-identical to the ``print()`` calls it replaced. This matters:
``scripts/run_all_benchmarks.py`` scrapes these lines for stage markers
(``STAGE_MARKERS``) and failure classification, and the per-kernel log files are
the captured stdout of ``run_morpher.py``. Adding a level/logger prefix here
would be a silent, load-bearing change to that contract.

One intentional behaviour difference: ``logging.StreamHandler`` flushes after
every record, while ``print()`` to a pipe is block-buffered. Runner messages now
interleave correctly with subprocess output in captured logs instead of
sometimes appearing after it.
"""

from __future__ import annotations

import logging
import sys

_CONFIGURED = False

# Diagnostics that were previously unconditional print() calls.
DEFAULT_LEVEL = logging.INFO


def configure_logging(level: int = DEFAULT_LEVEL, stream=None) -> None:
  """Attach the runner's stdout handler. Idempotent; safe to call repeatedly."""
  global _CONFIGURED
  logger = logging.getLogger("morpher_runner")
  if _CONFIGURED:
    logger.setLevel(level)
    return

  handler = logging.StreamHandler(stream if stream is not None else sys.stdout)
  handler.setFormatter(logging.Formatter("%(message)s"))
  logger.addHandler(handler)
  logger.setLevel(level)
  # Runner output is a leaf channel; don't also hand records to the root logger.
  logger.propagate = False
  _CONFIGURED = True


def get_logger(name: str) -> logging.Logger:
  """Return a package logger, configuring the handler on first use.

  Configuring lazily keeps ``import morpher_runner.mapper`` usable from ad-hoc
  scripts and tests without each one having to set logging up first.
  """
  if not _CONFIGURED:
    configure_logging()
  return logging.getLogger(name)
