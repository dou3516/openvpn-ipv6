/*
 *  OpenVPN -- An application to securely tunnel IP networks
 *             over a single UDP port, with support for SSL/TLS-based
 *             session authentication and key exchange,
 *             packet encryption, packet authentication, and
 *             packet compression.
 *
 *  Copyright (C) 2002-2003 James Yonan <jim@yonan.net>
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

#ifndef SOCKET_H
#define SOCKET_H

#include "buffer.h"
#include "common.h"
#include "error.h"

/* persistant across SIGUSR1s */
struct udp_socket_addr
{
  struct sockaddr_in local;
  struct sockaddr_in remote; /* initial remote */
  struct sockaddr_in actual; /* remote may change due to --float */
};

struct udp_socket
{
  bool set_outgoing_initial;
  bool remote_float;
  struct udp_socket_addr *addr;
  const char *ipchange_command;
  int mtu;                      /* OS discovered MTU, or 0 if unknown */
  int mtu_changed;              /* Set to true when mtu value is changed */
  int sd;			/* file descriptor for socket */
};

void
udp_socket_init (struct udp_socket *sock,
		 const char *local_host,
		 const char *remote_host,
		 int local_port,
		 int remote_port,
		 bool bind_local,
		 bool remote_float,
		 bool inetd,
		 struct udp_socket_addr *addr,
		 const char *ipchange_command,
		 int resolve_retry_seconds,
		 int mtu_discover_type);

void
udp_socket_set_outgoing_addr (const struct buffer *buf,
			      struct udp_socket *sock,
			      const struct sockaddr_in *addr);

void
udp_socket_incoming_addr (struct buffer *buf,
			  const struct udp_socket *sock,
			  const struct sockaddr_in *from_addr);


void
udp_socket_get_outgoing_addr (struct buffer *buf,
			      const struct udp_socket *sock,
			      struct sockaddr_in *addr);

void udp_socket_close (struct udp_socket *sock);

const char *
print_sockaddr_ex (const struct sockaddr_in *addr, bool do_port, const char* separator);

const char *
print_sockaddr (const struct sockaddr_in *addr);

/*
 * Inline functions
 */

static inline bool
addr_defined (const struct sockaddr_in *addr)
{
  return addr->sin_addr.s_addr != 0;
}

static inline bool
addr_match (const struct sockaddr_in *a1, const struct sockaddr_in *a2)
{
  return a1->sin_addr.s_addr == a2->sin_addr.s_addr && a1->sin_port == a2->sin_port;
}

/*
 * Check status, usually after a socket read or write
 */

extern unsigned int x_cs_info_level;
extern unsigned int x_cs_verbose_level;

void reset_check_status (void);
void set_check_status (unsigned int info_level, unsigned int verbose_level);
void x_check_status (int status, const char *description, struct udp_socket *sock);

static inline void
check_status (int status, const char *description, struct udp_socket *sock)
{
  if ((status < 0 && errno != EAGAIN) || check_debug_level (x_cs_verbose_level))
    x_check_status (status, description, sock);
}

#endif /* SOCKET_H */
