#!/usr/bin/env python3
"""Bot-facing HTTP bridge for SIE-Lab.

The bot never compiles C++ or parses CSV itself: it calls three JSON tools
(health / selftest / count / autotune).  All numbers come from the real
`bench` binary built from this repository; nothing is simulated.

Run:
    SIEVE_BENCH=build/bench SIEVE_CHECK=build/check python3 bot/bridge.py
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BENCH = os.environ.get("SIEVE_BENCH", os.path.join(ROOT, "build", "bench"))
CHECK = os.environ.get("SIEVE_CHECK", os.path.join(ROOT, "build", "check"))
HOST = os.environ.get("SIEVE_HOST", "127.0.0.1")
PORT = int(os.environ.get("SIEVE_PORT", "8787"))
MAX_LIMIT = 10_000_000_000
MAX_BUDGET = 180
_lock = threading.Lock()
_last_selftest: dict | None = None

FIELDS = ("limit", "wheel", "bucket", "segment_kb", "reps", "seconds",
          "primes", "segments", "marks", "threads")


def git_sha() -> str:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, stderr=subprocess.DEVNULL,
            timeout=5).decode().strip()[:12]
    except Exception:
        return "unknown"


def run_bench(limit: int, wheel: int = 210, bucket: bool = True,
              segment_kb: int = 32, reps: int = 2, timeout: int = 120) -> dict:
    if not 2 <= limit <= MAX_LIMIT:
        raise ValueError(f"limit must be in [2, {MAX_LIMIT}]")
    if wheel not in (2, 30, 210):
        raise ValueError("wheel must be one of 2, 30, 210")
    if segment_kb not in (4, 8, 16, 32, 64, 128, 256):
        raise ValueError("unsupported segment_kb")
    if not 1 <= reps <= 5:
        raise ValueError("reps must be 1..5")
    cmd = [BENCH, "--n", str(limit), "--wheel", str(wheel),
           "--bucket", "1" if bucket else "0", "--seg-kb", str(segment_kb),
           "--reps", str(reps)]
    out = subprocess.check_output(cmd, timeout=timeout, text=True).strip().splitlines()[-1]
    values = [int(v) for v in out.split(",")[:5]] + [float(out.split(",")[5])] + \
             [int(v) for v in out.split(",")[6:]]
    return dict(zip(FIELDS, values))


def run_selftest() -> dict:
    global _last_selftest
    t0 = time.time()
    out = subprocess.check_output([CHECK], timeout=180, text=True,
                                  stderr=subprocess.STDOUT)
    ok = "ALL CHECKS PASSED" in out
    result = {"ok": ok, "git_sha": git_sha(), "seconds": round(time.time() - t0, 2),
              "detail": out.strip().splitlines()[-1] if out.strip() else ""}
    _last_selftest = result
    return result


def autotune(limit: int, budget: int, top_k: int = 3) -> dict:
    """Two-stage search: coarse screen at 1e7, validate top-k at the target.

    Every candidate is executed by the real bench binary; the search never
    guesses a timing.  Returns the ranked measurements and the winner.
    """
    t0 = time.time()
    if not 2 <= limit <= MAX_LIMIT:
        raise ValueError("limit out of range")
    budget = max(5, min(budget, MAX_BUDGET))
    coarse_limit = min(limit, 10_000_000)
    stage1 = []
    for wheel in (2, 30, 210):
        for bucket in (False, True):
            for seg in (16, 32, 64):
                if time.time() - t0 > budget * 0.6:
                    break
                r = run_bench(coarse_limit, wheel, bucket, seg, 1, timeout=30)
                stage1.append({"wheel": wheel, "bucket": bucket, "segment_kb": seg,
                               "seconds": r["seconds"], "marks": r["marks"]})
    stage1.sort(key=lambda r: r["seconds"])
    candidates = stage1[:top_k]
    stage2 = []
    for c in candidates:
        if time.time() - t0 > budget:
            break
        r = run_bench(limit, c["wheel"], c["bucket"], c["segment_kb"], 2,
                      timeout=max(30, budget))
        stage2.append({**c, "limit": limit, "seconds_final": r["seconds"],
                       "primes": r["primes"], "marks_final": r["marks"]})
    stage2.sort(key=lambda r: r["seconds_final"])
    return {"limit": limit, "budget_seconds": budget,
            "elapsed_seconds": round(time.time() - t0, 2),
            "stage1_screened": len(stage1), "stage2": stage2,
            "winner": stage2[0] if stage2 else None,
            "git_sha": git_sha()}


class Handler(BaseHTTPRequestHandler):
    server_version = "SIE-LabBot/1.0"

    def log_message(self, format, *args):
        sys.stderr.write("[bot-bridge] " + (format % args) + "\n")

    def send_json(self, code: int, payload: dict) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/health":
            self.send_json(200, {"ok": True, "service": "sieve-lab", "git_sha": git_sha(),
                                 "bench": os.path.basename(BENCH),
                                 "last_selftest": _last_selftest})
        elif path == "/api/tools":
            with open(os.path.join(HERE, "tools.json"), "rb") as f:
                body = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif path == "/api/result":
            # Synchronous contract: results are returned in the calling response.
            self.send_json(200, {"mode": "synchronous", "jobs": {},
                                 "note": "POST returns the result directly"})
        else:
            self.send_json(404, {"ok": False, "error": "not found"})

    def do_POST(self):
        path = urlparse(self.path).path
        length = int(self.headers.get("Content-Length", "0"))
        try:
            req = json.loads(self.rfile.read(length) or b"{}")
        except ValueError:
            self.send_json(400, {"ok": False, "error": "invalid JSON"})
            return
        try:
            if path == "/api/selftest":
                with _lock:
                    result = run_selftest()
                self.send_json(200 if result["ok"] else 500, {"ok": result["ok"], **result})
            elif path == "/api/sieve/count":
                self.send_json(200, {"ok": True, **run_bench(
                    int(req.get("limit", 1_000_000)), int(req.get("wheel", 210)),
                    bool(req.get("bucket", True)), int(req.get("segment_kb", 32)),
                    int(req.get("reps", 2)))})
            elif path == "/api/sieve/autotune":
                self.send_json(200, {"ok": True, **autotune(
                    int(req.get("limit", 1_000_000_000)),
                    int(req.get("budget_seconds", 60)))})
            else:
                self.send_json(404, {"ok": False, "error": "not found"})
        except (ValueError, subprocess.TimeoutExpired, subprocess.CalledProcessError,
                FileNotFoundError) as exc:
            self.send_json(400, {"ok": False, "error": f"{type(exc).__name__}: {exc}"})


def main():
    if not os.path.exists(BENCH) or not os.path.exists(CHECK):
        sys.exit(f"missing binaries: {BENCH} / {CHECK}; build first")
    server = ThreadingHTTPServer((HOST, PORT), Handler)
    print(f"sieve-lab bot bridge on http://{HOST}:{PORT} "
          f"(bench={BENCH}, check={CHECK})", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
