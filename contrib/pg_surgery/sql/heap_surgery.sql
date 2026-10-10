create extension pg_surgery;

-- create a normal heap table and insert some rows.
-- use a temp table so that vacuum behavior doesn't depend on global xmin
create temp table htab (a int);
insert into htab values (100), (200), (300), (400), (500);

-- test empty TID array
select heap_force_freeze('htab'::regclass, ARRAY[]::tid[]);

-- nothing should be frozen yet
select * from htab where xmin = 2;

-- freeze forcibly
select heap_force_freeze('htab'::regclass, ARRAY['(0, 4)']::tid[]);

-- now we should have one frozen tuple
select ctid, xmax from htab where xmin = 2;

-- kill forcibly
select heap_force_kill('htab'::regclass, ARRAY['(0, 4)']::tid[]);

-- should be gone now
select * from htab where ctid = '(0, 4)';

-- should now be skipped because it's already dead
select heap_force_kill('htab'::regclass, ARRAY['(0, 4)']::tid[]);
select heap_force_freeze('htab'::regclass, ARRAY['(0, 4)']::tid[]);

-- freeze two TIDs at once while skipping an out-of-range block number
select heap_force_freeze('htab'::regclass,
						 ARRAY['(0, 1)', '(0, 3)', '(1, 1)']::tid[]);

-- we should now have two frozen tuples
select ctid, xmax from htab where xmin = 2;

-- out-of-range TIDs should be skipped
select heap_force_freeze('htab'::regclass, ARRAY['(0, 0)', '(0, 6)']::tid[]);

-- set up a new table with a redirected line pointer
-- use a temp table so that vacuum behavior doesn't depend on global xmin
create temp table htab2(a int);
insert into htab2 values (100);
update htab2 set a = 200;
vacuum htab2;

-- redirected TIDs should be skipped
select heap_force_kill('htab2'::regclass, ARRAY['(0, 1)']::tid[]);

-- now create an unused line pointer
select ctid from htab2;
update htab2 set a = 300;
select ctid from htab2;
vacuum freeze htab2;

-- unused TIDs should be skipped
select heap_force_kill('htab2'::regclass, ARRAY['(0, 2)']::tid[]);

-- multidimensional TID array should be rejected
select heap_force_kill('htab2'::regclass, ARRAY[['(0, 2)']]::tid[]);

-- TID array with nulls should be rejected
select heap_force_kill('htab2'::regclass, ARRAY[NULL]::tid[]);

-- but we should be able to kill the one tuple we have
select heap_force_kill('htab2'::regclass, ARRAY['(0, 3)']::tid[]);

-- a tid[] larger than 65535 entries must still finish
create temp table htab3(a int);
insert into htab3 values (1);
select heap_force_kill(
	'htab3'::regclass,
	array(select '(0,1)'::tid from generate_series(1, 65536)));
select count(*) from htab3;

-- materialized view.
-- note that we don't commit the transaction, so autovacuum can't interfere.
begin;
create materialized view mvw as select a from generate_series(1, 3) a;

select * from mvw where xmin = 2;
select heap_force_freeze('mvw'::regclass, ARRAY['(0, 3)']::tid[]);
select * from mvw where xmin = 2;

select heap_force_kill('mvw'::regclass, ARRAY['(0, 3)']::tid[]);
select * from mvw where ctid = '(0, 3)';
rollback;

-- check that it fails on an unsupported relkind
create view vw as select 1;
select heap_force_kill('vw'::regclass, ARRAY['(0, 1)']::tid[]);
select heap_force_freeze('vw'::regclass, ARRAY['(0, 1)']::tid[]);

-- A HOT/SIU chain collapse turns the chain root and each dead entry-bearing
-- member into an LP_REDIRECT to the live tuple.  pg_surgery operates on real
-- tuples and must leave the live row reachable after such a collapse.
create extension pageinspect;
create table htomb (id int primary key, a int, b int) with (fillfactor = 50);
create index htomb_a on htomb(a);
insert into htomb values (1, 10, 20);
-- Two HOT-indexed updates on an indexed attr, then prune: the dead mid-chain
-- versions collapse to LP_REDIRECTs to the live tuple.  INDEX_CLEANUP off keeps
-- the stale btree leaves (and hence the redirects) in place.
update htomb set a = 11 where id = 1;
update htomb set a = 12 where id = 1;
vacuum (index_cleanup off) htomb;
select n_hot_indexed > 0 as made_hot_indexed
  from pg_relation_hot_indexed_stats('htomb');
-- the live row is intact and reachable after the collapse
select id, a, b from htomb;
drop table htomb;

-- A collapse that keeps a *stub* (an xid-free forwarding LP_NORMAL item with
-- natts == 0), not just redirects: update two different indexed columns so the
-- first dead member's changed-attr bitmap is not subsumed by later hops and is
-- preserved as a stub.  pg_surgery must skip such a stub; forcing a
-- freeze/kill would overwrite its t_ctid forward link and corrupt the chain.
create table hstub (id int primary key, a int, b int) with (fillfactor = 50);
create index hstub_a on hstub(a);
create index hstub_b on hstub(b);
insert into hstub values (1, 10, 100);
update hstub set a = 11 where id = 1;   -- changes a
update hstub set a = 12 where id = 1;   -- changes a again (supersedes)
update hstub set b = 101 where id = 1;  -- changes b -> first hop's {a} kept as stub
vacuum (index_cleanup off) hstub;
-- Locate the stub: an LP_NORMAL item carrying HEAP_INDEXED_UPDATED (0x0800 in
-- t_infomask2) with zero live attributes (t_infomask2 natts bits == 0).
select lp as stub_off
  from heap_page_items(get_raw_page('hstub', 0))
  where lp_flags = 1                       -- LP_NORMAL
    and (t_infomask2 & 2048) <> 0          -- HEAP_INDEXED_UPDATED
    and (t_infomask2 & 2047) = 0           -- natts == 0 (stub sentinel)
  \gset
-- Force kill/freeze on the stub's tid: both must be refused with a NOTICE.
select heap_force_kill('hstub'::regclass, ARRAY[('(0,' || :'stub_off' || ')')]::tid[]);
select heap_force_freeze('hstub'::regclass, ARRAY[('(0,' || :'stub_off' || ')')]::tid[]);
-- The chain is untouched: the live row is still reachable through each index.
set enable_seqscan = off;
set enable_bitmapscan = off;
select id, a, b from hstub where a = 12;
select id, a, b from hstub where b = 101;
reset enable_bitmapscan;
reset enable_seqscan;
drop table hstub;
drop extension pageinspect;

-- cleanup.
drop view vw;
drop extension pg_surgery;
