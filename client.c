#include "can.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
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
#include <netdb.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>
static int connect_unix(int *pfd, const char *path)
{
	struct sockaddr_un su;
	memset(&su, 0, sizeof(su));
	su.sun_family = AF_UNIX;

	size_t len = strlen(path);
	if (len + 1 > sizeof(su.sun_path)) {
		fprintf(stderr, "path %s is too long\n", path);
		return -1;
	}

	memcpy(su.sun_path, path, len + 1);

	int fd = socket(AF_UNIX, SOCK_STREAM, PF_UNIX);
	if (fd < 0) {
		perror("socket");
		return -1;
	}
	socklen_t sulen = (char *)&su.sun_path[len + 1] - (char *)&su;
	if (connect(fd, (struct sockaddr *)&su, sulen)) {
		close(fd);
		perror("connect");
		return -1;
	}

	*pfd = fd;
	return 0;
}
#endif

static int connect_tcp(int *pfd, const char *host, const char *port)
{
	struct addrinfo *ai, *res;
	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, port, &hints, &res)) {
		perror("getaddrinfo");
		return -1;
	}
	int err = 0;

	for (ai = res; ai != NULL; ai = ai->ai_next) {
		int fd = (int)socket(ai->ai_family, ai->ai_socktype,
				     ai->ai_protocol);
		if (fd < 0) {
			continue;
		}

		if (connect(fd, ai->ai_addr, ai->ai_addrlen)) {
#ifdef _WIN32
			DWORD err = GetLastError();
			closesocket(fd);
			SetLastError(err);
#else
			int err = errno;
			close(fd);
			errno = err;
#endif
			continue;
		}
		*pfd = fd;
		return 0;
	}

	perror("connect");
	return -1;
}

static int do_connect(int *pfd, int argc, char **argv)
{
	switch (argc) {
#ifndef _WIN32
	case 2:
		return connect_unix(pfd, argv[1]);
#endif
	case 3:
		return connect_tcp(pfd, argv[1], argv[2]);
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

	int fd;
	if (do_connect(&fd, argc, argv)) {
#ifndef _WIN32
		fputs("usage: client unix-socket\n", stderr);
#endif
		fputs("usage: client host tcp-port\n", stderr);
		return 2;
	}

	struct canfd_frame f;
	f.can_id = CAN_EFF_FLAG | 0x18EEFFEC;
	f.len = 4;
	f.data[0] = 1;
	f.data[1] = 2;
	f.data[2] = 3;
	f.data[3] = 4;
	uint16_t len = sizeof(f);
	send(fd, (char *)&len, 2, 0);
	send(fd, (char *)&f, sizeof(f), 0);

	char buf[1024];
	int off = 0;
	int have = 0;

	for (;;) {
		memmove(buf, buf + off, have - off);
		have -= off;
		off = 0;

		int sz = recv(fd, buf + have, sizeof(buf) - have, 0);
		if (sz == 0) {
			fprintf(stderr, "RX EOF\n");
			return 0;
#ifndef _WIN32
		} else if (sz < 0 && errno == EINTR) {
			continue;
#endif
		} else if (sz < 0) {
			perror("recv");
			return 2;
		}
		have += sz;

		while (off + 2 <= have) {
			uint16_t len;
			memcpy(&len, buf + off, 2);
			if (off + 2 + len > have) {
				break;
			}
			char *data = buf + off + 2;
			off += 2 + len;
			struct canfd_frame f;
			if (len > sizeof(f)) {
				continue;
			}
			memcpy(&f, data, len);
			fprintf(stderr, "RX 0x%08X", f.can_id);
			for (int i = 0; i < f.len; i++) {
				fprintf(stderr, " %02X", f.data[i]);
			}
			fputs("\n", stderr);
		}
	}
}