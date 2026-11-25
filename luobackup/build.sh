#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 || ! -f $1 ]]; then
    printf 'Usage: %s /path/to/BungeeCord.jar\n' "$0" >&2
    exit 2
fi
project=$(cd -- "$(dirname -- "$0")" && pwd)
mkdir -p -- "$project/build"
stage=$(mktemp -d "$project/build/.java-XXXXXXXX")
trap 'rm -rf -- "$stage"' EXIT
mkdir -- "$stage/classes"
javac --release 8 -encoding UTF-8 -cp "$1" -d "$stage/classes" \
    "$project/src/top/luotiany1/backup/LuoBackup.java"
jar cf "$stage/LuoBackup.jar" -C "$stage/classes" . -C "$project/resources" .
mv -f -- "$stage/LuoBackup.jar" "$project/build/LuoBackup.jar"
