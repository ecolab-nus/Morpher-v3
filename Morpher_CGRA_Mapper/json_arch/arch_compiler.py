#!/usr/bin/env python3
"""
Compile a CGRA architecture DSL (.cgra) into the existing JSON format.
"""

from __future__ import annotations

import argparse
import ast
import json
import re
from collections import OrderedDict
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, Iterator, List, Optional, Tuple, Union


class ParseError(ValueError):
    pass


@dataclass(frozen=True)
class Line:
    lineno: int
    indent: int
    text: str


@dataclass(frozen=True)
class ModuleDef:
    name: str
    body: List["Stmt"]
    line: int


@dataclass(frozen=True)
class GridDef:
    width: int
    height: int
    body: List["Stmt"]
    line: int


@dataclass(frozen=True)
class Program:
    modules: List[ModuleDef]
    grid: GridDef


@dataclass(frozen=True)
class PortDecl:
    kind: str
    names: List[str]
    line: int


@dataclass(frozen=True)
class MemSizeDecl:
    size: int
    line: int


@dataclass(frozen=True)
class SubDecl:
    mod_type: str
    alias: str
    line: int


@dataclass(frozen=True)
class RegDecl:
    names: List[str]
    line: int


@dataclass(frozen=True)
class OpsDecl:
    latency: int
    ops: List[str]
    line: int


@dataclass(frozen=True)
class ConnectDecl:
    src: str
    dsts: List[str]
    line: int


@dataclass(frozen=True)
class IteratorSpec:
    var: str
    kind: str  # "range" or "list"
    start: Optional[int]
    end: Optional[int]
    values: Optional[List[Union[str, int]]]
    line: int


@dataclass(frozen=True)
class ForStmt:
    iters: List[IteratorSpec]
    body: List["Stmt"]
    line: int


@dataclass(frozen=True)
class PeDecl:
    mod_type: str
    alias: str
    condition: Optional[str]
    line: int


@dataclass(frozen=True)
class RoutingDecl:
    name: str
    line: int


@dataclass(frozen=True)
class MemBlockDecl:
    mod_type: str
    alias_template: str
    body: List["Stmt"]
    line: int


Stmt = Union[
    PortDecl,
    MemSizeDecl,
    SubDecl,
    RegDecl,
    OpsDecl,
    ConnectDecl,
    ForStmt,
    PeDecl,
    RoutingDecl,
    MemBlockDecl,
]


IDENT = r"[A-Za-z_][A-Za-z0-9_]*"


def split_top_level(text: str, delim: str = ",") -> List[str]:
    out: List[str] = []
    depth = 0
    curr: List[str] = []
    for ch in text:
        if ch == "[":
            depth += 1
        elif ch == "]":
            depth -= 1
            if depth < 0:
                raise ParseError(f"Unmatched ']' in: {text}")
        if ch == delim and depth == 0:
            piece = "".join(curr).strip()
            if piece:
                out.append(piece)
            curr = []
            continue
        curr.append(ch)
    if depth != 0:
        raise ParseError(f"Unmatched '[' in: {text}")
    piece = "".join(curr).strip()
    if piece:
        out.append(piece)
    return out


def strip_comment(line: str) -> str:
    if "#" in line:
        line = line.split("#", 1)[0]
    return line.rstrip("\n")


def parse_name_list(text: str, line: int) -> List[str]:
    names = [p.strip() for p in text.split(",")]
    names = [n for n in names if n]
    for n in names:
        if not re.fullmatch(IDENT, n):
            raise ParseError(f"Line {line}: invalid identifier '{n}'")
    return names


def parse_indexed_name(text: str, line: int) -> Tuple[str, List[str]]:
    m = re.fullmatch(rf"({IDENT})((?:\[[^\]]+\])*)", text.strip())
    if not m:
        raise ParseError(f"Line {line}: invalid indexed name '{text}'")
    base = m.group(1)
    suffix = m.group(2)
    exprs = re.findall(r"\[([^\]]+)\]", suffix) if suffix else []
    return base, [e.strip() for e in exprs]


class DSLParser:
    def __init__(self, source: str, file_name: str = "<input>") -> None:
        self.file_name = file_name
        self.lines = self._lex(source)
        self.i = 0

    def _lex(self, source: str) -> List[Line]:
        lines: List[Line] = []
        for lineno, raw in enumerate(source.splitlines(), start=1):
            if "\t" in raw:
                raise ParseError(f"{self.file_name}:{lineno}: tabs are not allowed")
            cooked = strip_comment(raw).rstrip()
            if not cooked.strip():
                continue
            indent = len(cooked) - len(cooked.lstrip(" "))
            text = cooked.strip()
            lines.append(Line(lineno=lineno, indent=indent, text=text))
        return lines

    def parse(self) -> Program:
        modules: List[ModuleDef] = []
        grid: Optional[GridDef] = None
        while self.i < len(self.lines):
            line = self.lines[self.i]
            if line.indent != 0:
                self._error(line, "top-level statements must start at column 1")
            if line.text.startswith("module "):
                modules.append(self._parse_module())
            elif line.text.startswith("grid "):
                if grid is not None:
                    self._error(line, "only one grid block is allowed")
                grid = self._parse_grid()
            else:
                self._error(line, f"unknown top-level statement: '{line.text}'")
        if grid is None:
            raise ParseError(f"{self.file_name}: no grid block found")
        return Program(modules=modules, grid=grid)

    def _parse_module(self) -> ModuleDef:
        line = self.lines[self.i]
        m = re.fullmatch(rf"module\s+({IDENT})\s*:\s*", line.text)
        if not m:
            self._error(line, "invalid module declaration")
        name = m.group(1)
        self.i += 1
        body = self._parse_block(parent_indent=line.indent)
        return ModuleDef(name=name, body=body, line=line.lineno)

    def _parse_grid(self) -> GridDef:
        line = self.lines[self.i]
        m = re.fullmatch(r"grid\s+(\d+)x(\d+)\s*:\s*", line.text)
        if not m:
            self._error(line, "invalid grid declaration, expected: grid WxH:")
        width = int(m.group(1))
        height = int(m.group(2))
        self.i += 1
        body = self._parse_block(parent_indent=line.indent)
        return GridDef(width=width, height=height, body=body, line=line.lineno)

    def _parse_block(self, parent_indent: int) -> List[Stmt]:
        if self.i >= len(self.lines):
            return []
        if self.lines[self.i].indent <= parent_indent:
            return []
        block_indent = self.lines[self.i].indent
        stmts: List[Stmt] = []
        while self.i < len(self.lines):
            line = self.lines[self.i]
            if line.indent < block_indent:
                break
            if line.indent > block_indent:
                self._error(line, "unexpected indentation")
            stmts.append(self._parse_stmt(block_indent))
        return stmts

    def _parse_stmt(self, curr_indent: int) -> Stmt:
        line = self.lines[self.i]
        text = line.text

        m = re.fullmatch(r"for\s+(.+)\s*:\s*", text)
        if m:
            specs = self._parse_iter_specs(m.group(1), line.lineno)
            self.i += 1
            body = self._parse_block(parent_indent=curr_indent)
            if not body:
                self._error(line, "for-loop body cannot be empty")
            return ForStmt(iters=specs, body=body, line=line.lineno)

        m = re.fullmatch(rf"mem\s+({IDENT})\s+as\s+(.+)\s*:\s*", text)
        if m:
            mod_type = m.group(1)
            alias_template = m.group(2).strip()
            self.i += 1
            body = self._parse_block(parent_indent=curr_indent)
            if not body:
                self._error(line, "mem block body cannot be empty")
            return MemBlockDecl(mod_type=mod_type, alias_template=alias_template, body=body, line=line.lineno)

        m = re.fullmatch(r"connect\s+(.+?)\s*(?:->|→)\s*(.+)", text)
        if m:
            src = m.group(1).strip()
            dsts = split_top_level(m.group(2).strip(), ",")
            if not dsts:
                self._error(line, "connect must have at least one destination")
            self.i += 1
            return ConnectDecl(src=src, dsts=dsts, line=line.lineno)

        m = re.fullmatch(r"ops\s+latency\s*=\s*(\d+)\s*:\s*(.+)", text)
        if m:
            latency = int(m.group(1))
            ops = [p.strip() for p in m.group(2).split(",") if p.strip()]
            if not ops:
                self._error(line, "ops declaration cannot be empty")
            self.i += 1
            return OpsDecl(latency=latency, ops=ops, line=line.lineno)

        m = re.fullmatch(rf"sub\s+({IDENT})\s+as\s+({IDENT})\s*", text)
        if m:
            self.i += 1
            return SubDecl(mod_type=m.group(1), alias=m.group(2), line=line.lineno)

        m = re.fullmatch(rf"pe\s+({IDENT})\s+as\s+({IDENT})(?:\s+at\s+(.+))?\s*", text)
        if m:
            self.i += 1
            cond = m.group(3).strip() if m.group(3) is not None else None
            return PeDecl(mod_type=m.group(1), alias=m.group(2), condition=cond, line=line.lineno)

        m = re.fullmatch(rf"routing\s+({IDENT})\s*", text)
        if m:
            self.i += 1
            return RoutingDecl(name=m.group(1), line=line.lineno)

        m = re.fullmatch(r"mem\s+size\s*=\s*(\d+)\s*", text)
        if m:
            self.i += 1
            return MemSizeDecl(size=int(m.group(1)), line=line.lineno)

        m = re.fullmatch(r"reg\s+(.+)", text)
        if m:
            self.i += 1
            return RegDecl(names=parse_name_list(m.group(1), line.lineno), line=line.lineno)

        for keyword in ("in", "out", "internal", "sockets", "isockets"):
            m = re.fullmatch(rf"{keyword}\s+(.+)", text)
            if m:
                self.i += 1
                return PortDecl(kind=keyword, names=parse_name_list(m.group(1), line.lineno), line=line.lineno)

        self._error(line, f"unknown statement: '{text}'")
        raise AssertionError("unreachable")

    def _parse_iter_specs(self, iter_text: str, line: int) -> List[IteratorSpec]:
        def split_iter_clauses(text: str) -> List[str]:
            clauses: List[str] = []
            buf: List[str] = []
            depth = 0
            for ch in text:
                if ch == "[":
                    depth += 1
                elif ch in ("]", ")") and depth > 0:
                    depth -= 1
                if ch == "," and depth == 0:
                    piece = "".join(buf).strip()
                    if piece:
                        clauses.append(piece)
                    buf = []
                    continue
                buf.append(ch)
            piece = "".join(buf).strip()
            if piece:
                clauses.append(piece)
            return clauses

        out: List[IteratorSpec] = []
        for clause in split_iter_clauses(iter_text):
            m = re.fullmatch(rf"({IDENT})\s+in\s+(.+)", clause.strip())
            if not m:
                raise ParseError(f"{self.file_name}:{line}: invalid loop iterator '{clause.strip()}'")
            var = m.group(1)
            rhs = m.group(2).strip()
            r = re.fullmatch(r"\[\s*(-?\d+)\s*\.\.\s*(-?\d+)\s*\)\s*", rhs)
            if r:
                out.append(
                    IteratorSpec(
                        var=var,
                        kind="range",
                        start=int(r.group(1)),
                        end=int(r.group(2)),
                        values=None,
                        line=line,
                    )
                )
                continue
            l = re.fullmatch(r"\[\s*(.*?)\s*\]\s*", rhs)
            if l:
                raw = l.group(1).strip()
                vals: List[Union[str, int]] = []
                if raw:
                    for p in split_top_level(raw, ","):
                        p = p.strip()
                        if re.fullmatch(r"-?\d+", p):
                            vals.append(int(p))
                        else:
                            vals.append(p)
                if not vals:
                    raise ParseError(f"{self.file_name}:{line}: empty symbolic iterator list")
                out.append(IteratorSpec(var=var, kind="list", start=None, end=None, values=vals, line=line))
                continue
            raise ParseError(
                f"{self.file_name}:{line}: invalid iterator domain '{rhs}', "
                "expected [A,B] or [0..N)"
            )
        return out

    def _error(self, line: Line, msg: str) -> None:
        raise ParseError(f"{self.file_name}:{line.lineno}: {msg}")


def substitute_symbols(text: str, bindings: Dict[str, Union[int, str]]) -> str:
    out = text
    for key, val in bindings.items():
        out = out.replace(f"{{{key}}}", str(val))
    return out


def eval_int_expr(expr: str, bindings: Dict[str, Union[int, str]], line: int) -> int:
    try:
        node = ast.parse(expr, mode="eval")
    except SyntaxError as exc:
        raise ParseError(f"Line {line}: invalid expression '{expr}'") from exc

    def walk(n: ast.AST) -> int:
        if isinstance(n, ast.Expression):
            return walk(n.body)
        if isinstance(n, ast.Constant) and isinstance(n.value, int):
            return int(n.value)
        if isinstance(n, ast.Name):
            if n.id not in bindings:
                raise ParseError(f"Line {line}: unknown variable '{n.id}' in expression '{expr}'")
            val = bindings[n.id]
            if not isinstance(val, int):
                raise ParseError(f"Line {line}: variable '{n.id}' is not numeric in expression '{expr}'")
            return val
        if isinstance(n, ast.BinOp) and isinstance(n.op, (ast.Add, ast.Sub)):
            lhs = walk(n.left)
            rhs = walk(n.right)
            return lhs + rhs if isinstance(n.op, ast.Add) else lhs - rhs
        if isinstance(n, ast.UnaryOp) and isinstance(n.op, (ast.UAdd, ast.USub)):
            v = walk(n.operand)
            return v if isinstance(n.op, ast.UAdd) else -v
        raise ParseError(f"Line {line}: unsupported expression '{expr}'")

    return walk(node)


def eval_bool_expr(expr: str, bindings: Dict[str, Union[int, str]], line: int) -> bool:
    try:
        node = ast.parse(expr, mode="eval")
    except SyntaxError as exc:
        raise ParseError(f"Line {line}: invalid condition '{expr}'") from exc

    def walk(n: ast.AST) -> Union[int, bool]:
        if isinstance(n, ast.Expression):
            return walk(n.body)
        if isinstance(n, ast.Constant) and isinstance(n.value, (int, bool)):
            return n.value
        if isinstance(n, ast.Name):
            if n.id not in bindings:
                raise ParseError(f"Line {line}: unknown variable '{n.id}' in condition '{expr}'")
            val = bindings[n.id]
            if not isinstance(val, (int, bool)):
                raise ParseError(f"Line {line}: variable '{n.id}' is not numeric/bool in condition '{expr}'")
            return val
        if isinstance(n, ast.UnaryOp) and isinstance(n.op, ast.Not):
            return not bool(walk(n.operand))
        if isinstance(n, ast.BoolOp) and isinstance(n.op, (ast.And, ast.Or)):
            vals = [bool(walk(v)) for v in n.values]
            return all(vals) if isinstance(n.op, ast.And) else any(vals)
        if isinstance(n, ast.BinOp) and isinstance(n.op, (ast.Add, ast.Sub)):
            lhs = walk(n.left)
            rhs = walk(n.right)
            if not isinstance(lhs, int) or not isinstance(rhs, int):
                raise ParseError(f"Line {line}: arithmetic in condition '{expr}' must be integer")
            return lhs + rhs if isinstance(n.op, ast.Add) else lhs - rhs
        if isinstance(n, ast.Compare):
            left = walk(n.left)
            for op, comparator in zip(n.ops, n.comparators):
                right = walk(comparator)
                if isinstance(op, ast.Eq):
                    ok = left == right
                elif isinstance(op, ast.NotEq):
                    ok = left != right
                elif isinstance(op, ast.Lt):
                    ok = left < right
                elif isinstance(op, ast.LtE):
                    ok = left <= right
                elif isinstance(op, ast.Gt):
                    ok = left > right
                elif isinstance(op, ast.GtE):
                    ok = left >= right
                else:
                    raise ParseError(f"Line {line}: unsupported comparator in condition '{expr}'")
                if not ok:
                    return False
                left = right
            return True
        raise ParseError(f"Line {line}: unsupported condition '{expr}'")

    return bool(walk(node))


def iterate_bindings(iters: List[IteratorSpec], base: Dict[str, Union[int, str]]) -> Iterator[Dict[str, Union[int, str]]]:
    if not iters:
        yield dict(base)
        return
    head = iters[0]
    tail = iters[1:]
    if head.kind == "range":
        assert head.start is not None and head.end is not None
        values: Iterable[Union[int, str]] = range(head.start, head.end)
    else:
        assert head.values is not None
        values = head.values
    for value in values:
        nxt = dict(base)
        nxt[head.var] = value
        yield from iterate_bindings(tail, nxt)


class ArchCompiler:
    def __init__(self, program: Program) -> None:
        self.program = program
        self.module_index = {m.name: m for m in program.modules}

    def compile(self) -> Dict[str, Any]:
        arch: "OrderedDict[str, Any]" = OrderedDict()
        for module in self.program.modules:
            arch[module.name] = self._compile_module(module)
        arch["CGRA"] = self._compile_grid(self.program.grid)
        return {"ARCH": arch}

    def _compile_module(self, module: ModuleDef) -> Dict[str, Any]:
        fields: "OrderedDict[str, Any]" = OrderedDict()
        connections: "OrderedDict[str, List[str]]" = OrderedDict()

        def ensure_array_field(key: str) -> List[Any]:
            if key not in fields:
                fields[key] = []
            return fields[key]

        def ensure_map_field(key: str) -> "OrderedDict[str, Any]":
            if key not in fields:
                fields[key] = OrderedDict()
            return fields[key]

        def add_connection(src: str, dst: str) -> None:
            if src not in connections:
                connections[src] = []
            if dst not in connections[src]:
                connections[src].append(dst)

        def emit_connect(stmt: ConnectDecl, bindings: Dict[str, Union[int, str]]) -> None:
            raw_src = substitute_symbols(stmt.src, bindings).strip()
            raw_dsts = [substitute_symbols(d, bindings).strip() for d in stmt.dsts]
            src = self._qualify_module_endpoint(raw_src)
            for raw_dst in raw_dsts:
                dst = self._qualify_module_endpoint(raw_dst)
                add_connection(src, dst)

        def expand_stmt(stmt: Stmt, bindings: Dict[str, Union[int, str]]) -> None:
            if isinstance(stmt, ConnectDecl):
                emit_connect(stmt, bindings)
                return
            if isinstance(stmt, ForStmt):
                for expanded in iterate_bindings(stmt.iters, bindings):
                    for inner in stmt.body:
                        expand_stmt(inner, expanded)
                return
            raise ParseError(f"Line {getattr(stmt, 'line', '?')}: only connect/for are allowed in module loops")

        for stmt in module.body:
            if isinstance(stmt, PortDecl):
                key = {
                    "in": "INPUTS",
                    "out": "OUTPUTS",
                    "internal": "INTERNALS",
                    "sockets": "SOCKETS",
                    "isockets": "ISOCKETS",
                }[stmt.kind]
                ensure_array_field(key).extend(stmt.names)
            elif isinstance(stmt, MemSizeDecl):
                if "MEM" in fields:
                    raise ParseError(f"Line {stmt.line}: duplicate mem declaration")
                fields["MEM"] = {"SIZE": stmt.size}
            elif isinstance(stmt, SubDecl):
                submods = ensure_map_field("SUBMODS")
                if stmt.mod_type not in submods:
                    submods[stmt.mod_type] = []
                submods[stmt.mod_type].append({"name": stmt.alias})
            elif isinstance(stmt, RegDecl):
                ensure_array_field("REGS").extend(stmt.names)
            elif isinstance(stmt, OpsDecl):
                ops = ensure_map_field("OPS")
                for op in stmt.ops:
                    if op in ops:
                        raise ParseError(f"Line {stmt.line}: duplicate op '{op}'")
                    ops[op] = stmt.latency
            elif isinstance(stmt, (ConnectDecl, ForStmt)):
                expand_stmt(stmt, {})
            elif isinstance(stmt, (PeDecl, RoutingDecl, MemBlockDecl)):
                raise ParseError(f"Line {stmt.line}: statement is only valid inside grid blocks")
            else:
                raise ParseError(f"Line {getattr(stmt, 'line', '?')}: unsupported statement")

        if connections:
            fields["CONNECTIONS"] = connections
        return fields

    def _compile_grid(self, grid: GridDef) -> Dict[str, Any]:
        fields: "OrderedDict[str, Any]" = OrderedDict()
        connections: "OrderedDict[str, List[str]]" = OrderedDict()
        mem_submods: "OrderedDict[str, List[Dict[str, str]]]" = OrderedDict()
        extra_submods: "OrderedDict[str, List[Dict[str, str]]]" = OrderedDict()

        pe_decls: List[PeDecl] = []
        routing_stmts: List[RoutingDecl] = []
        grid_subdecls: List[SubDecl] = []
        loop_and_mem_stmts: List[Stmt] = []
        for stmt in grid.body:
            if isinstance(stmt, PeDecl):
                pe_decls.append(stmt)
            elif isinstance(stmt, RoutingDecl):
                routing_stmts.append(stmt)
            elif isinstance(stmt, SubDecl):
                grid_subdecls.append(stmt)
            else:
                loop_and_mem_stmts.append(stmt)

        if not pe_decls:
            raise ParseError(f"Line {grid.line}: grid requires a 'pe TYPE as ALIAS' declaration")
        default_pe_decls = [d for d in pe_decls if d.condition is None]
        if len(default_pe_decls) != 1:
            raise ParseError(f"Line {grid.line}: grid requires exactly one default pe declaration without 'at ...'")
        default_pe = default_pe_decls[0]
        has_conditional_pe = any(d.condition is not None for d in pe_decls)

        instance_lookup: Dict[Tuple[str, Tuple[int, ...]], str] = {}
        grid_mods: List[Dict[str, Any]] = []
        for y in range(grid.height):
            for x in range(grid.width):
                chosen = default_pe
                for decl in pe_decls:
                    if decl.condition is None:
                        continue
                    if eval_bool_expr(decl.condition, {"x": x, "y": y}, decl.line):
                        chosen = decl

                include_name = (not has_conditional_pe) or (chosen.condition is not None)
                mod_entry: Dict[str, Any] = {"X": x, "Y": y, "MOD": chosen.mod_type}
                if include_name:
                    inst_name = f"{chosen.mod_type}_X{x}_Y{y}"
                    mod_entry["name"] = inst_name
                else:
                    # Matches CGRA.cpp ExpandPattern auto-naming when "name" is omitted.
                    inst_name = f"{chosen.mod_type}_X{x}|_Y{y}|"
                grid_mods.append(mod_entry)
                idx = (x, y)
                instance_lookup[(chosen.alias, idx)] = inst_name
                instance_lookup[(chosen.mod_type, idx)] = inst_name

        for subdecl in grid_subdecls:
            if subdecl.mod_type not in extra_submods:
                extra_submods[subdecl.mod_type] = []
            extra_submods[subdecl.mod_type].append({"name": subdecl.alias})

        pattern: "OrderedDict[str, Any]" = OrderedDict()
        pattern["PATTERN"] = "GRID"
        pattern["DIMS"] = {"X": grid.width, "Y": grid.height}
        pattern["MODS"] = grid_mods
        pattern["CONNECTIONS"] = []

        for routing in routing_stmts:
            if routing.name != "mesh4":
                raise ParseError(f"Line {routing.line}: unsupported routing '{routing.name}'")
            pattern["CONNECTIONS"].extend(
                [
                    {"FROM_X": "X", "FROM_Y": "Y", "FROM_PORT": "NORTH_O", "TO_X": "X", "TO_Y": "Y-1", "TO_PORT": "SOUTH_I"},
                    {"FROM_X": "X", "FROM_Y": "Y", "FROM_PORT": "EAST_O", "TO_X": "X+1", "TO_Y": "Y", "TO_PORT": "WEST_I"},
                    {"FROM_X": "X", "FROM_Y": "Y", "FROM_PORT": "WEST_O", "TO_X": "X-1", "TO_Y": "Y", "TO_PORT": "EAST_I"},
                    {"FROM_X": "X", "FROM_Y": "Y", "FROM_PORT": "SOUTH_O", "TO_X": "X", "TO_Y": "Y+1", "TO_PORT": "NORTH_I"},
                ]
            )

        def add_connection(src: str, dst: str) -> None:
            if src not in connections:
                connections[src] = []
            if dst not in connections[src]:
                connections[src].append(dst)

        def resolve_grid_ref(token: str, bindings: Dict[str, Union[int, str]], line: int) -> Optional[str]:
            base, exprs = parse_indexed_name(token, line)
            if not exprs:
                return base
            idx_vals: List[int] = []
            for expr in exprs:
                rendered = substitute_symbols(expr, bindings)
                idx_vals.append(eval_int_expr(rendered, bindings, line))
            return instance_lookup.get((base, tuple(idx_vals)))

        def eval_endpoint(token: str, bindings: Dict[str, Union[int, str]], line: int) -> Optional[str]:
            rendered = substitute_symbols(token.strip(), bindings)
            if "." not in rendered:
                return rendered
            left, right = rendered.split(".", 1)
            left_name = resolve_grid_ref(left, bindings, line)
            if left_name is None:
                return None
            return f"{left_name}.{right}"

        def register_mem_instance(block: MemBlockDecl, bindings: Dict[str, Union[int, str]]) -> None:
            base, exprs = parse_indexed_name(block.alias_template, block.line)
            if not exprs:
                raise ParseError(f"Line {block.line}: mem alias must be indexed, e.g., SPM[x][y]")
            idx_vals: List[int] = []
            for expr in exprs:
                rendered = substitute_symbols(expr, bindings)
                idx_vals.append(eval_int_expr(rendered, bindings, block.line))
            if len(idx_vals) == 2:
                inst_name = f"{base}_X{idx_vals[0]}_Y{idx_vals[1]}"
            elif len(idx_vals) == 1:
                inst_name = f"{base}_I{idx_vals[0]}"
            else:
                joined = "_".join(f"I{i}{v}" for i, v in enumerate(idx_vals))
                inst_name = f"{base}_{joined}"
            if block.mod_type not in mem_submods:
                mem_submods[block.mod_type] = []
            if {"name": inst_name} not in mem_submods[block.mod_type]:
                mem_submods[block.mod_type].append({"name": inst_name})

            idx = tuple(idx_vals)
            instance_lookup[(base, idx)] = inst_name
            if (block.mod_type, idx) not in instance_lookup:
                instance_lookup[(block.mod_type, idx)] = inst_name

        def expand_stmt(stmt: Stmt, bindings: Dict[str, Union[int, str]]) -> None:
            if isinstance(stmt, ConnectDecl):
                src = eval_endpoint(stmt.src, bindings, stmt.line)
                if src is None:
                    return
                for dst_tok in stmt.dsts:
                    dst = eval_endpoint(dst_tok, bindings, stmt.line)
                    if dst is None:
                        continue
                    add_connection(src, dst)
                return
            if isinstance(stmt, ForStmt):
                for expanded in iterate_bindings(stmt.iters, bindings):
                    for inner in stmt.body:
                        expand_stmt(inner, expanded)
                return
            if isinstance(stmt, MemBlockDecl):
                register_mem_instance(stmt, bindings)
                for inner in stmt.body:
                    expand_stmt(inner, bindings)
                return
            if isinstance(stmt, (PeDecl, RoutingDecl, SubDecl)):
                return
            raise ParseError(f"Line {getattr(stmt, 'line', '?')}: unsupported grid statement")

        for stmt in loop_and_mem_stmts:
            expand_stmt(stmt, {})

        submods_list: List[Dict[str, Any]] = [pattern]
        if extra_submods:
            submods_list.append(extra_submods)
        if mem_submods:
            submods_list.append(mem_submods)
        fields["SUBMODS"] = submods_list
        if connections:
            fields["CONNECTIONS"] = connections
        return fields

    @staticmethod
    def _qualify_module_endpoint(endpoint: str) -> str:
        endpoint = endpoint.strip()
        return endpoint if "." in endpoint else f"THIS.{endpoint}"


def compile_source(source: str, file_name: str = "<input>") -> Dict[str, Any]:
    parser = DSLParser(source, file_name=file_name)
    program = parser.parse()
    compiler = ArchCompiler(program)
    return compiler.compile()


def main() -> None:
    ap = argparse.ArgumentParser(description="Compile CGRA DSL (.cgra) to JSON arch format")
    ap.add_argument("input", type=Path, help="Input .cgra file")
    ap.add_argument("-o", "--output", type=Path, required=True, help="Output JSON file")
    args = ap.parse_args()

    source = args.input.read_text(encoding="utf-8")
    output = compile_source(source, file_name=str(args.input))
    args.output.write_text(json.dumps(output, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
