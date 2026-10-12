/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * net-echo.c - TCP echo server for the N32H787 HMI board.
 *
 * This is the shortest path from "the board is on the network" to "someone
 * else's program talks to mine".  It listens on a TCP port and sends every
 * byte it receives straight back, so you can drive it from a PC with nc(1)
 * and see the round trip.
 *
 * There are two ways to serve the clients, selected on the command line, and
 * they are meant to be read side by side:
 *
 *   (default)  one process, one thread, one poll() loop for every client
 *   -t         one thread per connection, blocking recv()/send() in each
 *
 * Both work on this board, and the reason one of them is not the obvious
 * "fork a process per connection" is the no-MMU kernel: fork() shares the
 * parent's address space until execve(), so a per-connection process earns
 * nothing.  Threads do not have that problem - they already share the one
 * address space on purpose - so -t is a real option, it just costs a stack
 * per connection.  See "无 MMU 环境的坑" in doc/sdk.md.
 *
 * In the default mode sockets are non-blocking, so one slow client cannot
 * stall the others: a connection with bytes still queued is only polled for
 * POLLOUT and never for POLLIN, so the peer's own socket buffer takes the
 * back-pressure instead of this process.  In -t mode a slow client only
 * blocks its own thread, which is the whole point of that mode.
 *
 * Usage:
 *   net-echo             listen on 0.0.0.0:2323
 *   net-echo 5000        listen on 0.0.0.0:5000
 *   net-echo -6          listen on IPv6 (dual-stack, also accepts IPv4)
 *   net-echo -t          one thread per connection instead of poll()
 *   net-echo -t -6 5000  any combination of the above
 *
 * Try it from the PC:
 *   nc <board-ip> 2323
 *   hello            -> the board echoes "hello" back
 *   Ctrl-D           -> close
 * Ctrl-C on the board stops the server.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DEFAULT_PORT	2323
#define MAX_CONN	16		/* concurrent clients */
#define IN_CHUNK	512		/* read() size per wake-up */
/*
 * Per-connection output backlog.  Bytes still queued here mean the peer's
 * receive window is full, and the server stops reading that connection until
 * they drain (see the poll() registration in serve_poll), so this bounds one
 * burst instead of letting a queue grow.
 */
#define OUT_CHUNK	16384
/* Numeric host and port only: 45 chars for an IPv6 literal, 5 for a port. */
#define HOST_LEN	INET6_ADDRSTRLEN
#define SERV_LEN	8
#define PEER_LEN	(HOST_LEN + SERV_LEN + 8)
/*
 * Stack for each -t worker.  It is set explicitly because the default can be
 * megabytes: multiplied by MAX_CONN that is more RAM than the board has.
 * The workers only do a recv()/send() loop, so this is generous.
 */
#define THREAD_STACK	(64 * 1024)

struct conn {
	int fd;
	char out[OUT_CHUNK];		/* bytes still to send */
	size_t out_len;			/* valid bytes in out[] */
	size_t out_off;			/* already sent */
};

static volatile sig_atomic_t stopping;

static void on_signal(int sig)
{
	(void)sig;
	stopping = 1;
}

/* Put one line on stderr, with the system error string when errno is set. */
static void complain(const char *what)
{
	if (errno)
		fprintf(stderr, "net-echo: %s: %s\n", what, strerror(errno));
	else
		fprintf(stderr, "net-echo: %s\n", what);
}

/* Printable "address:port" of a peer, for the connection log. */
static void peer_name(const struct sockaddr_storage *ss, socklen_t len,
		      char *buf, size_t size)
{
	char host[HOST_LEN], serv[SERV_LEN];

	if (getnameinfo((const struct sockaddr *)ss, len, host, sizeof(host),
			serv, sizeof(serv), NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
		snprintf(buf, size, "?");
		return;
	}
	snprintf(buf, size, "[%s]:%s", host, serv);
}

/*
 * accept() hands back a blocking socket even when the listener was not, so
 * turn it off explicitly: a peer that stops reading must never be able to
 * block the single poll() loop and stall everybody else.
 */
static int set_nonblock(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);

	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		return -1;
	return 0;
}

static void conn_close(struct conn *c)
{
	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;
	c->out_len = 0;
	c->out_off = 0;
}

/*
 * Reuse the first free slot; a connection exists exactly while fd >= 0.
 * Returns NULL only when all MAX_CONN slots are busy.
 */
static struct conn *conn_alloc(struct conn *conns, int nconns)
{
	int i;

	for (i = 0; i < nconns; i++)
		if (conns[i].fd < 0)
			return &conns[i];
	return NULL;
}

/* Flush as much of conns[i].out[] as the socket accepts right now. */
static int conn_flush(struct conn *c)
{
	while (c->out_off < c->out_len) {
		ssize_t n = send(c->fd, c->out + c->out_off,
				 c->out_len - c->out_off, MSG_NOSIGNAL);

		if (n > 0) {
			c->out_off += (size_t)n;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return 0;	/* socket full, come back later */
		if (n < 0 && errno == EINTR)
			continue;
		return -1;		/* real error, or the peer is gone */
	}
	c->out_len = c->out_off = 0;
	return 0;
}

/*
 * Handle one readable connection: read a chunk, queue it for echo, send as
 * much of the queue as fits right now.
 *
 * This is only called while the queue is empty: the poll() registration below
 * keeps asking for POLLIN only when there is nothing queued.  A peer that
 * stops reading therefore stops being read from as well, and its own send()
 * is what blocks - TCP back-pressure, not unbounded buffering here and not a
 * dropped connection either.
 */
static int conn_drain_input(struct conn *c)
{
	char buf[IN_CHUNK];
	ssize_t n;

	if (c->out_len != c->out_off)
		return 0;		/* queue not drained; never read now */

	n = recv(c->fd, buf, sizeof(buf), 0);
	if (n == 0)
		return -1;			/* peer closed cleanly */
	if (n < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			return 0;
		return -1;
	}
	memcpy(c->out, buf, (size_t)n);
	c->out_off = 0;
	c->out_len = (size_t)n;
	return conn_flush(c);
}

/*
 * Create the listening socket.  family picks AF_INET or AF_INET6; the IPv6
 * socket disables IPV6_V6ONLY so a single listener takes IPv4-mapped clients
 * too (one fd for the whole dual-stack port).
 */
static int make_listener(int family, const char *port, char *where, size_t wlen)
{
	struct addrinfo hints, *res, *ai;
	int fd = -1, on = 1, off = 0;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = family;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	if (getaddrinfo(NULL, port, &hints, &res) != 0) {
		complain("getaddrinfo");
		return -1;
	}

	for (ai = res; ai != NULL; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
		if (ai->ai_family == AF_INET6)
			setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off,
				   sizeof(off));
		if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 &&
		    listen(fd, 8) == 0) {
			char host[HOST_LEN], serv[SERV_LEN];

			if (getnameinfo(ai->ai_addr, ai->ai_addrlen, host,
					sizeof(host), serv, sizeof(serv),
					NI_NUMERICHOST | NI_NUMERICSERV) == 0)
				snprintf(where, wlen, "[%s]:%s", host, serv);
			else
				snprintf(where, wlen, "port %s", port);
			break;
		}
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);

	if (fd < 0)
		complain("can't bind a listening socket");
	return fd;
}

/*
 * --- thread-per-connection mode (-t) ---------------------------------------
 *
 * This is the other half of the comparison in the file header.  Threads share
 * the one address space the kernel gives every process, so one per connection
 * is legitimate here; a thread blocked in recv() only ever blocks itself.
 *
 * The threading is deliberately minimal: detached workers, one mutex for the
 * counter and one for the log, and zero shared connection state.  Each worker
 * owns its socket and frees itself, so there is nothing to join.
 */
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t count_lock = PTHREAD_MUTEX_INITIALIZER;
static int active_workers;

/* One whole log line per call; stdio itself is not thread safe. */
static void log_line(const char *fmt, ...)
{
	va_list ap;

	pthread_mutex_lock(&log_lock);
	va_start(ap, fmt);
	vfprintf(stdout, fmt, ap);
	va_end(ap);
	fflush(stdout);
	pthread_mutex_unlock(&log_lock);
}

/* Blocking send() of a whole buffer; returns 0 on success. */
static int send_all(int fd, const char *buf, size_t len)
{
	while (len > 0) {
		ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);

		if (n > 0) {
			buf += n;
			len -= (size_t)n;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		return -1;
	}
	return 0;
}

struct worker {
	int fd;
	char peer[PEER_LEN];
};

static void *echo_worker(void *arg)
{
	struct worker *w = arg;
	char buf[IN_CHUNK];

	for (;;) {
		struct pollfd p = { .fd = w->fd, .events = POLLIN };
		ssize_t n;
		int r;

		/*
		 * A timeout, instead of a plain blocking recv(): this way the
		 * worker notices Ctrl-C within one tick even while it is idle
		 * waiting for a peer that never sends anything.
		 */
		r = poll(&p, 1, 200);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (r == 0) {
			if (stopping)
				break;
			continue;
		}
		if (p.revents & (POLLERR | POLLHUP | POLLNVAL))
			break;
		if (!(p.revents & POLLIN))
			continue;

		n = recv(w->fd, buf, sizeof(buf), 0);
		if (n == 0)
			break;			/* peer closed cleanly */
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN ||
			    errno == EWOULDBLOCK)
				continue;
			break;
		}
		if (send_all(w->fd, buf, (size_t)n) != 0)
			break;			/* peer went away mid-echo */
	}

	log_line("net-echo: - %s\n", w->peer);
	close(w->fd);
	free(w);

	pthread_mutex_lock(&count_lock);
	active_workers--;
	pthread_mutex_unlock(&count_lock);
	return NULL;
}

static void worker_release(void)
{
	pthread_mutex_lock(&count_lock);
	active_workers--;
	pthread_mutex_unlock(&count_lock);
}

static int serve_threads(int listen_fd)
{
	pthread_attr_t attr;
	int have_attr = 0;

	if (pthread_attr_init(&attr) == 0) {
		pthread_attr_setstacksize(&attr, THREAD_STACK);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		have_attr = 1;
	}

	while (!stopping) {
		struct sockaddr_storage ss;
		socklen_t slen = sizeof(ss);
		struct worker *w;
		pthread_t tid;
		int client = accept(listen_fd, (struct sockaddr *)&ss, &slen);

		if (client < 0) {
			if (errno == EINTR || errno == EAGAIN ||
			    errno == EWOULDBLOCK)
				continue;
			complain("accept");
			break;
		}

		pthread_mutex_lock(&count_lock);
		if (active_workers >= MAX_CONN) {
			pthread_mutex_unlock(&count_lock);
			log_line("net-echo: too many clients, refusing\n");
			close(client);
			continue;
		}
		active_workers++;
		pthread_mutex_unlock(&count_lock);

		w = malloc(sizeof(*w));
		if (w == NULL) {
			complain("out of memory for a connection");
			close(client);
			worker_release();
			continue;
		}
		w->fd = client;
		peer_name(&ss, slen, w->peer, sizeof(w->peer));

		if (pthread_create(&tid, have_attr ? &attr : NULL,
				   echo_worker, w) != 0) {
			complain("pthread_create");
			close(client);
			free(w);
			worker_release();
			continue;
		}
		log_line("net-echo: + %s\n", w->peer);
	}

	if (have_attr)
		pthread_attr_destroy(&attr);

	/* Wait for the workers to see `stopping` and close their sockets. */
	for (;;) {
		int active;

		pthread_mutex_lock(&count_lock);
		active = active_workers;
		pthread_mutex_unlock(&count_lock);
		if (active == 0)
			break;
		usleep(20000);
	}
	return 0;
}

/*
 * --- poll() mode (default) -------------------------------------------------
 *
 * One loop for every client.  Nothing here blocks: the listening socket and
 * every connection are non-blocking, poll() reports which ones are ready, and
 * a connection is only asked to write while it still has bytes to flush.
 */
static int serve_poll(int listen_fd)
{
	/*
	 * static, not a local: with OUT_CHUNK per connection the array is a
	 * quarter of a megabyte and has no business on a stack.
	 * serve_poll() has a single instance, so one copy is all there is.
	 */
	static struct conn conns[MAX_CONN];
	int i;

	for (i = 0; i < MAX_CONN; i++)
		conns[i].fd = -1;

	while (!stopping) {
		struct pollfd pfd[MAX_CONN + 1];
		int nconns = 0, n, fd_to_conn[MAX_CONN + 1];
		struct conn *slot;

		/* Slot 0 is the listener, then one entry per live client. */
		pfd[0].fd = listen_fd;
		pfd[0].events = POLLIN;
		fd_to_conn[0] = -1;

		for (i = 0; i < MAX_CONN; i++) {
			if (conns[i].fd < 0)
				continue;
			pfd[++nconns].fd = conns[i].fd;
			/*
			 * Exactly one of the two: ask to flush while there is a
			 * backlog, and ask for input only once that backlog is
			 * gone.  Registering both would let a peer that never
			 * reads keep pushing data into this process.
			 */
			if (conns[i].out_len != conns[i].out_off)
				pfd[nconns].events = POLLOUT;
			else
				pfd[nconns].events = POLLIN;
			fd_to_conn[nconns] = i;
		}

		n = poll(pfd, (nfds_t)(nconns + 1), -1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			complain("poll");
			break;
		}

		if (pfd[0].revents & POLLIN) {
			struct sockaddr_storage ss;
			socklen_t slen = sizeof(ss);
			char peer[PEER_LEN];
			int client = accept(listen_fd,
					    (struct sockaddr *)&ss, &slen);

			if (client < 0) {
				if (errno != EAGAIN && errno != EWOULDBLOCK &&
				    errno != EINTR)
					complain("accept");
			} else {
				slot = conn_alloc(conns, MAX_CONN);
				if (slot == NULL) {
					fprintf(stderr, "net-echo: too many "
						"clients, refusing\n");
					close(client);
				} else if (set_nonblock(client) != 0) {
					complain("can't set the connection "
						 "non-blocking");
					close(client);
				} else {
					slot->fd = client;
					slot->out_len = slot->out_off = 0;
					peer_name(&ss, slen, peer, sizeof(peer));
					printf("net-echo: + %s\n", peer);
					fflush(stdout);
				}
			}
		}

		for (i = 1; i <= nconns; i++) {
			short re = pfd[i].revents;
			struct conn *c = &conns[fd_to_conn[i]];
			int drop = 0;

			if (re & (POLLERR | POLLHUP | POLLNVAL))
				drop = 1;
			else if (re & POLLOUT && conn_flush(c) < 0)
				drop = 1;
			else if (re & POLLIN && conn_drain_input(c) < 0)
				drop = 1;

			if (drop) {
				char peer[PEER_LEN] = "?";
				struct sockaddr_storage ss;
				socklen_t slen = sizeof(ss);

				if (getpeername(c->fd, (struct sockaddr *)&ss,
						&slen) == 0)
					peer_name(&ss, slen, peer, sizeof(peer));
				printf("net-echo: - %s\n", peer);
				fflush(stdout);
				conn_close(c);
			}
		}
	}

	for (i = 0; i < MAX_CONN; i++)
		conn_close(&conns[i]);
	return 0;
}

int main(int argc, char **argv)
{
	struct sigaction sa;
	const char *port = NULL;
	char where[PEER_LEN];
	int family = AF_INET;
	int use_threads = 0;
	int listen_fd, arg;

	for (arg = 1; arg < argc; arg++) {
		if (strcmp(argv[arg], "-6") == 0)
			family = AF_INET6;
		else if (strcmp(argv[arg], "-4") == 0)
			family = AF_INET;
		else if (strcmp(argv[arg], "-t") == 0)
			use_threads = 1;
		else if (strcmp(argv[arg], "-h") == 0 ||
			 strcmp(argv[arg], "--help") == 0) {
			printf("usage: %s [-4|-6] [-t] [port]\n", argv[0]);
			printf("  -t   one thread per connection "
			       "instead of one poll() loop\n");
			return 0;
		} else if (argv[arg][0] == '-') {
			fprintf(stderr, "net-echo: unknown option %s\n",
				argv[arg]);
			return 2;
		} else {
			port = argv[arg];
		}
	}
	if (port == NULL)
		port = "2323";

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	/* A peer that goes away mid-write must not kill the server. */
	signal(SIGPIPE, SIG_IGN);

	listen_fd = make_listener(family, port, where, sizeof(where));
	if (listen_fd < 0)
		return 1;

	printf("net-echo: listening on %s, %s, up to %d clients "
	       "(Ctrl-C to stop)\n", where,
	       use_threads ? "one thread per connection" : "one poll() loop",
	       MAX_CONN);
	fflush(stdout);

	if (use_threads)
		serve_threads(listen_fd);
	else
		serve_poll(listen_fd);

	close(listen_fd);
	printf("net-echo: stopped\n");
	return 0;
}
