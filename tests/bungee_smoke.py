"""Run the upgraded plugin against an isolated BungeeCord and fake game nodes."""

import json
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile

from integration import Lab, wait_for


def smoke(root, programs, bungee_jar, plugin_jar):
    lab = Lab(root, programs)
    proxy = root / "proxy"
    plugin_config = proxy / "plugins/LuoBackup"
    plugin_config.mkdir(parents=True)
    shutil.copyfile(plugin_jar, proxy / "plugins/LuoBackup.jar")
    token_file = json.dumps(str(lab.token_path("backup")))
    (plugin_config / "config.yml").write_text(
        f"master:\n  enabled: true\n  port: {lab.port}\n  token-file: {token_file}\n"
        "targets:\n  survival: world\n"
    )
    (proxy / "modules.yml").write_text("version: 2\nmodules: []\n")
    with socket.socket() as bind:
        bind.bind(("127.0.0.1", 0))
        port = bind.getsockname()[1]
    (proxy / "config.yml").write_text(
        f"listeners:\n- host: 127.0.0.1:{port}\n  priorities: [lobby]\n  query_enabled: false\n"
        "servers:\n  lobby:\n    address: 127.0.0.1:9\n    motd: Fixture\n    restricted: false\n"
    )
    lab.start()
    lab.start_worker()
    log = root / "bungee.log"
    try:
        with log.open("wb") as output:
            process = subprocess.Popen(["java", "-Xmx512m", "-Djline.terminal=jline.UnsupportedTerminal",
                                       "-jar", bungee_jar], cwd=proxy, stdin=subprocess.PIPE, text=True,
                                       stdout=output, stderr=output)
        lab.children.append(process)
        wait_for(lambda: "LuoBackup Master request client enabled" in log.read_text(errors="replace"), timeout=22)
        assert process.stdin is not None
        process.stdin.write("luobackup now survival bungee-smoke\n")
        process.stdin.flush()
        job = lab.phase("bungee-smoke", "succeeded")
        assert lab.running() == {"spigot", "bungee"}
        process.stdin.write("luobackup status bungee-smoke\n")
        process.stdin.flush()
        wait_for(lambda: "Master HTTP 200" in log.read_text(errors="replace"))
        process.stdin.write("end\n")
        process.stdin.flush()
        assert process.wait(timeout=15) == 0
        print(f"BungeeCord now/status verified via Master. Archive: {job['archive']}")
    except BaseException:
        print(log.read_text(errors="replace"), file=sys.stderr)
        raise
    finally:
        lab.close()


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="luo-bungee-test-") as directory:
        args = [Path(arg).resolve() for arg in sys.argv[1:]]
        smoke(Path(directory), args[:4], args[4], args[5])
