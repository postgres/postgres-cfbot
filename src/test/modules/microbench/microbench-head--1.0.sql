/* src/test/modules/microbench/microbench--1.0.sql */
/* Generated from microbench--1.0.sql.head and per-test install.sql files. */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION microbench" to load this file. \quit

CREATE DOMAIN microbench_format AS numeric(15, 2);

CREATE TYPE microbench_sample AS (
  op         text,
  avg_ns     float8,
  batch_size int8,
  id         int8,
  "group"    int8
);

CREATE TYPE microbench_stats AS (
  op            text,
  "group"       int8,
  avg           microbench_format,
  min           microbench_format,
  q1            microbench_format,
  med           microbench_format,
  q3            microbench_format,
  max           microbench_format,
  std           microbench_format
);


CREATE TYPE microbench_stats_with_count AS (
  op            text,
  "group"       int8,
  avg           microbench_format,
  min           microbench_format,
  q1            microbench_format,
  med           microbench_format,
  q3            microbench_format,
  max           microbench_format,
  std           microbench_format,
  count         int8
);

CREATE FUNCTION format_microbench(samples microbench_sample[])
RETURNS SETOF microbench_stats
LANGUAGE sql
STABLE
AS $$
  SELECT
    s.op,
    s."group",
    sum(s.avg_ns * s.batch_size) / NULLIF(sum(s.batch_size), 0),
    min(s.avg_ns),
    percentile_cont(0.25) WITHIN GROUP (ORDER BY s.avg_ns),
    percentile_cont(0.50) WITHIN GROUP (ORDER BY s.avg_ns),
    percentile_cont(0.75) WITHIN GROUP (ORDER BY s.avg_ns),
    max(s.avg_ns),
    stddev(s.avg_ns)
  FROM unnest(samples) AS s
  GROUP BY s.op, s."group"
  ORDER BY min(s.id), s."group" NULLS FIRST;
$$;


CREATE FUNCTION format_microbench_with_count(samples microbench_sample[])
RETURNS SETOF microbench_stats_with_count
LANGUAGE sql
STABLE
AS $$
  SELECT
    s.op,
    s."group",
    sum(s.avg_ns * s.batch_size) / NULLIF(sum(s.batch_size), 0),
    min(s.avg_ns),
    percentile_cont(0.25) WITHIN GROUP (ORDER BY s.avg_ns),
    percentile_cont(0.50) WITHIN GROUP (ORDER BY s.avg_ns),
    percentile_cont(0.75) WITHIN GROUP (ORDER BY s.avg_ns),
    max(s.avg_ns),
    stddev(s.avg_ns),
    sum(s.batch_size)
  FROM unnest(samples) AS s
  GROUP BY s.op, s."group"
  ORDER BY min(s.id), s."group" NULLS FIRST;
$$;
