
# Copyright (c) 2025, PostgreSQL Global Development Group

# This test verifies that a non-streamed transaction can launch a parallel apply
# worker, and that dependency tracking and commit order preservation work
# correctly during parallel apply.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

# Initialize publisher node
my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->append_conf('postgresql.conf',
    "max_prepared_transactions = 10");
$node_publisher->start;

# Create tables and insert initial data
$node_publisher->safe_psql(
    'postgres', qq(
    CREATE TABLE regress_tab (id int PRIMARY KEY, value text);

    CREATE TABLE tab_bin (k bytea PRIMARY KEY, v int);

    CREATE TABLE tab_ri_full (id int, value text);
    ALTER TABLE tab_ri_full REPLICA IDENTITY FULL;
    INSERT INTO tab_ri_full VALUES (1, 'test');

    CREATE TABLE tab_toast (a text NOT NULL, b text NOT NULL);
    ALTER TABLE tab_toast ALTER COLUMN a SET STORAGE EXTERNAL;
    CREATE UNIQUE INDEX tab_toast_ri_index on tab_toast (a, b);
    ALTER TABLE tab_toast REPLICA IDENTITY USING INDEX tab_toast_ri_index;
    INSERT INTO tab_toast(a, b) VALUES(repeat('1234567890', 200), '1234567890');

    CREATE TABLE regress_tab_pk (id int PRIMARY KEY);
    CREATE TABLE regress_tab_fk (id int PRIMARY KEY, fk int REFERENCES regress_tab_pk (id));

   CREATE TABLE pk_parted (
        id int PRIMARY KEY,
        value int
    ) PARTITION BY RANGE (id);

    CREATE TABLE pk_parted_1 (
        value int,
        id int NOT NULL
    );

    ALTER TABLE pk_parted ATTACH PARTITION pk_parted_1
    FOR VALUES FROM (1) TO (10);

    CREATE TABLE fk_parted (
        id int REFERENCES pk_parted(id),
        value int
    ) PARTITION BY RANGE (id);

    CREATE TABLE fk_parted_1 (
        value int,
        id int
    );

    ALTER TABLE fk_parted ATTACH PARTITION fk_parted_1
    FOR VALUES FROM (1) TO (10);
));
$node_publisher->safe_psql('postgres',
    "INSERT INTO regress_tab VALUES (generate_series(1, 10), 'test');");

# Create a publication
$node_publisher->safe_psql('postgres',
    "CREATE PUBLICATION regress_pub FOR ALL TABLES;");

# Initialize subscriber node
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
$node_subscriber->append_conf('postgresql.conf', "log_min_messages = debug1");
$node_subscriber->append_conf('postgresql.conf',
	"max_logical_replication_workers = 10
    max_prepared_transactions = 10");
$node_subscriber->start;

# Check if the extension injection_points is available, as it may be
# possible that this script is run with installcheck, where the module
# would not be installed by default.
if (!$node_subscriber->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$node_subscriber->safe_psql('postgres', 'CREATE EXTENSION injection_points;');

# Create a subscription
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';

$node_subscriber->safe_psql(
    'postgres', qq(
    CREATE TABLE regress_tab (id int PRIMARY KEY, value text);

    CREATE TABLE tab_bin (k bytea PRIMARY KEY, v int);

    CREATE TABLE tab_ri_full (id int, value text);
    ALTER TABLE tab_ri_full REPLICA IDENTITY FULL;

    CREATE TABLE tab_toast (a text NOT NULL, b text NOT NULL);
    ALTER TABLE tab_toast ALTER COLUMN a SET STORAGE EXTERNAL;
    CREATE UNIQUE INDEX tab_toast_ri_index on tab_toast (a, b);
    ALTER TABLE tab_toast REPLICA IDENTITY USING INDEX tab_toast_ri_index;

    CREATE TABLE regress_tab_pk (id int PRIMARY KEY);
    CREATE TABLE regress_tab_fk (id int PRIMARY KEY, fk int REFERENCES regress_tab_pk (id));

   CREATE TABLE pk_parted (
        id int PRIMARY KEY,
        value int
    ) PARTITION BY RANGE (id);

    CREATE TABLE pk_parted_1 (
        value int,
        id int NOT NULL
    );

    ALTER TABLE pk_parted ATTACH PARTITION pk_parted_1
    FOR VALUES FROM (1) TO (10);

    CREATE TABLE fk_parted (
        id int REFERENCES pk_parted(id),
        value int
    ) PARTITION BY RANGE (id);

    CREATE TABLE fk_parted_1 (
        value int,
        id int
    );

    ALTER TABLE fk_parted ATTACH PARTITION fk_parted_1
    FOR VALUES FROM (1) TO (10);
));
$node_subscriber->safe_psql('postgres',
    "CREATE SUBSCRIPTION regress_sub CONNECTION '$publisher_connstr' PUBLICATION regress_pub;");

# Wait for initial table sync to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, 'regress_sub');

##################################################
# Test that a non-streamed transaction can be applied in a parallel apply worker
##################################################

# Start a transaction to ensure the leader worker has seen the latest table sync
# READY state, ensuring parallel apply workers can be launched for later
# non-streamed transactions.
$node_publisher->safe_psql('postgres',
    "INSERT INTO regress_tab VALUES (generate_series(11, 20), 'test');");
$node_publisher->wait_for_catchup('regress_sub');

# Insert tuples again
$node_publisher->safe_psql('postgres',
    "INSERT INTO regress_tab VALUES (generate_series(21, 30), 'test');");
$node_publisher->wait_for_catchup('regress_sub');

# Verify the parallel apply worker is launched
my $result = $node_subscriber->safe_psql('postgres',
    "SELECT count(1) FROM pg_stat_activity WHERE backend_type = 'logical replication parallel worker'");
is($result, '1', "parallel apply worker is launched by a non-streamed transaction");

##################################################
# Test that the basic replica identity dependency tracking and commit order
# preservation work correctly during parallel apply.
##################################################

# Attach an injection_point. Parallel workers would wait before the commit
$node_subscriber->safe_psql('postgres',
	"SELECT injection_points_attach('parallel-worker-before-commit','wait');"
);

# Insert tuples on publisher
$node_publisher->safe_psql('postgres',
    "INSERT INTO regress_tab VALUES (generate_series(31, 40), 'test');");

# Wait until the parallel worker enters the injection point.
$node_subscriber->wait_for_event('logical replication parallel worker',
	'parallel-worker-before-commit');

my $offset = -s $node_subscriber->logfile;

# Insert tuples on publisher again. This transaction is independent from the
# previous one, but the parallel worker would wait till it finishes
$node_publisher->safe_psql('postgres',
    "INSERT INTO regress_tab VALUES (generate_series(41, 50), 'test');");

# Verify the parallel worker waits for the transaction
my $str = $node_subscriber->wait_for_log(qr/wait for depended xid ([1-9][0-9]+)/, $offset);
my $xid = $str =~ /wait for depended xid ([1-9][0-9]+)/;

ok(1, "commit order dependency detected for parallel apply");

$offset = -s $node_subscriber->logfile;

# Update tuples which have not been applied yet on subscriber because the
# parallel worker stops at the injection point. Newly assigned worker also
# waits for the same transactions as above.
$node_publisher->safe_psql('postgres',
    "UPDATE regress_tab SET value = 'updated' WHERE id BETWEEN 31 AND 35;");

# Verify the dependency is detected for the update
$node_subscriber->wait_for_log(qr/found conflicting replica identity change on table [1-9][0-9]+ from $xid/, $offset);

# Verify the parallel worker waits for the same transaction
$node_subscriber->wait_for_log(qr/wait for depended xid $xid/, $offset);

ok(1, "replica identity dependency detected for parallel apply");

# Wakeup the parallel worker. We detach first no to stop other parallel workers
$node_subscriber->safe_psql('postgres', qq[
    SELECT injection_points_detach('parallel-worker-before-commit');
    SELECT injection_points_wakeup('parallel-worker-before-commit');
]);

# Verify the parallel worker wakes up
$node_subscriber->wait_for_log(qr/finish waiting for depended xid $xid/, $offset);

$node_publisher->wait_for_catchup('regress_sub');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(1) FROM regress_tab");
is ($result, 50, 'inserts are replicated to subscriber');

$result =
  $node_subscriber->safe_psql('postgres',
    "SELECT count(1) FROM regress_tab WHERE value = 'updated'");
is ($result, 5, 'updates are also replicated to subscriber');

##################################################
# Test that dependency hash key comparison handles values containing zero
# bytes correctly when the subscription uses the binary option.
##################################################

$node_subscriber->safe_psql('postgres',
    "ALTER SUBSCRIPTION regress_sub DISABLE;");
$node_subscriber->poll_query_until('postgres',
       "SELECT count(*) = 0 FROM pg_stat_activity WHERE backend_type = 'logical replication apply worker'"
);
$node_subscriber->safe_psql(
    'postgres', "
    ALTER SUBSCRIPTION regress_sub SET (binary = true);
    ALTER SUBSCRIPTION regress_sub ENABLE;");

# Insert the test row and send a couple of warm-up transactions so that
# subsequent non-streamed transactions are assigned to parallel apply
# workers (see AllTablesyncsReady).
$node_publisher->safe_psql('postgres',
    "INSERT INTO tab_bin VALUES ('\\x6162006364', 0);");
$node_publisher->wait_for_catchup('regress_sub');
$node_publisher->safe_psql('postgres',
    "UPDATE tab_bin SET v = 0 WHERE k = '\\x6162006364';");
$node_publisher->wait_for_catchup('regress_sub');

# Attach an injection_point. Parallel workers would wait before the commit
$node_subscriber->safe_psql('postgres',
       "SELECT injection_points_attach('parallel-worker-before-commit','wait');"
);

# TX-1: update the row whose key contains a zero byte ('ab\0cd'). The
# parallel worker pauses before commit, keeping the key in the dependency
# hash table.
$node_publisher->safe_psql('postgres',
    "UPDATE tab_bin SET v = 1 WHERE k = '\\x6162006364';");

# Wait until the parallel worker enters the injection point.
$node_subscriber->wait_for_event('logical replication parallel worker',
       'parallel-worker-before-commit');

$offset = -s $node_subscriber->logfile;

# TX-2: update the same row (same key bytes). This must be detected as a
# conflict with TX-1.
$node_publisher->safe_psql('postgres',
    "UPDATE tab_bin SET v = 2 WHERE k = '\\x6162006364';");

# Verify the dependency is detected for the update
$str = $node_subscriber->wait_for_log(qr/found conflicting replica identity change on table [1-9][0-9]+ from ([1-9][0-9]+)/, $offset);
$xid = $str =~ /found conflicting replica identity change on table [1-9][0-9]+ from ([1-9][0-9]+)/;

# Verify the parallel worker waits for the same transaction
$node_subscriber->wait_for_log(qr/wait for depended xid $xid/, $offset);

ok(1, "replica identity dependency detected for binary key with zero bytes");

$offset = -s $node_subscriber->logfile;

# TX-3: insert a row whose key differs from TX-1's key only after the zero
# byte ('ab\0ef' vs 'ab\0cd'). This must NOT be treated as conflicting with
# TX-1: the keys are distinct when compared as raw bytes.
$node_publisher->safe_psql('postgres',
    "INSERT INTO tab_bin VALUES ('\\x6162006566', 3);");

# Wakeup the parallel workers and let everything apply
$node_subscriber->safe_psql('postgres', qq[
    SELECT injection_points_detach('parallel-worker-before-commit');
    SELECT injection_points_wakeup('parallel-worker-before-commit');
]);

$node_publisher->wait_for_catchup('regress_sub');

# Verify that no replica identity conflict was reported for TX-3
my $newlog = substr(slurp_file($node_subscriber->logfile), $offset);
unlike($newlog, qr/found conflicting replica identity change/,
       "no false dependency for keys differing after a zero byte");

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(1) FROM tab_bin");
is ($result, 2, 'changes are replicated to subscriber');

$result =
  $node_subscriber->safe_psql('postgres',
    "SELECT v FROM tab_bin WHERE k = '\\x6162006364'");
is ($result, 2, 'updates applied in commit order on subscriber');

# Disable binary mode for the subscription
$node_subscriber->safe_psql('postgres',
    "ALTER SUBSCRIPTION regress_sub DISABLE;");
$node_subscriber->poll_query_until('postgres',
       "SELECT count(*) = 0 FROM pg_stat_activity WHERE backend_type = 'logical replication apply worker'"
);
$node_subscriber->safe_psql(
    'postgres', "
    ALTER SUBSCRIPTION regress_sub SET (binary = false);
    ALTER SUBSCRIPTION regress_sub ENABLE;");

##################################################
# Test that the dependency tracking works correctly for unchanged toasted RI
# columns.
##################################################

# Attach an injection_point. Parallel workers would wait before the commit
$node_subscriber->safe_psql('postgres',
	"SELECT injection_points_attach('parallel-worker-before-commit','wait');"
);

# Update one replica identity column but keep toasted column unchanged
$node_publisher->safe_psql('postgres',
    "UPDATE tab_toast SET b = '1';");

# Wait until the parallel worker enters the injection point.
$node_subscriber->wait_for_event('logical replication parallel worker',
	'parallel-worker-before-commit');

$offset = -s $node_subscriber->logfile;

# Delete the updated row.
$node_publisher->safe_psql('postgres',
    "DELETE FROM tab_toast WHERE b = '1';");

# Verify the dependency is detected for the delete
$str = $node_subscriber->wait_for_log(qr/found conflicting replica identity change on table [1-9][0-9]+ from ([1-9][0-9]+)/, $offset);
$xid = $str =~ /found conflicting replica identity change on table [1-9][0-9]+ from ([1-9][0-9]+)/;

# Verify the parallel worker waits for the same transaction
$node_subscriber->wait_for_log(qr/wait for depended xid $xid/, $offset);

ok(1, "replica identity dependency from unchanged toasted column detected for parallel apply");

# Wakeup the parallel worker. We detach first no to stop other parallel workers
$node_subscriber->safe_psql('postgres', qq[
    SELECT injection_points_detach('parallel-worker-before-commit');
    SELECT injection_points_wakeup('parallel-worker-before-commit');
]);

# Verify the parallel worker wakes up
$node_subscriber->wait_for_log(qr/finish waiting for depended xid $xid/, $offset);

$node_publisher->wait_for_catchup('regress_sub');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(1) FROM tab_toast");
is ($result, 0, 'changes are replicated to subscriber');

##################################################
# Test that dependency tracking still works for REPLICA IDENTITY FULL when
# new tuple includes NULL key values.
##################################################

# Attach an injection_point. Parallel workers would wait before the commit
$node_subscriber->safe_psql('postgres',
	"SELECT injection_points_attach('parallel-worker-before-commit','wait');"
);

# Update one row to a non-NULL value and block commit in parallel worker.
$node_publisher->safe_psql('postgres',
        "UPDATE tab_ri_full SET value = NULL WHERE id = 1;");

# Wait until the parallel worker enters the injection point.
$node_subscriber->wait_for_event('logical replication parallel worker',
	'parallel-worker-before-commit');

$offset = -s $node_subscriber->logfile;

# This update sets a NULL value under REPLICA IDENTITY FULL. We must still
# detect dependency on the preceding update of the same row.
$node_publisher->safe_psql('postgres',
        "UPDATE tab_ri_full SET value = 'test' WHERE id = 1;");

# Verify the dependency is detected for the update with NULL key value.
$str = $node_subscriber->wait_for_log(qr/found conflicting replica identity change on table [1-9][0-9]+ from ([1-9][0-9]+)/, $offset);
$xid = $str =~ /found conflicting replica identity change on table [1-9][0-9]+ from ([1-9][0-9]+)/;

# Verify the parallel worker waits for the same transaction.
$node_subscriber->wait_for_log(qr/wait for depended xid $xid/, $offset);

ok(1, "replica identity FULL dependency with NULL values detected for parallel apply");

# Wakeup the parallel worker. We detach first no to stop other parallel workers
$node_subscriber->safe_psql('postgres', qq[
        SELECT injection_points_detach('parallel-worker-before-commit');
        SELECT injection_points_wakeup('parallel-worker-before-commit');
]);

# Verify the parallel worker wakes up.
$node_subscriber->wait_for_log(qr/finish waiting for depended xid $xid/, $offset);

$node_publisher->wait_for_catchup('regress_sub');

$result =
    $node_subscriber->safe_psql('postgres',
        "SELECT count(1) FROM tab_ri_full WHERE id = 1 AND value = 'test'");
is ($result, 1, 'update is replicated for REPLICA IDENTITY FULL table');

##################################################
# Test that table level dependency tracking by TRUNCATE work correctly during
# parallel apply.
##################################################

# Truncate the data for upcoming tests
$node_publisher->safe_psql('postgres', "TRUNCATE TABLE regress_tab;");
$node_publisher->wait_for_catchup('regress_sub');

# Attach an injection_point. Parallel workers would wait before the commit
$node_subscriber->safe_psql('postgres',
	"SELECT injection_points_attach('parallel-worker-before-commit','wait');"
);

# Insert tuples on publisher
$node_publisher->safe_psql('postgres',
    "TRUNCATE regress_tab;");

# Wait until the parallel worker enters the injection point.
$node_subscriber->wait_for_event('logical replication parallel worker',
	'parallel-worker-before-commit');

$offset = -s $node_subscriber->logfile;

# Insert a tuple that conflicts with the TRUNCATE operation.
$node_publisher->safe_psql('postgres',
    "INSERT INTO regress_tab VALUES (1, 'test');");

# Verify the dependency is detected for the insert
$str = $node_subscriber->wait_for_log(qr/found table-wide change affecting [1-9][0-9]+ from ([1-9][0-9]+)/, $offset);
$xid = $str =~ /found table-wide change affecting [1-9][0-9]+ from ([1-9][0-9]+)/;

ok(1, "table-wide dependency from TRUNCATE detected for parallel apply");

# Wakeup the parallel worker. We detach first no to stop other parallel workers
$node_subscriber->safe_psql('postgres', qq[
    SELECT injection_points_detach('parallel-worker-before-commit');
    SELECT injection_points_wakeup('parallel-worker-before-commit');
]);

# Verify the parallel worker waits for the same transaction
$node_subscriber->wait_for_log(qr/wait for depended xid $xid/, $offset);

# Verify the parallel worker wakes up
$node_subscriber->wait_for_log(qr/finish waiting for depended xid $xid/, $offset);

$node_publisher->wait_for_catchup('regress_sub');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(1) FROM regress_tab");
is ($result, 1, 'inserts are replicated to subscriber');

##################################################
# Test that the prepared transaction can be applied in a parallel apply worker,
# and that the commit order preservation work correctly.
##################################################

# Truncate the data for upcoming tests
$node_publisher->safe_psql('postgres', "TRUNCATE TABLE regress_tab;");
$node_publisher->wait_for_catchup('regress_sub');

$node_subscriber->safe_psql('postgres',
    "ALTER SUBSCRIPTION regress_sub DISABLE;");
$node_subscriber->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_activity WHERE backend_type = 'logical replication apply worker'"
);
$node_subscriber->safe_psql(
	'postgres', "
    ALTER SUBSCRIPTION regress_sub SET (two_phase = on);
    ALTER SUBSCRIPTION regress_sub ENABLE;");

$result = $node_subscriber->safe_psql('postgres',
    "SELECT count(1) FROM pg_stat_activity WHERE backend_type = 'logical replication parallel worker'");
is($result, '0', "no parallel apply workers exist after restart");

# Attach an injection_point. Parallel workers would wait before the prepare
$node_subscriber->safe_psql('postgres',
	"SELECT injection_points_attach('parallel-worker-before-prepare','wait');"
);

# PREPARE a transaction on publisher. It would be handled by a parallel apply
# worker.
$node_publisher->safe_psql('postgres', qq[
    BEGIN;
    INSERT INTO regress_tab VALUES (generate_series(51, 60), 'prepare');
    PREPARE TRANSACTION 'regress_prepare';
]);

# Wait until the parallel worker enters the injection point.
$node_subscriber->wait_for_event('logical replication parallel worker',
	'parallel-worker-before-prepare');

$offset = -s $node_subscriber->logfile;

# Insert tuples on publisher again. This transaction waits for the prepared
# transaction
$node_publisher->safe_psql('postgres',
    "INSERT INTO regress_tab VALUES (generate_series(61, 70), 'test');");

# Verify the parallel worker waits for the transaction
$str = $node_subscriber->wait_for_log(qr/wait for depended xid ([1-9][0-9]+)/, $offset);
$xid = $str =~ /wait for depended xid ([1-9][0-9]+)/;

ok(1, "commit order dependency from prepared transaction detected for parallel apply");

# Wakeup the parallel worker
$node_subscriber->safe_psql('postgres', qq[
    SELECT injection_points_detach('parallel-worker-before-prepare');
    SELECT injection_points_wakeup('parallel-worker-before-prepare');
]);

$node_subscriber->wait_for_log(qr/finish waiting for depended xid $xid/, $offset);

# COMMIT the prepared transaction. It is always handled by the leader
$node_publisher->safe_psql('postgres', "COMMIT PREPARED 'regress_prepare';");
$node_publisher->wait_for_catchup('regress_sub');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(1) FROM regress_tab");
is ($result, 20, 'inserts are replicated to subscriber');

##################################################
# Test that streaming transactions respect commit order preservation when
# non-streaming transactions are being applied in parallel workers.
##################################################

$node_publisher->append_conf('postgresql.conf',
   "logical_decoding_work_mem = 64kB");
$node_publisher->reload;

# Truncate the data for upcoming tests
$node_publisher->safe_psql('postgres', "TRUNCATE TABLE regress_tab;");
$node_publisher->wait_for_catchup('regress_sub');

# Attach the injection_point again
$node_subscriber->safe_psql('postgres',
	"SELECT injection_points_attach('parallel-worker-before-commit','wait');"
);

$node_publisher->safe_psql('postgres',
    "INSERT INTO regress_tab VALUES (generate_series(71, 80), 'test');");

# Wait until the parallel worker enters the injection point.
$node_subscriber->wait_for_event('logical replication parallel worker',
	'parallel-worker-before-commit');

# Run a transaction which would be streamed
my $h = $node_publisher->background_psql('postgres', on_error_stop => 0);

$offset = -s $node_subscriber->logfile;

$h->query_safe(
	q{
BEGIN;
UPDATE regress_tab SET value = 'streamed-updated' WHERE id BETWEEN 71 AND 80;
INSERT INTO regress_tab VALUES (generate_series(100, 5100), 'streamed');
});

# Verify the dependency is detected for the delete
$str = $node_subscriber->wait_for_log(qr/found conflicting replica identity change on table [1-9][0-9]+ from ([1-9][0-9]+)/, $offset);
$xid = $str =~ /found conflicting replica identity change on table [1-9][0-9]+ from ([1-9][0-9]+)/;

# Verify the parallel worker waits for the same transaction
$node_subscriber->wait_for_log(qr/wait for depended xid $xid/, $offset);

ok(1, "replica identity dependency from streamed txn detected for parallel apply");

# Wakeup the parallel worker
$node_subscriber->safe_psql('postgres', qq[
    SELECT injection_points_detach('parallel-worker-before-commit');
    SELECT injection_points_wakeup('parallel-worker-before-commit');
]);

# Verify the streamed transaction can be applied
$node_subscriber->wait_for_log(qr/finish waiting for depended xid $xid/, $offset);

$h->query_safe("COMMIT;");

$node_publisher->wait_for_catchup('regress_sub');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(1) FROM regress_tab");
is ($result, 5011, 'inserts are replicated to subscriber');

##################################################
# Test that mutable user-defined triggers force apply-time waiting so
# conflicting trigger side effects are serialized.
##################################################

# Truncate the data for upcoming tests
$node_publisher->safe_psql('postgres', "TRUNCATE TABLE regress_tab;");
$node_publisher->wait_for_catchup('regress_sub');

# The TRUNCATE invalidated the publisher's relmap entry for regress_tab, so
# the next transaction touching the table carries a fresh RELATION message.
# A RELATION message records a table-wide dependency, which would make TX-2
# below wait for TX-1 before even applying its change, hiding the
# unsafe-trigger wait this test exercises. Absorb the RELATION message with
# a dummy transaction (net zero rows) and let it commit fully.
$node_publisher->safe_psql('postgres',
    "INSERT INTO regress_tab VALUES (100); DELETE FROM regress_tab WHERE id = 100;");
$node_publisher->wait_for_catchup('regress_sub');

$node_subscriber->safe_psql('postgres', qq[
    CREATE OR REPLACE FUNCTION regress_trigger_guard_fn()
    RETURNS trigger
    LANGUAGE plpgsql
    AS \$\$
    BEGIN
        IF NOT EXISTS (SELECT 1 FROM public.regress_tab WHERE id = 1) THEN
            UPDATE public.regress_tab SET id = 2 WHERE id = 1;
        END IF;
        RETURN NEW;
    END;
    \$\$;

    CREATE TRIGGER regress_trigger_guard_tg
    BEFORE INSERT ON regress_tab
    FOR EACH ROW
    EXECUTE FUNCTION regress_trigger_guard_fn();
]);

# Ensure trigger fires during logical replication apply.
$node_subscriber->safe_psql('postgres',
	"ALTER TABLE regress_tab ENABLE REPLICA TRIGGER regress_trigger_guard_tg;"
);

# Hold the first parallel worker just before commit.
$node_subscriber->safe_psql('postgres',
	"SELECT injection_points_attach('parallel-worker-before-commit','wait');"
);

# TX-1: first insert; worker pauses at before-commit injection point.
$node_publisher->safe_psql('postgres',
    "INSERT INTO regress_tab VALUES (1);");

$node_subscriber->wait_for_event('logical replication parallel worker',
	'parallel-worker-before-commit');

$offset = -s $node_subscriber->logfile;

# TX-2: second insert. For unsafe trigger tables, this must wait before apply.
$node_publisher->safe_psql('postgres',
    "INSERT INTO regress_tab VALUES (2);");

$node_subscriber->wait_for_log(qr/found parallel unsafe change on table [1-9][0-9]* for action [0-9]+/, $offset);

$str = $node_subscriber->wait_for_log(qr/wait for depended xid ([1-9][0-9]+)/, $offset);
$xid = $str =~ /wait for depended xid ([1-9][0-9]+)/;

ok(1, "mutable trigger relation waits before apply in parallel mode");

# Resume workers.
$node_subscriber->safe_psql('postgres', qq[
    SELECT injection_points_detach('parallel-worker-before-commit');
    SELECT injection_points_wakeup('parallel-worker-before-commit');
]);

$node_subscriber->wait_for_log(qr/finish waiting for depended xid $xid/, $offset);
$node_publisher->wait_for_catchup('regress_sub');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(1) FROM regress_tab");
is ($result, 2, 'inserts are replicated to subscriber');

##################################################
# Test that the dependency tracking works correctly for local unique indexes on
# subscriber during parallel apply.
##################################################

# Truncate the data for upcoming tests
$node_publisher->safe_psql('postgres', "TRUNCATE TABLE regress_tab;");
$node_publisher->wait_for_catchup('regress_sub');

# Define an unique index on subscriber
$node_subscriber->safe_psql('postgres',
    "CREATE UNIQUE INDEX local_unique_idx ON regress_tab (value);");

$node_publisher->safe_psql('postgres',
    "INSERT INTO regress_tab VALUES (1, 'would conflict');");

$node_publisher->wait_for_catchup('regress_sub');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(1) FROM regress_tab");
is ($result, 1, 'the insert is replicated to subscriber');

# Attach an injection_point. Parallel workers would wait before the commit
$node_subscriber->safe_psql('postgres',
	"SELECT injection_points_attach('parallel-worker-before-commit','wait');"
);

# Delete the tuple on publisher.
$node_publisher->safe_psql('postgres',
    "DELETE FROM regress_tab WHERE id = 1;");

# Wait until the parallel worker enters the injection point.
$node_subscriber->wait_for_event('logical replication parallel worker',
	'parallel-worker-before-commit');

$offset = -s $node_subscriber->logfile;

# Insert tuples. This should conflict with the DELETE transaction, as both
# transactions modify the same key. The parallel worker will wait for the
# preceding transaction to finish.
$node_publisher->safe_psql('postgres',
    "INSERT INTO regress_tab VALUES (2, 'would conflict');");

# Verify the dependency is detected for the insert
$str = $node_subscriber->wait_for_log(qr/found conflicting local unique key change on table [1-9][0-9]+ from ([1-9][0-9]+)/, $offset);
$xid = $str =~ /found conflicting local unique key change on table [1-9][0-9]+ from ([1-9][0-9]+)/;

# Verify the parallel worker waits for the same transaction
$node_subscriber->wait_for_log(qr/wait for depended xid $xid/, $offset);

ok(1, "local unique key dependency detected for parallel apply");

# Wakeup the parallel worker
$node_subscriber->safe_psql('postgres', qq[
    SELECT injection_points_detach('parallel-worker-before-commit');
    SELECT injection_points_wakeup('parallel-worker-before-commit');
]);

# Verify the streamed transaction can be applied
$node_subscriber->wait_for_log(qr/finish waiting for depended xid $xid/, $offset);

$node_publisher->wait_for_catchup('regress_sub');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(1) FROM regress_tab");
is ($result, 1, 'inserts are replicated to subscriber');

# Test that the dependency tracking works correctly for local unique indexes on
# subscriber during parallel apply when the unique index has expression.
$node_subscriber->safe_psql('postgres', "DROP INDEX local_unique_idx;");
$node_subscriber->safe_psql('postgres',
    "CREATE UNIQUE INDEX local_unique_idx_expr ON regress_tab ((LOWER(value)));");

# Attach an injection_point. Parallel workers would wait before the commit
$node_subscriber->safe_psql('postgres',
	"SELECT injection_points_attach('parallel-worker-before-commit','wait');"
);

# Insert a tuple on publisher. Parallel worker would wait at the injection
# point
$node_publisher->safe_psql('postgres',
    "DELETE FROM regress_tab WHERE id = 2;");

# Wait until the parallel worker enters the injection point.
$node_subscriber->wait_for_event('logical replication parallel worker',
	'parallel-worker-before-commit');

$offset = -s $node_subscriber->logfile;

# Insert tuples. This should conflict with the DELETE transaction, as both
# transactions modify the same key value. The parallel worker will wait for the
# preceding transaction to finish.
$node_publisher->safe_psql('postgres',
    "INSERT INTO regress_tab VALUES (3, 'WOULD CONFLICT');");

# Verify the dependency is detected for the insert
$str = $node_subscriber->wait_for_log(qr/found conflicting local unique key change on table [1-9][0-9]+ from ([1-9][0-9]+)/, $offset);
$xid = $str =~ /found conflicting local unique key change on table [1-9][0-9]+ from ([1-9][0-9]+)/;

# Verify the parallel worker waits for the same transaction
$node_subscriber->wait_for_log(qr/wait for depended xid $xid/, $offset);

ok(1, "local unique key dependency from index expression detected for parallel apply");

# Wakeup the parallel worker
$node_subscriber->safe_psql('postgres', qq[
    SELECT injection_points_detach('parallel-worker-before-commit');
    SELECT injection_points_wakeup('parallel-worker-before-commit');
]);

# Verify the streamed transaction can be applied
$node_subscriber->wait_for_log(qr/finish waiting for depended xid $xid/, $offset);

$node_publisher->wait_for_catchup('regress_sub');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(1) FROM regress_tab");
is ($result, 1, 'inserts are replicated to subscriber');

# Cleanup
$node_subscriber->safe_psql('postgres', "DROP INDEX local_unique_idx_expr;");
$node_publisher->safe_psql('postgres', "TRUNCATE TABLE regress_tab;");
$node_publisher->wait_for_catchup('regress_sub');

##################################################
# Test that the dependency tracking works correctly when a local unique index
# exists only on a child (leaf) partition of a partitioned table.
#
# The publisher has a regular (non-partitioned) table.
# Table design:
#
# (publiser-side)         (subscriber-side)
# regress_part_tab ------ regress_part_tab
#                            |
#                            +----- regress_part_tab_1
#                                   (has an unique index on 'value', with
#                                    columns in a different order from root)
##################################################

# Publisher: plain table.
$node_publisher->safe_psql('postgres', qq[
    CREATE TABLE regress_part_tab (id int PRIMARY KEY, value text, marker text);
    ALTER TABLE regress_part_tab REPLICA IDENTITY FULL;
]);

# Subscriber: partitioned table with a leaf-only unique index on 'value'.  The
# leaf's physical column order differs from the root table's column order.
$node_subscriber->safe_psql('postgres', qq[
    CREATE TABLE regress_part_tab (id int PRIMARY KEY, value text, marker text)
        PARTITION BY RANGE (id);
    CREATE TABLE regress_part_tab_1 (value text, marker text, id int PRIMARY KEY);
    ALTER TABLE regress_part_tab ATTACH PARTITION regress_part_tab_1
        FOR VALUES FROM (1) TO (100);
    CREATE UNIQUE INDEX regress_part_tab_1_value_idx
        ON regress_part_tab_1 (value);
]);

$node_subscriber->safe_psql('postgres',
    "ALTER SUBSCRIPTION regress_sub REFRESH PUBLICATION WITH (copy_data = false);");

# Insert a row that will become the conflict anchor.
$node_publisher->safe_psql('postgres',
    "INSERT INTO regress_part_tab VALUES (1, 'leaf_unique_val', 'leaf_marker_val');");
$node_publisher->wait_for_catchup('regress_sub');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(1) FROM regress_part_tab");
is($result, '1', 'initial row inserted into partitioned subscriber table');

$node_subscriber->safe_psql('postgres',
	"SELECT injection_points_attach('parallel-worker-before-commit','wait');");

# Tx-1: Delete the row.  The parallel worker records a dependency using the
# leaf partition's local unique index metadata and then pauses at the injection
# point.
$node_publisher->safe_psql('postgres',
	"DELETE FROM regress_part_tab WHERE id = 1;");

$node_subscriber->wait_for_event('logical replication parallel worker',
	'parallel-worker-before-commit');

$offset = -s $node_subscriber->logfile;

# Tx-2: Insert a new row reusing value='leaf_unique_val'.  If applied before
# Tx-1 commits, the leaf's unique index would be violated.  The dependency
# tracking must detect this conflict and make Tx-2 wait for Tx-1.
$node_publisher->safe_psql('postgres',
    "INSERT INTO regress_part_tab VALUES (2, 'leaf_unique_val', 'leaf_marker_val');");

# Verify the dependency is detected via the leaf partition's unique index.
$str = $node_subscriber->wait_for_log(
	qr/found conflicting local unique key change on table [1-9][0-9]+ from ([1-9][0-9]+)/,
	$offset);
$xid = $str =~ /found conflicting local unique key change on table [1-9][0-9]+ from ([1-9][0-9]+)/;

$node_subscriber->wait_for_log(qr/wait for depended xid $xid/, $offset);

ok(1,
   "leaf-only unique key dependency with reordered leaf columns detected for partitioned table parallel apply");

$node_subscriber->safe_psql('postgres', qq[
    SELECT injection_points_detach('parallel-worker-before-commit');
    SELECT injection_points_wakeup('parallel-worker-before-commit');
]);

$node_subscriber->wait_for_log(qr/finish waiting for depended xid $xid/, $offset);
$node_publisher->wait_for_catchup('regress_sub');

# Net result: the DELETE (id=1) and INSERT (id=2) both commit; one row remains.
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(1) FROM regress_part_tab");
is($result, '1',
	'changes to partitioned subscriber table are replicated correctly');

# Replace the leaf-only unique index after the root relation's partitioned-table
# information has already been cached.  The leaf relcache invalidation must also
# invalidate the root relmap entry so the next dependency check uses the new
# leaf index metadata.
$node_subscriber->safe_psql('postgres', qq[
    DROP INDEX regress_part_tab_1_value_idx;
    CREATE UNIQUE INDEX regress_part_tab_1_marker_idx
        ON regress_part_tab_1 (marker);
]);

$node_subscriber->safe_psql('postgres',
    "SELECT injection_points_attach('parallel-worker-before-commit','wait');");

# Tx-1: Delete the row.  With a refreshed root cache, this records a dependency
# using the replacement leaf-only unique index on 'marker'.
$node_publisher->safe_psql('postgres',
    "DELETE FROM regress_part_tab WHERE id = 2;");

$node_subscriber->wait_for_event('logical replication parallel worker',
    'parallel-worker-before-commit');

$offset = -s $node_subscriber->logfile;

# Tx-2: Use a different value but reuse the marker.  A stale root cache would
# still track the dropped 'value' index and miss this dependency.
$node_publisher->safe_psql('postgres',
    "INSERT INTO regress_part_tab VALUES (3, 'different_leaf_val', 'leaf_marker_val');");

$str = $node_subscriber->wait_for_log(
    qr/found conflicting local unique key change on table [1-9][0-9]+ from ([1-9][0-9]+)/,
    $offset);
$xid = $str =~ /found conflicting local unique key change on table [1-9][0-9]+ from ([1-9][0-9]+)/;

$node_subscriber->wait_for_log(qr/wait for depended xid $xid/, $offset);

ok(1,
   "leaf relcache invalidation refreshes partitioned table local unique keys");

$node_subscriber->safe_psql('postgres', qq[
    SELECT injection_points_detach('parallel-worker-before-commit');
    SELECT injection_points_wakeup('parallel-worker-before-commit');
]);

$node_subscriber->wait_for_log(qr/finish waiting for depended xid $xid/, $offset);
$node_publisher->wait_for_catchup('regress_sub');

$result = $node_subscriber->safe_psql('postgres',
    "SELECT count(1) FROM regress_part_tab");
is($result, '1',
    'changes after leaf unique index replacement are replicated correctly');

##################################################
# Test that the dependency tracking works correctly for foreign keys on
# subscriber during parallel apply.
##################################################

# Test that when receiving table schema information for a referencing table, the
# subscriber correctly checks for dependencies on the referenced table and waits
# for its changes to complete.
$node_subscriber->safe_psql('postgres',
	"SELECT injection_points_attach('parallel-worker-before-commit','wait');"
);

# Enable foreign triggers on subscriber
my $pk_fk_trigger = $node_subscriber->safe_psql('postgres', qq[
    SELECT tgname
    FROM pg_trigger
    WHERE tgrelid = 'regress_tab_pk'::regclass
      AND tgconstraint > 0
    LIMIT 1
]);
chomp($pk_fk_trigger);

my $fk_fk_trigger = $node_subscriber->safe_psql('postgres', qq[
    SELECT tgname
    FROM pg_trigger
    WHERE tgrelid = 'regress_tab_fk'::regclass
      AND tgconstraint > 0
    LIMIT 1
]);
chomp($fk_fk_trigger);

$node_subscriber->safe_psql('postgres',
    qq[ALTER TABLE regress_tab_pk ENABLE REPLICA TRIGGER "$pk_fk_trigger";
    ALTER TABLE regress_tab_fk ENABLE REPLICA TRIGGER "$fk_fk_trigger";]);

$node_publisher->safe_psql('postgres',
    "INSERT INTO regress_tab_pk VALUES (2);");

$node_subscriber->wait_for_event('logical replication parallel worker',
	'parallel-worker-before-commit');

$offset = -s $node_subscriber->logfile;

$node_publisher->safe_psql('postgres',
    "INSERT INTO regress_tab_fk VALUES (1, 2);");

# Verify the dependency is detected on referenced table schema information
$str = $node_subscriber->wait_for_log(qr/found conflicting change on table [1-9][0-9]+ from ([1-9][0-9]+)/, $offset);
$xid = $str =~ /found conflicting change on table [1-9][0-9]+ from ([1-9][0-9]+)/;

# Verify the parallel worker waits for the same transaction
$node_subscriber->wait_for_log(qr/wait for depended xid $xid/, $offset);

ok(1, "referenced table dependency detected for parallel apply");

# Wakeup the parallel worker
$node_subscriber->safe_psql('postgres', qq[
    SELECT injection_points_detach('parallel-worker-before-commit');
    SELECT injection_points_wakeup('parallel-worker-before-commit');
]);

# Verify the streamed transaction can be applied
$node_subscriber->wait_for_log(qr/finish waiting for depended xid $xid/, $offset);
$node_publisher->wait_for_catchup('regress_sub');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(1) FROM regress_tab_pk");
is ($result, 1, 'insert is replicated to referenced table on subscriber');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(1) FROM regress_tab_fk");
is ($result, 1, 'insert is replicated to referencing table on subscriber');

# INSERT - INSERT case: Tx-1 inserts referenced tuple and Tx-2 inserts
# referencing tuple.

$node_subscriber->safe_psql('postgres',
	"SELECT injection_points_attach('parallel-worker-before-commit','wait');"
);

$node_publisher->safe_psql('postgres',
    "INSERT INTO regress_tab_pk VALUES (3);");

$node_subscriber->wait_for_event('logical replication parallel worker',
	'parallel-worker-before-commit');

$offset = -s $node_subscriber->logfile;

$node_publisher->safe_psql('postgres',
    "INSERT INTO regress_tab_fk VALUES (2, 3);");

# Verify the dependency for referenced key change is detected
$str = $node_subscriber->wait_for_log(qr/found conflicting referenced key change on table [1-9][0-9]+ from ([1-9][0-9]+)/, $offset);
$xid = $str =~ /found conflicting referenced key change on table [1-9][0-9]+ from ([1-9][0-9]+)/;

# Verify the parallel worker waits for the same transaction
$node_subscriber->wait_for_log(qr/wait for depended xid $xid/, $offset);

ok(1, "referenced key dependency detected for parallel apply");

# Wakeup the parallel worker
$node_subscriber->safe_psql('postgres', qq[
    SELECT injection_points_detach('parallel-worker-before-commit');
    SELECT injection_points_wakeup('parallel-worker-before-commit');
]);

# Verify the streamed transaction can be applied
$node_subscriber->wait_for_log(qr/finish waiting for depended xid $xid/, $offset);
$node_publisher->wait_for_catchup('regress_sub');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(1) FROM regress_tab_pk");
is ($result, 2, 'insert is replicated to referenced table on subscriber');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(1) FROM regress_tab_fk");
is ($result, 2, 'insert is replicated to referencing table on subscriber');

# DELETE - DELETE case: Tx-1 deletes referencing tuple and Tx-2 deletes
# referenced tuple.
$node_subscriber->safe_psql('postgres',
	"SELECT injection_points_attach('parallel-worker-before-commit','wait');"
);

$node_publisher->safe_psql('postgres',
    "DELETE FROM regress_tab_fk WHERE id = 1;");

$node_subscriber->wait_for_event('logical replication parallel worker',
	'parallel-worker-before-commit');

$offset = -s $node_subscriber->logfile;

$node_publisher->safe_psql('postgres',
    "DELETE FROM regress_tab_pk WHERE id = 2;");

$str = $node_subscriber->wait_for_log(qr/found conflicting foreign key change on table [1-9][0-9]+ from ([1-9][0-9]+)/, $offset);
$xid = $str =~ /found conflicting foreign key change on table [1-9][0-9]+ from ([1-9][0-9]+)/;

$node_subscriber->wait_for_log(qr/wait for depended xid $xid/, $offset);

ok(1, "foreign key dependency detected for parallel apply");

$node_subscriber->safe_psql('postgres', qq[
    SELECT injection_points_detach('parallel-worker-before-commit');
    SELECT injection_points_wakeup('parallel-worker-before-commit');
]);

$node_subscriber->wait_for_log(qr/finish waiting for depended xid $xid/, $offset);
$node_publisher->wait_for_catchup('regress_sub');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(1) FROM regress_tab_pk");
is ($result, 1, 'delete is replicated to referenced table on subscriber');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(1) FROM regress_tab_fk");
is ($result, 1, 'delete is replicated to referencing table on subscriber');

##################################################
# Test that the dependency tracking works correctly for foreign keys when both
# the referenced and referencing tables are partitioned.
##################################################

$node_subscriber->safe_psql('postgres',
	"SELECT injection_points_attach('parallel-worker-before-commit','wait');"
);

my $part_pk_trigger = $node_subscriber->safe_psql('postgres', qq[
        SELECT tgname
        FROM pg_trigger
        WHERE tgrelid = 'pk_parted_1'::regclass
            AND tgconstraint > 0
        LIMIT 1
]);
chomp($part_pk_trigger);

my $part_fk_trigger = $node_subscriber->safe_psql('postgres', qq[
        SELECT tgname
        FROM pg_trigger
        WHERE tgrelid = 'fk_parted_1'::regclass
            AND tgconstraint > 0
        LIMIT 1
]);
chomp($part_fk_trigger);

$node_subscriber->safe_psql('postgres',
        qq[ALTER TABLE pk_parted_1 ENABLE REPLICA TRIGGER "$part_pk_trigger";
        ALTER TABLE fk_parted_1 ENABLE REPLICA TRIGGER "$part_fk_trigger";]);

$node_publisher->safe_psql('postgres',
    "INSERT INTO pk_parted_1(id, value) VALUES (1, 10);");

$node_subscriber->wait_for_event('logical replication parallel worker',
	'parallel-worker-before-commit');

$offset = -s $node_subscriber->logfile;

$node_publisher->safe_psql('postgres',
    "INSERT INTO fk_parted_1(id, value) VALUES (1, 10);");

# Verify the dependency is detected on referenced table schema information
$str = $node_subscriber->wait_for_log(qr/found conflicting change on table [1-9][0-9]+ from ([1-9][0-9]+)/, $offset);
$xid = $str =~ /found conflicting change on table [1-9][0-9]+ from ([1-9][0-9]+)/;

# Verify the parallel worker waits for the same transaction
$node_subscriber->wait_for_log(qr/wait for depended xid $xid/, $offset);

ok(1, "partitioned referenced table dependency detected for parallel apply");

# Wakeup the parallel worker
$node_subscriber->safe_psql('postgres', qq[
    SELECT injection_points_detach('parallel-worker-before-commit');
    SELECT injection_points_wakeup('parallel-worker-before-commit');
]);

# Verify the streamed transaction can be applied
$node_subscriber->wait_for_log(qr/finish waiting for depended xid $xid/, $offset);
$node_publisher->wait_for_catchup('regress_sub');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(1) FROM pk_parted_1");
is ($result, 1, 'insert is replicated to referenced table on subscriber');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(1) FROM fk_parted_1");
is ($result, 1, 'insert is replicated to referencing table on subscriber');

# INSERT - INSERT case: Tx-1 inserts referenced tuple and Tx-2 inserts
# referencing tuple.

$node_subscriber->safe_psql('postgres',
	"SELECT injection_points_attach('parallel-worker-before-commit','wait');"
);

$node_publisher->safe_psql('postgres',
    "INSERT INTO pk_parted_1(id, value) VALUES (2, 20);");

$node_subscriber->wait_for_event('logical replication parallel worker',
	'parallel-worker-before-commit');

$offset = -s $node_subscriber->logfile;

$node_publisher->safe_psql('postgres',
    "INSERT INTO fk_parted_1(id, value) VALUES (2, 20);");

# Verify the dependency for referenced key change is detected
$str = $node_subscriber->wait_for_log(qr/found conflicting referenced key change on table [1-9][0-9]+ from ([1-9][0-9]+)/, $offset);
$xid = $str =~ /found conflicting referenced key change on table [1-9][0-9]+ from ([1-9][0-9]+)/;

# Verify the parallel worker waits for the same transaction
$node_subscriber->wait_for_log(qr/wait for depended xid $xid/, $offset);

ok(1, "partitioned referenced key dependency detected for parallel apply");

# Wakeup the parallel worker
$node_subscriber->safe_psql('postgres', qq[
    SELECT injection_points_detach('parallel-worker-before-commit');
    SELECT injection_points_wakeup('parallel-worker-before-commit');
]);

# Verify the streamed transaction can be applied
$node_subscriber->wait_for_log(qr/finish waiting for depended xid $xid/, $offset);
$node_publisher->wait_for_catchup('regress_sub');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(1) FROM pk_parted_1");
is ($result, 2, 'insert is replicated to referenced table on subscriber');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(1) FROM fk_parted_1");
is ($result, 2, 'insert is replicated to referencing table on subscriber');

##################################################
# Test that dependency tracking works correctly when local foreign keys are
# defined on partitioned subscriber tables mapped from regular publisher tables.
##################################################

$node_publisher->safe_psql('postgres', qq[
    CREATE TABLE regress_fk_root_pk (id int PRIMARY KEY, value int);
    CREATE TABLE regress_fk_root_fk (
        id int,
        pk_id int REFERENCES regress_fk_root_pk(id),
        value int,
        PRIMARY KEY (id, pk_id)
    );
]);

$node_subscriber->safe_psql('postgres', qq[
    CREATE TABLE regress_fk_root_pk (id int PRIMARY KEY, value int)
        PARTITION BY RANGE (id);
    CREATE TABLE regress_fk_root_pk_1 (value int, id int PRIMARY KEY);
    ALTER TABLE regress_fk_root_pk ATTACH PARTITION regress_fk_root_pk_1
        FOR VALUES FROM (1) TO (100);

    CREATE TABLE regress_fk_root_fk (
        id int,
        pk_id int REFERENCES regress_fk_root_pk(id),
        value int,
        PRIMARY KEY (id, pk_id)
    ) PARTITION BY RANGE (id);
    CREATE TABLE regress_fk_root_fk_1 (
        value int,
        pk_id int,
        id int,
        PRIMARY KEY (id, pk_id)
    );
    ALTER TABLE regress_fk_root_fk ATTACH PARTITION regress_fk_root_fk_1
        FOR VALUES FROM (1) TO (100);
]);

$node_subscriber->safe_psql('postgres',
    "ALTER SUBSCRIPTION regress_sub REFRESH PUBLICATION WITH (copy_data = false);");

my $root_fk_triggers = $node_subscriber->safe_psql('postgres', qq[
    SELECT format('ALTER TABLE %s ENABLE REPLICA TRIGGER %I;',
                  tgrelid::regclass, tgname)
    FROM pg_trigger
    WHERE tgrelid IN ('regress_fk_root_pk'::regclass,
                      'regress_fk_root_pk_1'::regclass,
                      'regress_fk_root_fk'::regclass,
                      'regress_fk_root_fk_1'::regclass)
      AND tgconstraint > 0
    ORDER BY tgrelid::regclass::text, tgname
]);

$node_subscriber->safe_psql('postgres', $root_fk_triggers);

# Warm the relation map entries for both newly added tables.  These tables are
# added after subscription creation with copy_data=false, so no table sync has
# opened them before the dependency scenario below.
$node_publisher->safe_psql('postgres', qq[
    BEGIN;
    INSERT INTO regress_fk_root_pk VALUES (50, 10);
    INSERT INTO regress_fk_root_fk VALUES (50, 50, 10);
    COMMIT;
]);
$node_publisher->wait_for_catchup('regress_sub');

$node_publisher->safe_psql('postgres', qq[
    BEGIN;
    DELETE FROM regress_fk_root_fk WHERE id = 50;
    DELETE FROM regress_fk_root_pk WHERE id = 50;
    COMMIT;
]);
$node_publisher->wait_for_catchup('regress_sub');

$node_subscriber->safe_psql('postgres',
    "SELECT injection_points_attach('parallel-worker-before-commit','wait');"
);

# Tx-1 inserts the referenced row through a root-mapped relation.  The root
# relmap entry must use partitioned-table FK metadata to record the dependency.
$node_publisher->safe_psql('postgres',
    "INSERT INTO regress_fk_root_pk VALUES (1, 10);");

$node_subscriber->wait_for_event('logical replication parallel worker',
    'parallel-worker-before-commit');

$offset = -s $node_subscriber->logfile;

$node_publisher->safe_psql('postgres',
    "INSERT INTO regress_fk_root_fk VALUES (1, 1, 10);");

$str = $node_subscriber->wait_for_log(qr/found conflicting referenced key change on table [1-9][0-9]+ from ([1-9][0-9]+)/, $offset);
$xid = $str =~ /found conflicting referenced key change on table [1-9][0-9]+ from ([1-9][0-9]+)/;

$node_subscriber->wait_for_log(qr/wait for depended xid $xid/, $offset);

ok(1,
   "root-mapped partitioned subscriber uses leaf FK metadata for referenced-key dependency");

$node_subscriber->safe_psql('postgres', qq[
    SELECT injection_points_detach('parallel-worker-before-commit');
    SELECT injection_points_wakeup('parallel-worker-before-commit');
]);

$node_subscriber->wait_for_log(qr/finish waiting for depended xid $xid/, $offset);
$node_publisher->wait_for_catchup('regress_sub');

$result = $node_subscriber->safe_psql('postgres',
    "SELECT count(1) FROM regress_fk_root_pk");
is($result, '1',
    'insert is replicated to root-mapped partitioned referenced table');

$result = $node_subscriber->safe_psql('postgres',
    "SELECT count(1) FROM regress_fk_root_fk");
is($result, '1',
    'insert is replicated to root-mapped partitioned referencing table');

done_testing();
