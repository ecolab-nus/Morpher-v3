#!/usr/bin/env python
import argparse
import csv
import json
import os
import re
import sys


WRAPPER_NAME_RE = re.compile(r"^SPM_B([0-9]+)_WRAPPER$")


def parse_args():
  parser = argparse.ArgumentParser(
    description="Populate architecture SPM wrapper DATA_LAYOUT entries from mem_alloc."
  )
  parser.add_argument("json_arch_template", help="Architecture JSON template path.")
  parser.add_argument("mem_alloc_file", help="Input mem_alloc file path.")
  parser.add_argument("bank_size", type=int, help="Size of each memory bank in bytes.")
  parser.add_argument("bank_number", type=int, help="Number of memory banks.")
  parser.add_argument("output_json", help="Output path for generated runtime architecture JSON.")
  parser.add_argument(
    "--dry-run",
    action="store_true",
    help="Validate template + mem_alloc and print summary without writing output JSON.",
  )
  return parser.parse_args()


def load_arch_template(path):
  try:
    with open(path, "r", encoding="utf-8") as infile:
      arch = json.load(infile)
  except FileNotFoundError as exc:
    raise RuntimeError("Architecture template not found: %s" % path) from exc
  except json.JSONDecodeError as exc:
    raise RuntimeError("Failed to parse JSON template %s: %s" % (path, str(exc))) from exc

  if not isinstance(arch, dict):
    raise RuntimeError("Architecture template must be a JSON object: %s" % path)

  arch_root = arch.get("ARCH")
  if not isinstance(arch_root, dict):
    raise RuntimeError("Architecture template missing object key ARCH: %s" % path)

  cgra = arch_root.get("CGRA")
  if not isinstance(cgra, dict):
    raise RuntimeError("Architecture template missing object key ARCH.CGRA: %s" % path)

  cgra_submods = cgra.get("SUBMODS")
  if not isinstance(cgra_submods, list):
    raise RuntimeError("Architecture template key ARCH.CGRA.SUBMODS must be a list: %s" % path)

  spm = arch_root.get("SPM")
  if not isinstance(spm, dict):
    raise RuntimeError("Architecture template missing object key ARCH.SPM: %s" % path)

  spm_mem = spm.get("MEM")
  if not isinstance(spm_mem, dict):
    raise RuntimeError("Architecture template missing object key ARCH.SPM.MEM: %s" % path)

  return arch, arch_root, cgra_submods


def load_mem_alloc(path, total_size):
  entries = {}
  try:
    with open(path, "r", encoding="utf-8", newline="") as infile:
      reader = csv.reader(infile)
      header = next(reader, None)
      if header is None:
        raise RuntimeError("mem_alloc file is empty: %s" % path)

      for line_no, row in enumerate(reader, start=2):
        if not row or all(not cell.strip() for cell in row):
          continue
        if len(row) != 2:
          raise RuntimeError(
            "Malformed mem_alloc line %d in %s: expected 2 columns, got %d."
            % (line_no, path, len(row))
          )

        var_name = row[0].strip()
        addr_str = row[1].strip()
        if not var_name:
          continue
        if var_name in entries:
          raise RuntimeError(
            "Duplicate variable name %r at line %d in %s." % (var_name, line_no, path)
          )
        try:
          addr = int(addr_str, 10)
        except ValueError as exc:
          raise RuntimeError(
            "Invalid integer address %r at line %d in %s." % (addr_str, line_no, path)
          ) from exc
        if addr < 0:
          raise RuntimeError("Negative address %d at line %d in %s." % (addr, line_no, path))
        if addr >= total_size:
          raise RuntimeError(
            "Address %d at line %d in %s is outside configured memory [0, %d)."
            % (addr, line_no, path, total_size)
          )
        entries[var_name] = addr
  except FileNotFoundError as exc:
    raise RuntimeError("mem_alloc file not found: %s" % path) from exc

  if not entries:
    raise RuntimeError("mem_alloc file has no usable entries: %s" % path)
  return entries


def split_mem_alloc_by_bank(mem_alloc, bank_size, bank_number):
  bank_entries = [[] for _ in range(bank_number)]
  sorted_entries = sorted(mem_alloc.items(), key=lambda item: (item[1], item[0]))
  for var_name, addr in sorted_entries:
    bank_idx = addr // bank_size
    if bank_idx < 0 or bank_idx >= bank_number:
      raise RuntimeError(
        "Address %d for variable %r maps to invalid bank index %d."
        % (addr, var_name, bank_idx)
      )
    bank_entries[bank_idx].append((var_name, addr))
  return bank_entries


def get_template_wrapper_modules(cgra_submods):
  wrapper_names = set()
  for submod_entry in cgra_submods:
    if not isinstance(submod_entry, dict):
      continue
    for mod_name in submod_entry.keys():
      if WRAPPER_NAME_RE.match(mod_name):
        wrapper_names.add(mod_name)
  return wrapper_names


def build_spm_wrapper_block(entries):
  data_layout = {}
  for var_name, addr in entries:
    data_layout[var_name] = addr
  return {
    "TSOCKETS": ["MEMPORT_P0", "MEMPORT_P1"],
    "SUBMODS": {"SPM": [{"name": "SPM0"}]},
    "DATA_LAYOUT": data_layout,
    "INIT_FILE": "../json/test_data.json",
    "CONNECTIONS": {
      "SPM0.PORT0": "THIS.MEMPORT_P0",
      "SPM0.PORT1": "THIS.MEMPORT_P1",
    },
  }


def update_arch_with_mem_layout(arch_root, cgra_submods, bank_entries, bank_size):
  expected_wrapper_names = {"SPM_B%d_WRAPPER" % idx for idx in range(len(bank_entries))}
  found_wrapper_names = get_template_wrapper_modules(cgra_submods)
  missing = expected_wrapper_names - found_wrapper_names
  extra = found_wrapper_names - expected_wrapper_names
  if missing or extra:
    raise RuntimeError(
      "Template SPM wrapper mismatch. Missing: %s | Extra: %s. "
      "Check bank_number and template CGRA.SUBMODS wrapper instances."
      % (
        ", ".join(sorted(missing)) if missing else "none",
        ", ".join(sorted(extra)) if extra else "none",
      )
    )

  # Remove any stale wrapper module definitions from previous generation.
  stale_keys = [key for key in arch_root.keys() if WRAPPER_NAME_RE.match(key)]
  for key in stale_keys:
    del arch_root[key]

  for bank_idx, entries in enumerate(bank_entries):
    wrapper_name = "SPM_B%d_WRAPPER" % bank_idx
    arch_root[wrapper_name] = build_spm_wrapper_block(entries)

  # Keep template SPM module size aligned to the bank_size used for generation.
  arch_root["SPM"]["MEM"]["SIZE"] = bank_size


def write_output(path, arch):
  output_dir = os.path.dirname(path)
  if output_dir:
    os.makedirs(output_dir, exist_ok=True)
  with open(path, "w", encoding="utf-8") as outfile:
    json.dump(arch, outfile, indent=2)
    outfile.write("\n")


def main():
  args = parse_args()
  if args.bank_size <= 0:
    raise RuntimeError("bank_size must be > 0, got %d." % args.bank_size)
  if args.bank_number <= 0:
    raise RuntimeError("bank_number must be > 0, got %d." % args.bank_number)

  total_size = args.bank_size * args.bank_number
  arch, arch_root, cgra_submods = load_arch_template(args.json_arch_template)
  mem_alloc = load_mem_alloc(args.mem_alloc_file, total_size)
  bank_entries = split_mem_alloc_by_bank(mem_alloc, args.bank_size, args.bank_number)

  update_arch_with_mem_layout(arch_root, cgra_submods, bank_entries, args.bank_size)

  print("Loaded %d mem_alloc entries from %s" % (len(mem_alloc), args.mem_alloc_file))
  for bank_idx, entries in enumerate(bank_entries):
    print("  Bank %d: %d variables" % (bank_idx, len(entries)))

  if args.dry_run:
    print("Dry-run complete: validated inputs and generated in-memory architecture JSON.")
    return

  write_output(args.output_json, arch)
  print("Wrote runtime architecture JSON: %s" % args.output_json)


if __name__ == "__main__":
  try:
    main()
  except RuntimeError as exc:
    print("Error: %s" % str(exc))
    sys.exit(1)
