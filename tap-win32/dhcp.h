/*
 *  TAP-Win32 -- A kernel driver to provide virtual tap device functionality
 *               on Windows.  Originally derived from the CIPE-Win32
 *               project by Damion K. Wilson, with extensive modifications by
 *               James Yonan.
 *
 *  All source code which derives from the CIPE-Win32 project is
 *  Copyright (C) Damion K. Wilson, 2003, and is released under the
 *  GPL version 2 (see below).
 *
 *  All other source code is Copyright (C) James Yonan, 2003-2004,
 *  and is released under the GPL version 2 (see below).
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program (see the file COPYING included with this
 *  distribution); if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#pragma pack(1)

//
// How many bad DHCPREQUESTs do we receive before we
// return a NAK?
//
// A bad DHCPREQUEST is defined to be one where the
// requestor doesn't know its IP address.
//

#define BAD_DHCPREQUEST_NAK_THRESHOLD 3

//
// UDP port numbers of DHCP messages.
//

#define BOOTPS_PORT 67
#define BOOTPC_PORT 68

//
// The DHCP message structure
//

typedef struct {
# define BOOTREQUEST 1
# define BOOTREPLY   2
  UCHAR op;          /* message op */

  UCHAR  htype;      /* hardware address type (e.g. '1' = 10Mb Ethernet) */
  UCHAR  hlen;       /* hardware address length (e.g. '6' for 10Mb Ethernet) */
  UCHAR  hops;       /* client sets to 0, may be used by relay agents */
  ULONG  xid;        /* transaction ID, chosen by client */
  USHORT secs;       /* seconds since request process began, set by client */
  USHORT flags;
  ULONG  ciaddr;     /* client IP address, client sets if known */
  ULONG  yiaddr;     /* 'your' IP address -- server's response to client */
  ULONG  siaddr;     /* server IP address */
  ULONG  giaddr;     /* relay agent IP address */
  UCHAR  chaddr[16]; /* client hardware address */
  UCHAR  sname[64];  /* optional server host name */
  UCHAR  file[128];  /* boot file name */
  ULONG  magic;      /* must be 0x63825363 (network order) */
} DHCP;

// Convenience macros for setting DHCP options
#define SET_DHCP_OPT(l, t, v) { l.type = t; l.len = sizeof (l.data); l.data = (v); }
#define SET_DHCP_END(l) { l->end.type = DHCP_END; }

#define SET_DHCP_PADDING(l) \
{ \
  int pi; \
  for (pi = 0; pi < sizeof (l->padding); ++pi) \
    l->padding[pi] = DHCP_PAD; \
}

typedef struct {
  UCHAR type;
} DHCPOPT0;

typedef struct {
  UCHAR type;
  UCHAR len;
  UCHAR data;
} DHCPOPT8;

typedef struct {
  UCHAR type;
  UCHAR len;
  ULONG data;
} DHCPOPT32;

typedef struct {
  ETH_HEADER eth;
  IPHDR ip;
  UDPHDR udp;
  DHCP dhcp;
} DHCPPRE;

typedef struct {
  DHCPPRE pre;
  DHCPOPT8 msg_type;
  DHCPOPT32 server_id;
  DHCPOPT32 lease_time;
  DHCPOPT32 renew_time;
  DHCPOPT32 rebind_time;
  DHCPOPT32 netmask;
  DHCPOPT0 end;
} DHCP_OFFER_ACK;

typedef struct {
  DHCPPRE pre;
  DHCPOPT8 msg_type;
  DHCPOPT32 server_id;
  DHCPOPT0 end;
} DHCP_NAK;

#pragma pack()

//
// DHCP Option types
//

#define DHCP_MSG_TYPE    53  /* message type (u8) */
#define DHCP_PARM_REQ    55  /* parameter request list: c1 (u8), ... */
#define DHCP_CLIENT_ID   61  /* client ID: type (u8), i1 (u8), ... */
#define DHCP_IP          50  /* requested IP addr (u32) */
#define DHCP_NETMASK      1  /* subnet mask (u32) */
#define DHCP_LEASE_TIME  51  /* lease time sec (u32) */
#define DHCP_RENEW_TIME  58  /* renewal time sec (u32) */
#define DHCP_REBIND_TIME 59  /* rebind time sec (u32) */
#define DHCP_SERVER_ID   54  /* server ID: IP addr (u32) */
#define DHCP_PAD          0
#define DHCP_END        255

//
// DHCP Messages types
//

#define DHCPDISCOVER 1
#define DHCPOFFER    2
#define DHCPREQUEST  3
#define DHCPDECLINE  4
#define DHCPACK      5
#define DHCPNAK      6
#define DHCPRELEASE  7
#define DHCPINFORM   8

#if DBG

VOID
DumpDHCP (const ETH_HEADER *eth,
	  const IPHDR *ip,
	  const UDPHDR *udp,
	  const DHCP *dhcp,
	  const int optlen);

#endif
