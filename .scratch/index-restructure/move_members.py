#!/usr/bin/env python3
"""Throwaway mover for quest index-restructure: moves basic_index's multi-line member function
definitions out of the class body into one header per mechanism (map.txt), as out-of-class
template definitions. Bodies keep their statements; only indentation changes.

  move_members.py list  INDEX_HH            members and line ranges, unassigned ones marked
  move_members.py apply INDEX_HH MAP [--inline]  rewrites INDEX_HH, writes the pieces beside it

A definition on one line stays in the class. Declarations keep the leading comments, defaults and
specifiers; definitions drop static/explicit/virtual/friend and default arguments, gain the
template header and the class qualifier, and take a trailing return type when the return type
names a member of the class (it would not resolve at namespace scope).
"""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from pathlib import Path

CLASS = "basic_index"


@dataclass
class Item:
    kind: str  # "func", "decl", "type", "label"
    start: int  # offset of the item's first code character
    end: int  # offset one past its last character
    body: int = -1  # offset of the body's "{" (func)
    text: str = ""


def mask_code(src: str) -> str:
    """The source with comments, strings and chars blanked (same length, newlines kept)."""
    out = list(src)
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2) + 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        elif c == '"' or (c == "'" and not (i > 0 and src[i - 1].isalnum())):
            q = c
            j = i + 1
            while src[j] != q:
                j += 2 if src[j] == "\\" else 1
            for k in range(i + 1, j):
                out[k] = " "
            i = j + 1
        else:
            i += 1
    return "".join(out)


def match(code: str, i: int, open_: str, close: str) -> int:
    depth = 0
    for j in range(i, len(code)):
        if code[j] == open_:
            depth += 1
        elif code[j] == close:
            depth -= 1
            if depth == 0:
                return j
    raise ValueError(f"unbalanced {open_} at {i}")


def class_span(src: str, code: str) -> tuple[int, int]:
    m = re.search(r"^template <class Layout>\nclass basic_index \{\n", src, re.M)
    assert m
    open_ = m.end() - 2
    return open_, match(code, open_, "{", "}")


def items_of(code: str, lo: int, hi: int) -> list[Item]:
    items: list[Item] = []
    i = lo
    start = -1
    paren = 0
    while i < hi:
        c = code[i]
        if start < 0:
            if c.isspace():
                i += 1
                continue
            start = i
        if c == "(":
            paren += 1
        elif c == ")":
            paren -= 1
        elif c == ":" and paren == 0 and code[start:i].strip() in ("public", "private", "protected"):
            items.append(Item("label", start, i + 1))
            start = -1
        elif c == ";" and paren == 0:
            items.append(Item("decl", start, i + 1))
            start = -1
        elif c == "{" and paren == 0:
            head = code[start:i]
            close = match(code, i, "{", "}")
            bare = re.sub(r"^\s*(template\s*<[^{;]*?>\s*)+", "", head)
            if re.match(r"\s*(struct|class|union|enum)\b", bare):
                semi = code.index(";", close)
                items.append(Item("type", start, semi + 1))
                i, start = semi + 1, -1
                continue
            if "(" in bare and not re.search(r"(?<![=!<>])=(?!=)", bare.split("(")[0].replace("operator=", "")):
                items.append(Item("func", start, close + 1, body=i))
                i, start = close + 1, -1
                continue
            i = close + 1  # a brace initializer
            continue
        i += 1
    assert start < 0, code[start : start + 200]
    return items


def member_names(src: str, code: str, items: list[Item]) -> set[str]:
    names = {CLASS}
    for it in items:
        t = code[it.start : it.end]
        t = re.sub(r"^\s*(template\s*<[^{;]*?>\s*)+", "", t)
        m = re.match(r"\s*(?:struct|class|union|enum(?:\s+class)?)\s+(\w+)", t)
        if m:
            names.add(m.group(1))
        m = re.match(r"\s*using\s+(\w+)\s*=", t)
        if m:
            names.add(m.group(1))
    return names


def split_top(s: str, sep: str = ",") -> list[str]:
    parts, depth, cur = [], 0, []
    for ch in s:
        if ch in "(<[{":
            depth += 1
        elif ch in ")>]}":
            depth -= 1
        if ch == sep and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    parts.append("".join(cur))
    return parts


def strip_default(param: str) -> str:
    depth = 0
    for j, ch in enumerate(param):
        if ch in "(<[{":
            depth += 1
        elif ch in ")>]}":
            depth -= 1
        elif ch == "=" and depth == 0 and param[j - 1] not in "=!<>" and param[j + 1 : j + 2] != "=":
            return param[:j].rstrip()
    return param


def strip_defaults(lst: str) -> str:
    return ",".join(strip_default(p) for p in split_top(lst))


@dataclass
class Func:
    item: Item
    name: str
    line0: int  # 1-based first line
    line1: int  # 1-based last line
    decl: str  # replacement text in the class (indented)
    defn: str  # out-of-class definition (namespace level)


def build(src: str, code: str, it: Item, names: set[str], add_inline: bool) -> Func:
    line_start = src.rfind("\n", 0, it.start) + 1
    indent = src[line_start : it.start]
    assert indent == "\t", (indent, src[it.start : it.start + 80])
    sig = src[it.start : it.body].rstrip()
    csig = code[it.start : it.body].rstrip()
    # The definition's text, one tab less on every line after the first.
    dedent = lambda t: t.replace("\n\t", "\n")  # noqa: E731
    # Template headers of the member itself.
    heads = []
    k = 0
    while True:
        m = re.match(r"\s*template\s*<", csig[k:])
        if not m:
            break
        a = k + m.end() - 1
        b = match(csig, a, "<", ">")
        heads.append(sig[k:a].strip() + " <" + strip_defaults(sig[a + 1 : b]) + ">")
        k = b + 1
    while csig[k].isspace():
        k += 1
    p0 = csig.index("(", k)
    if csig[k:p0].rstrip().endswith("operator"):
        p0 = csig.index("(", p0 + 1)
    p1 = match(csig, p0, "(", ")")
    lead = sig[k:p0]
    params = sig[p0 + 1 : p1]
    tail = sig[p1 + 1 :]
    m = re.match(r"(?s)(.*?)(~?\b\w+|operator\S+)\s*$", lead)
    assert m, lead
    pre, name = m.group(1), m.group(2)
    attrs = re.findall(r"\[\[[^\]]*\]\]\s*", pre)
    pre = re.sub(r"\[\[[^\]]*\]\]\s*", "", pre)
    specs = []
    while True:
        m = re.match(r"(static|explicit|virtual|friend|constexpr|inline)\s+", pre)
        if not m:
            break
        if m.group(1) in ("constexpr", "inline"):
            specs.append(m.group(1))
        pre = pre[m.end() :]
    ret = pre.strip()
    if add_inline and "constexpr" not in specs and "inline" not in specs:
        specs.insert(0, "inline")
    trailing = ret and any(t in names for t in re.findall(r"(?<!::)(?<!\w)\b[A-Za-z_]\w*", ret))
    req = re.search(r"\s*\brequires\b", tail)
    quals, reqs = (tail[: req.start()], tail[req.start() :]) if req else (tail, "")
    if trailing:
        quals = quals.rstrip() + " -> " + ret
        ret = "auto"
    out = ["template <class Layout>"] + heads
    first = "".join(attrs) + " ".join(specs + ([ret] if ret else []))
    first = (first + " " if first else "") + f"{CLASS}<Layout>::{name}(" + strip_defaults(params) + ")" + quals + reqs
    out.append(dedent(first))
    body = src[it.body : it.end]
    after_close = src[it.end : src.find("\n", it.end)]
    assert after_close.strip() == "" or after_close.strip().startswith("//"), after_close
    defn = "\n".join(out) + (" " if not reqs else "\n") + dedent(body) + after_close
    if reqs:
        assert src[it.body - 2 : it.body] == "\n\t", repr(src[it.body - 4 : it.body + 2])
    decl = sig + ";"
    line0 = src.count("\n", 0, it.start) + 1
    line1 = src.count("\n", 0, it.end) + 1
    return Func(it, name, line0, line1, decl, defn)


def parse(path: Path, add_inline: bool = False):
    src = path.read_text()
    code = mask_code(src)
    lo, hi = class_span(src, code)
    items = items_of(code, lo + 1, hi)
    names = member_names(src, code, items)
    funcs = []
    for it in items:
        if it.kind != "func":
            continue
        if "\n" not in src[it.start : it.end]:
            continue  # one-line definitions stay in the class
        funcs.append(build(src, code, it, names, add_inline))
    return src, code, lo, hi, items, funcs


def read_map(path: Path) -> list[tuple[str, str, list[str]]]:
    """map.txt: `header: one-line description` then indented member names (name or name@line)."""
    groups = []
    for raw in path.read_text().splitlines():
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        if not raw[0].isspace():
            h, _, d = raw.partition(":")
            groups.append((h.strip(), d.strip(), []))
        else:
            groups[-1][2].extend(raw.split())
    return groups


def assign(funcs: list[Func], groups) -> dict[int, str]:
    where: dict[int, str] = {}
    for h, _, keys in groups:
        for key in keys:
            name, _, line = key.partition("@")
            hits = [f for f in funcs if f.name == name and (not line or f.line0 <= int(line) <= f.line1)]
            assert hits, f"map names no moved member: {key}"
            for f in hits:
                assert f.line0 not in where, f"{key} assigned twice"
                where[f.line0] = h
    return where


def main() -> int:
    cmd, path = sys.argv[1], Path(sys.argv[2])
    add_inline = "--inline" in sys.argv
    src, code, lo, hi, items, funcs = parse(path, add_inline)
    if cmd == "list":
        groups = read_map(Path(sys.argv[3])) if len(sys.argv) > 3 and not sys.argv[3].startswith("--") else []
        where = assign(funcs, groups) if groups else {}
        for f in funcs:
            print(f"{f.line0:5}-{f.line1:5} {f.line1 - f.line0 + 1:4} {where.get(f.line0, '?'):24} {f.name}")
        tot: dict[str, int] = {}
        for f in funcs:
            tot[where.get(f.line0, "?")] = tot.get(where.get(f.line0, "?"), 0) + f.line1 - f.line0 + 1
        for h, n in sorted(tot.items()):
            print(f"# {h}: {n} lines")
        return 0
    groups = read_map(Path(sys.argv[3]))
    where = assign(funcs, groups)
    missing = [f"{f.name}@{f.line0}" for f in funcs if f.line0 not in where]
    assert not missing, f"unassigned: {missing}"
    # The class with each definition replaced by its declaration.
    out, pos = [], 0
    for f in funcs:
        out.append(src[pos : f.item.start])
        out.append(f.decl)
        pos = f.item.end
    out.append(src[pos:])
    new = "".join(out)
    tail = "\n}  // namespace strata\n"
    assert new.endswith(tail)
    incs = "".join(f'#include "strata/core/{h}"\n' for h, _, _ in groups)
    new += "\n// The member definitions, one header per mechanism (docs/mechanisms/index-layout.md).\n" + incs
    path.write_text(new)
    for h, desc, _ in groups:
        defs = [f.defn for f in funcs if where[f.line0] == h]
        piece = f"// {desc}\n#pragma once\n\n#include \"strata/core/index.hh\"\n\nnamespace strata {{\n\n"
        piece += "\n\n".join(defs) + "\n\n}  // namespace strata\n"
        (path.parent / h).write_text(piece)
    return 0


if __name__ == "__main__":
    sys.exit(main())
