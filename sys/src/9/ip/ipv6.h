/* 
 * This file is part of the UCB release of Plan 9. It is subject to the license
 * terms in the LICENSE file found in the top-level directory of this
 * distribution and at http://akaros.cs.berkeley.edu/files/Plan9License. No
 * part of the UCB release of Plan 9, including this file, may be copied,
 * modified, propagated, or distributed except according to the terms contained
 * in the LICENSE file.
 */

/*
 * Internet Protocol Version 6
 *
 * rfc2460 defines the protocol.
 * rfc4291 defines the address prefices.
 *
 * global unicast is anything but unspecified (::), loopback (::1),
 * multicast (ff00::/8), and link-local unicast (fe80::/10).
 *
 * site-local (fec0::/10) is now deprecated by rfc3879.
 *
 * Unique Local IPv6 Unicast Addresses are defined by rfc4193.
 * prefix is fc00::/7, scope is global, routing is limited to roughly a site.
 */
#define isv6mcast(addr)	  ((addr)[0] == 0xff)
#define islinklocal(addr) ((addr)[0] == 0xfe && ((addr)[1] & 0xc0) == 0x80)

#define optexsts(np)	(nhgets((np)->ploadlen) > 24)
#define issmcast(addr)	(memcmp((addr), v6solicitednode, 13) == 0)

#ifndef MIN
#define MIN(a, b) ((a) <= (b)? (a): (b))
#endif

enum {				/* Header Types */
	HBH		= 0,	/* hop-by-hop multicast routing protocol */
	ICMP		= 1,
	IGMP		= 2,
	GGP		= 3,
	IPINIP		= 4,
	ST		= 5,
	TCP		= 6,
	UDP		= 17,
	ISO_TP4		= 29,
	RH		= 43,
	FH		= 44,
	IDRP		= 45,
	RSVP		= 46,
	AH		= 51,
	ESP		= 52,
	ICMPv6		= 58,
	NNH		= 59,
	DOH		= 60,
	ISO_IP		= 80,
	IGRP		= 88,
	OSPF		= 89,

	Maxhdrtype	= 256,
};

enum {
	/* multicast flags and scopes */

//	Well_known_flg	= 0,
//	Transient_flg	= 1,

//	Interface_local_scop = 1,
	Link_local_scop	= 2,
//	Site_local_scop	= 5,
//	Org_local_scop	= 8,
	Global_scop	= 14,

	/* various prefix lengths */
	SOLN_PREF_LEN	= 13,

	/* icmpv6 unreach codes */
	icmp6_no_route		= 0,
	icmp6_ad_prohib		= 1,
	icmp6_unassigned	= 2,
	icmp6_adr_unreach	= 3,
	icmp6_port_unreach	= 4,
	icmp6_unkn_code		= 5,

	/* various flags & constants */
	v6MINTU		= 1280,
	HOP_LIMIT	= 255,
	ETHERHDR_LEN	= 14,
	IPV6HDR_LEN	= 40,
	IPV4HDR_LEN	= 20,

	/* option types */

	SRC_LLADDR	= 1,
	TARGET_LLADDR	= 2,
	PREFIX_INFO	= 3,
	REDIR_HEADER	= 4,
	MTU_OPTION	= 5,

	SRC_UNSPEC	= 0,
	SRC_UNI		= 1,
	TARG_UNI	= 2,
	TARG_MULTI	= 3,

	Tunitent	= 1,
	Tuniproxy	= 2,
	Tunirany	= 3,

	/* Router constants (all times in milliseconds) */
	MAX_INIT_RTR_ADVERT_INTVL = 16000,
	MAX_INIT_RTR_ADVERTS	= 3,
	MAX_FINAL_RTR_ADVERTS	= 3,
	MIN_DELAY_BETWEEN_RAS	= 3000,
	MAX_RA_DELAY_TIME	= 500,

	/* Host constants */
	MAX_RTR_SOLICIT_DELAY	= 1000,
	RTR_SOLICIT_INTVL	= 4000,
	MAX_RTR_SOLICITS	= 3,

	/* Node constants */
	MAX_MULTICAST_SOLICIT	= 3,
	MAX_UNICAST_SOLICIT	= 3,
	MAX_ANYCAST_DELAY_TIME	= 1000,
	MAX_NEIGHBOR_ADVERT	= 3,
	REACHABLE_TIME		= 30000,
	RETRANS_TIMER		= 1000,
	DELAY_FIRST_PROBE_TIME	= 5000,
};

typedef struct Ip6hdr	Ip6hdr;
typedef struct Opthdr	Opthdr;
typedef struct Routinghdr Routinghdr;
typedef struct Fraghdr6	Fraghdr6;

struct	Ip6hdr {
	uint8_t	vcf[4];		/* version:4, traffic class:8, flow label:20 */
	uint8_t	ploadlen[2];	/* payload length: packet length - 40 */
	uint8_t	proto;		/* next header type */
	uint8_t	ttl;		/* hop limit */
	uint8_t	src[IPaddrlen];
	uint8_t	dst[IPaddrlen];
};

struct	Opthdr {
	uint8_t	nexthdr;
	uint8_t	len;
};

struct	Routinghdr {
	uint8_t	nexthdr;
	uint8_t	len;
	uint8_t	rtetype;
	uint8_t	segrem;
};

struct	Fraghdr6 {
	uint8_t	nexthdr;
	uint8_t	res;
	uint8_t	offsetRM[2];	/* Offset, Res, M flag */
	uint8_t	id[4];
};

extern uint8_t v6allnodesN[IPaddrlen];
extern uint8_t v6allnodesL[IPaddrlen];
extern uint8_t v6allroutersN[IPaddrlen];
extern uint8_t v6allroutersL[IPaddrlen];
extern uint8_t v6allnodesNmask[IPaddrlen];
extern uint8_t v6allnodesLmask[IPaddrlen];
extern uint8_t v6allroutersS[IPaddrlen];
extern uint8_t v6solicitednode[IPaddrlen];
extern uint8_t v6solicitednodemask[IPaddrlen];
extern uint8_t v6Unspecified[IPaddrlen];
extern uint8_t v6loopback[IPaddrlen];
extern uint8_t v6loopbackmask[IPaddrlen];
extern uint8_t v6linklocal[IPaddrlen];
extern uint8_t v6linklocalmask[IPaddrlen];
extern uint8_t v6glunicast[IPaddrlen];
extern uint8_t v6multicast[IPaddrlen];
extern uint8_t v6multicastmask[IPaddrlen];

extern int v6llpreflen;
extern int v6lbpreflen;
extern int v6mcpreflen;
extern int v6snpreflen;
extern int v6aNpreflen;
extern int v6aLpreflen;

extern int ReTransTimer;

void ipv62smcast(uint8_t *, uint8_t *);
void icmpns(Fs *f, uint8_t* src, int suni, uint8_t* targ, int tuni,
	    uint8_t* mac);
void icmpna(Fs *f, uint8_t* src, uint8_t* dst, uint8_t* targ, uint8_t* mac,
	    uint8_t flags);
void icmpttlexceeded6(Fs *f, Ipifc *ifc, Block *bp);
void icmppkttoobig6(Fs *f, Ipifc *ifc, Block *bp);
void icmphostunr(Fs *f, Ipifc *ifc, Block *bp, int code, int free);
