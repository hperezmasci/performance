#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>
#include <sys/epoll.h>
#include <time.h>

#include "utils.h"

#define LOGLEVEL INFO		// log from this level
#define SVRPORT 9090		// server port
#define SVRADDR "127.0.0.1"	// server ip address
#define MSGSIZE 512		// menssage size
#define MAXCONC 3500		// max concurrency
#define MAXCONN 1000000		// max number of connections (0 => no limit)
#define STATSFRQ 10000		// frequency to log stats

/*
	XXX FIXME:
	extra room in global context (filedescriptors are not always assigned
	sequentially), should use another kind of structure?
	(hashes? malloc and realloc?)
*/
#define GCEXTRA 100

typedef enum {
	WAIT_ACK,
	SEND,
	RECV,
	END
} state_t;

typedef struct {
	state_t state;
	char sendbuf[MSGSIZE];
	int sendbuf_end;
	int sendptr;
} context_t;

struct {
	int conn;
	int recv;
	int sent;
} stats;

// problema: los file descriptors no se asignan siempre de forma secuencial
// lo que hace que add_connection pinche cuando está llegando al máximo de FDs
// dado que no alcanza el espacio. Resolución quick & dirty: agregando
// espacio en global_context.
context_t
global_context[MAXCONC + GCEXTRA];


void
del_connection(int epollfd, int sockfd)
{
	if (epoll_ctl(epollfd, EPOLL_CTL_DEL, sockfd, NULL) < 0)
		logdie("del_connection(%d): epoll_ctl: %s", strerror(errno));
	if (close(sockfd) == -1)
		logdie("del_connection(%d): close: %s", strerror(errno));
}

int
add_connection(int epollfd, struct sockaddr *saddr)
{
	int sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd == -1)
		logdie("add_connection: socket: %s", strerror(errno));

	make_socket_non_blocking(sockfd);

	if (connect(sockfd, saddr, sizeof(*saddr)) && errno != EINPROGRESS)
		logdie("add_connection: connect: %s", strerror(errno));

	// set socket for input events
	struct epoll_event event = {
		.data.fd = sockfd,
		.events = EPOLLIN
	};

	// add socket and expected event to epoll context (epollfd)
	if (epoll_ctl(epollfd, EPOLL_CTL_ADD, sockfd, &event) < 0)
		logdie("add_connection: epoll_ctl: %s", strerror(errno));

	// initialize context
	context_t *ctx = &global_context[sockfd];
	ctx->state = WAIT_ACK;
	ctx->sendptr = 0;
	ctx->sendbuf_end = 0;

	stats.conn++;

	if (!(stats.conn % STATSFRQ)) {
		logger(INFO, "connects: %d", stats.conn);
		logger(INFO, "msg sent: %d", stats.sent);
		logger(INFO, "msg recv: %d", stats.recv);
	}

	return sockfd;
}

void
chg_epoll_mode(int epollfd, int sockfd, uint32_t mode)
{
	// set socket for output events
	struct epoll_event event = {
		.data.fd = sockfd,
		.events = mode
	};
	// add socket and expected event to epoll context (epollfd)
	if (epoll_ctl(epollfd, EPOLL_CTL_MOD, sockfd, &event) < 0)
		logdie("epoll_ctl: %s", strerror(errno));
}

state_t
do_recv(int fd)
{
	/*
		Revisa global_context[fd].
		Si está WAIT_ACK, recibir el '*' (y no debería haber nada mas)
		y pasar a SEND
		Si está en RECV, recibir. Si terminó, pasar a END
		En una primera versión podemos soportar un mensaje por conexión

		No puede estar en otro estado.
	*/
	char buf[MSGSIZE];
	int nbytes;

	context_t *ctx = &global_context[fd];
	switch (ctx->state) {
	case WAIT_ACK:
		nbytes = recv(fd, buf, sizeof buf, 0);

		if (nbytes == 1 && buf[0] == '*') {
			// happy path
			logger(DEBUG, "do_recv(%d): ACK -> SEND", fd);
			ctx->state = SEND;
			break;
		}

		if (nbytes == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			// The socket is not *really* ready for recv; wait until it is.
			logger(INFO, "do_recv(%d): EAGAIN | EWOULDBLOCK", fd);
			break;
		}

		// handle error 
		if (nbytes == -1)
			logger(WARN, "do_recv(%d): %s", fd, strerror(errno));
		else if (nbytes == 0)
			logger(WARN, "do_recv(%d): server closed connection unexpectedly", fd);
		else
			logger(WARN, "do_recv(%d): server sent unexpected message", fd);
		
		ctx->state = END;
		break;

	case RECV:
		nbytes = recv(fd, buf, sizeof buf, 0);

		if (nbytes > 0) {
			// happy path
			//*** TODO: multiple receive for a single msg not supported yet
			logger(DEBUG, "do_recv(%d): msg received -> END", fd);
			ctx->state = END;
			stats.recv++;
			break;
		}

		if (nbytes == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			// The socket is not *really* ready for recv; wait until it is.
			logger(INFO, "do_recv(%d): EAGAIN | EWOULDBLOCK", fd);
			break;
		}

		// handle error 
		if (nbytes == -1)
			logger(WARN, "do_recv: %s", strerror(errno));
		else if (nbytes == 0)
			logger(WARN, "do_recv(%d): server closed connection unexpectedly", fd);

		ctx->state = END;
		break;

	default:
		logdie("do_recv(%d): unexpected state %d", fd, ctx->state);
	}
	return ctx->state;
}

state_t
do_send(int fd)
{
	/*
		Revisar global_context[fd]
		Si está SEND, enviar (si aún hay data para enviar)
		Si terminó de enviar, pasar a RECV.

		No puede estar en otro estado.
	*/

	context_t *ctx = &global_context[fd];
	if (ctx->state != SEND)
		logdie("do_send(%d): forbidden state %d", fd, ctx->state);

	// XXX TODO: por ahora mando siempre lo mismo
	char *msg = "^mensaje$";
	strncpy(ctx->sendbuf, msg, sizeof ctx->sendbuf);
	ctx->sendptr = 0;
	ctx->sendbuf_end = strlen(msg);

	size_t len = ctx->sendbuf_end - ctx->sendptr;
	int nbytes = send(fd, &ctx->sendbuf[ctx->sendptr], len, 0);

	if (nbytes == len) {
		logger(DEBUG, "do_send(%d): message sent", fd);
		stats.sent++;
		ctx->state = RECV;
	}
	else if (nbytes > 0) {
		logger(DEBUG, "do_send(%d): sent %d bytes", fd, nbytes);
		// XXX TODO: para esto necesito implementar bien el multi-envío x mensaje
		logger(WARN, "do_send(%d): multiple sends per message not supported yet", fd);
		ctx->state = END;
	}
	else {
		// nbytes == -1
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			logger(INFO, "do_send(%d): EAGAIN | EWOULDBLOCK", fd);
		else {
			logger(WARN, "do_send(%d): %s", fd, strerror(errno));
			ctx->state = END;
		}
	}

	return ctx->state;
}

int main(int argc, const char **argv)
{

	int portnum = SVRPORT;
	char addr[16] = SVRADDR;
	struct sockaddr_in serv_addr;

	int maxconn = MAXCONN;

	stats.conn = 0;
	stats.recv = 0;
	stats.sent = 0;

	set_loglevel(LOGLEVEL);

	if (argc >= 2) {
		strncpy(addr, argv[1], 15);
	}
	if (argc >= 3) {
		portnum = atoi(argv[2]);
	}

	// prepare server endpoint structure
	bzero(&serv_addr, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	if (inet_pton(AF_INET, addr, &serv_addr.sin_addr) <= 0) {
		logdie("invalid address / Address not supported %s", addr);
	}
	serv_addr.sin_port = htons(portnum);

	int epollfd = epoll_create1(0);
	if (epollfd < 0)
		logdie("epoll_create1: %s", strerror(errno));

	// array to hold events for epoll_wait
	struct epoll_event *events = calloc(MAXCONC, sizeof(struct epoll_event));
	if (events == NULL)
		logdie("calloc: %s", strerror(errno));

	// establish MAXCONC connections
	for (int i = 0; i < MAXCONC; i++) {
		int fd = add_connection(epollfd, (struct sockaddr *)&serv_addr);
		if (!(i % 1000))
			logger(DEBUG, "connection %d open on %d", i, fd);
	}

	logger(INFO, "%ld - all %d connections are open", time(NULL), MAXCONC);

	while (maxconn != 0 ? stats.conn < maxconn : 1) {
		int nready = epoll_wait(epollfd, events, MAXCONC, -1);
		if (nready == -1)
			logdie("epoll_wait: %s", strerror(errno));

		for (int i = 0; i < nready; i++) {
			int fd = events[i].data.fd;
			uint32_t ev = events[i].events;
			if (ev & EPOLLIN) {
				// ready for receiving
				logger(DEBUG, "ready for rceiving on %d", fd);

				state_t state = do_recv(fd);

				if (state == SEND)
					chg_epoll_mode(epollfd, fd, EPOLLOUT);
				else if (state == END) {
					// cierro conexión y conecto nuevamente
					del_connection(epollfd, fd);
					int oldfd = fd;
					logger(DEBUG, "connection closed on %d", fd);
					fd = add_connection(epollfd, (struct sockaddr *)&serv_addr);
					logger(DEBUG, "connection re-opened on %d", fd);
					if (oldfd != fd)
						logger(WARN, "old fd %d differs from new fd %d", oldfd, fd);

				}

			}
			else if (ev & EPOLLOUT) {
				// ready for sending
				logger(DEBUG, "ready for sending on %d", fd);

				state_t state = do_send(fd);

				if (state == RECV)
					chg_epoll_mode(epollfd, fd, EPOLLIN);
				else if (state == END) {
					// cierro conexión y conecto nuevamente
					del_connection(epollfd, fd);
					logger(DEBUG, "connection closed on %d", fd);
					fd = add_connection(epollfd, (struct sockaddr *)&serv_addr);
					logger(DEBUG, "connection re-opened on %d", fd);
				}
			}
			else logdie( "unexpected event type on %d, ev: %d", fd, ev);
		} // for nready
	}	 // event loop
}
