import concurrent.futures
import asyncio
import hashlib
import http.client
import importlib.util
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import zipfile


def wait_for(callback, timeout=12):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            result = callback()
            if result:
                return result
        except (ConnectionError, http.client.HTTPException):
            pass
        time.sleep(0.04)
    raise AssertionError("condition did not become true")


class Lab:
    def __init__(self, root, programs, workers=4):
        self.root = root
        self.master, lifecycle, self.client, self.worker = map(Path, programs)
        self.children = []
        self.tokens = {scope: secrets.token_urlsafe(32) for scope in ("observer", "backup", "worker", "qq")}
        with socket.socket() as bind:
            bind.bind(("127.0.0.1", 0))
            self.port = bind.getsockname()[1]
        for directory in ("state", "archives", "staging", "fake", "game/world", "game/plugins"):
            (root / directory).mkdir(parents=True)
        self.world = root / "game/world"
        (self.world / "realm.dat").write_bytes(b"test world contents")
        (self.world / "区域.dat").write_bytes(b"UTF-8 entry name")
        (self.world / "session.lock").touch()
        (root / "game/plugins/secret.txt").write_text("excluded fixture")
        self.set_running()
        config = {
            "port": self.port,
            "http_workers": workers,
            "state_dir": str(root / "state"),
            "archive_dir": str(root / "archives"),
            "staging_dir": str(root / "staging"),
            "lifecycle": str(lifecycle),
            "action_timeout_seconds": 4,
            "archive_timeout_seconds": 2,
            "proxy_actions": ["backup", "status"],
            "qq_commands": {"token_file": str(self.token_path("qq")), "actions": ["help", "jobs", "status"]},
            "nodes": [
                {"id": "spigot", "depends_on": [], "save": True},
                {"id": "bungee", "depends_on": ["spigot", "empty"], "save": False},
                {"id": "empty", "depends_on": [], "save": True},
            ],
            "profiles": {
                "world": {"source": str(root / "game"), "include": ["world"]},
                "broken": {"source": str(root / "game"), "include": ["missing"]},
            },
        }
        for scope, token in self.tokens.items():
            self.token_path(scope).write_text(token, encoding="ascii")
            self.token_path(scope).chmod(0o600)
            if scope != "qq":
                config[f"{scope}_token_file"] = str(self.token_path(scope))
        self.config = root / "master.json"
        self.config.write_text(json.dumps(config), encoding="utf-8")
        self.master_identity = {}
        self.worker_identity = {}
        if sys.platform == "linux" and os.geteuid() == 0:
            master_uid, worker_uid, shared_gid = 61001, 61002, 61003
            self.master_identity = {"user": master_uid, "group": master_uid, "extra_groups": [shared_gid]}
            self.worker_identity = {"user": worker_uid, "group": shared_gid, "extra_groups": []}
            root.chmod(0o711)
            for directory in ("state", "archives", "fake"):
                os.chown(root / directory, master_uid, master_uid)
                (root / directory).chmod(0o700)
            stage = root / "staging"
            os.chown(stage, master_uid, shared_gid)
            stage.chmod(0o3770)
            fence = stage / ".archive-io.lock"
            fence.touch()
            os.chown(fence, master_uid, shared_gid)
            fence.chmod(0o640)
            for scope in self.tokens:
                os.chown(self.token_path(scope), master_uid, shared_gid)
            self.token_path("worker").chmod(0o640)

    def token_path(self, scope):
        return self.root / f"{scope}.token"

    def set_running(self):
        for node in ("bungee", "spigot"):
            (self.root / f"fake/{node}.running").touch()

    def request(self, path, *, scope=None, key=None, body=None):
        headers = {"Content-Type": "application/json"}
        if scope:
            headers["Authorization"] = "Bearer " + self.tokens[scope]
        if key is not None:
            headers["Idempotency-Key"] = key
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=4)
        try:
            conn.request("GET" if body is None else "POST", path, json.dumps(body) if body is not None else None, headers)
            response = conn.getresponse()
            data = response.read()
            if data and response.getheader("Content-Type", "").startswith("application/json"):
                data = json.loads(data)
            return response.status, data
        finally:
            conn.close()

    def spawn(self, program, label, *args):
        identity = self.worker_identity if label == "worker" else self.master_identity
        with (self.root / f"{label}.log").open("ab") as log:
            child = subprocess.Popen([program, "--config", self.config, *args], stdout=log, stderr=log,
                                     env={**os.environ, "LUO_TEST_LIFECYCLE": str(self.root / "fake")}, **identity)
        self.children.append(child)
        return child

    def start(self, ready=True, console_socket=None):
        args = ("--console-socket", console_socket) if console_socket else ()
        self.master_process = self.spawn(self.master, "master", *args)
        endpoint = "/readyz" if ready else "/healthz"
        wait_for(lambda: self.request(endpoint)[0] == 200)

    def start_worker(self):
        self.worker_process = self.spawn(self.worker, "worker")
        wait_for(lambda: self.request("/v1/topology", scope="observer")[1]["worker_online"])

    def submit(self, key, profile="world", scope="backup"):
        return self.request("/v1/backups", scope=scope, key=key, body={"profile": profile})

    def phase(self, key, expected):
        def inspect():
            code, data = self.request("/v1/backups/" + key, scope="observer")
            return data if code == 200 and data["phase"] == expected else None
        return wait_for(inspect)

    def running(self):
        return {file.stem for file in (self.root / "fake").glob("*.running")}

    @staticmethod
    def kill(process):
        if process.poll() is None:
            process.kill()
        process.wait(timeout=5)

    def close(self):
        for process in reversed(self.children):
            self.kill(process)


def test(lab):
    lab.start()
    assert lab.request("/v1/topology")[0] == 401
    assert lab.submit("no-token", scope=None)[0] == 401
    assert lab.submit("observer", scope="observer")[0] == 401
    assert lab.submit("offline")[0] == 503
    assert lab.submit("..bad")[0] == 400
    def command(scope, line):
        return lab.request("/v1/commands", scope=scope, body={"command": line})

    assert command("qq", "help")[1]["actions"] == ["help", "jobs", "status"]
    assert command("qq", "backup forbidden world")[0] == 403
    assert command("qq", "end")[0] == 403
    assert command("backup", "end")[0] == 403
    assert command("backup", "topology")[0] == 403
    assert command("backup", "sh -c echo")[0] == 403
    assert command("backup", "backup injected world\nend")[0] == 400
    assert command("observer", "status repeat")[0] == 401
    assert command("worker", "status repeat")[0] == 401
    assert lab.request("/v1/topology", scope="backup")[0] == 401
    lab.start_worker()
    with concurrent.futures.ThreadPoolExecutor(8) as pool:
        replies = list(pool.map(lambda _: lab.submit("repeat"), range(16)))
    assert all(status == 202 and body["id"] == "repeat" and "lease" not in body for status, body in replies)
    assert lab.submit("repeat", "broken")[0] == 409
    completed = lab.phase("repeat", "succeeded")
    archive = lab.root / "archives" / completed["archive"]
    assert hashlib.sha256(archive.read_bytes()).hexdigest() == completed["sha256"]
    with zipfile.ZipFile(archive) as zip_file:
        assert set(zip_file.namelist()) == {"world/", "world/realm.dat", "world/区域.dat"}
        assert zip_file.read("world/realm.dat") == b"test world contents"
        assert zip_file.testzip() is None
    assert lab.running() == {"spigot", "bungee"}
    assert (lab.root / "fake/actions").read_text().splitlines() == [
        "stop bungee", "save spigot", "stop spigot", "start spigot", "start bungee"
    ]
    sdk_result = subprocess.check_output([lab.client, str(lab.port), lab.token_path("observer"), "status", "repeat"])
    assert json.loads(sdk_result)["phase"] == "succeeded"
    assert command("qq", "status repeat")[1]["phase"] == "succeeded"
    if importlib.util.find_spec("aiohttp") is not None:
        sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "integrations/mcqq"))
        from master_commands import dispatch_master

        async def exercise_qq():
            owners = ["owner-identity"]
            settings = {"token_file": lab.token_path("qq"), "port": lab.port}
            owner = SimpleNamespace(id="evt-1", author=SimpleNamespace(member_openid=owners[0]))
            admin = SimpleNamespace(id="evt-2", author=SimpleNamespace(member_openid="game-admin"))
            assert "仅 Bot 主人" in await dispatch_master(admin, "console_command", "master", "/jobs", owners, {})
            assert "仅 Bot 主人" in await dispatch_master(owner, "game_command", "master", "/jobs", owners, {})
            assert await dispatch_master(owner, "console_command", "proxy", "/jobs", owners, {}) is None
            assert "succeeded" in await dispatch_master(owner, "console_command", "master", "/status repeat", owners, settings)
            assert "403" in await dispatch_master(owner, "console_command", "master", "/end", owners, settings)

        asyncio.run(exercise_qq())
    assert "worker_resume_requested" in (lab.root / "worker.log").read_text()

    assert lab.submit("archive-error", "broken")[0] == 202
    assert lab.phase("archive-error", "failed")["error"] == "archive_failed"
    assert lab.running() == {"spigot", "bungee"}
    assert not list((lab.root / "staging").glob("*.part"))

    (lab.root / "fake/save-spigot.fail").touch()
    assert lab.submit("save-error")[0] == 202
    assert lab.phase("save-error", "failed")["error"] == "stop_or_journal_failed"
    (lab.root / "fake/save-spigot.fail").unlink()
    assert lab.running() == {"spigot", "bungee"}

    lab.kill(lab.worker_process)
    worker_path = "/v1/backup-worker/job"
    assert lab.request(worker_path, scope="worker")[0] == 204
    assert lab.submit("receipt")[0] == 202
    lab.phase("receipt", "awaiting_backup")
    assert not lab.running()
    assert lab.submit("other")[0] == 409
    lease = lab.request(worker_path, scope="worker")[1]["lease"]
    route = "/v1/backups/receipt/resume"
    assert lab.request(route, scope="worker", body={"lease": "wrong", "result": "failed"})[0] == 409
    receipt = {"lease": lease, "result": "failed"}
    assert lab.request(route, scope="backup", body=receipt)[0] == 401
    assert lab.request(route, scope="worker", body=receipt)[0] == 202
    lab.phase("receipt", "failed")
    assert lab.request(route, scope="worker", body=receipt)[0] == 202
    assert lab.request(route, scope="worker", body={"lease": lease, "result": "verified", "sha256": "0" * 64})[0] == 409

    assert lab.submit("digest-mismatch")[0] == 202
    lab.phase("digest-mismatch", "awaiting_backup")
    lease = lab.request(worker_path, scope="worker")[1]["lease"]
    part_name = "." + hashlib.sha256(b"digest-mismatch").hexdigest() + ".part"
    (lab.root / "staging" / part_name).write_bytes(b"invalid archive")
    code, _ = lab.request("/v1/backups/digest-mismatch/resume", scope="worker",
                          body={"lease": lease, "result": "verified", "sha256": "0" * 64})
    assert code == 202
    assert lab.phase("digest-mismatch", "failed")["error"] == "archive_verification_failed"
    assert lab.running() == {"spigot", "bungee"}

    assert lab.submit("timeout")[0] == 202
    lab.phase("timeout", "awaiting_backup")
    assert lab.phase("timeout", "failed")["error"] == "archive_timeout"
    assert lab.running() == {"spigot", "bungee"}

    assert lab.request(worker_path, scope="worker")[0] == 204
    assert lab.submit("crash")[0] == 202
    lab.phase("crash", "awaiting_backup")
    lab.kill(lab.master_process)
    assert not lab.running()
    lab.start()
    assert lab.phase("crash", "failed")["error"] == "interrupted"
    assert lab.running() == {"spigot", "bungee"}
    assert lab.submit("repeat")[1]["phase"] == "succeeded"

    assert lab.request(worker_path, scope="worker")[0] == 204
    (lab.root / "fake/start-spigot.fail").touch()
    assert lab.submit("recovery")[0] == 202
    lab.phase("recovery", "awaiting_backup")
    lease = lab.request(worker_path, scope="worker")[1]["lease"]
    assert lab.request("/v1/backups/recovery/resume", scope="worker", body={"lease": lease, "result": "failed"})[0] == 202
    lab.phase("recovery", "recovery_required")
    assert lab.request("/healthz")[0] == 200
    assert lab.request("/readyz")[0] == 503
    assert lab.submit("blocked")[0] == 503
    (lab.root / "fake/start-spigot.fail").unlink()
    lab.kill(lab.master_process)
    lab.start()
    lab.phase("recovery", "failed")
    assert lab.running() == {"spigot", "bungee"}
    lab.start_worker()
    (lab.root / "fake/start-spigot.fail").touch()
    assert lab.submit("recovery-verified")[0] == 202
    lab.phase("recovery-verified", "recovery_required")
    assert list((lab.root / "staging").glob("*.part"))
    (lab.root / "fake/start-spigot.fail").unlink()
    lab.kill(lab.master_process)
    lab.start()
    recovered = lab.phase("recovery-verified", "succeeded")
    assert (lab.root / "archives" / recovered["archive"]).is_file()
    assert not list((lab.root / "staging").glob("*.part"))
    assert lab.running() == {"spigot", "bungee"}
    lab.kill(lab.worker_process)
    assert b"luo_master_ready 1" in lab.request("/metrics", scope="observer")[1]
    if os.name == "posix":
        lab.master_process.terminate()
        assert lab.master_process.wait(timeout=8) == 0
        private = lab.root / "private"
        private.mkdir(mode=0o700)
        if lab.master_identity:
            os.chown(private, lab.master_identity["user"], lab.master_identity["group"])
        socket_path = private / "console.sock"
        lab.start(console_socket=socket_path)
        assert socket_path.stat().st_mode & 0o077 == 0

        def attach(lines):
            return subprocess.check_output([lab.master, "--attach", socket_path], input=lines, text=True, timeout=6)

        assert '"end"' in attach("help\n")
        if lab.worker_identity:
            denied = subprocess.run([lab.master, "--attach", socket_path], input="end\n", text=True,
                                    capture_output=True, timeout=6, **lab.worker_identity)
            assert denied.returncode != 0
        assert lab.request(worker_path, scope="worker")[0] == 204
        assert lab.submit("console-end-test")[0] == 202
        lab.phase("console-end-test", "awaiting_backup")
        assert "409" in attach("end\n")
        assert lab.request("/healthz")[0] == 200
        lab.phase("console-end-test", "failed")
        assert '"maintenance":null' in attach("jobs\n")
        assert '"status":"draining"' in attach("end\n")
        assert lab.master_process.wait(timeout=8) == 0
        assert not socket_path.exists()
        assert lab.running() == {"spigot", "bungee"}
    else:
        lab.kill(lab.master_process)
        before = (lab.root / "fake/actions").read_bytes()
        console = subprocess.run([lab.master, "--config", lab.config, "--console"], input="help\njobs\nend\n",
                                 text=True, capture_output=True, check=True, timeout=10)
        assert '"status":"draining"' in console.stdout
        assert before == (lab.root / "fake/actions").read_bytes()


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="luo-master-test-") as root:
        lab = Lab(Path(root), sys.argv[1:])
        try:
            test(lab)
        except BaseException:
            for item in sorted(lab.root.glob("*.log")):
                print(item.name, item.read_text(encoding="utf-8", errors="replace"), sep="\n", file=sys.stderr)
            raise
        finally:
            lab.close()
