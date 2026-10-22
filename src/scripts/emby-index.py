#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build media_server index.tsv from the Emby API.

Env:
  EMBY_DB         optional Emby library.db (preferred, no API key)
  EMBY_URL        default http://127.0.0.1:8096
  EMBY_API_KEY    required for API mode
  EMBY_PATH_FROM  optional container path prefix (default /media)
  EMBY_PATH_TO    replacement host path prefix (default /srv/media)
  INDEX_OUT       default /srv/media/index.tsv
"""

import json
import os
import sys
import urllib.parse
import urllib.request

EMBY_URL = os.environ.get("EMBY_URL", "http://127.0.0.1:8096").rstrip("/")
API_KEY = os.environ.get("EMBY_API_KEY", "")
EMBY_DB = os.environ.get("EMBY_DB", "")
PATH_FROM = os.environ.get("EMBY_PATH_FROM", "/media")
PATH_TO = os.environ.get("EMBY_PATH_TO", "/srv/media")
OUT = os.environ.get("INDEX_OUT", "/srv/media/index.tsv")

def from_db():
    import sqlite3
    con = sqlite3.connect(f"file:{EMBY_DB}?mode=ro", uri=True)
    rows = con.execute(
        "SELECT Id, guid, Path FROM MediaItems "
        "WHERE Path IS NOT NULL AND Path != '' AND Path NOT LIKE '%MetadataPath%'")
    out = []
    for numeric_id, guid, path in rows:
        # Emby playback URLs use the numeric MediaItems.Id.  Keep the GUID
        # as a compatibility alias for API clients that address items by GUID.
        item_id = str(numeric_id).lower()
        if PATH_FROM and path.startswith(PATH_FROM):
            path = PATH_TO + path[len(PATH_FROM):]
        out.append((item_id, path))
        if guid:
            out.append((guid.hex() if isinstance(guid, bytes) else str(guid).lower(), path))
    return out


def from_api():
    if not API_KEY:
        print("EMBY_API_KEY is required for API mode", file=sys.stderr)
        return None
    query = urllib.parse.urlencode({
        "Recursive": "true",
        "IncludeItemTypes": "Movie,Episode,Audio",
        "Fields": "Path",
        "api_key": API_KEY,
    })
    url = f"{EMBY_URL}/Items?{query}"
    with urllib.request.urlopen(url, timeout=30) as r:
        data = json.load(r)
    out = []
    for it in data.get("Items", []):
        item_id = it.get("Id")
        path = it.get("Path")
        if not item_id or not path:
            continue
        if PATH_FROM and path.startswith(PATH_FROM):
            path = PATH_TO + path[len(PATH_FROM):]
        out.append((item_id.lower(), path))
    return out


def main():
    if EMBY_DB:
        items = from_db()
    else:
        items = from_api()
        if items is None:
            return 2

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as f:
        for item_id, path in items:
            f.write(f"{item_id}\t{path}\n")
    print(f"wrote {len(items)} items to {OUT}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
