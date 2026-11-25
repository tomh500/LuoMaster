import concurrent.futures
import json
from pathlib import Path
import platform
import random
import statistics
import sys
import tempfile
import time

from integration import Lab


def sample(lab, count=500, clients=16):
    def request(_):
        start = time.perf_counter()
        code, body = lab.request("/v1/topology", scope="observer")
        assert code == 200, code
        phase = body["maintenance"]["phase"] if body["maintenance"] else "idle"
        return 1000 * (time.perf_counter() - start), phase

    start = time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(clients) as pool:
        samples = list(pool.map(request, range(count)))
    elapsed = time.perf_counter() - start
    latencies = sorted(latency for latency, _ in samples)
    return {
        "requests_per_second": round(count / elapsed, 1),
        "median_ms": round(statistics.median(latencies), 3),
        "p95_ms": round(latencies[(95 * count + 99) // 100 - 1], 3),
        "phase_counts": {phase: sum(name == phase for _, name in samples) for phase in sorted({p for _, p in samples})},
    }


def benchmark(programs, workers):
    with tempfile.TemporaryDirectory(prefix="luo-master-bench-") as root:
        lab = Lab(Path(root), programs, workers=workers)
        try:
            config = json.loads(lab.config.read_text())
            config["archive_timeout_seconds"] = 30
            lab.config.write_text(json.dumps(config))
            data = random.Random(11).randbytes(64 * (1 << 20))
            (lab.world / "benchmark.dat").write_bytes(data)
            lab.start()
            lab.start_worker()
            idle = sample(lab)
            start = time.perf_counter()
            assert lab.submit("benchmark")[0] == 202
            lab.phase("benchmark", "awaiting_backup")
            active = sample(lab)
            lab.phase("benchmark", "succeeded")
            total = round(time.perf_counter() - start, 3)
            return {"http_workers": workers, "idle": idle, "active": active, "backup_seconds": total}
        finally:
            lab.close()


if __name__ == "__main__":
    print(json.dumps({"os": platform.platform(), "clients": 16, "requests_per_sample": 500,
                      "source_mib": 64, "runs": [benchmark(sys.argv[1:], n) for n in (1, 2, 4) * 3]}, indent=2))
