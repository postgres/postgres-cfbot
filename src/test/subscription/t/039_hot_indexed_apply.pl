
# Copyright (c) 2026, PostgreSQL Global Development Group

# Per-subscription hot_indexed_on_apply option: parser, catalog round-trip,
# ALTER behaviour, and apply-path gating under each of the three modes.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my $publisher = PostgreSQL::Test::Cluster->new('publisher');
$publisher->init(allows_streaming => 'logical');
$publisher->start;

my $subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$subscriber->init;
$subscriber->start;

my $pub_conninfo = $publisher->connstr . ' dbname=postgres';

# --- Schema ----------------------------------------------------------------
# tab_extra has an extra btree index beyond the primary key on the
# subscriber side; that is the schema shape that subset_only must demote
# to non-HOT on apply but always must let through.
$publisher->safe_psql('postgres',
	q{CREATE TABLE tab_extra (id int PRIMARY KEY, payload int, tag text)});

# tab_pk has only the primary key; indexed-attr set is a subset of the PK
# attrs, so subset_only and always should both allow HOT-indexed on apply.
$publisher->safe_psql('postgres',
	q{CREATE TABLE tab_pk (id int PRIMARY KEY, payload int)});

$publisher->safe_psql('postgres',
	q{CREATE PUBLICATION pub FOR TABLE tab_extra, tab_pk});

# Subscriber mirrors both tables.  tab_extra has the extra secondary index
# only on the subscriber, which is the schema-divergence case the option
# gates.
$subscriber->safe_psql('postgres',
	q{CREATE TABLE tab_extra (id int PRIMARY KEY, payload int, tag text)});
$subscriber->safe_psql('postgres',
	q{CREATE INDEX tab_extra_payload_idx ON tab_extra(payload)});
$subscriber->safe_psql('postgres',
	q{CREATE TABLE tab_pk (id int PRIMARY KEY, payload int)});

# --- Parser / catalog checks ----------------------------------------------
# Default on fresh subscription is 's' (subset_only).
$subscriber->safe_psql('postgres', qq{
	CREATE SUBSCRIPTION sub_default
	  CONNECTION '$pub_conninfo'
	  PUBLICATION pub
	  WITH (connect = false, slot_name = NONE, enabled = false,
	        create_slot = false);
});
is( $subscriber->safe_psql('postgres',
		q{SELECT subhotindexedonapply FROM pg_subscription
		  WHERE subname = 'sub_default'}),
	's',
	'fresh subscription defaults to subset_only');

# Explicit 'always' is stored as 'a'.
$subscriber->safe_psql('postgres', qq{
	CREATE SUBSCRIPTION sub_always_p
	  CONNECTION '$pub_conninfo'
	  PUBLICATION pub
	  WITH (connect = false, slot_name = NONE, enabled = false,
	        create_slot = false, hot_indexed_on_apply = 'always');
});
is( $subscriber->safe_psql('postgres',
		q{SELECT subhotindexedonapply FROM pg_subscription
		  WHERE subname = 'sub_always_p'}),
	'a',
	'CREATE with hot_indexed_on_apply = always stores a');

# ALTER SUBSCRIPTION SET updates the column.
$subscriber->safe_psql('postgres',
	q{ALTER SUBSCRIPTION sub_default SET (hot_indexed_on_apply = 'off')});
is( $subscriber->safe_psql('postgres',
		q{SELECT subhotindexedonapply FROM pg_subscription
		  WHERE subname = 'sub_default'}),
	'o',
	'ALTER SUBSCRIPTION SET hot_indexed_on_apply = off stores o');

# Unknown values are rejected.
my ($ret, $stdout, $stderr) = $subscriber->psql('postgres', qq{
	CREATE SUBSCRIPTION sub_bogus
	  CONNECTION '$pub_conninfo'
	  PUBLICATION pub
	  WITH (connect = false, slot_name = NONE, enabled = false,
	        create_slot = false, hot_indexed_on_apply = 'bogus');
});
isnt($ret, 0, 'bogus hot_indexed_on_apply value is rejected');
like($stderr,
	 qr/unrecognized value for subscription parameter "hot_indexed_on_apply"/,
	 'bogus hot_indexed_on_apply value reports the expected error');

# Drop the placeholder subscriptions so we can rebuild with real slots.
$subscriber->safe_psql('postgres', 'DROP SUBSCRIPTION sub_default');
$subscriber->safe_psql('postgres', 'DROP SUBSCRIPTION sub_always_p');

# --- Apply-path behaviour -------------------------------------------------
# Pre-populate both sides identically so we can use copy_data=false and
# avoid duplicate-key conflicts when we recreate subscriptions across the
# three test cases.  We update non-overlapping id ranges per case so the
# pg_stat counters segment cleanly.
$publisher->safe_psql('postgres',
	q{INSERT INTO tab_extra
	  SELECT g, 0, 't' FROM generate_series(1, 200) g});
$publisher->safe_psql('postgres',
	q{INSERT INTO tab_pk
	  SELECT g, 0 FROM generate_series(1, 200) g});
$subscriber->safe_psql('postgres',
	q{INSERT INTO tab_extra
	  SELECT g, 0, 't' FROM generate_series(1, 200) g});
$subscriber->safe_psql('postgres',
	q{INSERT INTO tab_pk
	  SELECT g, 0 FROM generate_series(1, 200) g});

# Helper: read counters and poll up to 10 s for n_tup_upd to reach a
# minimum target value (the apply worker flushes pgstat asynchronously).
sub poll_counters
{
	my ($node, $table, $upd_target) = @_;

	my $deadline = time() + 10;
	my $row = '';
	while (1)
	{
		$row = $node->safe_psql('postgres',
			qq{SELECT coalesce(n_tup_upd, 0),
			          coalesce(n_tup_hot_upd, 0),
			          coalesce(n_tup_hot_indexed_upd, 0)
			   FROM pg_stat_user_tables WHERE relname = '$table'});
		my ($upd) = split /\|/, $row;
		last if ($upd + 0) >= $upd_target || time() >= $deadline;
		usleep(100_000);
	}
	my ($upd, $hot, $hot_idx) = split /\|/, $row;
	return ($upd + 0, $hot + 0, $hot_idx + 0);
}

# Helper: fire UPDATEs that touch the indexed payload column on a given
# id range and return the deltas in (n_tup_upd, n_tup_hot_upd,
# n_tup_hot_indexed_upd) on the subscriber.
sub apply_updates_and_read
{
	my ($table, $sub_name, $id_lo, $id_hi) = @_;

	my ($upd0, $hot0, $hotidx0) =
	  poll_counters($subscriber, $table, 0);

	for my $i ($id_lo .. $id_hi)
	{
		$publisher->safe_psql('postgres',
			"UPDATE $table SET payload = payload + 1 WHERE id = $i");
	}
	$publisher->wait_for_catchup($sub_name);

	my $n = $id_hi - $id_lo + 1;
	my ($upd1, $hot1, $hotidx1) =
	  poll_counters($subscriber, $table, $upd0 + $n);
	note("$table $sub_name $id_lo..$id_hi: dn_upd="
		 . ($upd1 - $upd0) . " dhot=" . ($hot1 - $hot0)
		 . " dhotidx=" . ($hotidx1 - $hotidx0));
	return ($upd1 - $upd0, $hot1 - $hot0, $hotidx1 - $hotidx0);
}

# Case 1: off, subscriber-only secondary index.  HOT-indexed must be
# suppressed on tab_extra.  Plain HOT updates also stay zero because every
# UPDATE touches `payload` which is indexed on the subscriber.
$subscriber->safe_psql('postgres', qq{
	CREATE SUBSCRIPTION sub_off
	  CONNECTION '$pub_conninfo'
	  PUBLICATION pub
	  WITH (slot_name = 'sub_off_slot', create_slot = true,
	        hot_indexed_on_apply = 'off', copy_data = false);
});
$publisher->wait_for_catchup('sub_off');

my (undef, undef, $off_extra_hotidx) =
  apply_updates_and_read('tab_extra', 'sub_off', 1, 20);
is($off_extra_hotidx, 0,
   'hot_indexed_on_apply = off: no HOT-indexed updates on tab_extra');

$subscriber->safe_psql('postgres', 'DROP SUBSCRIPTION sub_off');

# Case 2: subset_only.  On tab_pk (no secondary index, indexed-attr set is
# a subset of PK attrs), classic HOT must fire because `payload` is not
# indexed there.  On tab_extra (subscriber's `payload` index is NOT covered
# by the PK), the apply worker must demote to non-HOT just like 'off'.
$subscriber->safe_psql('postgres', qq{
	CREATE SUBSCRIPTION sub_subset
	  CONNECTION '$pub_conninfo'
	  PUBLICATION pub
	  WITH (slot_name = 'sub_subset_slot', create_slot = true,
	        hot_indexed_on_apply = 'subset_only', copy_data = false);
});
$publisher->wait_for_catchup('sub_subset');

my (undef, $ss_pk_hot, $ss_pk_hotidx) =
  apply_updates_and_read('tab_pk', 'sub_subset', 1, 20);
cmp_ok($ss_pk_hot, '>', 0,
	   'hot_indexed_on_apply = subset_only: classic HOT fires on tab_pk');

my (undef, undef, $ss_extra_hotidx) =
  apply_updates_and_read('tab_extra', 'sub_subset', 21, 40);
is($ss_extra_hotidx, 0,
   'hot_indexed_on_apply = subset_only: no HOT-indexed on tab_extra');

$subscriber->safe_psql('postgres', 'DROP SUBSCRIPTION sub_subset');

# Case 3: always.  Unconditional HOT-indexed eligibility.  On tab_extra
# updates touching the indexed payload column should now run on the
# HOT-indexed path: n_tup_hot_indexed_upd must increase.
$subscriber->safe_psql('postgres', qq{
	CREATE SUBSCRIPTION sub_always
	  CONNECTION '$pub_conninfo'
	  PUBLICATION pub
	  WITH (slot_name = 'sub_always_slot', create_slot = true,
	        hot_indexed_on_apply = 'always', copy_data = false);
});
$publisher->wait_for_catchup('sub_always');

my (undef, undef, $al_extra_hotidx) =
  apply_updates_and_read('tab_extra', 'sub_always', 41, 80);
cmp_ok($al_extra_hotidx, '>', 0,
	   'hot_indexed_on_apply = always: HOT-indexed fires on tab_extra');

# ALTER back to off and verify the apply worker picks up the new mode.
$subscriber->safe_psql('postgres',
	q{ALTER SUBSCRIPTION sub_always SET (hot_indexed_on_apply = 'off')});
is( $subscriber->safe_psql('postgres',
		q{SELECT subhotindexedonapply FROM pg_subscription
		  WHERE subname = 'sub_always'}),
	'o',
	'ALTER sub_always SET hot_indexed_on_apply = off persists');

# Drive another batch of updates and confirm n_tup_hot_indexed_upd does NOT
# advance after the worker rereads the catalog.
my (undef, undef, $post_alter_hotidx) =
  apply_updates_and_read('tab_extra', 'sub_always', 81, 100);
is($post_alter_hotidx, 0,
   'ALTER to off freezes n_tup_hot_indexed_upd after worker reread');

$subscriber->safe_psql('postgres', 'DROP SUBSCRIPTION sub_always');

# --- Subscriber INSERT-after-replicated-UPDATE per mode -------------------
#
# Verify that a subscriber INSERT using the OLD value of a replicated
# UPDATE's indexed column succeeds without a spurious unique-violation
# under each apply mode.  Use a dedicated table (tab_uk) so the unique
# constraint can be defined up-front and the test does not collide with
# pre-populated rows from the apply-path scenarios above.
#
# Publisher updates row $upd_id changing payload from 0 to 999.  The
# subscriber then inserts a fresh row with payload=0 (the pre-update
# value).  Under all three modes _bt_check_unique's recheck of the
# conflicting tuple's live key must recognize the stale leaf entry pointing
# at the chain root, so the INSERT succeeds.

$publisher->safe_psql('postgres',
	q{CREATE TABLE tab_uk (
	    id      int PRIMARY KEY,
	    payload int,
	    tag     text,
	    UNIQUE (payload, tag))});
$subscriber->safe_psql('postgres',
	q{CREATE TABLE tab_uk (
	    id      int PRIMARY KEY,
	    payload int,
	    tag     text,
	    UNIQUE (payload, tag))});
$publisher->safe_psql('postgres',
	q{ALTER PUBLICATION pub ADD TABLE tab_uk});

for my $mode ('off', 'subset_only', 'always')
{
	my $base_id = ($mode eq 'off') ? 1
	              : ($mode eq 'subset_only') ? 100 : 200;
	my $upd_id  = $base_id + 1;
	my $ins_id  = $base_id + 2;

	# Seed a row that we will UPDATE on the publisher (payload starts at 0),
	# and drain the apply for it before changing payload.
	$publisher->safe_psql('postgres',
		"INSERT INTO tab_uk VALUES ($upd_id, 0, 'mode_$mode')");

	$subscriber->safe_psql('postgres', qq{
		CREATE SUBSCRIPTION sub_uk_$mode
		  CONNECTION '$pub_conninfo'
		  PUBLICATION pub
		  WITH (slot_name = 'sub_uk_${mode}_slot', create_slot = true,
		        hot_indexed_on_apply = '$mode', copy_data = true);
	});
	$publisher->wait_for_catchup("sub_uk_$mode");

	# Publisher UPDATE: payload 0 -> 999.
	$publisher->safe_psql('postgres',
		"UPDATE tab_uk SET payload = 999 WHERE id = $upd_id");
	$publisher->wait_for_catchup("sub_uk_$mode");

	# Subscriber INSERT with the OLD payload value but a unique tag.  The
	# existing chain leaf with key (0, 'mode_$mode') is now stale: the
	# live tuple at the chain root has payload=999.  _bt_check_unique
	# rechecks the conflicting tuple's live key and recognizes the stale
	# leaf, allowing this INSERT to succeed.
	my ($r, $out, $err) = $subscriber->psql('postgres',
		"INSERT INTO tab_uk VALUES ($ins_id, 0, 'fresh_$mode')");
	is($r, 0,
	   "hot_indexed_on_apply = $mode: "
	   . "subscriber INSERT with old payload value succeeds");
	like($err, qr/^$/,
		 "hot_indexed_on_apply = $mode: "
		 . "INSERT did not raise an error");

	$subscriber->safe_psql('postgres',
		"DROP SUBSCRIPTION sub_uk_$mode");
}

# --- always-mode safety with indexed attrs beyond the replica identity -----
#
# Amit's corner: under hot_indexed_on_apply = 'always' the apply worker may
# run a HOT-indexed update even when the table has an indexed attribute that
# is NOT covered by the replica identity.  The read-side staleness mechanism
# must still let the apply worker's later replica-identity lookups find the
# row, and DELETE/UPDATE replication must converge, including when the
# replica-identity column itself is cycled away and back (ABA).
#
# tab_ri: replica identity is a UNIQUE index on rid (not the PK), and there
# is an extra secondary index on payload that is NOT part of the replica
# identity.  So a payload change makes the payload leaf stale, and an rid
# ABA cycle makes the rid (replica-identity) leaf stale, both while
# 'always' keeps the updates on the HOT chain.
$publisher->safe_psql('postgres', q{
	CREATE TABLE tab_ri (id int PRIMARY KEY, rid int NOT NULL, payload int);
	CREATE UNIQUE INDEX tab_ri_rid_uk ON tab_ri(rid);
	ALTER TABLE tab_ri REPLICA IDENTITY USING INDEX tab_ri_rid_uk;
	CREATE PUBLICATION pub_ri FOR TABLE tab_ri;
});
$subscriber->safe_psql('postgres', q{
	CREATE TABLE tab_ri (id int PRIMARY KEY, rid int NOT NULL, payload int);
	CREATE UNIQUE INDEX tab_ri_rid_uk ON tab_ri(rid);
	ALTER TABLE tab_ri REPLICA IDENTITY USING INDEX tab_ri_rid_uk;
	CREATE INDEX tab_ri_payload_idx ON tab_ri(payload);
});

$publisher->safe_psql('postgres',
	q{INSERT INTO tab_ri VALUES (1, 10, 0), (2, 20, 0)});

$subscriber->safe_psql('postgres', qq{
	CREATE SUBSCRIPTION sub_ri
	  CONNECTION '$pub_conninfo'
	  PUBLICATION pub_ri
	  WITH (slot_name = 'sub_ri_slot', create_slot = true,
	        hot_indexed_on_apply = 'always', copy_data = true);
});
# Wait for the initial table COPY to finish, not just streaming catch-up, so
# the seeded rows are present before we start updating them.
$subscriber->wait_for_subscription_sync($publisher, 'sub_ri');

# Cycle the replica-identity column away and back (ABA), and also churn the
# extra payload index, all replicated under 'always'.  Each step is a
# HOT-indexed update on the subscriber that leaves a stale leaf.
$publisher->safe_psql('postgres', q{
	UPDATE tab_ri SET rid = 11, payload = payload + 1 WHERE id = 1;
	UPDATE tab_ri SET rid = 10, payload = payload + 1 WHERE id = 1;  -- rid ABA
	UPDATE tab_ri SET payload = payload + 1 WHERE id = 2;
});
$publisher->wait_for_catchup('sub_ri');

# Confirm the apply worker actually took the HOT-indexed path on tab_ri (the
# whole point of 'always' with indexed attrs beyond the replica identity).
# Without this the convergence/verify_heapam asserts below could pass
# vacuously if eligibility silently regressed to plain non-HOT.
my (undef, undef, $ri_hotidx) = poll_counters($subscriber, 'tab_ri', 3);
cmp_ok($ri_hotidx, '>', 0,
	   'always-mode: HOT-indexed path fired on tab_ri (rid/payload churn)');

# A subsequent replicated UPDATE keyed by the replica identity (rid) must
# find the row despite the stale rid/payload leaves the ABA left behind.
$publisher->safe_psql('postgres',
	q{UPDATE tab_ri SET payload = 100 WHERE id = 1});
# And a replicated DELETE resolved through the replica-identity index must
# also find and remove the right row.
$publisher->safe_psql('postgres', q{DELETE FROM tab_ri WHERE id = 2});
$publisher->wait_for_catchup('sub_ri');

is( $subscriber->safe_psql('postgres',
		q{SELECT rid || ':' || payload FROM tab_ri WHERE id = 1}),
	'10:100',
	'always-mode: replicated UPDATE found the row via RI after rid ABA');
is( $subscriber->safe_psql('postgres',
		q{SELECT count(*) FROM tab_ri WHERE id = 2}),
	'0',
	'always-mode: replicated DELETE found the row via RI with stale leaves');
# Full convergence cross-check.
is( $subscriber->safe_psql('postgres',
		q{SELECT string_agg(id || ',' || rid || ',' || payload, ';' ORDER BY id)
		  FROM tab_ri}),
	$publisher->safe_psql('postgres',
		q{SELECT string_agg(id || ',' || rid || ',' || payload, ';' ORDER BY id)
		  FROM tab_ri}),
	'always-mode: tab_ri converges between publisher and subscriber');

# verify_heapam finds no corruption in the HOT-indexed chains left behind.
$subscriber->safe_psql('postgres', 'CREATE EXTENSION IF NOT EXISTS amcheck');
is( $subscriber->safe_psql('postgres',
		q{SELECT count(*) FROM verify_heapam('tab_ri')}),
	'0',
	'always-mode: verify_heapam clean on tab_ri after stale-leaf churn');

$subscriber->safe_psql('postgres', 'DROP SUBSCRIPTION sub_ri');

done_testing();
