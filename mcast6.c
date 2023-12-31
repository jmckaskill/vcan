#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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
#endif

int main(int argc, char *argv[])
{
#ifdef _WIN32
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

	int port = htons(atoi(argv[2]));
	struct in6_addr addr;
	if (inet_pton(AF_INET6, argv[1], &addr) <= 0) {
		fprintf(stderr, "failed to parse %s as an IP\n", argv[1]);
		return 2;
	}

	int fd = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);

	int reuse = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse,
		       sizeof(reuse))) {
		perror("reuseaddr");
		return 2;
	}

#if 0
	int loopback = 1;
	if (setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, (char *)&loopback,
		       sizeof(loopback))) {
		perror("loopback");
		return 2;
	}
#endif

	struct ipv6_mreq mreq;
	memset(&mreq, 0, sizeof(mreq));
	mreq.ipv6mr_multiaddr = addr;
	// mreq.ipv6mr_interface = 1;
	setsockopt(fd, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP, (char *)&mreq,
		   sizeof(mreq));

	struct sockaddr_in6 si;
	memset(&si, 0, sizeof(si));
	si.sin6_family = AF_INET6;
	si.sin6_port = port;
	si.sin6_addr = in6addr_any;
	if (bind(fd, (struct sockaddr *)&si, sizeof(si))) {
		perror("bind");
		return 2;
	}

	struct sockaddr_in6 to;
	memset(&to, 0, sizeof(to));
	to.sin6_family = AF_INET6;
	to.sin6_port = port;
	to.sin6_addr = addr;
	// to.sin6_scope_id = if_nametoindex("lo");
	sendto(fd, "hello", 5, 0, (struct sockaddr *)&to, sizeof(to));

	for (;;) {
		char buf[64];
		int n = recv(fd, buf, sizeof(buf), 0);
		fprintf(stderr, "recvd %d %.*s\n", n, n > 0 ? n : 0, buf);
	}

	return 0;
}
