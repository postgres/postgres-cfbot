/*-------------------------------------------------------------------------
 *
 * protocol.h
 *		Definitions of the request/response codes for the wire protocol.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/protocol.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

/* These are the request codes sent by the frontend. */

#define PqMsg_Bind					'B'
#define PqMsg_Close					'C'
#define PqMsg_Describe				'D'
#define PqMsg_Execute				'E'
#define PqMsg_FunctionCall			'F'
#define PqMsg_Flush					'H'
#define PqMsg_Parse					'P'
#define PqMsg_Query					'Q'
#define PqMsg_Sync					'S'
#define PqMsg_Terminate				'X'
#define PqMsg_CopyFail				'f'
#define PqMsg_GSSResponse			'p'
#define PqMsg_PasswordMessage		'p'
#define PqMsg_SASLInitialResponse	'p'
#define PqMsg_SASLResponse			'p'


/* These are the response codes sent by the backend. */

#define PqMsg_ParseComplete			'1'
#define PqMsg_BindComplete			'2'
#define PqMsg_CloseComplete			'3'
#define PqMsg_NotificationResponse	'A'
#define PqMsg_CommandComplete		'C'
#define PqMsg_DataRow				'D'
#define PqMsg_ErrorResponse			'E'
#define PqMsg_CopyInResponse		'G'
#define PqMsg_CopyOutResponse		'H'
#define PqMsg_EmptyQueryResponse	'I'
#define PqMsg_BackendKeyData		'K'
#define PqMsg_NoticeResponse		'N'
#define PqMsg_AuthenticationRequest 'R'
#define PqMsg_ParameterStatus		'S'
#define PqMsg_RowDescription		'T'
#define PqMsg_FunctionCallResponse	'V'
#define PqMsg_CopyBothResponse		'W'
#define PqMsg_ReadyForQuery			'Z'
#define PqMsg_NoData				'n'
#define PqMsg_PortalSuspended		's'
#define PqMsg_ParameterDescription	't'
#define PqMsg_NegotiateProtocolVersion 'v'


/* These are the codes sent by both the frontend and backend. */

#define PqMsg_CopyDone				'c'
#define PqMsg_CopyData				'd'


/* These are the codes sent by parallel workers to leader processes. */

#define PqMsg_Progress              'P'


/* Replication codes sent by the primary (wrapped in CopyData messages). */

#define PqReplMsg_Keepalive			'k'
#define PqReplMsg_PrimaryStatusUpdate 's'
#define PqReplMsg_WALData			'w'


/* Replication codes sent by the standby (wrapped in CopyData messages). */

#define PqReplMsg_HotStandbyFeedback 'h'
#define PqReplMsg_PrimaryStatusRequest 'p'
#define PqReplMsg_StandbyStatusUpdate 'r'


/* Codes used for backups via COPY OUT (wrapped in CopyData messages). */

#define PqBackupMsg_Manifest		'm'
#define PqBackupMsg_NewArchive		'n'
#define PqBackupMsg_ProgressReport	'p'


/* These are the authentication request codes sent by the backend. */

#define AUTH_REQ_OK			0	/* User is authenticated  */
#define AUTH_REQ_KRB4		1	/* Kerberos V4. Not supported any more. */
#define AUTH_REQ_KRB5		2	/* Kerberos V5. Not supported any more. */
#define AUTH_REQ_PASSWORD	3	/* Password */
#define AUTH_REQ_CRYPT		4	/* crypt password. Not supported any more. */
#define AUTH_REQ_MD5		5	/* md5 password */
/* 6 is available.  It was used for SCM creds, not supported any more. */
#define AUTH_REQ_GSS		7	/* GSSAPI without wrap() */
#define AUTH_REQ_GSS_CONT	8	/* Continue GSS exchanges */
#define AUTH_REQ_SSPI		9	/* SSPI negotiate without wrap() */
#define AUTH_REQ_SASL	   10	/* Begin SASL authentication */
#define AUTH_REQ_SASL_CONT 11	/* Continue SASL authentication */
#define AUTH_REQ_SASL_FIN  12	/* Final SASL message */
#define AUTH_REQ_MAX	   AUTH_REQ_SASL_FIN	/* maximum AUTH_REQ_* value */

/*
 * Bind message extension flags for _pq_.cursor.
 *
 * These values are part of the wire protocol and must not change.
 * Both the server and libpq need these definitions, so they live here
 * rather than in libpq-fe.h alone.
 */
#define PQ_BIND_CURSOR_SCROLL		0x0001	/* SCROLL */
#define PQ_BIND_CURSOR_NO_SCROLL	0x0002	/* NO SCROLL */
#define PQ_BIND_CURSOR_HOLD			0x0004	/* WITH HOLD */
#define PQ_BIND_CURSOR_VALID_FLAGS	(PQ_BIND_CURSOR_SCROLL | \
									 PQ_BIND_CURSOR_NO_SCROLL | \
									 PQ_BIND_CURSOR_HOLD)

/*
 * Execute message extension flags for _pq_.cursor.
 *
 * The low three bits select a fetch direction, with the same meaning as the
 * identically named direction of the SQL FETCH command.  PQ_FETCH_DEFAULT
 * asks for no fetch behavior at all: the row-count field of the Execute
 * message governs, exactly as it does without the extension.
 *
 * These values are part of the wire protocol and must not change.  They are
 * deliberately independent of the server's FetchDirection enum (parsenodes.h)
 * so that it can be reordered or extended freely.
 */
#define PQ_FETCH_DEFAULT			0x0000	/* honor the row-count field */
#define PQ_FETCH_FORWARD			0x0001	/* FETCH FORWARD count */
#define PQ_FETCH_BACKWARD			0x0002	/* FETCH BACKWARD count */
#define PQ_FETCH_ABSOLUTE			0x0003	/* FETCH ABSOLUTE count */
#define PQ_FETCH_RELATIVE			0x0004	/* FETCH RELATIVE count */
#define PQ_FETCH_DIRECTION_MASK		0x0007
#define PQ_FETCH_MOVE				0x0008	/* reposition only, no rows */
#define PQ_FETCH_VALID_FLAGS		(PQ_FETCH_DIRECTION_MASK | \
									 PQ_FETCH_MOVE)

/*
 * Portable spelling of "all remaining rows" for the fetch count.  The
 * server-internal FETCH_ALL is LONG_MAX, which is platform dependent, so the
 * wire protocol reserves the largest Int64 instead.
 */
#define PQ_FETCH_ALL				INT64_MAX

#endif							/* PROTOCOL_H */
