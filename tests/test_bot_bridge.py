#!/usr/bin/env python3
"""End-to-end test of the bot bridge against the real binaries.

Starts bot/bridge.py on a free port, then exercises every tool the way the bot
does (native function-call JSON -> HTTP -> compiled bench/check), and asserts
the response contract.  No mocks, no fabricated numbers.
"""
import json
import os
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FAILS = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        FAILS.append(what)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def call(port, path, payload=None, timeout=240):
    url = f"http://127.0.0.1:{port}{path}"
    data = None if payload is None else json.dumps(payload).encode()
    req = urllib.request.Request(url, data=data,
                                 headers={"Content-Type": "application/json"},
                                 method="POST" if payload is not None else "GET")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")


def main():
    port = free_port()
    env = dict(os.environ, SIEVE_PORT=str(port))
    proc = subprocess.Popen([sys.executable, "bot/bridge.py"], cwd=ROOT, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        for _ in range(60):
            try:
                code, body = call(port, "/health", timeout=5)
                if code == 200:
                    break
            except Exception:
                time.sleep(0.2)
        else:
            raise RuntimeError("bridge did not start")

        check(body.get("service") == "sieve-lab", "health reports service name")
        check(bool(body.get("git_sha")), "health reports git sha")

        code, tools = call(port, "/api/tools")
        names = {t["name"] for t in tools["tools"]}
        check(code == 200 and names == {"sieve_selftest", "sieve_count", "sieve_autotune"},
              "tool schema lists the three bot tools")

        code, st = call(port, "/api/selftest", {})
        check(code == 200 and st.get("ok") is True, "selftest passes end-to-end")

        code, res = call(port, "/api/sieve/count",
                         {"limit": 1000000, "wheel": 210, "bucket": True,
                          "segment_kb": 32, "reps": 1})
        check(code == 200 and res.get("primes") == 78498, "count(1e6) = 78498")
        check(res.get("seconds", 0) > 0 and res.get("marks", 0) > 0,
              "count reports measured time and marks")

        code, bad = call(port, "/api/sieve/count", {"limit": 10**12})
        check(code == 400 and bad.get("ok") is False, "out-of-range limit rejected")

        code, tune = call(port, "/api/sieve/autotune",
                          {"limit": 10000000, "budget_seconds": 25})
        check(code == 200 and tune.get("winner") is not None, "autotune returns a winner")
        check(tune["stage1_screened"] >= 6, "autotune screened multiple configurations")
        winner = tune["winner"]
        check(winner["primes"] == 664579, "autotune validated pi(1e7) = 664579")
        check(winner["seconds_final"] > 0, "autotune winner has a measured time")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()

    print(f"\n{'ALL BRIDGE TESTS PASSED' if not FAILS else str(len(FAILS)) + ' FAILURES'}")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())