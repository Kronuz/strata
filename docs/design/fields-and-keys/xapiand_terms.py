#!/usr/bin/env python3
"""Indexes a corpus with a real Xapiand (Xapian engine) and dumps every document's terms.

Usage: xapiand_terms.py BIN CORPUS_NDJSON OUTDIR NAME

Starts BIN (--solo, a fresh data directory under OUTDIR), PUTs each line at /NAME/<n> as the docs
suite does with bank, commits, stops the node, and runs `xapian-delve -1 -v -r <n>` on each shard
(the installed Xapian tool; no Xapian source is read). Writes OUTDIR/NAME.terms: one line per
(document, term), `shard:docid <TAB> wdf <TAB> term-hex`. Terms are Xapiand's real ones: `_id`, the
geo `_point`, the namespace's partial paths, stems, global and accuracy terms included.
"""

import http.client
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

BIN, CORPUS, OUT, NAME = (
    os.path.abspath(sys.argv[1]),
    sys.argv[2],
    os.path.abspath(sys.argv[3]),
    sys.argv[4],
)
PORT = int(os.environ.get("PORT", "18997"))


def call(method, path, body=None):
    c = http.client.HTTPConnection("localhost", PORT, timeout=60)
    c.request(method, path, body=body, headers={"Content-Type": "application/json"})
    r = c.getresponse()
    return r.status, r.read()


def main():
    data = os.path.join(OUT, f"data-{NAME}")
    shutil.rmtree(data, ignore_errors=True)
    node = subprocess.Popen(
        [BIN, "--port", str(PORT), "--name", "fknode", "--solo", "-D", data],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        for _ in range(120):
            try:
                if call("GET", "/")[0] == 200:
                    break
            except OSError:
                time.sleep(0.5)
        lines = Path(CORPUS).read_text().splitlines()
        for n, line in enumerate(lines, 1):
            status, body = call(
                "PUT", f"/{NAME}/{n}" + ("?commit" if n == len(lines) else ""), line
            )
            assert status in (200, 201, 204), (n, status, body[:200])
    finally:
        node.terminate()
        node.wait()
    shards = [
        r for r, _, fs in os.walk(data) if "iamglass" in fs and f"/{NAME}/" in r + "/"
    ]
    # delve 2.1 prints each term raw, then " wdf termfreq"; checked below against the shard's total length
    term_re = re.compile(rb"(.*?) (\d+) (\d+)\n", re.DOTALL)
    docs = 0
    with open(os.path.join(OUT, f"{NAME}.terms"), "w") as o:
        for s, shard in enumerate(sorted(shards)):
            info = subprocess.run(
                ["xapian-delve", shard], capture_output=True, text=True, check=True
            ).stdout
            last = int(
                re.search(r"highest document id ever used = (\d+)", info).group(1)
            )
            total = (
                float(re.search(r"average document length = ([\d.]+)", info).group(1))
                * last
            )
            wdfs = 0
            for n in range(1, last + 1):
                out = subprocess.run(
                    ["xapian-delve", "-1", "-v", "-r", str(n), shard],
                    capture_output=True,
                    check=False,
                ).stdout
                body = out.split(b"\n", 1)[1] if out.startswith(b"Term List") else b""
                for m in term_re.finditer(body):
                    wdfs += int(m.group(2))
                    o.write(f"{s}:{n}\t{int(m.group(2))}\t{m.group(1).hex()}\n")
                docs += bool(body)
            assert abs(wdfs - total) < 1, (shard, wdfs, total)
    print(f"{len(shards)} shards, {docs} documents")


main()
