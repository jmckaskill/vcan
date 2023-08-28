#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
static inline void perror(const char *msg)
{
	char buf[256];
	DWORD sz = FormatMessageA(
		FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL, GetLastError(), 0, buf, sizeof(buf), NULL);
	fprintf(stderr, "%s: %.*s\n", msg, (int)sz, buf);
}
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <net/if.h>
typedef int SOCKET;
#define INVALID_SOCKET -1
#define closesocket close
#endif

static int load_address(const char *host, const char *service,
			struct sockaddr_storage *ss)
{
	struct addrinfo *res;
	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV | AI_PASSIVE;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = IPPROTO_UDP;

	if (getaddrinfo(host, service, &hints, &res)) {
		perror("getaddrinfo");
		return -1;
	}

	int err = -1;
	if (sizeof(*ss) >= res->ai_addrlen) {
		memcpy(ss, res->ai_addr, res->ai_addrlen);
		err = 0;
	}
	freeaddrinfo(res);
	return err;
}

static bool is_multicast(const struct sockaddr *addr)
{
	switch (addr->sa_family) {
	case AF_INET: {
		struct sockaddr_in *si = (struct sockaddr_in *)addr;
		return (ntohl(si->sin_addr.s_addr) >> 28) == 0xE;
	}
	case AF_INET6: {
		struct sockaddr_in6 *si = (struct sockaddr_in6 *)addr;
		return si->sin6_addr.s6_addr[0] == 0xFF;
	}
	default:
		return false;
	}
}

static int join_multicast(SOCKET fd, const struct sockaddr *bind_addr,
			  const struct sockaddr *send_addr)
{
	switch (send_addr->sa_family) {
	case AF_INET: {
		struct sockaddr_in *sa = (struct sockaddr_in *)send_addr;
		struct sockaddr_in *ba = (struct sockaddr_in *)bind_addr;
		struct ip_mreq mreq;
		memset(&mreq, 0, sizeof(mreq));
		mreq.imr_multiaddr = sa->sin_addr;
		mreq.imr_interface = ba->sin_addr;
		return setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
				  (char *)&mreq, sizeof(mreq));
	}
	case AF_INET6: {
		struct sockaddr_in6 *sa = (struct sockaddr_in6 *)send_addr;
		struct ipv6_mreq mreq;
		memset(&mreq, 0, sizeof(mreq));
		mreq.ipv6mr_multiaddr = sa->sin6_addr;
		mreq.ipv6mr_interface = sa->sin6_scope_id;
		return setsockopt(fd, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP,
				  (char *)&mreq, sizeof(mreq));
	}
	default:
		return -1;
	}
}

int main(int argc, char *argv[])
{
#ifdef _WIN32
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

	struct sockaddr_storage bind_addr;
	struct sockaddr_storage send_addr;

	if (argc < 4 || load_address(argv[1], argv[3], &bind_addr) ||
	    load_address(argv[2], argv[3], &send_addr) ||
	    bind_addr.ss_family != send_addr.ss_family) {
		fprintf(stderr, "failed to load address\n");
		return 2;
	}

	SOCKET fd = socket(bind_addr.ss_family, SOCK_DGRAM, IPPROTO_UDP);
	if (fd == INVALID_SOCKET) {
		perror("socket");
		return 1;
	}

	int reuse = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse,
		       sizeof(reuse))) {
		perror("reuse");
		return 1;
	}

	int bcast = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (char *)&bcast,
		       sizeof(bcast))) {
		perror("broadcast");
		return 1;
	}

#if 0
	int loopback = 1;
	if (setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, (char *)&loopback,
		       sizeof(loopback))) {
		perror("loopback");
		return 2;
	}
#endif

	if (bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr))) {
		perror("bind");
		return 2;
	}

	if (is_multicast((struct sockaddr *)&send_addr) &&
	    join_multicast(fd, (struct sockaddr *)&bind_addr,
			   (struct sockaddr *)&send_addr)) {
		perror("join group");
		return 1;
	}

	if (sendto(fd, "hello", 5, 0, (struct sockaddr *)&send_addr,
		   sizeof(send_addr)) != 5) {
		perror("send");
		return 1;
	}

	for (;;) {
		char buf[64];
		int n = recv(fd, buf, sizeof(buf), 0);
		fprintf(stderr, "recvd %d %.*s\n", n, n > 0 ? n : 0, buf);
	}
}