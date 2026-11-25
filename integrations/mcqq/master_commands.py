"""Owner-only adapter for MCQQBridge's parsed /大发命令 master branch."""

import asyncio
import hashlib
import json
import logging
from pathlib import Path
import re

import aiohttp


log = logging.getLogger(__name__)


async def dispatch_master(message, action, target, command, owners, config):
    """Return None for existing non-Master command targets."""
    if target != "master":
        return None
    author = getattr(message, "author", None)
    member = str(getattr(author, "member_openid", "") or "")
    allowed = bool(action == "console_command" and member and isinstance(owners, (list, tuple)) and
                   member in owners and getattr(message, "id", None))
    subject = hashlib.sha256(member.encode()).hexdigest()[:16] if member else "missing"
    if not allowed:
        log.warning("Master command denied for QQ subject %s", subject)
        return "仅 Bot 主人可使用 /大发命令 master <命令>。"

    line = command.removeprefix("/") if isinstance(command, str) else ""
    if not line or len(line.encode()) > 256 or any(c in line for c in "\r\n\0"):
        return "Master 命令格式无效。"
    try:
        token_file = Path(config["token_file"])
        port = int(config.get("port", 18776))
        if not token_file.is_absolute() or not 1024 <= port <= 65535:
            raise ValueError("invalid Master client configuration")
        with token_file.open("rb") as source:
            token = source.read(131).removesuffix(b"\n").removesuffix(b"\r")
        if not re.fullmatch(rb"[!-~]{32,128}", token):
            raise ValueError("invalid Master credential")
        async with aiohttp.ClientSession(timeout=aiohttp.ClientTimeout(total=3), trust_env=False) as session:
            async with session.post(f"http://127.0.0.1:{port}/v1/commands", json={"command": line},
                                    headers={"Authorization": "Bearer " + token.decode("ascii")},
                                    allow_redirects=False) as response:
                raw = bytearray()
                async for block in response.content.iter_chunked(512):
                    raw.extend(block)
                    if len(raw) > 4096:
                        raise ValueError("Master reply too large")
                body = json.loads(raw)
                log.info("Master replied %s to QQ subject %s", response.status, subject)
                return f"Master {response.status} {json.dumps(body, ensure_ascii=False)}"
    except (aiohttp.ClientError, asyncio.TimeoutError, OSError, TypeError, ValueError, KeyError):
        log.warning("Master command unavailable for QQ subject %s", subject)
        return "Master 暂时不可用。若已提交备份，请用相同操作 ID 查询或重试。"
