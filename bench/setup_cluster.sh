#!/bin/bash
#
# Spin up a throwaway PG cluster from a specified install dir, configured for
# logical decoding, and print env vars to source.
#
# Usage:
#   eval "$(./setup_cluster.sh /tmp/pg_master_install /tmp/pg_master_data 55432)"
#   # now PGPORT, PGHOST etc. point at the new cluster
#
# Stop with:
#   $PGBINDIR/pg_ctl -D $PGDATA stop

set -euo pipefail

if [[ $# -lt 1 ]]; then
  cat <<EOF >&2
Usage: $0 <pg_install_dir> [datadir] [port]
  pg_install_dir: e.g. /tmp/pg_master_install (contains bin/initdb etc.)
  datadir:        default \$pg_install_dir/data
  port:           default 55432
EOF
  exit 1
fi

PGBINDIR="$1/bin"
PGDATA="${2:-$1/data}"
PGPORT="${3:-55432}"

[[ ! -x "$PGBINDIR/initdb" ]] && { echo "no initdb at $PGBINDIR" >&2; exit 1; }

# Stop if a previous cluster is running here.
if [[ -f "$PGDATA/postmaster.pid" ]]; then
  echo "stopping existing cluster at $PGDATA" >&2
  "$PGBINDIR/pg_ctl" -D "$PGDATA" stop -m fast || true
  sleep 1
fi

if [[ ! -d "$PGDATA/base" ]]; then
  echo "initdb at $PGDATA" >&2
  "$PGBINDIR/initdb" -D "$PGDATA" --auth=trust >/dev/null
fi

# Apply logical-decoding-friendly settings.
cat >> "$PGDATA/postgresql.auto.conf" <<EOF
# bench/setup_cluster.sh
wal_level = logical
max_wal_senders = 10
max_replication_slots = 10
shared_buffers = 256MB
logical_decoding_work_mem = 64kB
port = $PGPORT
unix_socket_directories = '$PGDATA'
EOF

echo "starting cluster" >&2
"$PGBINDIR/pg_ctl" -D "$PGDATA" -l "$PGDATA/server.log" start

# Wait until ready.
for i in {1..20}; do
  if "$PGBINDIR/pg_isready" -h "$PGDATA" -p "$PGPORT" >/dev/null 2>&1; then
    break
  fi
  sleep 0.5
done

# Emit env-vars to eval.
cat <<EOF
export PGBINDIR=$PGBINDIR
export PGDATA=$PGDATA
export PGHOST=$PGDATA
export PGPORT=$PGPORT
export PGDATABASE=postgres
export PGUSER=$USER
export PATH=$PGBINDIR:\$PATH
EOF
