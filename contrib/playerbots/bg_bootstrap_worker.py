#!/usr/bin/env python3
"""BG bootstrap worker for playerbot_bg_bootstrap_queue.

This worker polls Character DB bootstrap requests produced by the in-core
`sql_queue` provider and executes an operator-provided command to materialize
bot sessions.

Environment variables:
  PB_BG_DB_HOST (default: 127.0.0.1)
  PB_BG_DB_PORT (default: 3306)
  PB_BG_DB_USER (required)
  PB_BG_DB_PASSWORD (required)
  PB_BG_DB_NAME (default: characters)
  PB_BG_POLL_SECONDS (default: 2)
  PB_BG_BATCH_SIZE (default: 10)
  PB_BG_BOOTSTRAP_CMD (required)

PB_BG_BOOTSTRAP_CMD supports placeholders:
  {id} {team_id} {battleground_type_id} {bot_name_prefix}

Example:
  PB_BG_BOOTSTRAP_CMD='python3 /opt/botruntime/spawn.py --team {team_id} --bg {battleground_type_id} --prefix {bot_name_prefix}'
"""

from __future__ import annotations

import os
import shlex
import subprocess
import time
from dataclasses import dataclass

import pymysql


@dataclass
class Config:
    host: str
    port: int
    user: str
    password: str
    db_name: str
    poll_seconds: int
    batch_size: int
    command_template: str


def load_config() -> Config:
    user = os.environ.get("PB_BG_DB_USER", "")
    password = os.environ.get("PB_BG_DB_PASSWORD", "")
    command_template = os.environ.get("PB_BG_BOOTSTRAP_CMD", "")

    if not user or not password:
        raise RuntimeError("PB_BG_DB_USER and PB_BG_DB_PASSWORD are required")
    if not command_template:
        raise RuntimeError("PB_BG_BOOTSTRAP_CMD is required")

    return Config(
        host=os.environ.get("PB_BG_DB_HOST", "127.0.0.1"),
        port=int(os.environ.get("PB_BG_DB_PORT", "3306")),
        user=user,
        password=password,
        db_name=os.environ.get("PB_BG_DB_NAME", "characters"),
        poll_seconds=int(os.environ.get("PB_BG_POLL_SECONDS", "2")),
        batch_size=max(1, int(os.environ.get("PB_BG_BATCH_SIZE", "10"))),
        command_template=command_template,
    )


def process_once(conn: pymysql.connections.Connection, config: Config) -> int:
    processed = 0
    with conn.cursor() as cur:
        cur.execute("START TRANSACTION")
        cur.execute(
            """
            SELECT id, team_id, battleground_type_id, bot_name_prefix
            FROM playerbot_bg_bootstrap_queue
            WHERE state = 'queued'
            ORDER BY requested_at ASC, id ASC
            LIMIT %s
            FOR UPDATE SKIP LOCKED
            """,
            (config.batch_size,),
        )
        rows = cur.fetchall()

        if not rows:
            conn.commit()
            return 0

        ids = [row[0] for row in rows]
        cur.execute(
            "UPDATE playerbot_bg_bootstrap_queue SET state='processing', attempts=attempts+1 WHERE id IN ({})".format(
                ",".join(["%s"] * len(ids))
            ),
            ids,
        )
        conn.commit()

    for row in rows:
        request_id, team_id, bg_type, prefix = row
        cmd = config.command_template.format(
            id=request_id,
            team_id=team_id,
            battleground_type_id=bg_type,
            bot_name_prefix=prefix,
        )
        result = subprocess.run(shlex.split(cmd), capture_output=True, text=True)

        next_state = "done" if result.returncode == 0 else "failed"
        error_text = ""
        if result.returncode != 0:
            error_text = (result.stderr or result.stdout or f"exit {result.returncode}")[:255]

        with conn.cursor() as cur:
            cur.execute(
                """
                UPDATE playerbot_bg_bootstrap_queue
                SET state=%s, processed_at=NOW(), last_error=%s
                WHERE id=%s
                """,
                (next_state, error_text, request_id),
            )
            conn.commit()

        processed += 1

    return processed


def main() -> None:
    config = load_config()
    conn = pymysql.connect(
        host=config.host,
        port=config.port,
        user=config.user,
        password=config.password,
        database=config.db_name,
        autocommit=False,
    )

    try:
        while True:
            count = process_once(conn, config)
            if count == 0:
                time.sleep(config.poll_seconds)
    finally:
        conn.close()


if __name__ == "__main__":
    main()
