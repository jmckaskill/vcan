#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#pragma comment(lib, "ws2_32.lib")
static inline void perror(const char *msg)
{
	char buf[256];
	DWORD sz = FormatMessageA(
		FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL, GetLastError(), 0, buf, sizeof(buf), NULL);
	fprintf(stderr, "%s: %.*s\n", msg, (int)sz, buf);
}
typedef SOCKET fd_t;
#else
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#define closesocket(FD) close(FD)
typedef int fd_t;
#define INVALID_SOCKET -1
static const char *term_unlink_path;

static void on_sigterm(int sig)
{
	if (term_unlink_path) {
		unlink(term_unlink_path);
	}
	_exit(0);
}

static int bind_unix(fd_t *pfd, const char *path)
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

	fd_t fd = socket(AF_UNIX, SOCK_STREAM, PF_UNIX);
	if (fd < 0 || fcntl(fd, F_SETFL, O_NONBLOCK)) {
		perror("socket");
		return -1;
	}
	socklen_t sulen = (char *)&su.sun_path[len + 1] - (char *)&su;
	if (bind(fd, (struct sockaddr *)&su, sulen) || listen(fd, SOMAXCONN)) {
		perror("bind");
		return -1;
	}

	*pfd = fd;
	term_unlink_path = path;
	return 0;
}
#endif

static int bind_tcp(fd_t *pfd, const char *host, const char *port)
{
	struct addrinfo *ai, *res;
	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_flags = AI_PASSIVE;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, port, &hints, &res)) {
		perror("getaddrinfo");
		return -1;
	}

	for (ai = res; ai != NULL; ai = ai->ai_next) {
#ifdef _WIN32
		fd_t fd = WSASocket(ai->ai_family, ai->ai_socktype,
				    ai->ai_protocol, NULL, 0,
				    WSA_FLAG_OVERLAPPED);
		if (fd == INVALID_SOCKET) {
			continue;
		}
#else
		fd_t fd =
			socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0 || fcntl(fd, F_SETFL, O_NONBLOCK)) {
			continue;
		}
#endif

		if (!bind(fd, ai->ai_addr, ai->ai_addrlen) &&
		    !listen(fd, SOMAXCONN)) {
			*pfd = fd;
			return 0;
		}
		closesocket(fd);
	}

	perror("bind");
	return -1;
}

static int do_bind(fd_t *pfd, int argc, char **argv)
{
	switch (argc) {
#ifndef _WIN32
	case 2:
		return bind_unix(pfd, argv[1]);
#endif
	case 3:
		return bind_tcp(pfd, argv[1], argv[2]);
	default:
		return -1;
	}
}

struct remote {
	struct remote *next, *prev;
	fd_t fd;
	int sz;
#ifdef _WIN32
	OVERLAPPED ol;
#endif
	char buf[1024];
};

static struct remote *remotes;
static struct remote *free_list;

static struct remote *new_remote(fd_t fd)
{
	struct remote *r = malloc(sizeof(*r));
	r->fd = fd;
	r->sz = 0;
	r->next = NULL;
	r->prev = NULL;
#ifdef _WIN32
	memset(&r->ol, 0, sizeof(r->ol));
#endif
	return r;
}

static void add_remote(struct remote *r)
{
	if (remotes) {
		r->next = remotes;
		r->prev = remotes->prev;
		r->next->prev = r;
		r->prev->next = r;
	} else {
		remotes = r;
		r->next = r;
		r->prev = r;
	}
}

static void close_remote(struct remote *r)
{
	if (r->next == r) {
		remotes = NULL;
	} else {
		r->next->prev = r->prev;
		r->prev->next = r->next;
		if (remotes == r) {
			remotes = r->next;
		}
	}
	r->prev = NULL;
	r->next = free_list;
	free_list = r;
}

static void free_remotes(void)
{
	for (struct remote *r = free_list; r != NULL;) {
		struct remote *n = r->next;
		closesocket(r->fd);
		free(r);
		r = n;
	}
	free_list = NULL;
}

static int frame_bytes(struct remote *b)
{
	char *p = b->buf;
	char *e = b->buf + b->sz;
	while (p + 2 <= e) {
		uint16_t len;
		memcpy(&len, p, 2);
		if (p + 2 + len > e) {
			break;
		}
		p += 2 + len;
	}
	return p - b->buf;
}

static int nonblock_send(struct remote *t, char *buf, int n);

static void distribute_data(struct remote *r)
{
	int n = frame_bytes(r);
	if (!n) {
		return;
	}

	for (struct remote *t = r->next; t != r;) {
		struct remote *next = t->next;
		if (nonblock_send(t, r->buf, n)) {
			close_remote(t);
		}
		t = next;
	}

	if (n < r->sz) {
		memmove(r->buf, r->buf + n, r->sz - n);
	}
	r->sz -= n;
}

#ifdef _WIN32

static int nonblock_send(struct remote *t, char *buf, int n)
{
	DWORD written;
	OVERLAPPED ol;
	memset(&ol, 0, sizeof(ol));
	return !WriteFile((HANDLE)t->fd, buf, n, &written, &ol) || written != n;
}

static void read_more(struct remote *r)
{
	DWORD read;
	while (ReadFile((HANDLE)r->fd, r->buf + r->sz, sizeof(r->buf) - r->sz,
			&read, &r->ol)) {
		r->sz += read;
		distribute_data(r);
	}
	if (GetLastError() != ERROR_IO_PENDING) {
		perror("read file");
		close_remote(r);
	}
}

static struct remote *accept_more(HANDLE iocp, LPFN_ACCEPTEX acceptex,
				  SOCKET lfd)
{
	for (;;) {
		SOCKET cfd = WSASocket(AF_INET6, SOCK_STREAM, IPPROTO_TCP, NULL,
				       0, WSA_FLAG_OVERLAPPED);

		struct remote *r = new_remote(cfd);
		if (CreateIoCompletionPort((HANDLE)cfd, iocp, (ULONG_PTR)r,
					   0) == NULL) {
			perror("add to iocp");
		}
		if (!SetFileCompletionNotificationModes(
			    (HANDLE)cfd,
			    FILE_SKIP_COMPLETION_PORT_ON_SUCCESS)) {
			perror("set skip on success");
		}

		DWORD addrsz;
		BOOL ret = acceptex(lfd, cfd, r->buf, 0,
				    16 + sizeof(struct sockaddr_in6),
				    16 + sizeof(struct sockaddr_in6), &addrsz,
				    &r->ol);
		if (ret) {
			add_remote(r);
			read_more(r);
			continue;
		} else if (WSAGetLastError() == WSA_IO_PENDING) {
			return r;
		} else {
			perror("acceptex");
			exit(1);
		}
	}
}

int main(int argc, char *argv[])
{
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);

	SOCKET fd;
	if (do_bind(&fd, argc, argv)) {
		fputs("usage ./vcand host tcp-port\n", stderr);
		return 2;
	}

	HANDLE iocp = CreateIoCompletionPort((HANDLE)fd, NULL, 0, 1);
	if (iocp == INVALID_HANDLE_VALUE) {
		perror("CreateIoCompletionPort");
		return 1;
	}

	DWORD dwBytes;
	GUID GuidAcceptEx = WSAID_ACCEPTEX;
	LPFN_ACCEPTEX lpfnAcceptEx;
	int res = WSAIoctl(fd, SIO_GET_EXTENSION_FUNCTION_POINTER,
			   &GuidAcceptEx, sizeof(GuidAcceptEx), &lpfnAcceptEx,
			   sizeof(lpfnAcceptEx), &dwBytes, NULL, NULL);
	if (res == SOCKET_ERROR) {
		perror("get acceptex");
		return 1;
	}

	struct remote *next = accept_more(iocp, lpfnAcceptEx, fd);

	OVERLAPPED_ENTRY ev[16];
	DWORD num;
	while (GetQueuedCompletionStatusEx(iocp, ev, sizeof(ev) / sizeof(ev[0]),
					   &num, INFINITE, TRUE)) {
		for (int i = 0; i < num; i++) {
			struct remote *r = (void *)ev[i].lpCompletionKey;
			if (!r) {
				add_remote(next);
				read_more(next);
				next = accept_more(iocp, lpfnAcceptEx, fd);
			} else if (r->prev) {
				DWORD rcvd;
				if (GetOverlappedResult((HANDLE)r->fd, &r->ol,
							&rcvd, FALSE)) {
					r->sz += rcvd;
					distribute_data(r);
					read_more(r);
				} else {
					close_remote(r);
				}
			}
		}

		free_remotes();
	}
	return 1;
}
#else
static int nonblock_send(struct remote *t, char *buf, int n)
{
	int r;
	do {
		r = send(t->fd, buf, n, MSG_NOSIGNAL);
	} while (r < 0 && errno == EINTR);
	return r != n;
}
static void read_more(struct remote *r)
{
	int sz;
	for (;;) {
		int n = recv(r->fd, r->buf + r->sz, sizeof(r->buf) - r->sz,
			     MSG_NOSIGNAL);
		if (n < 0 && errno == EINTR) {
			continue;
		} else if (n < 0 && errno == EAGAIN) {
			break;
		} else if (n < 0) {
			perror("recv");
			close_remote(r);
			return;
		} else if (!n) {
			close_remote(r);
			return;
		}
		r->sz += n;
		distribute_data(r);
	}
}

static void accept_more(int efd, int lfd)
{
	for (;;) {
		int fd = accept(lfd, NULL, NULL);
		if (fd < 0 && errno == EINTR) {
			continue;
		} else if (fd < 0 && errno == EAGAIN) {
			break;
		} else if (fd < 0) {
			perror("accept");
			exit(1);
		}
		fcntl(fd, F_SETFL, O_NONBLOCK);
		struct remote *r = new_remote(fd);
		struct epoll_event ev = {
			.events = EPOLLIN | EPOLLET,
			.data.ptr = r,
		};
		epoll_ctl(efd, EPOLL_CTL_ADD, fd, &ev);
		add_remote(r);
		read_more(r);
	}
}
int main(int argc, char *argv[])
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = &on_sigterm;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	int lfd;
	if (do_bind(&lfd, argc, argv)) {
		fputs("usage ./vcand host tcp-port\n", stderr);
		fputs("usage ./vcand unix-socket\n", stderr);
		return 2;
	}

	int efd = epoll_create1(0);
	struct epoll_event ev = {
		.events = EPOLLIN | EPOLLET,
		.data.ptr = NULL,
	};
	if (efd < 0 || epoll_ctl(efd, EPOLL_CTL_ADD, lfd, &ev)) {
		perror("epoll");
		return 2;
	}

	accept_more(efd, lfd);

	for (;;) {
		struct epoll_event ev[16];
		int n = epoll_wait(efd, ev, sizeof(ev) / sizeof(ev[0]), -1);
		if (n < 0 && errno == EINTR) {
			continue;
		} else if (n <= 0) {
			break;
		}

		for (int i = 0; i < n; i++) {
			struct remote *r = ev[i].data.ptr;
			if (!r) {
				accept_more(efd, lfd);
			} else if (r->prev) {
				read_more(r);
			}
		}

		free_remotes();
	}
}
#endif