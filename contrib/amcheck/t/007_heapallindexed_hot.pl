# Copyright (c) 2026, PostgreSQL Global Development Group

# heapallindexed must compare keys at valid anchors, not rebuild TIDs.
use strict;
use warnings FATAL => 'all';

use File::Copy qw(copy);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('hot');
$node->init(no_data_checksums => 1);
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql(
	'postgres', q(
	CREATE EXTENSION amcheck;
	CREATE EXTENSION pageinspect;
	CREATE TABLE hot(id int PRIMARY KEY, txt text, k int, payload int)
	  WITH (fillfactor = 50, autovacuum_enabled = false);
	CREATE UNIQUE INDEX hot_expr ON hot(lower(txt));
	CREATE INDEX hot_k ON hot(k);
	CREATE INDEX hot_partial ON hot(id) WHERE k > 0;
	CREATE INDEX hot_include ON hot(id) INCLUDE (k);
	INSERT INTO hot VALUES (1, 'one', 0, 0);
));

sub check_indexes
{
	my ($label) = @_;
	for my $index (qw(hot_pkey hot_expr hot_k hot_partial hot_include))
	{
		for my $check (qw(bt_index_check bt_index_parent_check))
		{
			my ($ret, $out, $err) =
			  $node->psql('postgres', "SELECT $check('$index', true)");
			is($ret, 0, "$label: $check $index") or diag($err);
		}
	}
}

check_indexes('before update');
$node->safe_psql('postgres', 'UPDATE hot SET k = 1');
check_indexes('selective update');
$node->safe_psql('postgres', 'UPDATE hot SET payload = 1');
check_indexes('unchanged indexed attributes');
$node->safe_psql('postgres',
	'BEGIN; UPDATE hot SET k = 0; UPDATE hot SET k = 1; COMMIT');
check_indexes('predicate leave and reenter');
$node->safe_psql('postgres', 'REINDEX TABLE hot');
check_indexes('rebuilt anchors');

# An old registered snapshot must resolve the old version in both passes,
# even after another session updates and vacuums the table.
my $reader = $node->background_psql('postgres');
$reader->query_safe(
	'BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT * FROM hot');
$node->safe_psql('postgres', 'UPDATE hot SET k = 2, txt = \'two\'');
$node->safe_psql('postgres', 'VACUUM hot');
for my $index (qw(hot_pkey hot_expr hot_k hot_partial hot_include))
{
	is($reader->query_safe("SELECT bt_index_check('$index', true)"),
		'', "old snapshot: $index");
}
$reader->query_safe('COMMIT');
$reader->quit;
check_indexes('after concurrent update');

# Pause expression evaluation in the heap pass after the leaf fingerprints
# have been collected. Updates and VACUUM can run under bt_index_check's lock.
$node->safe_psql(
	'postgres', q(
	CREATE FUNCTION checked_key(text) RETURNS text LANGUAGE plpgsql IMMUTABLE AS
	$$ BEGIN
	  IF current_setting('application_name') = 'amcheck_pause' THEN
	    PERFORM pg_advisory_xact_lock(4242);
	  END IF;
	  RETURN lower($1);
	END $$;
	CREATE TABLE concurrent_hot(id int, txt text, k int)
	  WITH (fillfactor = 50, autovacuum_enabled = false);
	INSERT INTO concurrent_hot
	  SELECT g, CASE WHEN g % 2 = 0 THEN 'same' END, 0
	  FROM generate_series(1, 100) g;
	CREATE INDEX concurrent_hot_expr ON concurrent_hot(checked_key(txt));
	CREATE INDEX concurrent_hot_k ON concurrent_hot(k);
));
my $locker = $node->background_psql('postgres');
$locker->query_safe('SELECT pg_advisory_lock(4242)');
my $checker = $node->background_psql('postgres');
$checker->query_until(
	qr/checking/, q(
	\echo checking
	SET application_name = 'amcheck_pause';
	SELECT bt_index_check('concurrent_hot_expr', true);
));
$node->poll_query_until(
	'postgres', q(
	SELECT count(*) = 1 FROM pg_stat_activity
	WHERE wait_event = 'advisory' AND query LIKE '%bt_index_check%'
)) or die 'checker did not reach heap pass';
$node->safe_psql('postgres', 'UPDATE concurrent_hot SET k = 1');
$node->safe_psql('postgres', 'VACUUM concurrent_hot');
$locker->query_safe('SELECT pg_advisory_unlock(4242)');
$locker->quit;
$checker->query_safe('SELECT 1');
$checker->quit;
pass('concurrent update and vacuum between fingerprint passes');
is( $node->safe_psql(
		'postgres',
		q(SELECT bt_index_parent_check('concurrent_hot_expr', true, true))),
	'',
	'posting lists, nulls and by-reference keys after concurrent update');

# A pruned stub retains its old t_hoff even though its natts and null flags
# are cleared. Nine attributes with nulls need a larger header; ADD COLUMN
# must not make us locate the old bitmap using the new relation width.
$node->safe_psql(
	'postgres', q(
	CREATE TABLE wide_stub(id int, a int, b int, c int, d int, e int,
	  f int, g int, h int) WITH (autovacuum_enabled = false);
	CREATE INDEX wide_stub_id ON wide_stub(id);
	CREATE INDEX wide_stub_a ON wide_stub(a);
	CREATE INDEX wide_stub_b ON wide_stub(b);
	INSERT INTO wide_stub(id, a, b) VALUES (1, 0, 0);
));
my $wide_stub_hoff = $node->safe_psql(
	'postgres', q(
	SELECT t_hoff FROM heap_page_items(get_raw_page('wide_stub', 0))
	WHERE lp = 1
));
$node->safe_psql(
	'postgres', q(
	BEGIN;
	UPDATE wide_stub SET a = 1;
	UPDATE wide_stub SET b = 1;
	COMMIT;
	VACUUM wide_stub;
	ALTER TABLE wide_stub ADD COLUMN i int, ADD COLUMN j int,
	  ADD COLUMN k int, ADD COLUMN l int, ADD COLUMN m int,
	  ADD COLUMN n int, ADD COLUMN o int, ADD COLUMN p int;
));
is( $node->safe_psql(
		'postgres', qq(
	SELECT count(*) FROM heap_page_items(get_raw_page('wide_stub', 0))
	WHERE (t_infomask2 & 2047) = 0 AND (t_infomask2 & 2048) <> 0
	  AND t_hoff = $wide_stub_hoff AND split_part(t_ctid::text, ',', 1) = chr(40) || '9'
)),
	'1',
	'pruned stub retains nine-attribute bitmap and null-header space');
for my $index (qw(wide_stub_id wide_stub_a wide_stub_b))
{
	for my $check (qw(bt_index_check bt_index_parent_check))
	{
		is($node->safe_psql('postgres', "SELECT $check('$index', true)"),
			'', "old-width stub: $check $index");
	}
}

for my $qual ('id = 1', 'a = 1', 'b = 1')
{
	is( $node->safe_psql(
			'postgres', qq(
		SET enable_seqscan = off;
		SET enable_bitmapscan = off;
		SELECT id, a, b FROM wide_stub WHERE $qual;
	)),
		'1|1|1',
		"old-width stub: index probe $qual");
}

# A non-HOT update may legitimately link to another heap page. Only HOT
# links are constrained to remain on the page that the index entry names.
$node->safe_psql(
	'postgres', q(
	CREATE TABLE moved(id int, k int, pad text)
	  WITH (autovacuum_enabled = false);
	ALTER TABLE moved ALTER COLUMN pad SET STORAGE PLAIN;
	CREATE INDEX moved_id ON moved(id);
	CREATE INDEX moved_k ON moved(k);
	INSERT INTO moved SELECT g, 0, repeat('x', 1800)
	  FROM generate_series(1, 4) g;
	UPDATE moved SET k = 1, pad = repeat('y', 3000) WHERE id = 1;
));
is( $node->safe_psql(
		'postgres', q(
	SELECT (t_infomask2 & 16384) = 0 AND
	  split_part(t_ctid::text, ',', 1) <> chr(40) || '0'
	FROM heap_page_items(get_raw_page('moved', 0)) WHERE lp = 1
)),
	't',
	'non-HOT update links to another page');
for my $check (qw(bt_index_check bt_index_parent_check))
{
	is($node->safe_psql('postgres', "SELECT $check('moved_id', true)"),
		'', "off-page update: $check unchanged index");
	is($node->safe_psql('postgres', "SELECT $check('moved_k', true)"),
		'', "off-page update: $check changed index");
}

# Save an index before changing its heap. Restoring it afterwards leaves a
# structurally valid tree that cannot cover the new visible version.
for my $kind (qw(missing wrong_key stale_aba))
{
	$node->safe_psql(
		'postgres', qq(
		CREATE TABLE $kind(id int, k int) WITH (autovacuum_enabled = false);
		CREATE INDEX ${kind}_id ON $kind(id);
		CREATE INDEX ${kind}_k ON $kind(k);
	));
	$node->safe_psql('postgres', "INSERT INTO $kind VALUES (1, 0)")
	  unless $kind eq 'missing';
	my $rel = $node->safe_psql('postgres',
		"SELECT pg_relation_filepath('${kind}_k')");
	my $path = $node->data_dir . "/$rel";
	my $saved = "$path.saved";
	$node->stop;
	copy($path, $saved) or die "copy $path: $!";
	$node->start;
	if ($kind eq 'missing')
	{
		$node->safe_psql('postgres', "INSERT INTO $kind VALUES (1, 0)");
	}
	else
	{
		# One transaction retains every hop, including the ABA history.
		$node->safe_psql('postgres',
				"BEGIN; UPDATE $kind SET k = 1;"
			  . ($kind eq 'stale_aba' ? "UPDATE $kind SET k = 0;" : '')
			  . 'COMMIT');
	}
	$node->stop;
	copy($saved, $path) or die "restore $path: $!";
	$node->start;
	for my $check (qw(bt_index_check bt_index_parent_check))
	{
		my ($ret, $out, $err) =
		  $node->psql('postgres', "SELECT $check('${kind}_k', true)");
		isnt($ret, 0, "$kind: $check rejects corruption");
		like($err, qr/lacks matching index tuple/, "$kind: missing coverage");
	}
}

# A broken root link must not make a visible heap tuple disappear from the
# coverage check merely because the index-to-heap walk finds no version.
$node->safe_psql(
	'postgres', q(
	CREATE TABLE broken_chain(id int PRIMARY KEY, k int);
	CREATE INDEX broken_chain_k ON broken_chain(k);
	INSERT INTO broken_chain VALUES (1, 0);
	UPDATE broken_chain SET k = 1;
));
my $heap_path =
  $node->data_dir . '/'
  . $node->safe_psql('postgres',
	q(SELECT pg_relation_filepath('broken_chain')));
my $tuple_offset = $node->safe_psql(
	'postgres',
	q(SELECT lp_off FROM heap_page_items(get_raw_page('broken_chain', 0))
	  WHERE lp = 1));
$node->stop;
open(my $heap, '+<', $heap_path) or die "open $heap_path: $!";
binmode $heap;
# t_ctid's offset number is at byte 16 in HeapTupleHeaderData.
sysseek($heap, $tuple_offset + 16, 0) or die "seek: $!";
syswrite($heap, pack('S', 200)) == 2 or die "write: $!";
close($heap) or die "close: $!";
$node->start;
my ($broken_ret, $broken_out, $broken_err) = $node->psql('postgres',
	q(SELECT bt_index_check('broken_chain_pkey', true)));
isnt($broken_ret, 0, 'broken anchor link is rejected');
like(
	$broken_err,
	qr/invalid HOT chain/,
	'broken anchor reports heap corruption');

# Changing an immutable expression without rebuilding leaves the right TID
# with the wrong key, even though no indexed heap attribute changed.
$node->safe_psql(
	'postgres', q(
	CREATE FUNCTION index_key(int) RETURNS int LANGUAGE sql IMMUTABLE
	  AS 'SELECT $1';
	CREATE TABLE wrong_value(id int);
	INSERT INTO wrong_value VALUES (1);
	CREATE INDEX wrong_value_idx ON wrong_value(index_key(id));
	CREATE OR REPLACE FUNCTION index_key(int) RETURNS int LANGUAGE sql IMMUTABLE
	  AS 'SELECT $1 + 1';
));
my ($ret, $out, $err) =
  $node->psql('postgres', "SELECT bt_index_check('wrong_value_idx', true)");
isnt($ret, 0, 'wrong stored key is rejected');
like($err, qr/lacks matching index tuple/, 'wrong stored key lacks coverage');

# A catalog AM alias using the heap routine needs the same version-TID
# fingerprints as heap itself, for changed and unchanged indexes alike.
$node->safe_psql(
	'postgres', q(
	CREATE ACCESS METHOD heap_alias TYPE TABLE HANDLER heap_tableam_handler;
	CREATE TABLE alias_t(id int PRIMARY KEY, k int) USING heap_alias;
	CREATE INDEX alias_k ON alias_t(k);
	INSERT INTO alias_t VALUES (1, 0);
	UPDATE alias_t SET k = 1;
));
for my $index (qw(alias_t_pkey alias_k))
{
	for my $check (qw(bt_index_check bt_index_parent_check))
	{
		my ($ret, $out, $err) =
		  $node->psql('postgres', "SELECT $check('$index', true)");
		is($ret, 0, "heap alias: $check $index") or diag($err);
	}
}

# Corrupt only a single leaf TID, leaving the tree structurally valid. In
# particular, InvalidBlockNumber is also P_NEW and must never reach a buffer
# read, which would extend the heap rather than report corruption.
my $block_size = $node->safe_psql('postgres', 'SHOW block_size');
for my $case (
	[ 'p_new', 0xffffffff, 1 ],
	[ 'at_eof', 1, 1 ],
	[ 'past_eof', 2, 1 ],
	[ 'zero_offset', 0, 0 ],
	[ 'max_offset', 0, 65535 ])
{
	my ($name, $block, $offset) = @$case;
	$node->safe_psql(
		'postgres', qq(
		CREATE TABLE bad_$name(id int);
		INSERT INTO bad_$name VALUES (1);
		CREATE INDEX bad_${name}_idx ON bad_$name(id);
	));
	my $index_path =
	  $node->data_dir . '/'
	  . $node->safe_psql('postgres',
		"SELECT pg_relation_filepath('bad_${name}_idx')");
	my $heap_path =
	  $node->data_dir . '/'
	  . $node->safe_psql('postgres',
		"SELECT pg_relation_filepath('bad_$name')");
	# pd_upper locates the sole tuple without depending on ItemId bit layout.
	my $upper = $node->safe_psql('postgres',
		"SELECT upper FROM page_header(get_raw_page('bad_${name}_idx', 1))");
	$node->stop;
	is(-s $heap_path, $block_size, "$name: one-block heap before check");
	open(my $index, '+<', $index_path) or die "open $index_path: $!";
	binmode $index;
	sysseek($index, $block_size + $upper, 0) or die "seek: $!";
	syswrite($index, pack('S3', $block >> 16, $block & 65535, $offset)) == 6
	  or die "write: $!";
	close($index) or die "close: $!";
	$node->start;

	for my $check (qw(bt_index_check bt_index_parent_check))
	{
		# Continuing in the SAME backend proves this was a controlled ERROR,
		# not a crash followed by postmaster recovery and a new connection.
		my ($ret, $out, $err) = $node->psql(
			'postgres', qq(
			SELECT $check('bad_${name}_idx', true);
			SELECT 'alive', pg_relation_size('bad_$name'),
			  EXISTS (SELECT FROM bad_$name);
		),
			on_error_stop => 0,
			extra_params => ['--set=VERBOSITY=verbose']);
		is($ret, 0, "$name: $check backend survives");
		like(
			$err,
			qr/ERROR:  XX002: invalid heap TID .* in index "bad_${name}_idx"/,
			"$name: $check reports index corruption");
		is($out, "alive|$block_size|t",
			"$name: $check leaves heap size and row intact");
		is(-s $heap_path,
			$block_size, "$name: $check does not extend heap file");
	}
}

# A valid leaf TID must not expose the runtime walker's assumptions about
# untrusted downstream tuple headers. Restore each page after the check so
# byte equality also proves that checking did not repair or damage the heap.
$node->safe_psql(
	'postgres', q(
	CREATE TABLE bad_chain(id int PRIMARY KEY, k int)
	  WITH (autovacuum_enabled = false);
	CREATE INDEX bad_chain_k ON bad_chain(k);
	INSERT INTO bad_chain VALUES (1, 0);
	UPDATE bad_chain SET k = 1;
));
my $chain_path = $node->data_dir . '/'
  . $node->safe_psql('postgres', q(SELECT pg_relation_filepath('bad_chain')));
my @chain_offsets = split /\n/,
  $node->safe_psql('postgres',
	q(SELECT lp_off FROM heap_page_items(get_raw_page('bad_chain', 0)) ORDER BY lp)
  );
# Resolve hint bits before taking a byte-for-byte baseline.
$node->safe_psql('postgres',
	q(SELECT bt_index_check('bad_chain_pkey', true)));
$node->stop;
my $chain_page = PostgreSQL::Test::Utils::slurp_file($chain_path);
for my $case (
	qw(cross_page zero_link past_link max_link cycle stub_natts
	stub_bitmap tuple_natts tuple_hoff tuple_nulls short_item outside_item
	dead_end redirect_end))
{
	my $page = $chain_page;
	my ($root, $next) = @chain_offsets;
	if ($case eq 'cross_page')
	{
		substr($page, $root + 12, 4) = pack('S2', 0, 1);
	}
	elsif ($case =~ /^(zero_link|past_link|max_link|cycle)$/)
	{
		my %offset =
		  (zero_link => 0, past_link => 3, max_link => 65535, cycle => 1);
		substr($page, $root + 16, 2) = pack('S', $offset{$case});
	}
	elsif ($case =~ /^stub_/)
	{
		# Turn the crossed member into a stub. Its natts is stored in the
		# block half of t_ctid, not in the ordinary tuple natts field.
		substr($page, $next + 18, 4) = pack('S2', 0x8800, 0x0a00);
		substr($page, $next + 12, 6) =
		  pack('S3', 0, $case eq 'stub_natts' ? 65535 : 2, 1);
		# A valid natts cannot rescue a bitmap overlapping the header.
		substr($page, $next + 22, 1) =
		  pack('C', $case eq 'stub_bitmap' ? 40 : 24);
	}
	elsif ($case eq 'tuple_natts')
	{
		my $mask = unpack('S', substr($page, $next + 18, 2));
		substr($page, $next + 18, 2) = pack('S', ($mask & ~0x7ff) | 2047);
	}
	elsif ($case eq 'tuple_hoff')
	{
		substr($page, $next + 22, 1) = pack('C', 255);
	}
	elsif ($case eq 'tuple_nulls')
	{
		my $mask = unpack('S', substr($page, $next + 18, 2));
		substr($page, $next + 18, 2) = pack('S', ($mask & ~0x7ff) | 16);
		$mask = unpack('S', substr($page, $next + 20, 2));
		substr($page, $next + 20, 2) = pack('S', $mask | 1);
	}
	elsif ($case =~ /_end$/)
	{
		# Pruning can retire a chain member while an older link still names
		# its offset. The runtime walk stops here rather than following it.
		my $little = pack('L', 1) eq pack('C4', 1, 0, 0, 0);
		my $flags = $case eq 'dead_end' ? 3 : 2;
		my $lp = $flags << 15;
		$lp |= 32767 << ($little ? 0 : 17) if $case eq 'redirect_end';
		substr($page, 28, 4) = pack('L', $lp);
	}
	else
	{
		# ItemId bitfields have native byte order. Preserve LP_NORMAL while
		# replacing either length or offset, including on big-endian hosts.
		my $little = pack('L', 1) eq pack('C4', 1, 0, 0, 0);
		my $lp = unpack('L', substr($page, 28, 4));
		my ($shift, $value) =
		  $case eq 'short_item'
		  ? ($little ? 17 : 0, 8)
		  : ($little ? 0  : 17, $block_size - 8);
		$lp = ($lp & ~(32767 << $shift)) | ($value << $shift);
		substr($page, 28, 4) = pack('L', $lp);
	}
	open(my $fh, '>', $chain_path) or die "open $chain_path: $!";
	binmode $fh;
	print $fh $page;
	close($fh) or die "close: $!";
	$node->start;
	for my $check (qw(bt_index_check bt_index_parent_check))
	{
		is( $node->safe_psql(
				'postgres', "SELECT $check('bad_chain_pkey', false)"),
			'',
			"$case: $check structural control");
		my ($ret, $out, $err) = $node->psql(
			'postgres', qq(
			SET statement_timeout = '5s';
			SELECT $check('bad_chain_pkey', true);
			SELECT 'alive', pg_relation_size('bad_chain');
		),
			on_error_stop => 0,
			extra_params => ['--set=VERBOSITY=verbose']);
		is($ret, 0, "$case: $check backend survives");
		if ($case =~ /_end$/)
		{
			is($err, '', "$case: $check stops at retired member");
		}
		else
		{
			like(
				$err,
				qr/ERROR:  XX001: invalid HOT chain/,
				"$case: $check reports heap corruption");
		}
		is( $out,
			($case =~ /_end$/ ? "\n" : '') . "alive|$block_size",
			"$case: $check leaves heap size intact");
	}
	$node->stop;
	is(PostgreSQL::Test::Utils::slurp_file($chain_path),
		$page, "$case: heap bytes unchanged");
}
open(my $restore, '>', $chain_path) or die "open $chain_path: $!";
binmode $restore;
print $restore $chain_page;
close($restore) or die "close: $!";
$node->start;
is($node->safe_psql('postgres', q(SELECT id, k FROM bad_chain)),
	'1|1', 'restored chain retains data');

$node->stop;
done_testing();
