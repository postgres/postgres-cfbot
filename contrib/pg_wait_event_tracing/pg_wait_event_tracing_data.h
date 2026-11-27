/*-------------------------------------------------------------------------
 *
 * pg_wait_event_tracing_data.h
 *    Dense wait-event map, with per-class capacities checked against
 *    src/backend/utils/activity/wait_event_names.txt on this master
 *    (headroom >= 8 events per class; see pg_wait_event_tracing_capacity()
 *    and the "capacity" regression test, which enforce that this table is
 *    bumped in the same commit that runs a class out of headroom).
 *
 * Static counts at the time these capacities were chosen: Lock 12, Buffer 4,
 * Activity 18, Client 9, Extension 1 (dynamic; WaitEventExtensionNew()
 * grows this at runtime), IPC 64, Timeout 11, IO 83, InjectionPoint 0
 * (dynamic; WaitEventInjectionPointNew()).  LWLock is not part of this
 * table: its "capacity" is the pg_wait_event_tracing.max_tranches GUC,
 * since LWLock waits are tracked through a per-backend hash, not a flat
 * per-event array.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_WAIT_EVENT_TRACING_DATA_H
#define PG_WAIT_EVENT_TRACING_DATA_H

#define PWET_RAW_CLASSES	12
#define PWET_DENSE_CLASSES	9
#define PWET_NUM_EVENTS		560

static const int8 pwet_class_dense[PWET_RAW_CLASSES] = {
	-1,							/* 0x00: unused */
	-1,							/* 0x01: LWLock (uses hash) */
	-1,							/* 0x02: unused */
	0,							/* 0x03: Lock */
	1,							/* 0x04: Buffer */
	2,							/* 0x05: Activity */
	3,							/* 0x06: Client */
	4,							/* 0x07: Extension */
	5,							/* 0x08: IPC */
	6,							/* 0x09: Timeout */
	7,							/* 0x0a: IO */
	8							/* 0x0b: InjectionPoint */
};

static const int pwet_class_nevents[PWET_DENSE_CLASSES] = {
	32,							/* Lock: 12 in use, was 16 (headroom 4) */
	16,							/* Buffer: 4 in use */
	32,							/* Activity: 18 in use */
	32,							/* Client: 9 in use, was 16 (headroom 7) */
	128,						/* Extension: dynamic */
	128,						/* IPC: 64 in use, was 64 (headroom 0) */
	32,							/* Timeout: 11 in use, was 16 (headroom 5) */
	128,						/* IO: 83 in use */
	32							/* InjectionPoint: dynamic; only ever
								 * populated in injection-points-enabled
								 * test builds, so kept small */
};

static const int pwet_class_offset[PWET_DENSE_CLASSES] = {
	0,							/* Lock */
	32,							/* Buffer */
	48,							/* Activity */
	80,							/* Client */
	112,						/* Extension */
	240,						/* IPC */
	368,						/* Timeout */
	400,						/* IO */
	528							/* InjectionPoint */
};

static const uint8 pwet_dense_to_classid[PWET_DENSE_CLASSES] = {
	0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b
};

/*
 * Class names, in the same order as the dense arrays above, matching what
 * pg_wait_events.type reports for each (see generate-wait_event_types.pl,
 * which derives the type string from the ClassName section name).  Used by
 * pg_wait_event_tracing_capacity() so its output can be compared directly
 * against "SELECT type, count(*) FROM pg_wait_events GROUP BY type".
 */
static const char *const pwet_class_names[PWET_DENSE_CLASSES] = {
	"Lock",
	"Buffer",
	"Activity",
	"Client",
	"Extension",
	"IPC",
	"Timeout",
	"IO",
	"InjectionPoint"
};

#endif							/* PG_WAIT_EVENT_TRACING_DATA_H */
