import sys

if len(sys.argv) != 6:
    print(f"Usage: {sys.argv[0]} <json_arch> <mem_alloc_file> <bank_size> <bank_number> <mem_json_arch>")
    sys.exit(1)

json_arch = sys.argv[1]
mem_alloc_file = sys.argv[2]
bank_size = int(sys.argv[3])
bank_number = int(sys.argv[4])
mem_json_arch = sys.argv[5]


def read_mem_alloc():
    """Read mem_alloc.txt, skipping blank lines and entries with empty var_names."""
    mem_alloc = {}
    with open(mem_alloc_file, 'r') as f:
        lines = f.readlines()
    for line in lines[1:]:  # skip header
        line = line.strip()
        if not line:
            continue
        parts = [x.strip() for x in line.split(',')]
        if len(parts) != 2:
            print(f"Warning: skipping malformed line in mem_alloc: {line!r}")
            continue
        var_name, addr_str = parts
        if not var_name:
            continue  # skip entries with no variable name
        mem_alloc[var_name] = int(addr_str)
    return mem_alloc


def generate_spm_blocks(mem_alloc):
    """Generate SPM wrapper JSON blocks for each bank."""
    lines = []
    for bank_idx in range(bank_number):
        bank_start = bank_idx * bank_size
        bank_end = (bank_idx + 1) * bank_size
        bank_vars = {var: addr for var, addr in mem_alloc.items()
                     if bank_start <= addr < bank_end}

        lines.append(f'\n"SPM_B{bank_idx}_WRAPPER" : {{')
        lines.append('\n  "TSOCKETS" : [')
        lines.append('\n    "MEMPORT_P0", "MEMPORT_P1"')
        lines.append('\n  ],')
        lines.append('\n  "SUBMODS" : {')
        lines.append('\n    "SPM" : [{"name" : "SPM0"}]')
        lines.append('\n  },')
        lines.append('\n  "DATA_LAYOUT" :{')
        entries = [f'\n    "{var}":{addr}' for var, addr in bank_vars.items()]
        for i, entry in enumerate(entries):
            lines.append(entry + (',' if i < len(entries) - 1 else ''))
        lines.append('\n  },')
        lines.append('\n  "INIT_FILE" : "../json/test_data.json",')
        lines.append('\n  "CONNECTIONS" : {')
        lines.append('\n    "SPM0.PORT0" : "THIS.MEMPORT_P0",')
        lines.append('\n    "SPM0.PORT1" : "THIS.MEMPORT_P1"')
        lines.append('\n  }')
        lines.append('\n},')
        lines.append('\n')
    return lines


mem_alloc = read_mem_alloc()

with open(json_arch, 'r') as f:
    arch_lines = f.readlines()

new_lines = []
for line in arch_lines:
    if '"CGRA" :{' in line:
        for spm_line in generate_spm_blocks(mem_alloc):
            new_lines.append(spm_line)
    new_lines.append(line)

with open(mem_json_arch, 'w') as f:
    f.writelines(new_lines)
