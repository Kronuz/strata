"""C1 of docs/design/hybrid-search.md: idle cost and p99 per tenant count, with strata included.

Every system runs natively on this host (bench/hybrid/setup.sh installs them under C1_HOME):
  c1.py gen                  the shared vectors: 1M x 768 float32 unit rows, 1,000 queries
  c1.py load SYS N           N tenants of 1,000 vectors into SYS's data dir; SYS stopped after
  c1.py measure SYS N        SYS started on the loaded data, 30 s idle, memory and idle CPU,
                             1,000 queries (cold, warm1, warm2) with recall@10, memory again
  c1.py clean SYS N          the data dir removed
  c1.py dry N [SYS...]       load, measure and clean each system: the harness end to end
SYS: qdrant (payload tenancy), pg (shared table + btree(tenant)), es (shared index, filtered kNN),
lance (table per tenant, flat), strata (index per tenant behind a catalog, exact scan).
Results: C1_HOME/results/SYS-N-load.json and SYS-N.json.
"""

from __future__ import annotations

import ctypes
import json
import os
import random
import shutil
import signal
import subprocess
import sys
import time
from collections.abc import Callable
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
H = Path(os.environ.get("C1_HOME", HERE.parent.parent / ".scratch" / "c1"))
D, PER, TOTAL, NQ, K = 768, 1000, 1_000_000, 1000, 10
IDLE_S = float(os.environ.get("C1_IDLE", "30"))
PORTS = {"qdrant": 16333, "pg": 15432, "es": 19200}
SYSTEMS = ("strata", "pg", "qdrant", "lance", "es")


def log(*a: object) -> None:
    print(time.strftime("%H:%M:%S"), *a, file=sys.stderr, flush=True)


def vectors() -> np.ndarray:
    return np.memmap(H / "vec.f32", dtype=np.float32, mode="r", shape=(TOTAL, D))


def queries() -> np.ndarray:
    return np.fromfile(H / "q.f32", dtype=np.float32).reshape(NQ, D)


def rows(t: int) -> np.ndarray:
    return np.asarray(vectors()[t * PER : (t + 1) * PER])


def sample(n: int) -> list[int]:
    r = random.Random(42)
    return r.sample(range(n), NQ) if n >= NQ else [r.randrange(n) for _ in range(NQ)]


def gen() -> None:
    # The same seed and draw order as C1's first run (strata docs/perf/c1-field-tenants.md).
    rng = np.random.default_rng(20261027)

    def unit(n: int) -> np.ndarray:
        x = rng.standard_normal((n, D), dtype=np.float32)
        x /= np.linalg.norm(x, axis=1, keepdims=True)
        return x

    v = np.memmap(H / "vec.tmp", dtype=np.float32, mode="w+", shape=(TOTAL, D))
    for i in range(0, TOTAL, 50_000):
        v[i : i + 50_000] = unit(50_000)
    v.flush()
    del v
    unit(NQ).tofile(H / "q.f32")
    os.replace(H / "vec.tmp", H / "vec.f32")


def disk(path: Path) -> dict[str, int]:
    out = subprocess.run(["du", "-sk", str(path)], capture_output=True, text=True, check=False).stdout.split()
    files = sum(len(f) for _, _, f in os.walk(path))
    return {"mb": round(int(out[0]) / 1024) if out else 0, "files": files}


def free_gb() -> float:
    st = os.statvfs(H)
    return st.f_bavail * st.f_frsize / 2**30


# --- process meters (macOS proc_pid_rusage; Linux /proc) ---------------------------------------


class _RU(ctypes.Structure):  # rusage_info_v2's first fields
    _fields_ = (
        [("uuid", ctypes.c_uint8 * 16)]
        + [(n, ctypes.c_uint64) for n in ("user", "system", "idle_wkups", "intr_wkups", "pageins", "wired", "resident", "footprint")]
        + [("rest", ctypes.c_uint64 * 32)]
    )


_libproc = ctypes.CDLL("/usr/lib/libproc.dylib") if sys.platform == "darwin" else None
_TB = None


def _ns_per_tick() -> float:
    # rusage times are mach absolute ticks; convert with the timebase.
    global _TB
    if _TB is None:

        class TB(ctypes.Structure):
            _fields_ = [("numer", ctypes.c_uint32), ("denom", ctypes.c_uint32)]

        tb = TB()
        ctypes.CDLL("/usr/lib/libSystem.dylib").mach_timebase_info(ctypes.byref(tb))
        _TB = tb.numer / tb.denom
    return _TB


def tree(pid: int) -> list[int]:
    """pid and its descendants."""
    out = subprocess.run(["ps", "-A", "-o", "pid=,ppid="], capture_output=True, text=True, check=False).stdout
    kids: dict[int, list[int]] = {}
    for line in out.splitlines():
        p, pp = map(int, line.split())
        kids.setdefault(pp, []).append(p)
    seen, todo = [], [pid]
    while todo:
        p = todo.pop()
        seen.append(p)
        todo += kids.get(p, [])
    return seen


def meter(pid: int) -> dict[str, float]:
    """Resident, footprint (dirty private, the anon analogue), CPU seconds and wakeups of a process tree."""
    m = {"rss_mb": 0.0, "footprint_mb": 0.0, "cpu_s": 0.0, "wakeups": 0.0, "procs": 0.0}
    for p in tree(pid):
        ru = _RU()
        if _libproc is None or _libproc.proc_pid_rusage(p, 2, ctypes.byref(ru)) != 0:
            continue
        m["rss_mb"] += ru.resident / 2**20
        m["footprint_mb"] += ru.footprint / 2**20
        m["cpu_s"] += (ru.user + ru.system) * _ns_per_tick() / 1e9
        m["wakeups"] += ru.idle_wkups + ru.intr_wkups
        m["procs"] += 1
    return {k: round(v, 3) for k, v in m.items()}


def idle(pid: int) -> dict[str, float]:
    a = meter(pid)
    time.sleep(IDLE_S)
    b = meter(pid)
    return {**b, "idle_s": IDLE_S, "idle_cpu_s": round(b["cpu_s"] - a["cpu_s"], 4), "idle_wakeups": b["wakeups"] - a["wakeups"]}


def pct(xs: list[float]) -> dict[str, float]:
    s = sorted(xs)
    return {"p50": round(s[len(s) // 2] * 1e3, 3), "p99": round(s[int(len(s) * 0.99) - 1] * 1e3, 3), "max": round(s[-1] * 1e3, 3), "n": len(s)}


def parallel(fn: Callable[[int], None], n: int, threads: int = 4) -> None:
    """fn over tenants [0, n) on `threads` client threads (C1's loaders used 4)."""
    from concurrent.futures import ThreadPoolExecutor

    with ThreadPoolExecutor(threads) as ex:
        list(ex.map(fn, range(n)))


def wait_for(ok: Callable[[], bool], what: str, timeout: float = 600) -> float:
    t = time.time()
    while time.time() - t < timeout:
        try:
            if ok():
                return time.time() - t
        except Exception:  # noqa: BLE001, S110 (not up yet: connection refused, 503)
            pass
        time.sleep(0.25)
    raise RuntimeError(f"{what} not ready in {timeout} s")


# --- servers ------------------------------------------------------------------------------------


class Server:
    name = ""

    def __init__(self, n: int) -> None:
        self.n = n
        self.dd = H / "data" / f"{self.name}-{n}"
        self.proc: subprocess.Popen[bytes] | None = None

    def spawn(self, argv: list[str], env: dict[str, str] | None = None) -> None:
        logf = open(H / "logs" / f"{self.name}-{self.n}.log", "ab")  # noqa: SIM115 (the child keeps it)
        self.proc = subprocess.Popen(argv, env={**os.environ, **(env or {})}, stdout=logf, stderr=logf, start_new_session=True)

    stop_signal = signal.SIGTERM

    def stop(self) -> None:
        # The whole group: Elasticsearch's launcher runs the server as a child.
        if self.proc and self.proc.poll() is None:
            os.killpg(self.proc.pid, self.stop_signal)
            try:
                self.proc.wait(180)
            except subprocess.TimeoutExpired:
                os.killpg(self.proc.pid, signal.SIGKILL)
                self.proc.wait()
        self.proc = None

    def pid(self) -> int:
        assert self.proc
        return self.proc.pid


class Http(Server):
    def r(self, method: str, path: str, body: object = None, data: str | None = None) -> dict:

        hdr = {"Content-Type": "application/x-ndjson"} if data is not None else None
        x = self.sess.request(method, self.url + path, json=body, data=data, headers=hdr, timeout=900)
        if x.status_code >= 300:
            raise RuntimeError(f"{method} {path} -> {x.status_code} {x.text[:400]}")
        return x.json()

    def __init__(self, n: int) -> None:
        import requests

        super().__init__(n)
        self.url = f"http://127.0.0.1:{PORTS[self.name]}"
        self.sess = requests.Session()


class Qdrant(Http):
    """One collection, payload tenancy: hnsw m 0 / payload_m 16, a tenant keyword index (C1's working shape)."""

    name = "qdrant"

    def start(self) -> float:
        self.dd.mkdir(parents=True, exist_ok=True)
        p = PORTS["qdrant"]
        env = {
            "QDRANT__STORAGE__STORAGE_PATH": str(self.dd),
            "QDRANT__STORAGE__SNAPSHOTS_PATH": str(self.dd / "snapshots"),
            "QDRANT__SERVICE__HTTP_PORT": str(p),
            "QDRANT__SERVICE__GRPC_PORT": str(p + 1),
            "QDRANT__TELEMETRY_DISABLED": "true",
            "QDRANT__SERVICE__HOST": "127.0.0.1",
        }
        self.spawn([str(H / "qdrant" / "qdrant")], env)
        return wait_for(lambda: self.sess.get(self.url + "/readyz", timeout=2).status_code == 200, "qdrant")

    def version(self) -> str:
        return self.r("GET", "/")["version"]

    def load(self) -> None:
        self.r(
            "PUT",
            "/collections/mt",
            {"vectors": {"size": D, "distance": "Dot"}, "hnsw_config": {"m": 0, "payload_m": 16}, "optimizers_config": {"indexing_threshold": 0}},
        )
        self.r("PUT", "/collections/mt/index", {"field_name": "tenant", "field_schema": {"type": "keyword", "is_tenant": True}})

        def one(t: int) -> None:
            a = rows(t)
            for i in range(0, PER, 250):
                pts = [{"id": t * PER + i + j, "vector": v, "payload": {"tenant": f"t{t}"}} for j, v in enumerate(a[i : i + 250].tolist())]
                self.r("PUT", "/collections/mt/points?wait=true", {"points": pts})

        parallel(one, self.n)
        self.r("PATCH", "/collections/mt", {"optimizers_config": {"indexing_threshold": 20000}})
        wait_for(self.settled, "qdrant optimizer", 1500)

    def settled(self) -> bool:
        x = self.r("GET", "/collections/mt")["result"]
        return x["status"] == "green" and x.get("optimizer_status") == "ok"

    def query(self, t: int, q: list[float]) -> list[int]:
        x = self.r("POST", "/collections/mt/points/query", {"query": q, "limit": K, "filter": {"must": [{"key": "tenant", "match": {"value": f"t{t}"}}]}})
        return [p["id"] - t * PER for p in x["result"]["points"]]


class PG(Server):
    """One shared table with btree(tenant); each query an exact sort over its tenant's rows (C1's cheapest shape)."""

    name = "pg"
    stop_signal = signal.SIGINT  # fast shutdown

    def start(self) -> float:
        b = H / "pg" / "bin"
        if not (self.dd / "PG_VERSION").exists():
            self.dd.parent.mkdir(parents=True, exist_ok=True)
            subprocess.run([str(b / "initdb"), "-D", str(self.dd), "-U", "postgres", "--auth=trust", "-E", "UTF8"], check=True, capture_output=True)
        self.spawn([str(b / "postgres"), "-D", str(self.dd), "-p", str(PORTS["pg"]), "-k", str(self.dd), "-c", "listen_addresses=127.0.0.1"])
        self.conn = None
        return wait_for(lambda: self.connect().close() is None, "postgres")

    def connect(self):
        import psycopg

        return psycopg.connect(host="127.0.0.1", port=PORTS["pg"], user="postgres", dbname="postgres", connect_timeout=5, autocommit=True)

    def version(self) -> str:
        with self.connect() as c:
            c.execute("create extension if not exists vector")
            v = c.execute("select version()").fetchone()[0].split(",")[0]
            return v + " pgvector " + c.execute("select extversion from pg_extension where extname='vector'").fetchone()[0]

    def load(self) -> None:
        # Binary COPY: tenant int4, id int4, then pgvector's binary vector (int16 dim, int16 unused, float4s).
        dt = np.dtype(
            [("nf", ">i2"), ("l1", ">i4"), ("t", ">i4"), ("l2", ">i4"), ("id", ">i4"), ("l3", ">i4"), ("dim", ">i2"), ("un", ">i2"), ("v", ">f4", (D,))]
        )
        with self.connect() as c:
            c.execute("create table items(tenant int, id int, v vector(768))")
            with c.cursor().copy("copy items(tenant, id, v) from stdin (format binary)") as cp:
                cp.write(b"PGCOPY\n\xff\r\n\0" + bytes(8))
                for t in range(self.n):
                    b = np.empty(PER, dt)
                    b["nf"], b["l1"], b["t"], b["l2"], b["id"], b["l3"], b["dim"], b["un"], b["v"] = 3, 4, t, 4, np.arange(PER), 4 + 4 * D, D, 0, rows(t)
                    cp.write(b.tobytes())
                cp.write(b"\xff\xff")
            c.execute("create index on items(tenant)")
            c.execute("vacuum analyze items")

    def query(self, t: int, q: list[float]) -> list[int]:
        if self.conn is None:
            self.conn = self.connect()
        qs = "[" + ",".join(f"{x:.7g}" for x in q) + "]"
        return [r[0] for r in self.conn.execute("select id from items where tenant = %s order by v <#> %s::vector limit 10", (t, qs)).fetchall()]

    def stop(self) -> None:
        if getattr(self, "conn", None) is not None:
            self.conn.close()
            self.conn = None
        super().stop()


class ES(Http):
    """One shared index, filtered kNN (num_candidates 50), 1 shard, 0 replicas, 8 GiB heap (C1's shape and heap)."""

    name = "es"

    def start(self) -> float:
        self.dd.mkdir(parents=True, exist_ok=True)
        p = PORTS["es"]
        # The disk watermarks are off: this host's disk is past the 95% flood stage, which would make
        # every index read-only; a setting of the host, not of the workload.
        self.spawn(
            [
                str(H / "es" / "bin" / "elasticsearch"),
                "-E",
                f"path.data={self.dd}",
                "-E",
                f"path.logs={H / 'logs' / 'es'}",
                "-E",
                "discovery.type=single-node",
                "-E",
                "xpack.security.enabled=false",
                "-E",
                f"http.port={p}",
                "-E",
                f"transport.port={p + 100}",
                "-E",
                "network.host=127.0.0.1",
                "-E",
                "cluster.routing.allocation.disk.threshold_enabled=false",
            ],
            {"ES_JAVA_OPTS": "-Xms8g -Xmx8g"},
        )
        return wait_for(
            lambda: self.sess.get(self.url + "/_cluster/health?wait_for_status=green&timeout=5s", timeout=10).status_code == 200, "elasticsearch", 900
        )

    def version(self) -> str:
        return self.r("GET", "/")["version"]["number"]

    def load(self) -> None:
        vm = {"type": "dense_vector", "dims": D, "index": True, "similarity": "dot_product"}
        self.r(
            "PUT", "/mt", {"settings": {"number_of_shards": 1, "number_of_replicas": 0}, "mappings": {"properties": {"v": vm, "tenant": {"type": "keyword"}}}}
        )

        def one(t: int) -> None:
            out = []
            for i, v in enumerate(rows(t).tolist()):
                out.append(json.dumps({"index": {"_index": "mt", "_id": t * PER + i}}))
                out.append(json.dumps({"v": v, "tenant": f"t{t}"}))
            res = self.r("POST", "/_bulk", data="\n".join(out) + "\n")
            if res["errors"]:
                raise RuntimeError("bulk item error " + json.dumps(next(i for i in res["items"] if "error" in i["index"]))[:500])

        parallel(one, self.n)
        self.r("POST", "/mt/_refresh")
        self.r("POST", "/mt/_flush")

    def query(self, t: int, q: list[float]) -> list[int]:
        kn = {"field": "v", "query_vector": q, "k": K, "num_candidates": 50, "filter": {"term": {"tenant": f"t{t}"}}}
        x = self.r("POST", "/mt/_search", {"knn": kn, "size": K, "_source": False})
        return [int(h["_id"]) - t * PER for h in x["hits"]["hits"]]


class Embedded(Server):
    """An in-process engine in a child process, on a line protocol: it prints `ready`; `pass NAME` runs the
    sample's queries timed inside the engine and writes `seconds id...` lines to OUT/NAME.txt; `quit` exits."""

    def cmd(self, line: str) -> str:
        assert self.proc and self.proc.stdin and self.proc.stdout
        self.proc.stdin.write((line + "\n").encode())
        self.proc.stdin.flush()
        return self.read_line()

    def read_line(self) -> str:
        assert self.proc and self.proc.stdout
        got = self.proc.stdout.readline().decode().strip()
        if not got:
            raise RuntimeError(f"{self.name} exited: {self.proc.wait()}")
        return got

    def serve(self, argv: list[str]) -> float:
        t = time.time()
        logf = open(H / "logs" / f"{self.name}-{self.n}.log", "ab")  # noqa: SIM115 (the child keeps it)
        self.proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=logf, start_new_session=True)
        if self.read_line() != "ready":
            raise RuntimeError(f"{self.name}: no ready line")
        return time.time() - t

    def stop(self) -> None:
        if self.proc and self.proc.poll() is None:
            try:
                self.cmd("quit")
            except Exception:  # noqa: BLE001, S110 (already exiting)
                pass
            self.proc.wait(120)
        self.proc = None


class Lance(Embedded):
    name = "lance"

    def load(self) -> None:
        import lancedb
        import pyarrow as pa

        db = lancedb.connect(str(self.dd))
        for t in range(self.n):
            arr = pa.FixedSizeListArray.from_arrays(pa.array(rows(t).reshape(-1), type=pa.float32()), D)
            db.create_table(f"t{t}", pa.table({"id": pa.array(range(PER), type=pa.int32()), "vector": arr}))

    def start(self) -> float:
        return self.serve([sys.executable, __file__, "_lance", str(self.n), str(self.dd)])

    def version(self) -> str:
        import lancedb

        return lancedb.__version__


def lance_serve(n: int, dd: str) -> None:
    import lancedb

    db = lancedb.connect(dd)
    smp, qs, handles = sample(n), queries(), {}
    print("ready", flush=True)
    for line in sys.stdin:
        w = line.split()
        if w[0] == "quit":
            return
        with open(H / "out" / f"{w[1]}.txt", "w") as f:
            for i, t in enumerate(smp):
                a = time.perf_counter()
                h = handles.get(t) or db.open_table(f"t{t}")
                handles[t] = h
                ids = [r["id"] for r in h.search(qs[i]).metric("dot").limit(K).select(["id"]).to_list()]
                f.write(f"{time.perf_counter() - a:.9f} {' '.join(map(str, ids))}\n")
        print("done", flush=True)


class Strata(Embedded):
    """One strata index per tenant (bench/hybrid/c1_strata.cc), opened through a catalog as a host does."""

    name = "strata"

    def bin(self) -> str:
        return os.environ.get("C1_STRATA", str(HERE.parent.parent / "build-c1" / "c1_strata"))

    def load(self) -> None:
        subprocess.run([self.bin(), "load", str(self.dd), str(self.n), str(H / "vec.f32")], check=True, stdout=sys.stderr)

    def start(self) -> float:
        with open(H / "out" / "sample.txt", "w") as f:
            f.write("\n".join(map(str, sample(self.n))) + "\n")
        return self.serve([self.bin(), "serve", str(self.dd), str(self.n), str(H / "q.f32"), str(H / "out" / "sample.txt"), str(H / "out")])

    def version(self) -> str:
        return subprocess.run(["git", "-C", str(HERE), "rev-parse", "--short=12", "HEAD"], capture_output=True, text=True, check=False).stdout.strip()


def make(sys_: str, n: int) -> Server:
    return {"qdrant": Qdrant, "pg": PG, "es": ES, "lance": Lance, "strata": Strata}[sys_](n)


def res_path(name: str) -> Path:
    return H / "results" / f"{name}.json"


def save(name: str, out: dict) -> None:
    res_path(name).write_text(json.dumps(out, indent=1) + "\n")


# --- phases -------------------------------------------------------------------------------------


def load(sys_: str, n: int) -> dict:
    s = make(sys_, n)
    name = f"{sys_}-{n}-load"
    if s.dd.exists():
        shutil.rmtree(s.dd)
    s.dd.mkdir(parents=True)
    out: dict = {"system": sys_, "tenants": n, "per_tenant": PER, "dim": D, "free_gb_before": round(free_gb(), 1)}
    try:
        if isinstance(s, Embedded):
            out["version"] = s.version()
            t = time.time()
            s.load()
            out["load_s"] = round(time.time() - t, 1)
        else:
            out["start_s"] = round(s.start(), 1)
            out["version"] = s.version()
            t = time.time()
            s.load()
            out["load_s"] = round(time.time() - t, 1)
    finally:
        s.stop()
        out["disk"] = disk(s.dd)
        save(name, out)
        log(name, json.dumps(out))
    return out


def exact(n: int) -> list[set[int]]:
    smp, qs = sample(n), queries()
    return [set(np.argsort(-(rows(t) @ qs[i]))[:K].tolist()) for i, t in enumerate(smp)]


def measure(sys_: str, n: int) -> dict:
    s = make(sys_, n)
    if not s.dd.exists():
        raise SystemExit(f"{s.dd} missing: load first")
    name = f"{sys_}-{n}"
    want = exact(n)
    smp, qs = sample(n), queries()
    out: dict = {
        "system": sys_,
        "tenants": n,
        "per_tenant": PER,
        "dim": D,
        "queries": NQ,
        "host": os.uname().nodename.split(".")[0],
        "in_process": isinstance(s, Embedded),
        "disk": disk(s.dd),
    }
    try:
        out["start_s"] = round(s.start(), 2)
        out["version"] = s.version()
        out["idle_start"] = idle(s.pid())
        for ph in ("cold", "warm1", "warm2"):
            xs, ids = [], []
            if isinstance(s, Embedded):
                if s.cmd(f"pass {ph}") != "done":
                    raise RuntimeError("pass failed")
                for line in (H / "out" / f"{ph}.txt").read_text().splitlines():
                    w = line.split()
                    xs.append(float(w[0]))
                    ids.append([int(x) for x in w[1:]])
            else:
                ql = qs.tolist()
                for i, t in enumerate(smp):
                    a = time.perf_counter()
                    ids.append(s.query(t, ql[i]))
                    xs.append(time.perf_counter() - a)
            out[ph] = pct(xs)
            out[ph]["recall10"] = round(sum(len(want[i] & set(r)) for i, r in enumerate(ids)) / (K * NQ), 4)
            out[ph]["short"] = sum(len(r) != K for r in ids)
            save(name, out)
        out["idle_after"] = idle(s.pid())
    finally:
        s.stop()
        save(name, out)
        log(name, json.dumps(out))
    if out["warm2"]["short"]:
        raise SystemExit(f"{name}: {out['warm2']['short']} queries returned fewer than {K} hits")
    return out


def clean(sys_: str, n: int) -> None:
    shutil.rmtree(make(sys_, n).dd, ignore_errors=True)


def main() -> None:
    for d in ("results", "logs", "data", "out"):
        (H / d).mkdir(parents=True, exist_ok=True)
    a = sys.argv[1:]
    if a[0] == "gen":
        gen()
    elif a[0] == "_lance":
        lance_serve(int(a[1]), a[2])
    elif a[0] in ("load", "measure", "clean"):
        {"load": load, "measure": measure, "clean": clean}[a[0]](a[1], int(a[2]))
    elif a[0] == "dry":
        failed = []
        for sys_ in a[2:] or SYSTEMS:
            try:
                load(sys_, int(a[1]))
                measure(sys_, int(a[1]))
            except BaseException as e:  # noqa: BLE001 (each system's failure is reported, the rest still run)
                failed.append(f"{sys_}: {type(e).__name__}: {e}")
                log("FAILED", failed[-1])
            finally:
                clean(sys_, int(a[1]))
        if failed:
            raise SystemExit("dry run failures:\n" + "\n".join(failed))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
