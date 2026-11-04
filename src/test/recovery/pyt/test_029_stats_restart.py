# Copyright (c) 2021-2026, PostgreSQL Global Development Group

"""Port of src/test/recovery/t/029_stats_restart.pl.

Tests statistics handling around restarts, including handling of crashes and
invalid stats files, as well as restoring stats after "normal" restarts.
"""

import datetime
import pathlib
import shutil
from typing import NamedTuple

# The counters each group of checks compares before and after a restart or a
# pg_stat_reset_shared(). Named rather than dicts so a mistyped field is an
# AttributeError here instead of a KeyError inside a comparison.


class IoStats(NamedTuple):
    writes: int
    reads: int


class CheckpointStats(NamedTuple):
    count: int
    reset: datetime.datetime


class WalStats(NamedTuple):
    records: int
    wal_bytes: int
    reset: datetime.datetime


def test_stats_restart(create_pg, tmp_path):
    node = create_pg("primary", allows_streaming=True, conf={"track_functions": "all"})

    db_under_test = "test"

    # node.sql() runs on the default 'postgres' database, which is what the
    # stats queries need; 'test' is only touched by the workload below, since
    # connecting to it eagerly would create database stats and break the
    # "stats were discarded" checks. node.sql() reconnects automatically after
    # every (re)start, so no manual reconnect is needed.

    def have_stats(kind, dboid, objid):
        return node.sql("SELECT pg_stat_have_stats($1, $2, $3)", kind, dboid, objid)

    def io_stats(context, obj, backend_type):
        return IoStats(
            *node.sql(
                "SELECT writes, reads FROM pg_stat_io "
                "WHERE context = $1 AND object = $2 AND backend_type = $3",
                context,
                obj,
                backend_type,
            )
        )

    def checkpoint_stats():
        return CheckpointStats(
            *node.sql(
                "SELECT num_timed + num_requested, stats_reset FROM pg_stat_checkpointer"
            )
        )

    def wal_stats():
        return WalStats(
            *node.sql("SELECT wal_records, wal_bytes, stats_reset FROM pg_stat_wal")
        )

    def trigger_funcrel_stat():
        # A fresh connection each time: it must connect to 'test' (generating
        # the stats under test), and the previous one is dead after a restart.
        node.connect(dbname=db_under_test).sql_batch(
            "SELECT * FROM tab_stats_crash_discard_test1",
            "SELECT func_stats_crash_discard1()",
            "SELECT pg_stat_force_next_flush()",
        )

    # Check some WAL statistics after a fresh startup.  The startup process
    # should have done WAL reads, and initialization some WAL writes.
    standalone = io_stats("init", "wal", "standalone backend")
    startup = io_stats("normal", "wal", "startup")

    assert standalone.writes > 0, "startup: increased standalone backend IO writes"
    assert startup.reads > 0, "startup: increased startup IO reads"

    # create test objects
    node.sql(f"CREATE DATABASE {db_under_test}")
    with node.connect(dbname=db_under_test) as dconn:
        dconn.sql(
            "CREATE TABLE tab_stats_crash_discard_test1 AS "
            "SELECT generate_series(1,100) AS a"
        )
        dconn.sql(
            "CREATE FUNCTION func_stats_crash_discard1() RETURNS VOID AS 'select 2;' "
            "LANGUAGE SQL IMMUTABLE"
        )

        # collect object oids
        dboid = dconn.sql(
            "SELECT oid FROM pg_database WHERE datname = $1", db_under_test
        )
        funcoid = dconn.sql("SELECT 'func_stats_crash_discard1()'::regprocedure::oid")
        tableoid = dconn.sql("SELECT 'tab_stats_crash_discard_test1'::regclass::oid")

    def assert_stats_exist(exist, phase):
        """Assert the database, function and relation stats are all (not) there.

        Every phase below checks the same three kinds together, so the phase name
        is an argument rather than a prefix repeated in each message.
        """
        for kind, objid in (
            ("database", 0),
            ("function", funcoid),
            ("relation", tableoid),
        ):
            assert have_stats(kind, dboid, objid) == exist, f"{phase}: {kind} stats"

    # generate stats and flush them
    trigger_funcrel_stat()

    # verify stats objects exist
    assert_stats_exist(True, "initial")

    # regular shutdown
    node.stop()

    # backup stats files
    statsfile = tmp_path / "discard_stats1"

    assert not statsfile.exists(), "backup statsfile cannot already exist"

    og_stats = pathlib.Path(node.datadir) / "pg_stat" / "pgstat.stat"

    assert og_stats.is_file(), "origin stats file must exist"

    shutil.copy(og_stats, statsfile)

    ## test discarding of stats file after crash etc

    node.start()

    assert_stats_exist(True, "copy")

    node.stop("immediate")

    assert not og_stats.exists(), "no stats file should exist after immediate shutdown"

    # copy the old stats back to test we discard stats after crash restart
    shutil.copy(statsfile, og_stats)
    node.start()

    # stats should have been discarded
    assert_stats_exist(False, "post immediate")

    # get rid of backup statsfile
    statsfile.unlink()

    # generate new stats and flush them
    trigger_funcrel_stat()

    assert_stats_exist(True, "post immediate, new")

    # regular shutdown
    node.stop()

    ## check an invalid stats file is handled

    # normal startup and no issues despite invalid stats file
    og_stats.write_text("ZZZZZZZZZZZZZ")
    node.start()

    # no stats present due to invalid stats file
    assert_stats_exist(False, "invalid_overwrite")

    ## check invalid stats file starting with valid contents, but followed by
    ## invalid content is handled.

    trigger_funcrel_stat()
    node.stop()
    with open(og_stats, "a") as f:
        f.write("XYZ")
    node.start()

    assert_stats_exist(False, "invalid_append")

    ## checks related to stats persistency around restarts and resets

    # Ensure enough checkpoints to protect against races for test after reset,
    # even on very slow machines.
    node.sql("CHECKPOINT")
    node.sql("CHECKPOINT")

    ## check checkpoint and wal stats are incremented due to restart

    ckpt_start = checkpoint_stats()
    wal_start = wal_stats()
    node.pg_ctl("restart")

    ckpt_restart = checkpoint_stats()
    wal_restart = wal_stats()

    assert ckpt_start.count < ckpt_restart.count, (
        "post restart: increased checkpoint count"
    )

    assert wal_start.records < wal_restart.records, (
        "post restart: increased wal record count"
    )

    assert wal_start.wal_bytes < wal_restart.wal_bytes, (
        "post restart: increased wal bytes"
    )

    assert ckpt_start.reset == ckpt_restart.reset, (
        "post restart: checkpoint stats_reset equal"
    )

    assert wal_start.reset == wal_restart.reset, "post restart: wal stats_reset equal"

    ## Check that checkpoint stats are reset, WAL stats aren't affected

    node.sql("SELECT pg_stat_reset_shared('checkpointer')")
    ckpt_reset = checkpoint_stats()
    wal_ckpt_reset = wal_stats()

    assert ckpt_restart.count > ckpt_reset.count, (
        "post ckpt reset: checkpoint count smaller"
    )

    assert ckpt_start.reset < ckpt_reset.reset, "post ckpt reset: stats_reset newer"

    assert wal_restart.records <= wal_ckpt_reset.records, (
        "post ckpt reset: wal record count not affected by reset"
    )

    assert wal_start.reset == wal_ckpt_reset.reset, (
        "post ckpt reset: wal stats_reset equal"
    )

    ## check that checkpoint stats stay reset after restart

    node.pg_ctl("restart")
    ckpt_restart_reset = checkpoint_stats()
    wal_restart2 = wal_stats()

    assert ckpt_restart_reset.count < ckpt_restart.count, (
        "post ckpt reset & restart: checkpoint still reset"
    )

    assert ckpt_restart_reset.reset == ckpt_reset.reset, (
        "post ckpt reset & restart: stats_reset same"
    )

    assert wal_ckpt_reset.records < wal_restart2.records, (
        "post ckpt reset & restart: increased wal record count"
    )

    assert wal_ckpt_reset.wal_bytes < wal_restart2.wal_bytes, (
        "post ckpt reset & restart: increased wal bytes"
    )

    assert wal_start.reset == wal_restart2.reset, (
        "post ckpt reset & restart: wal stats_reset equal"
    )

    ## check WAL stats stay reset

    node.sql("SELECT pg_stat_reset_shared('wal')")
    wal_reset = wal_stats()

    assert wal_reset.records < wal_restart2.records, (
        "post wal reset: smaller record count"
    )

    assert wal_reset.wal_bytes < wal_restart2.wal_bytes, "post wal reset: smaller bytes"
    assert wal_reset.reset > wal_restart2.reset, "post wal reset: newer stats_reset"

    node.pg_ctl("restart")
    wal_reset_restart = wal_stats()

    assert wal_reset_restart.records < wal_restart2.records, (
        "post wal reset & restart: smaller record count"
    )

    assert wal_reset.wal_bytes < wal_restart2.wal_bytes, (
        "post wal reset & restart: smaller bytes"
    )

    assert wal_reset.reset > wal_restart2.reset, (
        "post wal reset & restart: newer stats_reset"
    )

    # An immediate restart bumps the WAL stats_reset timestamp.
    node.stop("immediate")
    node.start()
    wal_restart_immediate = wal_stats()

    assert wal_reset_restart.reset < wal_restart_immediate.reset, (
        "post immediate restart: reset timestamp is new"
    )

    node.stop()
