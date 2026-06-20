/*-------------------------------------------------------------------------
 *
 * proxy_protocol.c
 *	  Functions related to parse the PROXY Protocol (versions 1 and 2).
 *	  https://www.haproxy.org/download/2.9/doc/proxy-protocol.txt
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/libpq/proxy_protocol.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <arpa/inet.h>
#include <netinet/in.h>

#include "common/ip.h"
#include "libpq/ifaddr.h"
#include "libpq/libpq.h"
#include "libpq/proxy_protocol.h"
#include "port/pg_bswap.h"
#include "utils/guc.h"
#include "utils/guc_hooks.h"
#include "utils/memutils.h"
#include "utils/varlena.h"

/* Raw text of the proxy_networks GUC. */
char	   *ProxyNetworks = NULL;

/*
 * Pre-parsed form of proxy_networks, produced by the check hook and
 * installed by the assign hook.  Stored with guc_malloc() so that the GUC
 * machinery owns its lifetime.
 */
typedef struct ProxyNet
{
	struct sockaddr_storage addr;	/* network address */
	struct sockaddr_storage mask;	/* network mask */
} ProxyNet;

typedef struct ProxyNets
{
	bool		trust_unix;		/* trust Unix-socket peers to send a header */
	int			nnets;
	ProxyNet	nets[FLEXIBLE_ARRAY_MEMBER];
} ProxyNets;

/*
 * The special proxy_networks token to trust Unix-domain socket peers
 * to send a PROXY header.
 */
#define PROXY_UNIX_TOKEN	"unix"

static ProxyNets *proxy_networks = NULL;

/* The 12-byte PROXY protocol v2 signature. */
static const uint8 v2_signature[12] =
"\x0d\x0a\x0d\x0a\x00\x0d\x0a\x51\x55\x49\x54\x0a";

/* Largest possible v1 header line, including the trailing CRLF. */
#define PROXY_V1_MAX_LEN	107

/* Supported v2 address families. */
#define PROXY_V2_AF_UNSPEC	0x00
#define PROXY_V2_TCP4		0x11
#define PROXY_V2_TCP6		0x21

/*
 * Length of the leading address block for each supported v2 family.
 * Any bytes beyond these lengths are TLV vectors and are skipped.
 */
#define PROXY_V2_TCP4_ADDRLEN	12	/* 2 x 4-byte addr + 2 x 2-byte port */
#define PROXY_V2_TCP6_ADDRLEN	36	/* 2 x 16-byte addr + 2 x 2-byte port */

/* Supported v2 commands */
#define PROXY_V2_CMD_LOCAL 0x0
#define PROXY_V2_CMD_PROXY 0x1

/*
 * Parse a single "address" or "address/masklen" network specification into a
 * ProxyNet.  Returns true on success.  No DNS is performed (numeric host
 * only), so this is safe to call from a GUC check hook.
 */
static bool
parse_proxy_network(const char *spec, ProxyNet *net)
{
	char	   *str = pstrdup(spec);
	char	   *slash;
	struct addrinfo hints;
	struct addrinfo *gai_result = NULL;
	bool		ok = false;

	memset(net, 0, sizeof(*net));

	slash = strchr(str, '/');
	if (slash)
		*slash = '\0';

	MemSet(&hints, 0, sizeof(hints));
	hints.ai_flags = AI_NUMERICHOST;
	hints.ai_family = AF_UNSPEC;

	if (pg_getaddrinfo_all(str, NULL, &hints, &gai_result) == 0 &&
		gai_result != NULL &&
		gai_result->ai_addrlen <= sizeof(net->addr))
	{
		memcpy(&net->addr, gai_result->ai_addr, gai_result->ai_addrlen);

		ok = pg_sockaddr_cidr_mask(&net->mask, slash ? slash + 1 : NULL,
								   net->addr.ss_family) >= 0;
	}

	if (gai_result)
		pg_freeaddrinfo_all(hints.ai_family, gai_result);
	pfree(str);
	return ok;
}

/*
 * GUC check hook for proxy_networks.  Parses the comma-separated
 * list of networks into a ProxyNets structure stored in *extra.
 */
bool
check_proxy_networks(char **newval, void **extra, GucSource source)
{
	char	   *rawstring;
	List	   *elemlist;
	ListCell   *l;
	int			nnets;
	int			i;
	ProxyNets  *result;

	/* Need a modifiable copy of the string */
	rawstring = pstrdup(*newval);

	if (!SplitGUCList(rawstring, ',', &elemlist))
	{
		GUC_check_errdetail("List syntax is invalid.");
		pfree(rawstring);
		list_free(elemlist);
		return false;
	}

	nnets = list_length(elemlist);

	result = (ProxyNets *) guc_malloc(LOG,
									  offsetof(ProxyNets, nets) +
									  nnets * sizeof(ProxyNet));
	if (result == NULL)
	{
		pfree(rawstring);
		list_free(elemlist);
		return false;
	}
	result->nnets = 0;
	result->trust_unix = false;

	i = 0;
	foreach(l, elemlist)
	{
		char	   *tok = (char *) lfirst(l);

		if (pg_strcasecmp(tok, PROXY_UNIX_TOKEN) == 0)
		{
			result->trust_unix = true;
			continue;
		}

		if (!parse_proxy_network(tok, &result->nets[i]))
		{
			GUC_check_errdetail("Invalid network specification: \"%s\".", tok);
			guc_free(result);
			pfree(rawstring);
			list_free(elemlist);
			return false;
		}
		i++;
	}
	result->nnets = i;

	pfree(rawstring);
	list_free(elemlist);

	*extra = result;
	return true;
}

/*
 * GUC assign hook for proxy_networks.
 */
void
assign_proxy_networks(const char *newval, void *extra)
{
	proxy_networks = (ProxyNets *) extra;
}

/*
 * Reports whether PROXY protocol parsing is enabled.
 */
bool
ProxyProtocolEnabled(void)
{
	return proxy_networks != NULL &&
		(proxy_networks->nnets > 0 || proxy_networks->trust_unix);
}

/*
 * Reports whether the given peer address falls within one of the trusted
 * proxy networks.
 */
static bool
proxy_source_trusted(const SockAddr *raddr)
{
	int			i;

	if (proxy_networks == NULL)
		return false;

	if (raddr->addr.ss_family == AF_UNIX)
		return proxy_networks->trust_unix;

	if (raddr->addr.ss_family != AF_INET &&
		raddr->addr.ss_family != AF_INET6)
		return false;

	for (i = 0; i < proxy_networks->nnets; i++)
	{
		if (raddr->addr.ss_family == proxy_networks->nets[i].addr.ss_family &&
			pg_range_sockaddr(&raddr->addr,
							  &proxy_networks->nets[i].addr,
							  &proxy_networks->nets[i].mask))
			return true;
	}
	return false;
}

/*
 * Reports whether this connection must begin with a PROXY header.
 */
bool
ProxyProtocolRequired(Port *port)
{
	return proxy_source_trusted(&port->raddr);
}

/*
 * Build an IPv4 or IPv6 SockAddr from a numeric address string and a port
 * string for PROXY v1.
 */
static bool
make_v1_sockaddr(int family, const char *addr, const char *port, SockAddr *sa)
{
	char	   *endptr;
	long		portnum;

	errno = 0;
	portnum = strtol(port, &endptr, 10);
	if (endptr == port || *endptr != '\0' || errno != 0 ||
		portnum < 0 || portnum > 65535)
		return false;

	memset(&sa->addr, 0, sizeof(sa->addr));

	if (family == AF_INET)
	{
		struct sockaddr_in *sin = (struct sockaddr_in *) &sa->addr;

		if (inet_pton(AF_INET, addr, &sin->sin_addr) != 1)
			return false;
		sin->sin_family = AF_INET;
		sin->sin_port = pg_hton16((uint16) portnum);
		sa->salen = sizeof(struct sockaddr_in);
	}
	else
	{
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *) &sa->addr;

		if (inet_pton(AF_INET6, addr, &sin6->sin6_addr) != 1)
			return false;
		sin6->sin6_family = AF_INET6;
		sin6->sin6_port = pg_hton16((uint16) portnum);
		sa->salen = sizeof(struct sockaddr_in6);
	}

	return true;
}

/*
 * Parse a human-readable PROXY header (version 1).
 */
static ProxyProtocolResult
parse_proxy_v1(Port *port, bool *have_client, SockAddr *client)
{
	char		line[PROXY_V1_MAX_LEN + 1];
	int			len = 0;
	char	   *saveptr;
	char	   *proto;
	char	   *src_addr;
	char	   *dst_addr;
	char	   *src_port;
	char	   *dst_port;
	int			family;

	/* The first four bytes ("PROX") have already been consumed by the caller */
	line[len++] = 'P';
	line[len++] = 'R';
	line[len++] = 'O';
	line[len++] = 'X';

	for (;;)
	{
		int			c = pq_getbyte();

		if (c == EOF)
			return PROXY_PROTO_ERROR;
		if (len >= PROXY_V1_MAX_LEN)
			return PROXY_PROTO_ERROR;	/* no CRLF within the size limit */
		line[len++] = (char) c;
		if (c == '\n')
			break;
	}

	/* The line must end with CRLF. */
	if (len < 2 || line[len - 2] != '\r' || line[len - 1] != '\n')
		return PROXY_PROTO_ERROR;
	line[len - 2] = '\0';

	/* It must start with the exact "PROXY " token. */
	if (strncmp(line, "PROXY ", 6) != 0)
		return PROXY_PROTO_ERROR;

	proto = strtok_r(line + 6, " ", &saveptr);
	if (proto == NULL)
		return PROXY_PROTO_ERROR;

	if (strcmp(proto, "UNKNOWN") == 0)
	{
		/* Address is unknown.  Keep the real peer address. */
		*have_client = false;
		return PROXY_PROTO_DONE;
	}
	else if (strcmp(proto, "TCP4") == 0)
		family = AF_INET;
	else if (strcmp(proto, "TCP6") == 0)
		family = AF_INET6;
	else
		return PROXY_PROTO_ERROR;

	src_addr = strtok_r(NULL, " ", &saveptr);
	dst_addr = strtok_r(NULL, " ", &saveptr);
	src_port = strtok_r(NULL, " ", &saveptr);
	dst_port = strtok_r(NULL, " ", &saveptr);

	if (src_addr == NULL || dst_addr == NULL ||
		src_port == NULL || dst_port == NULL)
		return PROXY_PROTO_ERROR;

	/* No further tokens are allowed. */
	if (strtok_r(NULL, " ", &saveptr) != NULL)
		return PROXY_PROTO_ERROR;

	if (!make_v1_sockaddr(family, src_addr, src_port, client))
		return PROXY_PROTO_ERROR;

	*have_client = true;
	return PROXY_PROTO_DONE;
}

/*
 * Parse a binary PROXY header (version 2).
 */
static ProxyProtocolResult
parse_proxy_v2(Port *port, bool *have_client, SockAddr *client)
{
	uint8		sigrest[8];
	uint8		hdr[4];
	uint8		ver,
				cmd,
				fam;
	uint16		datalen;
	uint16		toread;
	uint8		data[PROXY_V2_TCP6_ADDRLEN];	/* largest supported address
												 * block */

	/* Read and verify the remaining 8 bytes of the signature. */
	if (pq_getbytes(sigrest, sizeof(sigrest)) == EOF)
		return PROXY_PROTO_ERROR;

	if (memcmp(sigrest, v2_signature + 4, sizeof(sigrest)) != 0)
		return PROXY_PROTO_ERROR;

	/* Read version+command, family+protocol, and the 2-byte length. */
	if (pq_getbytes(hdr, sizeof(hdr)) == EOF)
		return PROXY_PROTO_ERROR;

	ver = hdr[0] >> 4;
	cmd = hdr[0] & 0x0f;
	fam = hdr[1];
	datalen = ((uint16) hdr[2] << 8) | hdr[3];

	if (ver != 0x2)
		return PROXY_PROTO_ERROR;

	/*
	 * Read the address block.  We only need the leading address bytes.  Any
	 * trailing TLV vectors are discarded so that the stream stays in sync for
	 * the genuine startup packet.
	 */
	toread = Min(datalen, (uint16) sizeof(data));

	if (toread > 0 && pq_getbytes(data, toread) == EOF)
		return PROXY_PROTO_ERROR;

	if (datalen > toread && pq_discardbytes(datalen - toread) == EOF)
		return PROXY_PROTO_ERROR;

	switch (cmd)
	{
		case PROXY_V2_CMD_LOCAL:	/* mainly used for health checks */
			{
				*have_client = false;
				return PROXY_PROTO_DONE;
			}
		case PROXY_V2_CMD_PROXY:
			break;
		default:
			return PROXY_PROTO_ERROR;

	}

	memset(&client->addr, 0, sizeof(client->addr));

	switch (fam)
	{
		case PROXY_V2_TCP4:
			{
				struct sockaddr_in *sin = (struct sockaddr_in *) &client->addr;

				/* The address block must be present.  TLVs may follow it. */
				if (datalen < PROXY_V2_TCP4_ADDRLEN)
					return PROXY_PROTO_ERROR;
				sin->sin_family = AF_INET;
				memcpy(&sin->sin_addr, data, 4);	/* source address */
				memcpy(&sin->sin_port, data + 8, 2);	/* source port */
				client->salen = sizeof(struct sockaddr_in);
				*have_client = true;
				break;
			}
		case PROXY_V2_TCP6:
			{
				struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *) &client->addr;

				/* The address block must be present.  TLVs may follow it. */
				if (datalen < PROXY_V2_TCP6_ADDRLEN)
					return PROXY_PROTO_ERROR;
				sin6->sin6_family = AF_INET6;
				memcpy(&sin6->sin6_addr, data, 16); /* source address */
				memcpy(&sin6->sin6_port, data + 32, 2); /* source port */
				client->salen = sizeof(struct sockaddr_in6);
				*have_client = true;
				break;
			}
		case PROXY_V2_AF_UNSPEC:
		default:
			*have_client = false;
			break;
	}

	return PROXY_PROTO_DONE;
}

/*
 * Try to process a PROXY protocol header.
 *
 */
ProxyProtocolResult
ProcessProxyProtocol(Port *port, const char firstbytes[4])
{
	bool		is_v1;
	bool		is_v2;
	bool		have_client = false;
	SockAddr	client;
	ProxyProtocolResult result;

	/* Only connections from a trusted proxy are eligible */
	if (!proxy_source_trusted(&port->raddr))
		return PROXY_PROTO_NONE;

	is_v1 = (memcmp(firstbytes, "PROX", 4) == 0);
	is_v2 = (memcmp(firstbytes, v2_signature, 4) == 0);

	if (!is_v1 && !is_v2)
		return PROXY_PROTO_NONE;

	if (is_v1)
		result = parse_proxy_v1(port, &have_client, &client);
	else
		result = parse_proxy_v2(port, &have_client, &client);

	if (result != PROXY_PROTO_DONE)
		return result;

	/* Header accepted */
	port->proxy_protocol = true;
	if (have_client)
	{
		port->proxy_addr = port->raddr;
		port->raddr = client;
	}

	pq_endmsgread();

	return PROXY_PROTO_DONE;
}
