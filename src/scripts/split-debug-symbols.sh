#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

usage()
{
  echo "usage: $0 <install-prefix> [symbols-dir]" >&2
  exit 2
}

[ "$#" -ge 1 ] || usage

prefix=$(realpath "$1")
symbols_dir=${2:-"$prefix/symbols"}
objcopy_bin=${OBJCOPY:-objcopy}
strip_bin=${STRIP:-strip}

[ -d "$prefix" ] || {
  echo "install prefix not found: $prefix" >&2
  exit 1
}

mkdir -p "$symbols_dir"

search_dirs=()
for dir in bin sbin lib libexec; do
  [ -d "$prefix/$dir" ] && search_dirs+=("$prefix/$dir")
done

count=0
skipped=0

mapfile -d '' files < <(find "${search_dirs[@]}" -type f -print0)

for file in "${files[@]}"; do
  magic=$(head -c 4 "$file" | od -An -tx1 | tr -d ' \n')
  [ "$magic" = "7f454c46" ] || continue

  if ! readelf -S "$file" 2>/dev/null | grep -q '\.debug_info'; then
    skipped=$((skipped + 1))
    continue
  fi

  rel=${file#"$prefix"/}
  symfile="$symbols_dir/$rel.debug"
  mkdir -p "$(dirname "$symfile")"

  "$objcopy_bin" --only-keep-debug "$file" "$symfile"
  chmod 644 "$symfile"
  "$strip_bin" --strip-unneeded "$file"

  debuglink=$(realpath --relative-to="$(dirname "$file")" "$symfile")
  (
    cd "$(dirname "$file")"
    "$objcopy_bin" --add-gnu-debuglink="$debuglink" "$(basename "$file")"
  )

  link_target=$(realpath --relative-to="$(dirname "$file")" "$symfile")
  link_path="$(dirname "$file")/$(basename "$symfile")"
  if [ -e "$link_path" ] && [ ! -L "$link_path" ]; then
    echo "warning: $link_path exists and is not a symlink" >&2
  else
    ln -sfn "$link_target" "$link_path"
  fi

  count=$((count + 1))
done

echo "split debug symbols: $count ELF files, $skipped without .debug_info"
echo "symbols directory: $symbols_dir"
