#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""SQLite-backed HLS generation queue.

The queue only schedules work.  Encoding remains in emby-hls.py, outside
VPP, so the data plane never runs ffmpeg.
"""

from __future__ import annotations

import argparse
import json
import os
import sqlite3
import subprocess
import sys
import time
from pathlib import Path


DEFAULT_DB = Path(os.environ.get("HLS_QUEUE_DB", "/var/lib/vpp-hls/queue.sqlite3"))
DEFAULT_INDEX = Path(os.environ.get("INDEX_OUT", "/srv/media/index.tsv"))
DEFAULT_MEDIA_ROOT = Path(os.environ.get("MEDIA_ROOT", "/srv/media"))
DEFAULT_HLS_ROOT = Path(os.environ.get("HLS_ROOT", "/srv/media/hls"))
DEFAULT_LOG_DIR = Path(os.environ.get("HLS_QUEUE_LOG_DIR", "/var/log/vpp-hls"))

SCHEMA = """
CREATE TABLE IF NOT EXISTS tasks (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  item_id TEXT NOT NULL,
  source TEXT NOT NULL UNIQUE,
  status TEXT NOT NULL DEFAULT 'pending',
  priority INTEGER NOT NULL DEFAULT 100,
  attempts INTEGER NOT NULL DEFAULT 0,
  created_at TEXT NOT NULL,
  started_at TEXT,
  finished_at TEXT,
  error TEXT
);
CREATE INDEX IF NOT EXISTS tasks_status_priority
  ON tasks(status, priority, id);
"""


def now() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def open_db(path: Path) -> sqlite3.Connection:
    path.parent.mkdir(parents=True, exist_ok=True)
    con = sqlite3.connect(str(path), timeout=30)
    con.row_factory = sqlite3.Row
    con.execute("PRAGMA journal_mode=WAL")
    con.executescript(SCHEMA)
    return con


def load_index(index: Path) -> list[tuple[str, Path]]:
    if not index.is_file():
        raise SystemExit(f"index not found: {index}")

    by_source: dict[str, tuple[str, Path]] = {}
    for line in index.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#") or "\t" not in line:
            continue
        item_id, raw_path = line.split("\t", 1)
        source = Path(raw_path)
        if not source.is_file():
            continue
        key = str(source.resolve())
        old = by_source.get(key)
        # Prefer the numeric Emby ID over a GUID alias for the output path.
        if old is None or (item_id.isdigit() and not old[0].isdigit()):
            by_source[key] = (item_id, source)
    return list(by_source.values())


def enqueue(args: argparse.Namespace) -> int:
    items = load_index(args.index)
    wanted = set(args.items)
    selected = [(item_id, source) for item_id, source in items
                if args.all or item_id in wanted]
    if not args.all and wanted - {item_id for item_id, _ in items}:
        missing = sorted(wanted - {item_id for item_id, _ in items})
        raise SystemExit(f"itemId not found in index: {', '.join(missing)}")

    con = open_db(args.db)
    inserted = 0
    skipped = 0
    for item_id, source in selected:
        if args.skip_existing and (args.hls_root / item_id / "master.m3u8").is_file():
            skipped += 1
            continue
        cur = con.execute(
            "INSERT OR IGNORE INTO tasks "
            "(item_id, source, status, priority, created_at) "
            "VALUES (?, ?, 'pending', ?, ?)",
            (item_id, str(source.resolve()), args.priority, now()),
        )
        inserted += cur.rowcount
    con.commit()
    print(f"enqueued {inserted}, skipped {skipped}, selected {len(selected)}")
    return 0


def show_status(args: argparse.Namespace) -> int:
    con = open_db(args.db)
    rows = con.execute(
        "SELECT status, COUNT(*) AS n FROM tasks GROUP BY status ORDER BY status"
    ).fetchall()
    print("status counts:")
    for row in rows:
        print(f"  {row['status']:10s} {row['n']}")
    print("next tasks:")
    for row in con.execute(
        "SELECT id, item_id, priority, attempts, status, source "
        "FROM tasks WHERE status IN ('pending', 'running') "
        "ORDER BY priority, id LIMIT 20"
    ):
        print(
            f"  #{row['id']} {row['status']} priority={row['priority']} "
            f"attempts={row['attempts']} item={row['item_id']} {row['source']}"
        )
    return 0


def retry_failed(args: argparse.Namespace) -> int:
    con = open_db(args.db)
    cur = con.execute(
        "UPDATE tasks SET status='pending', error=NULL "
        "WHERE status='failed' AND (? = 0 OR item_id = ?)",
        (0 if args.all else 1, args.item_id or ""),
    )
    con.commit()
    print(f"requeued {cur.rowcount} task(s)")
    return 0


def probe_codecs(ffprobe: str, source: Path) -> tuple[str, str]:
    cmd = [
        ffprobe, "-v", "error", "-show_entries", "stream=codec_type,codec_name",
        "-of", "json", str(source),
    ]
    try:
        data = json.loads(subprocess.check_output(cmd, text=True))
    except (OSError, subprocess.CalledProcessError, json.JSONDecodeError):
        return "", ""
    video = audio = ""
    for stream in data.get("streams", []):
        if stream.get("codec_type") == "video" and not video:
            video = stream.get("codec_name", "")
        elif stream.get("codec_type") == "audio" and not audio:
            audio = stream.get("codec_name", "")
    return video, audio


def claim(con: sqlite3.Connection) -> sqlite3.Row | None:
    con.execute("BEGIN IMMEDIATE")
    row = con.execute(
        "SELECT * FROM tasks WHERE status='pending' "
        "ORDER BY priority, id LIMIT 1"
    ).fetchone()
    if row is None:
        con.commit()
        return None
    con.execute(
        "UPDATE tasks SET status='running', started_at=?, attempts=attempts+1 "
        "WHERE id=? AND status='pending'",
        (now(), row["id"]),
    )
    con.commit()
    return row


def log_tail(path: Path, limit: int = 4000) -> str:
    try:
        with path.open("rb") as f:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            f.seek(max(0, size - limit))
            return f.read().decode("utf-8", "replace")
    except OSError:
        return ""


def run_task(args: argparse.Namespace, con: sqlite3.Connection, row: sqlite3.Row) -> bool:
    item_id = row["item_id"]
    source = Path(row["source"])
    master = args.hls_root / item_id / "master.m3u8"
    if args.skip_existing and master.is_file():
        con.execute(
            "UPDATE tasks SET status='done', finished_at=?, error=NULL WHERE id=?",
            (now(), row["id"]),
        )
        con.commit()
        print(f"#{row['id']} {item_id}: already has master.m3u8")
        return True

    copy_video = args.mode == "copy"
    if args.mode == "auto":
        video, _audio = probe_codecs(args.ffprobe, source)
        copy_video = video == "h264"

    args.log_dir.mkdir(parents=True, exist_ok=True)
    log_path = args.log_dir / f"task-{row['id']}-{item_id}.log"
    hls_script = Path(__file__).with_name("emby-hls.py")
    cmd = [
        sys.executable, str(hls_script), item_id,
        "--segment-time", str(args.segment_time),
        "--max-width", str(args.max_width),
        "--prune",
    ]
    if copy_video:
        cmd.append("--copy")

    env = os.environ.copy()
    env.update({
        "MEDIA_ROOT": str(args.media_root),
        "INDEX_OUT": str(args.index),
        "HLS_ROOT": str(args.hls_root),
        "FFMPEG": args.ffmpeg,
        "FFPROBE": args.ffprobe,
    })

    print(f"#{row['id']} {item_id}: {'copy' if copy_video else 'transcode'} -> {master}")
    with log_path.open("ab") as log:
        log.write(f"\n=== {now()} task {row['id']} {item_id} ===\n".encode())
        log.flush()
        try:
            rc = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT,
                                env=env).returncode
        except OSError as exc:
            rc = 127
            log.write(f"worker exec failed: {exc}\n".encode())

    if rc == 0 and master.is_file():
        con.execute(
            "UPDATE tasks SET status='done', finished_at=?, error=NULL WHERE id=?",
            (now(), row["id"]),
        )
        con.commit()
        print(f"#{row['id']} {item_id}: done")
        return True

    attempts = row["attempts"] + 1
    status = "pending" if attempts < args.max_attempts else "failed"
    error = log_tail(log_path)
    con.execute(
        "UPDATE tasks SET status=?, finished_at=?, error=? WHERE id=?",
        (status, now(), error, row["id"]),
    )
    con.commit()
    print(f"#{row['id']} {item_id}: failed rc={rc}, status={status}, log={log_path}")
    return False


def run_worker(args: argparse.Namespace) -> int:
    args.log_dir.mkdir(parents=True, exist_ok=True)
    con = open_db(args.db)
    # A single worker owns the queue; recover tasks left running by a crash.
    con.execute("UPDATE tasks SET status='pending' WHERE status='running'")
    con.commit()

    while True:
        row = claim(con)
        if row is None:
            if args.once:
                return 0
            time.sleep(args.poll)
            continue
        ok = run_task(args, con, row)
        if args.once:
            return 0 if ok else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db", type=Path, default=DEFAULT_DB)
    parser.add_argument("--index", type=Path, default=DEFAULT_INDEX)
    parser.add_argument("--media-root", type=Path, default=DEFAULT_MEDIA_ROOT)
    parser.add_argument("--hls-root", type=Path, default=DEFAULT_HLS_ROOT)
    parser.add_argument("--log-dir", type=Path, default=DEFAULT_LOG_DIR)
    parser.add_argument("--ffmpeg", default=os.environ.get("FFMPEG", "ffmpeg"))
    parser.add_argument("--ffprobe", default=os.environ.get("FFPROBE", "ffprobe"))

    sub = parser.add_subparsers(dest="command", required=True)

    enq = sub.add_parser("enqueue", help="enqueue existing media")
    enq.add_argument("items", nargs="*")
    enq.add_argument("--all", action="store_true")
    enq.add_argument("--priority", type=int, default=100)
    enq.add_argument("--skip-existing", action="store_true")
    enq.set_defaults(func=enqueue)

    status = sub.add_parser("status", help="show queue status")
    status.set_defaults(func=show_status)

    retry = sub.add_parser("retry", help="requeue failed tasks")
    retry.add_argument("item_id", nargs="?")
    retry.add_argument("--all", action="store_true")
    retry.set_defaults(func=retry_failed)

    worker = sub.add_parser("run", help="run queue worker")
    worker.add_argument("--once", action="store_true")
    worker.add_argument("--mode", choices=("auto", "copy", "transcode"),
                        default="auto")
    worker.add_argument("--segment-time", type=int, default=4)
    worker.add_argument("--max-width", type=int, default=1920)
    worker.add_argument("--max-attempts", type=int, default=3)
    worker.add_argument("--poll", type=float, default=10)
    worker.add_argument("--skip-existing", action="store_true", default=True)
    worker.set_defaults(func=run_worker)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
