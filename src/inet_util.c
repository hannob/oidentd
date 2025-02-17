/*
** inet_util.c - oidentd network utility functions.
** Copyright (c) 2001-2006 Ryan McCabe <ryan@numb.org>
** Copyright (c) 2018-2019 Janik Rabe  <oidentd@janikrabe.com>
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License, version 2,
** as published by the Free Software Foundation.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#define _GNU_SOURCE
#include <config.h>

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdarg.h>
#include <ctype.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <syslog.h>
#include <netdb.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "oidentd.h"
#include "util.h"
#include "missing.h"
#include "inet_util.h"
#include "options.h"

static int setup_bind(const struct addrinfo *ai, in_port_t listen_port);

static int setup_bind(const struct addrinfo *ai, in_port_t listen_port) {
	int ret;
	const int one = 1;
	int listenfd;

	listenfd = socket(ai->ai_family, SOCK_STREAM, 0);
	if (listenfd == -1) {
		debug("socket: %s", strerror(errno));
		return -1;
	}

	switch (ai->ai_family) {
#if WANT_IPV6
		case AF_INET6:
			SIN6(ai->ai_addr)->sin6_port = listen_port;
			if (setsockopt(listenfd, IPPROTO_IPV6, IPV6_V6ONLY, &one,
						sizeof(one)) != 0) {
				debug("setsockopt IPV6_V6ONLY: %s", strerror(errno));
				return -1;
			}
			break;
#endif

		case AF_INET:
			SIN4(ai->ai_addr)->sin_port = listen_port;
			break;
		default:
			debug("address family %d not supported", ai->ai_family);
			return -1;
	}

	ret = setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (ret != 0) {
		debug("setsockopt: %s", strerror(errno));
		return -1;
	}

	ret = bind(listenfd, ai->ai_addr, ai->ai_addrlen);
	if (ret != 0) {
		debug("bind: %s", strerror(errno));
		return -1;
	}

	if (listen(listenfd, SOMAXCONN) != 0) {
		debug("listen: %s", strerror(errno));
		return -1;
	}

	return listenfd;
}

/*
** Setup the listening socket(s).
*/

int *setup_listen(struct sockaddr_storage **listen_addr, in_port_t listen_port) {
	int ret;
	int *bound_fds = NULL;
	char listen_port_str[64];
	struct addrinfo hints, *res, *cur;
	unsigned int naddr = 0;

	if (listen_addr) {
		do {
			cur = xcalloc(1, sizeof(struct addrinfo));

			cur->ai_family = listen_addr[naddr]->ss_family;

			switch (cur->ai_family) {
#if WANT_IPV6
				case AF_INET6:
					cur->ai_addrlen = sizeof(struct sockaddr_in6);
					break;
#endif
				case AF_INET:
					cur->ai_addrlen = sizeof(struct sockaddr_in);
					break;
				default:
					debug("address family %d not supported", cur->ai_family);
					free(cur);
					free(listen_addr[naddr]);
					return NULL;
			}

			cur->ai_addr = xmalloc(cur->ai_addrlen);
			memcpy(cur->ai_addr, listen_addr[naddr], cur->ai_addrlen);

			ret = setup_bind(cur, listen_port);
			free(cur->ai_addr);
			free(cur);
			free(listen_addr[naddr]);

			if (ret == -1)
				return NULL;

			bound_fds = xrealloc(bound_fds, (naddr + 2) * sizeof(int));
			bound_fds[naddr] = ret;
			bound_fds[++naddr] = -1;
		} while (listen_addr[naddr]);

		return bound_fds;
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = PF_UNSPEC;
	hints.ai_flags = AI_PASSIVE;
	hints.ai_socktype = SOCK_STREAM;

	snprintf(listen_port_str, sizeof(listen_port_str), "%d", listen_port);

	ret = getaddrinfo(NULL, listen_port_str, &hints, &res);
	if (ret != 0) {
		debug("getaddrinfo: %s", gai_strerror(ret));
		return NULL;
	}

	cur = res;
	if (cur) {
		size_t bound_addr = 0;
		size_t fdlen = 4;

		bound_fds = xmalloc(fdlen * sizeof(int));

		do {
			ret = setup_bind(cur, listen_port);
			if (ret == -1)
				goto bind_next;

			if (bound_addr >= fdlen - 1) {
				fdlen += 4;
				bound_fds = xrealloc(bound_fds, fdlen * sizeof(int));
			}

			bound_fds[bound_addr++] = ret;

			bind_next:
				cur = cur->ai_next;
		} while (cur);

		bound_fds[bound_addr++] = -1;
		bound_fds = xrealloc(bound_fds, bound_addr * sizeof(int));
		freeaddrinfo(res);
	} else
		return NULL;

	return bound_fds;
}

/*
** Read at most "len" bytes from socket "sock" into "buf".
*/

ssize_t sock_read(int sock, char *buf, ssize_t len) {
	char c;
	ssize_t i;
	ssize_t ret;

	if (!buf)
		return 0;

	for (i = 1; i < len; ++i) {
top:
		ret = read(sock, &c, 1);
		if (ret == 1) {
			*buf++ = c;

			if (c == '\n')
				break;
		} else if (ret == 0) {
			if (i == 1)
				return 0;

			break;
		} else if (errno == EINTR) {
			goto top;
		} else {
			return 0;
		}
	}

	*buf = '\0';
	return i;
}

/*
** Write to a socket, deal with interrupted and incomplete writes.  Returns
** the number of characters written to the socket on success, -1 on failure.
*/

ssize_t sock_write(int sock, void *buf, ssize_t len) {
	ssize_t written = 0;

	while (len > 0) {
		ssize_t n;

		n = write(sock, buf, (size_t) len);
		if (n == -1) {
			if (errno == EINTR)
				continue;
			return -1;
		}

		written += n;
		len -= n;
		buf = (char *) buf + n;
	}

	return written;
}

/*
** printf-like function that writes to sockets.
*/

ssize_t sockprintf(int fd, const char *fmt, ...) {
	va_list ap;
	char *buf;
	ssize_t ret;

	va_start(ap, fmt);
	ret = vasprintf(&buf, fmt, ap);
	va_end(ap);

	if (ret == -1)
		return -1;

	ret = sock_write(fd, buf, ret);
	free(buf);

	return ret;
}

/*
** Return the canonical hostname of the given address.
*/

inline int get_hostname(struct sockaddr_storage *addr,
						char *hostbuf,
						socklen_t len)
{
	int ret;

	ret = getnameinfo((struct sockaddr *) addr, sizeof(struct sockaddr_storage),
					hostbuf, len, NULL, 0, NI_NAMEREQD);

	return ret;
}

/*
** Get the port associated with a TCP service name.
*/

int get_port(const char *name, in_port_t *port) {
	struct servent *servent;

	servent = getservbyname(name, "tcp");
	if (servent) {
		*port = ntohs(servent->s_port);
	} else {
		char *end;
		long temp_port;

		temp_port = strtol(name, &end, 10);

		if (*end != '\0')
			return -1;

		if (!VALID_PORT(temp_port))
			return -1;

		*port = (in_port_t) temp_port;
	}

	return 0;
}

/*
** Return a network byte ordered IPv4 or IPv6 address.
*/

int get_addr(const char *hostname, struct sockaddr_storage *addr) {
	struct addrinfo *res;
	size_t len;

	if (getaddrinfo(hostname, NULL, NULL, &res) != 0)
		return -1;

	switch (res->ai_addr->sa_family) {
		case AF_INET:
			len = sizeof(struct sockaddr_in);
			break;
#if WANT_IPV6
		case AF_INET6:
			len = sizeof(struct sockaddr_in6);
			break;
#endif
		default:
			goto out_fail;
	}

	if (len < (size_t) res->ai_addrlen)
		goto out_fail;

	memcpy(addr, res->ai_addr, res->ai_addrlen);
	freeaddrinfo(res);

	return 0;

out_fail:
	freeaddrinfo(res);
	return -1;
}

/*
** Returns the address set in the appropriate
** sockaddr struct.
*/

inline void *sin_addr(struct sockaddr_storage *ss) {
#if WANT_IPV6
	if (ss->ss_family == AF_INET6)
		return &SIN6(ss)->sin6_addr;
#endif

	return &SIN4(ss)->sin_addr;
}

/*
** Return string IPv4 or IPv6 address.
*/

inline void get_ip(	struct sockaddr_storage *ss,
					char *buf,
					socklen_t len)
{
	inet_ntop(ss->ss_family, sin_addr(ss), buf, len);
}

/*
** Returns true if the two in4 addresses are equal, false
** if they aren't.
*/

inline bool sin4_equal(	struct sockaddr_storage *ss1,
						struct sockaddr_storage *ss2)
{
	return (SIN4(ss1)->sin_addr.s_addr == SIN4(ss2)->sin_addr.s_addr);
}

/*
** Setup "ss" as an IPv4 sockaddr struct with the address specified by
** "addr"
*/

void sin_setv4(in_addr_t addr, struct sockaddr_storage *ss) {
	memset(ss, 0, sizeof(struct sockaddr_storage));
	ss->ss_family = AF_INET;
	memcpy(&SIN4(ss)->sin_addr, &addr, sizeof(addr));
}

#if WANT_IPV6

inline bool sin6_equal(	struct sockaddr_storage *ss1,
						struct sockaddr_storage *ss2)
{
	return IN6_ARE_ADDR_EQUAL(&SIN6(ss1)->sin6_addr, &SIN6(ss2)->sin6_addr);
}

/*
** Setup "ss" as an IPv6 sockaddr struct with the address specified by
** "sin6"
*/

void sin_setv6(struct in6_addr *sin6, struct sockaddr_storage *ss) {
	memset(ss, 0, sizeof(struct sockaddr_storage));
	ss->ss_family = AF_INET6;
	memcpy(&SIN6(ss)->sin6_addr, sin6, sizeof(struct sockaddr_in6));
}

#endif

/*
** Returns the length of the sockaddr struct.
*/

inline size_t sin_len(const struct sockaddr_storage *ss __notused) {
#if WANT_IPV6
	if (ss->ss_family == AF_INET6)
		return sizeof(struct sockaddr_in6);
#endif

	return sizeof(struct sockaddr_in);
}

/*
** Returns the length of the address portion of the sockaddr
** structure.
*/

inline size_t sin_addr_len(const struct sockaddr_storage *ss __notused) {
#if WANT_IPV6
	if (ss->ss_family == AF_INET6)
		return sizeof(struct in6_addr);
#endif

	return sizeof(struct in_addr);
}

/*
** Copies a sockaddr struct.
*/

inline void sin_copy(	struct sockaddr_storage *ss1,
						const struct sockaddr_storage *ss2)
{
	memset(ss1, 0, sizeof(struct sockaddr_storage));
	memcpy(ss1, ss2, sin_len(ss2));
}

/*
** Returns the port set in the sockaddr struct.
*/

inline in_port_t sin_port(const struct sockaddr_storage *ss) {
#if WANT_IPV6
	if (ss->ss_family == AF_INET6)
		return SIN6(ss)->sin6_port;
#endif

	return SIN4(ss)->sin_port;
}

/*
** Sets the port for the approprite socket family.
*/

inline void sin_set_port(in_port_t port, struct sockaddr_storage *ss) {
#if WANT_IPV6
	if (ss->ss_family == AF_INET6)
		SIN6(ss)->sin6_port = port;
#endif

	SIN4(ss)->sin_port = port;
}

/*
** Checks whether two addresses are equal.
*/

inline bool sin_equal(	struct sockaddr_storage *ss1,
						struct sockaddr_storage *ss2)
{
#if WANT_IPV6
	if (ss1->ss_family == AF_INET6)
		return sin6_equal(ss1, ss2);
#endif

	return sin4_equal(ss1, ss2);
}

/*
** Converts an IPv6-mapped IPv4 address to an IPv4 address.
*/

inline void sin_extractv4(void *in6, struct in_addr *in4) {
	/* XXX - Is there a cleaner portable way to do this? */
	memcpy(in4, ((char *) in6) + 12, sizeof(struct in_addr));
}

/*
** Converts an IPv4 address to an IPv6-mapped IPv4 address.
*/

inline void sin_mapv4to6(void *in4, struct in6_addr *in6) {
	/* XXX - Is there a cleaner portable way to do this? */
	char *in6_ptr = (char *) in6;
	memset(in6_ptr, 0, 10);
	memset(in6_ptr + 10, 0xFF, 2);
	memcpy(in6_ptr + 12, in4, sizeof(struct in_addr));
}
