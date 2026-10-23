#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generate VOD HLS files for media_server.

The VPP media_server never runs ffmpeg.  This script reads index.tsv and
writes a versioned HLS tree under HLS_ROOT:

  HLS_ROOT/<itemId>/master.m3u8
  HLS_ROOT/<itemId>/<version>/main/index.m3u8
  HLS_ROOT/<itemId>/<version>/main/seg_00000.ts

The master playlist is replaced atomically after ffmpeg finishes, so a
client never sees a half-written playlist.

Environment:
  MEDIA_ROOT   media root, default /srv/media
  INDEX_OUT    index file, default /srv/media/index.tsv
  HLS_ROOT     HLS output root, default <MEDIA_ROOT>/hls
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import NoReturn


def fail(message: str) -> NoReturn:
    print(f"emby-hls: {message}", file=sys.stderr)
    raise SystemExit(1)


def load_index(index: Path) -> dict[str, Path]:
    if not index.is_file():
        fail(f"index not found: {index}")

    items: dict[str, Path] = {}
    for line in index.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#") or "\t" not in line:
            continue
        item_id, path = line.split("\t", 1)
        if item_id and path:
            items[item_id] = Path(path)
    return items


def check_source(path: Path, media_root: Path) -> Path:
    try:
        path.resolve().relative_to(media_root.resolve())
    except ValueError:
        fail(f"source escapes media root: {path}")
    if not path.is_file():
        fail(f"source is not a regular file: {path}")
    return path


def probe_video(ffprobe: str, path: Path) -> tuple[int, int, int]:
    command = [
        ffprobe,
        "-v",
        "error",
        "-select_streams",
        "v:0",
        "-show_entries",
        "stream=width,height,bit_rate",
        "-of",
        "json",
        str(path),
    ]
    try:
        output = subprocess.check_output(command, text=True)
        stream = json.loads(output)["streams"][0]
        width = int(stream["width"])
        height = int(stream["height"])
        bit_rate = int(stream.get("bit_rate") or 0)
    except (subprocess.CalledProcessError, KeyError, IndexError, ValueError,
            json.JSONDecodeError) as exc:
        fail(f"ffprobe failed for {path}: {exc}")

    if width <= 0 or height <= 0:
        fail(f"no usable video stream in {path}")
    return width, height, bit_rate


def scaled_size(width: int, height: int, max_width: int) -> tuple[int, int]:
    if max_width <= 0 or width <= max_width:
        return width - (width % 2), height - (height % 2)
    out_width = max_width - (max_width % 2)
    out_height = round(height * out_width / width)
    return out_width, out_height - (out_height % 2)


def generate(item_id: str, source: Path, args: argparse.Namespace) -> Path:
    item_root = args.hls_root / item_id
    item_root.mkdir(parents=True, exist_ok=True)

    version = time.strftime("%Y%m%d%H%M%S", time.gmtime())
    version = f"v{version}-{os.getpid()}"
    variant_dir = item_root / version / "main"
    variant_dir.mkdir(parents=True)

    segment_pattern = str(variant_dir / "seg_%05d.ts")
    playlist = str(variant_dir / "index.m3u8")
    command = [
        args.ffmpeg,
        "-hide_banner",
        "-nostdin",
        "-y",
        "-i",
        str(source),
        "-map",
        "0:v:0",
        "-map",
        "0:a:0?",
        "-sn",
        "-dn",
    ]

    if args.copy:
        command += ["-c:v", "copy"]
    else:
        command += [
            "-c:v",
            "libx264",
            "-preset",
            args.preset,
            "-crf",
            str(args.crf),
            "-profile:v",
            "high",
            "-pix_fmt",
            "yuv420p",
            "-force_key_frames",
            f"expr:gte(t,n_forced*{args.segment_time})",
            "-sc_threshold",
            "0",
        ]
        if args.max_width > 0:
            command += ["-vf", f"scale='min({args.max_width},iw)':-2"]

    command += [
        "-c:a",
        "aac",
        "-b:a",
        args.audio_bitrate,
        "-ac",
        "2",
        "-max_muxing_queue_size",
        "4096",
        "-f",
        "hls",
        "-hls_time",
        str(args.segment_time),
        "-hls_playlist_type",
        "vod",
        "-hls_flags",
        "independent_segments",
        "-hls_segment_filename",
        segment_pattern,
        playlist,
    ]

    print(f"encoding {item_id}: {source} -> {variant_dir}")
    try:
        subprocess.run(command, check=True)
    except subprocess.CalledProcessError:
        shutil.rmtree(item_root / version, ignore_errors=True)
        raise

    width, height, bit_rate = probe_video(args.ffprobe, source)
    out_width, out_height = scaled_size(width, height, args.max_width)
    bandwidth = max(bit_rate, 2_000_000) if not args.copy else bit_rate
    if bandwidth <= 0:
        bandwidth = 5_000_000

    master = "\n".join(
        [
            "#EXTM3U",
            "#EXT-X-VERSION:3",
            "#EXT-X-INDEPENDENT-SEGMENTS",
            f"#EXT-X-STREAM-INF:BANDWIDTH={bandwidth},RESOLUTION={out_width}x{out_height}",
            f"{version}/main/index.m3u8",
            "",
        ]
    )
    master_path = item_root / "master.m3u8"
    temporary = item_root / f".master.m3u8.{os.getpid()}.tmp"
    temporary.write_text(master, encoding="ascii")
    os.replace(temporary, master_path)

    if args.prune:
        for child in item_root.iterdir():
            if child.is_dir() and child.name != version:
                shutil.rmtree(child, ignore_errors=True)

    return master_path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("items", nargs="*", help="Emby itemId to encode")
    parser.add_argument("--all", action="store_true", help="encode every file in index")
    parser.add_argument("--index", type=Path,
                        default=Path(os.environ.get("INDEX_OUT", "/srv/media/index.tsv")))
    parser.add_argument("--media-root", type=Path,
                        default=Path(os.environ.get("MEDIA_ROOT", "/srv/media")))
    parser.add_argument("--hls-root", type=Path,
                        default=Path(os.environ.get("HLS_ROOT", "/srv/media/hls")))
    parser.add_argument("--ffmpeg", default=os.environ.get("FFMPEG", "ffmpeg"))
    parser.add_argument("--ffprobe", default=os.environ.get("FFPROBE", "ffprobe"))
    parser.add_argument("--segment-time", type=int, default=6)
    parser.add_argument("--preset", default="veryfast")
    parser.add_argument("--crf", type=int, default=20)
    parser.add_argument("--audio-bitrate", default="128k")
    parser.add_argument("--max-width", type=int, default=1920)
    parser.add_argument("--copy", action="store_true",
                        help="copy video if the source codec is HLS-compatible")
    parser.add_argument("--prune", action="store_true",
                        help="remove old generated versions for each item")
    args = parser.parse_args()

    if args.segment_time <= 0:
        fail("--segment-time must be positive")
    if args.crf < 0 or args.crf > 51:
        fail("--crf must be between 0 and 51")
    if not args.items and not args.all:
        parser.error("specify one or more itemIds, or --all")

    items = load_index(args.index)
    if args.all:
        selected = list(items)
    else:
        selected = args.items

    seen: set[Path] = set()
    generated = 0
    for item_id in selected:
        if item_id not in items:
            fail(f"itemId not found in index: {item_id}")
        source = check_source(items[item_id], args.media_root)
        if args.all:
            resolved = source.resolve()
            if resolved in seen:
                continue
            seen.add(resolved)
        master = generate(item_id, source, args)
        print(f"ready {item_id}: {master}")
        print(f"  /Videos/{item_id}/master.m3u8")
        generated += 1

    print(f"generated {generated} item(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
