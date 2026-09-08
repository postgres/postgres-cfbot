#!/usr/bin/env python

import random
import psycopg2
import time
import psutil
import sys
import subprocess

conn = None

ts = int(time.time())

result_file = open(f'results-{ts}.log', 'w')
crash_file = open(f'crashes-{ts}.log', 'w')
query_file = open(f'queries-{ts}.log', 'w')
schema_file = open(f'schema-{ts}.log', 'w')


def get_mem_usage():
	result = subprocess.run(['./get-mem.sh'], stdout=subprocess.PIPE)
	return result.stdout.decode('utf-8').strip()

def get_timing():
	result = subprocess.run(['./get-timing.sh'], stdout=subprocess.PIPE)
	return result.stdout.decode('utf-8').strip()

def get_estimate():
	result = subprocess.run(['./get-estimate.sh'], stdout=subprocess.PIPE)
	return result.stdout.decode('utf-8').strip()

def get_difficulty():
	result = subprocess.run(['./get-difficulty.sh'], stdout=subprocess.PIPE)
	return result.stdout.decode('utf-8').strip()

def get_rels(p):
	result = subprocess.run(['./get-rels.sh', p], stdout=subprocess.PIPE)
	return result.stdout.decode('utf-8').strip()

def generate_schema(schema, ntables, ncols, indexes = False):

	conn = psycopg2.connect('host=localhost user=tomas dbname=test')

	cur = conn.cursor()

	for t in range(0, ntables):
		cols = [f'c_{t}_{c} int' for c in range(0, ncols)]
		cols = ', '.join(cols)
		cur.execute(f'DROP TABLE IF EXISTS t_{t};')
		cur.execute(f'CREATE TABLE t_{t} ({cols});')

		schema_file.write(f'---------- {seed} ----------')
		schema_file.write(f'DROP TABLE IF EXISTS t_{t};')
		schema_file.write(f'CREATE TABLE t_{t} ({cols});')

		if indexes:
			for c in range(0, ncols):
				cur.execute(f'CREATE INDEX ON t_{t} (c_{t}_{c});')
				schema_file.write(f'CREATE INDEX ON t_{t} (c_{t}_{c});')

	schema_file.flush()

	cur.execute('commit')

def generate_data(schema, ntables, ncols):

	conn = psycopg2.connect('host=localhost user=tomas dbname=test')

	cur = conn.cursor()

	for t in range(0, ntables):
		cols = ', '.join(['i/100' for c in range(0, ncols)])
		cur.execute(f'INSERT INTO  t_{t} SELECT {cols} FROM generate_series(1,10000) s(i);')
		schema_file.write(f'INSERT INTO  t_{t} SELECT {cols} FROM generate_series(1,10000) s(i);')

	schema_file.flush()

	cur.execute('analyze')
	cur.execute('commit')


def generate_query(seed, schema, ntables, ncols):

	global peak_mem_usage

	conn = psycopg2.connect('host=localhost user=tomas dbname=test')

	nclauses = 0

	# probability that there's a clause to an earlier clause (how dense the join graph is)
	pclause = random.random()

	# 10% chance all joins are of the same type
	fixed_join_type = None
	if (random.random() < 0.25):
		#fixed_join_type = random.choice(['INNER JOIN', 'LEFT JOIN', 'RIGHT JOIN', 'FULL JOIN'])
		fixed_join_type = random.choice(['INNER JOIN', 'LEFT JOIN', 'RIGHT JOIN'])

	# edges in the join graph
	edges = []
	joins = {'INNER JOIN': 0, 'LEFT JOIN': 0, 'RIGHT JOIN': 0, 'FULL JOIN': 0}

	configure_session(conn)

	random.seed(seed)

	sql = f'/* query {seed} */ EXPLAIN SELECT * FROM t_0'

	# add tables one to one, with clauses to earlier tables
	for t1 in range(1, ntables):

		# randomly generate clauses, determined by pclause probability
		clauses = []

		for t2 in range(0, t1 - 1):

			# should we have a clause for this table?
			if random.random() < pclause:

				# random columns in either table
				c1 = random.randint(0, ncols-1)
				c2 = random.randint(0, ncols-1)

				clauses.append(f'(t_{t1}.c_{t1}_{c1} = t_{t2}.c_{t2}_{c2})')
				edges.append([t1,t2])

		# if there's no clause, randomly pick a table and force a clause
		# XXX maybe we could/should try joins without clauses

		if len(clauses) == 0:

			# random preceding table
			t2 = random.randint(0, t1 - 1)

			# random columns
			c1 = random.randint(0, ncols-1)
			c2 = random.randint(0, ncols-1)

			clauses.append(f'(t_{t1}.c_{t1}_{c1} = t_{t2}.c_{t2}_{c2})')
			edges.append([t1,t2])

		nclauses += len(clauses)
		clauses = ' AND '.join(clauses)

		if fixed_join_type is not None:
			join_type = fixed_join_type
		else:
			#join_type = random.choice(['INNER JOIN', 'LEFT JOIN', 'RIGHT JOIN', 'FULL JOIN'])
			join_type = random.choice(['INNER JOIN', 'LEFT JOIN', 'RIGHT JOIN'])

		joins[join_type] += 1

		sql += f'\n    {join_type} t_{t1} ON ({clauses})'

	maxclauses = int((ntables * (ntables - 1)) / 2)
	frac = int(100.0 * nclauses / maxclauses)
	pclause = int(100 * pclause)

	cur = conn.cursor()

	try:

		query_file.write(f'========== {seed} ==========\n')
		query_file.write(f'{sql}\n\n')
		query_file.flush()

		s = time.time()
		cur.execute(sql)
		e = time.time()

		elapsed = 1000 * (e - s)

		mem = get_mem_usage()
		timing = get_timing()
		estimate = get_estimate()
		difficulty = get_difficulty()

		r1 = get_rels('standard_join_search')
		r2 = get_rels('estimate_join_search_effort')

		print(f'{seed} mem "{mem}" timing "{timing}" estimate "{estimate}" difficulty "{difficulty}" r1 "{r1}" r2 "{r2}"')

		if (r1 != r2) or (r1 != str(ntables)):
			sys.exit(1)

		inner_count = joins['INNER JOIN']
		left_count = joins['LEFT JOIN']
		right_count = joins['RIGHT JOIN']
		full_count = joins['FULL JOIN']

		result_file.write(f'{seed} {schema} {ntables} {ncols} {pclause} {nclauses} {maxclauses} {frac} {inner_count} {left_count} {right_count} {full_count} {estimate} {difficulty} {mem} {timing} {elapsed}\n')
		result_file.flush()

	except Exception as ex:

		crash_file.write(f'{seed} {schema} {ntables} {ncols} {nclauses}\n')

		s = str(ex)
		crash_file.write(f'{s}\n\n')
		crash_file.flush()

		# sleep so that the database can restart
		time.sleep(5)

	sys.stdout.flush()


def configure_session(conn):

	cur = conn.cursor()

	cur.execute(f'set join_collapse_limit = 32')
	cur.execute(f'set geqo = off')
	cur.execute(f'set log_planner_stats = on')
	cur.execute(f'set join_search_estimate = on')

	cur.close()


if __name__ == '__main__':

	seed = 0
	schema = 0

	# generate random data sets
	for r in range(0, 100):

		schema += 1
		generate_schema(schema, 16, 32, True)
		generate_data(schema, 16, 32)

		for r2 in range(0, 100):

			for ntables in range(2, 15):

				for s in range(0, 10):

					seed += 1
					generate_query(seed, schema, ntables, 32)

