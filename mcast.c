#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
	int port = htons(atoi(argv[2]));
	struct in_addr addr;
	if (inet_pton(AF_INET, argv[1], &addr) <= 0) {
		fprintf(stderr, "failed to parse %s as an IP\n", argv[1]);
		return 2;
	}

	int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	int reuse = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse))) {
		perror("reuseaddr");
		return 2;
	}

#if 0
	int loopback = 1;
	if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loopback,
		       sizeof(loopback))) {
		perror("loopback");
		return 2;
	}
#endif

#if 1
	struct in_addr iface;
	iface.s_addr = htonl(INADDR_LOOPBACK);
	if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &iface,
		       sizeof(iface))) {
		perror("iface");
		return 2;
	}
#endif

	struct ip_mreq mreq;
	memset(&mreq, 0, sizeof(mreq));
	mreq.imr_multiaddr = addr;
	mreq.imr_interface.s_addr = ntohl(INADDR_LOOPBACK);
	setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

	struct sockaddr_in si;
	memset(&si, 0, sizeof(si));
	si.sin_family = AF_INET;
	si.sin_port = port;
	si.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(fd, (struct sockaddr *)&si, sizeof(si))) {
		perror("bind");
		return 2;
	}

	struct sockaddr_in to;
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = port;
	to.sin_addr = addr;
	sendto(fd, "hello", 5, 0, (struct sockaddr *)&to, sizeof(to));

	for (;;) {
		char buf[64];
		int n = recv(fd, buf, sizeof(buf), 0);
		fprintf(stderr, "recvd %d %.*s\n", n, n > 0 ? n : 0, buf);
	}

	return 0;
}
