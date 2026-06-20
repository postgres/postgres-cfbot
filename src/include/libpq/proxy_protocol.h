/*-------------------------------------------------------------------------
 *
 * proxy_protocol.h
 *    Interface of libpq/proxy_protocol.c
 *
 * src/include/libpq/proxy_protocol.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROXY_PROTOCOL_H
#define PROXY_PROTOCOL_H

#include "libpq/libpq-be.h"

/* Comma-separated list of networks allowed to send PROXY headers. */
extern PGDLLIMPORT char *ProxyNetworks;

/* Types of PROXY protocol parsing results. */
typedef enum ProxyProtocolResult
{
	PROXY_PROTO_NONE,			/* not a PROXY header (or not from a trusted
								 * source).  Caller should handle the data
								 * normally */
	PROXY_PROTO_DONE,			/* a valid PROXY header was consumed, port has
								 * been updated and the real startup packet
								 * should now be read */
	PROXY_PROTO_ERROR,			/* a PROXY header from a trusted source was
								 * malformed, the connection must be closed */
} ProxyProtocolResult;

extern bool ProxyProtocolEnabled(void);

extern bool ProxyProtocolRequired(Port *port);

extern ProxyProtocolResult ProcessProxyProtocol(Port *port,
												const char firstbytes[4]);

#endif
