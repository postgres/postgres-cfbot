--
-- Test split location randomization B-tree indexes.
--

set client_min_messages TO 'warning';
create extension if not exists pageinspect;
reset client_min_messages;

create table nbtree_split_random(id int) with (autovacuum_enabled = off);

create index nbtree_split_random_id_idx on nbtree_split_random using btree (id);

insert into nbtree_split_random SELECT g from generate_series(1, 150000) g;

-- The idea is to verify that free space across index pages is more variable
-- than without randomization. For that build a histogram of free space and
-- check how many buckets we end up with:
--
-- * Without randomization the insert above usually produces only two buckets,
--   one after filtering out buckets with only one value in it.
-- 
-- * With randomization there are usually about six buckets after filtering out
--   small ones.
--
-- For the test we define lowest number of buckers to be three as an empirical
-- value, which differentiates two cases.
with range as (
    select
        min(free_size) as min,
        max(free_size) as max
    from bt_multi_page_stats('nbtree_split_random_id_idx', 1, -1)
), hist as (
    select width_bucket(
        free_size::float / page_size::float,
        min::float / page_size::float,
        max::float / page_size::float, 100
    ) as buckets, count(*)
    from bt_multi_page_stats('nbtree_split_random_id_idx', 1, -1),  range
    group by buckets having count(*) > 1 order by buckets
)
select count(*) > 2 from hist;

drop extension pageinspect;
