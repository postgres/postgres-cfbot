#!/usr/bin/env bash
# Build two postgres variants for tepid (HOT-indexed) A/B benchmarks.
#
# Env vars (all optional):
#   REPO          path to postgres source repo (default: $HOME/ws/postgres/tepid, or /scratch/siu-bench/repo)
#   BENCH         bench root (default: /scratch/siu-bench)
#   MASTER_REV    revision for the "master" variant (default: tepid's merge-base with origin/master)
#   TEPID_REV     revision for the "tepid"  variant (default: tepid)
#   JOBS          parallel compile jobs (default: nproc or 8)
set -euo pipefail

BENCH=${BENCH:-/scratch/siu-bench}
JOBS=${JOBS:-$( (command -v nproc >/dev/null && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 8 )}
if [ -z "${REPO:-}" ]; then
  for candidate in "$HOME/ws/postgres/tepid" "$BENCH/repo" /scratch/pg; do
    if [ -d "$candidate/.git" ]; then REPO=$candidate; break; fi
  done
fi
: "${REPO:?REPO not set and no default found}"
cd "$REPO"

TEPID_REV=${TEPID_REV:-tepid}
MASTER_REV=${MASTER_REV:-$(git merge-base "$TEPID_REV" origin/master 2>/dev/null || git merge-base "$TEPID_REV" master)}

echo "REPO=$REPO  MASTER=$MASTER_REV  TEPID=$TEPID_REV  JOBS=$JOBS  BENCH=$BENCH"

die() { printf 'build: %s\n' "$*" >&2; exit 1; }
[ -n "$MASTER_REV" ] || die "could not resolve MASTER_REV; set it explicitly or ensure origin/master exists"
if git status --porcelain | grep -v '^??' | grep -q .; then
  die "repo has unstaged/uncommitted changes; stash or commit first"
fi

build_variant() {
  local name=$1
  local rev=$2
  local prefix=$BENCH/$name
  echo "=== building $name ($rev) into $prefix"
  rm -rf "$prefix"
  mkdir -p "$prefix"
  git checkout --quiet --detach "$rev"
  local bld=$BENCH/_build_$name
  rm -rf "$bld"
  meson setup "$bld" --prefix="$prefix/usr/local/pgsql" \
    -Dbuildtype=release -Dcassert=false \
    "-Dextra_version=-siubench-$name" >/dev/null
  meson compile -C "$bld" -j "$JOBS"
  meson install -C "$bld" --destdir=/ >/dev/null
  "$prefix/usr/local/pgsql/bin/postgres" --version
}

ORIG=$(git symbolic-ref --quiet --short HEAD || git rev-parse HEAD)
trap 'git checkout --quiet "$ORIG"' EXIT

build_variant master "$MASTER_REV"
build_variant tepid  "$TEPID_REV"
