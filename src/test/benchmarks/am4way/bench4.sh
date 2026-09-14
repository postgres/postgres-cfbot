#!/bin/bash
# [DO NOT MERGE] 2-way table-AM benchmark: HEAP vs FLUX
# Both AMs run the full OLTP workload.
export PATH=$HOME/pgi/bin:$PATH
D=/tmp/bench4data
RESULTS=~/bench4_results.txt
: > $RESULTS
newcluster() {
  pkill -9 -f "postgres -D $D" 2>/dev/null; sleep 1
  find $D -mindepth 1 -delete 2>/dev/null; rmdir $D 2>/dev/null
  initdb -D $D --no-sync >/dev/null 2>&1
  cat >> $D/postgresql.conf <<CONF
unix_socket_directories = '/tmp'
shared_buffers = 2GB
max_wal_size = 8GB
fsync = off
CONF
  pg_ctl -D $D -l /tmp/bench4.log -w -t 40 start >/dev/null 2>&1
}
alive() { psql -h /tmp -d postgres -tAqc "SELECT 1" 2>/dev/null | grep -q 1; }

echo "=== 2-way AM benchmark $(date -u) ===" | tee -a $RESULTS
echo "host: $(nproc) vCPU, $(free -g|awk '/Mem/{print $2}')G RAM; PG -O2 cassert, fsync=off" | tee -a $RESULTS
echo "" | tee -a $RESULTS

for AM in heap flux; do
  newcluster
  echo "----- AM=$AM -----" | tee -a $RESULTS
  # Tier 1: capability probe (scaling insert size to find the sustainable point)
  ok_rows=0
  for N in 100 1000 5000 20000; do
    if ! alive; then break; fi
    r=$(psql -h /tmp -d postgres -tAqc "DROP TABLE IF EXISTS t" -c "CREATE TABLE t(id int, k int, v bigint) USING $AM" \
         -c "INSERT INTO t SELECT g,g%1000,g FROM generate_series(1,$N) g" \
         -c "SELECT count(*) FROM t" 2>&1 | tail -1)
    if alive && echo "$r" | grep -qE "^$N$"; then ok_rows=$N; else echo "  bulk-insert crashes at N=$N" | tee -a $RESULTS; break; fi
  done
  echo "  max sustained bulk insert: $ok_rows rows" | tee -a $RESULTS
  if [ "$ok_rows" -lt 5000 ]; then
    echo "  => $AM NOT benchmark-capable at OLTP scale (bulk path incomplete)" | tee -a $RESULTS
    echo "" | tee -a $RESULTS
    continue
  fi
  # Tier 2: OLTP micro-benchmark (heap/flux) - timed insert / update / indexed-update / select
  newcluster
  psql -h /tmp -d postgres -tAqc "CREATE TABLE b(id int primary key, k int, v bigint, pad text) USING $AM" >/dev/null 2>&1
  T0=$(date +%s.%N)
  psql -h /tmp -d postgres -tAqc "INSERT INTO b SELECT g,g%1000,g,repeat('x',48) FROM generate_series(1,200000) g" >/dev/null 2>&1
  T1=$(date +%s.%N)
  psql -h /tmp -d postgres -tAqc "CREATE INDEX b_k ON b(k)" >/dev/null 2>&1
  T2=$(date +%s.%N)
  psql -h /tmp -d postgres -tAqc "UPDATE b SET v=v+1 WHERE id%2=0" >/dev/null 2>&1
  T3=$(date +%s.%N)
  psql -h /tmp -d postgres -tAqc "UPDATE b SET k=k+1000000 WHERE id%50=0" >/dev/null 2>&1
  T4=$(date +%s.%N)
  SEL=$(psql -h /tmp -d postgres -tAqc "SELECT count(*) FROM b WHERE k BETWEEN 100 AND 200" 2>&1|tail -1)
  L0=$(psql -h /tmp -d postgres -tAqc "SELECT pg_current_wal_lsn()")
  psql -h /tmp -d postgres -tAqc "UPDATE b SET v=v+1 WHERE id%2=0" >/dev/null 2>&1
  L1=$(psql -h /tmp -d postgres -tAqc "SELECT pg_current_wal_lsn()")
  WAL=$(psql -h /tmp -d postgres -tAqc "SELECT pg_wal_lsn_diff('$L1','$L0')::bigint")
  printf "  insert200k=%.2fs  index=%.2fs  update100k=%.2fs  idxupd4k=%.2fs  sel=%s  wal_per_100k_update=%sB\n" \
    "$(echo "$T1-$T0"|bc)" "$(echo "$T2-$T1"|bc)" "$(echo "$T3-$T2"|bc)" "$(echo "$T4-$T3"|bc)" "$SEL" "$WAL" | tee -a $RESULTS
  echo "" | tee -a $RESULTS
done
pkill -9 -f "postgres -D $D" 2>/dev/null
echo "BENCH4_DONE" | tee -a $RESULTS
