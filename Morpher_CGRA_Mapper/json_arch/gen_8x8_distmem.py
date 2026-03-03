#!/usr/bin/env python3
"""
Generate hycube_8x8_distmem.json — 8x8 fully distributed memory CGRA.

Every PE is PE_MEM_LOCAL with its own SPM (64 total).
FU_MEM_LOCAL adds compound fused ops (LOAD_ADD_STORE, etc.) at latency=1.
"""

import json
import sys

GRID_X = 8
GRID_Y = 8
SPM_SIZE = 1024


def build_arch():
    arch = {}

    # -------------------------------------------------------------------------
    # MDP_LOCAL — datapath with two separate memory ports (load + store)
    # -------------------------------------------------------------------------
    arch["MDP_LOCAL"] = {
        "INPUTS": ["I1", "I2", "P"],
        "INTERNALS": ["T_INT"],
        "OUTPUTS": ["T"],
        "SOCKETS": ["MEMPORT_LOAD", "MEMPORT_STORE"],
        "CONNECTIONS": {
            "THIS.T_INT": ["THIS.T"]
        }
    }

    # -------------------------------------------------------------------------
    # SPM — scratchpad with two ports (PORT0=load, PORT1=store)
    # -------------------------------------------------------------------------
    arch["SPM"] = {
        "SOCKETS": ["PORT0", "PORT1"],
        "MEM": {"SIZE": SPM_SIZE}
    }

    # -------------------------------------------------------------------------
    # FU_MEM_LOCAL — functional unit with compound ops + all standard ops
    # -------------------------------------------------------------------------
    compound_ops = {
        "LOAD_ADD_STORE":  1,
        "LOAD_SUB_STORE":  1,
        "LOAD_MUL_STORE":  1,
        "LOAD_AND_STORE":  1,
        "LOAD_OR_STORE":   1,
        "LOAD_XOR_STORE":  1,
    }

    standard_ops = {
        "NOP": 2, "ADD": 2, "SUB": 2, "MUL": 2, "SEXT": 2,
        "DIV": 2, "LS": 2, "RS": 2, "ARS": 2, "AND": 2,
        "OR": 2, "XOR": 2, "SELECT": 2, "CMERGE": 2, "CMP": 2,
        "CLT": 2, "BR": 2, "CGT": 2, "MOVCL": 2,
        "LOADCL": 2, "LOAD": 2, "LOADH": 2, "LOADB": 2,
        "STORE": 2, "STOREH": 2, "STOREB": 2,
        "OLOADCL": 2, "OLOAD": 2, "OLOADH": 2, "OLOADB": 2,
        "OSTORE": 2, "OSTOREH": 2, "OSTOREB": 2,
        "JUMPL": 2, "MOVC": 2,
    }

    all_ops = {**standard_ops, **compound_ops}

    arch["FU_MEM_LOCAL"] = {
        "INPUTS": ["DP0_I1", "DP0_I2", "DP0_P"],
        "OUTPUTS": ["DP0_T"],
        "ISOCKETS": ["MEMPORT_LOAD", "MEMPORT_STORE"],
        "SUBMODS": {
            "MDP_LOCAL": [{"name": "DP0"}]
        },
        "OPS": all_ops,
        "CONNECTIONS": {
            "THIS.DP0_I1": ["DP0.I1"],
            "THIS.DP0_I2": ["DP0.I2"],
            "THIS.DP0_P":  ["DP0.P"],
            "DP0.T":       ["THIS.DP0_T"],
            "DP0.MEMPORT_LOAD":  ["THIS.MEMPORT_LOAD"],
            "DP0.MEMPORT_STORE": ["THIS.MEMPORT_STORE"],
        }
    }

    # -------------------------------------------------------------------------
    # PE_MEM_LOCAL — PE with local distributed memory
    # -------------------------------------------------------------------------
    arch["PE_MEM_LOCAL"] = {
        "INPUTS":    ["NORTH_I", "WEST_I", "EAST_I", "SOUTH_I"],
        "OUTPUTS":   ["NORTH_O", "WEST_O", "EAST_O", "SOUTH_O"],
        "INTERNALS": ["NORTH_XBARI", "EAST_XBARI", "WEST_XBARI", "SOUTH_XBARI"],
        "ISOCKETS":  ["MEMPORT_LOAD", "MEMPORT_STORE"],
        "SUBMODS": {
            "FU_MEM_LOCAL": [{"name": "FU0"}]
        },
        "REGS": ["NR", "ER", "WR", "SR", "TREG"],
        "CONNECTIONS": {
            "THIS.NORTH_I": ["THIS.NR", "THIS.NORTH_XBARI"],
            "THIS.NR":      ["THIS.NORTH_XBARI"],

            "THIS.EAST_I":  ["THIS.ER", "THIS.EAST_XBARI"],
            "THIS.ER":      ["THIS.EAST_XBARI"],

            "THIS.WEST_I":  ["THIS.WR", "THIS.WEST_XBARI"],
            "THIS.WR":      ["THIS.WEST_XBARI"],

            "THIS.SOUTH_I": ["THIS.SR", "THIS.SOUTH_XBARI"],
            "THIS.SR":      ["THIS.SOUTH_XBARI"],

            "THIS.NORTH_XBARI": [
                "FU0.DP0_I1", "FU0.DP0_I2", "FU0.DP0_P",
                "THIS.NORTH_O", "THIS.WEST_O", "THIS.EAST_O", "THIS.SOUTH_O"
            ],
            "THIS.EAST_XBARI": [
                "FU0.DP0_I1", "FU0.DP0_I2", "FU0.DP0_P",
                "THIS.NORTH_O", "THIS.WEST_O", "THIS.EAST_O", "THIS.SOUTH_O"
            ],
            "THIS.WEST_XBARI": [
                "FU0.DP0_I1", "FU0.DP0_I2", "FU0.DP0_P",
                "THIS.NORTH_O", "THIS.WEST_O", "THIS.EAST_O", "THIS.SOUTH_O"
            ],
            "THIS.SOUTH_XBARI": [
                "FU0.DP0_I1", "FU0.DP0_I2", "FU0.DP0_P",
                "THIS.NORTH_O", "THIS.WEST_O", "THIS.EAST_O", "THIS.SOUTH_O"
            ],

            "FU0.DP0_T": [
                "THIS.NORTH_O", "THIS.EAST_O", "THIS.WEST_O", "THIS.SOUTH_O",
                "THIS.TREG"
            ],
            "THIS.TREG": [
                "FU0.DP0_I1", "FU0.DP0_I2", "FU0.DP0_P",
                "THIS.NORTH_O", "THIS.EAST_O", "THIS.WEST_O", "THIS.SOUTH_O"
            ],

            "FU0.MEMPORT_LOAD":  ["THIS.MEMPORT_LOAD"],
            "FU0.MEMPORT_STORE": ["THIS.MEMPORT_STORE"],
        }
    }

    # -------------------------------------------------------------------------
    # CGRA grid: 8x8 PE_MEM_LOCAL + 64 SPM instances
    # -------------------------------------------------------------------------
    mods = []
    for y in range(GRID_Y):
        for x in range(GRID_X):
            mods.append({
                "X": x,
                "Y": y,
                "MOD": "PE_MEM_LOCAL",
                "name": f"PE_MEM_LOCAL_X{x}_Y{y}"
            })

    spm_instances = {}
    for y in range(GRID_Y):
        for x in range(GRID_X):
            spm_name = f"SPM_X{x}_Y{y}"
            spm_instances.setdefault("SPM", []).append({"name": spm_name})

    grid_connections = [
        {"FROM_X": "X", "FROM_Y": "Y",   "FROM_PORT": "NORTH_O",
         "TO_X": "X",   "TO_Y": "Y-1",  "TO_PORT": "SOUTH_I"},
        {"FROM_X": "X", "FROM_Y": "Y",   "FROM_PORT": "EAST_O",
         "TO_X": "X+1", "TO_Y": "Y",    "TO_PORT": "WEST_I"},
        {"FROM_X": "X", "FROM_Y": "Y",   "FROM_PORT": "WEST_O",
         "TO_X": "X-1", "TO_Y": "Y",    "TO_PORT": "EAST_I"},
        {"FROM_X": "X", "FROM_Y": "Y",   "FROM_PORT": "SOUTH_O",
         "TO_X": "X",   "TO_Y": "Y+1",  "TO_PORT": "NORTH_I"},
    ]

    submods = [
        {
            "PATTERN": "GRID",
            "DIMS": {"X": GRID_X, "Y": GRID_Y},
            "MODS": mods,
            "CONNECTIONS": grid_connections,
        },
        spm_instances,
    ]

    # Per-PE SPM connections (MEMPORT_LOAD → PORT0, MEMPORT_STORE → PORT1)
    cgra_connections = {}
    for y in range(GRID_Y):
        for x in range(GRID_X):
            pe_name  = f"PE_MEM_LOCAL_X{x}_Y{y}"
            spm_name = f"SPM_X{x}_Y{y}"
            cgra_connections[f"{pe_name}.MEMPORT_LOAD"]  = [f"{spm_name}.PORT0"]
            cgra_connections[f"{pe_name}.MEMPORT_STORE"] = [f"{spm_name}.PORT1"]

    arch["CGRA"] = {
        "SUBMODS": submods,
        "CONNECTIONS": cgra_connections,
    }

    return {"ARCH": arch}


if __name__ == "__main__":
    out_path = "hycube_8x8_distmem.json"
    if len(sys.argv) > 1:
        out_path = sys.argv[1]

    data = build_arch()
    out_str = json.dumps(data, indent=2)
    with open(out_path, "w") as f:
        f.write(out_str)
    print(f"Written to {out_path}")
