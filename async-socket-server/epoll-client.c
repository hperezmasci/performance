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

#define SVRPORT 9090        // server port
#define SVRADDR "127.0.0.1" // server ip address
#define MSGSIZE 512         // menssage size
#define MAXCONC 3         // max concurrency

/*
    XXX FIXME:
    extra room in global context (filedescriptors are not always assigned
    sequentially), should use another kind of structure?
    (hashes? malloc and realloc?)
*/
#define GCEXTRA 100

typedef enum
{
    WAIT_ACK,
    SEND,
    RECV,
    END
} state_t;

typedef struct
{
    state_t state;
    uint8_t sendbuf[MSGSIZE];
    int sendbuf_end;
    int sendptr;
} context_t;

// problema: los file descriptors no se asignan siempre de forma secuencial
// lo que hace que add_connection pinche cuando está llegando al máximo de FDs
// dado que no alcanza el espacio. Resolución quick & dirty: agregando
// espacio en global_context.
context_t global_context[MAXCONC + GCEXTRA];

void add_connection(int epollfd, struct sockaddr *saddr)
{
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1)
        die("Error on socket: %s\n", strerror(errno));

    make_socket_non_blocking(sockfd);

    if (connect(sockfd, saddr, sizeof(*saddr)) &&
        errno != EINPROGRESS)
        die("Error connecting: %s\n", strerror(errno));

    // set socket for input events
    struct epoll_event event = {
        .data.fd = sockfd,
        .events = EPOLLIN};

    // add socket and expected event to epoll context (epollfd)
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, sockfd, &event) < 0)
        die("error with epoll_ctl: %s", strerror(errno));

    // initialize context
    context_t *context = &global_context[sockfd];
    context->state = WAIT_ACK;
}

void
chg_epoll_mode(int epollfd, int sockfd, uint32_t mode)
{
    // set socket for output events
    struct epoll_event event = {
        .data.fd = sockfd,
        .events = mode};
    // add socket and expected event to epoll context (epollfd)
    if (epoll_ctl(epollfd, EPOLL_CTL_MOD, sockfd, &event) < 0)
        die("Error: epoll_ctl: %s", strerror(errno));
}

state_t
do_recv(int sockfd)
{
    /*
        XXX TODO:
        Hay que ver el global_context[fd].
        Si está WAIT_ACK, recibir el '*' (y no debería haber nada mas)
        y pasar a SEND
        Si está en RECV, recibir. Si terminó, pasar a END
        En una primera versión podemos soportar un mensaje por conexión

        No puede estar en otro estado.
    */
    char buf[MSGSIZE];

    context_t *context = &global_context[sockfd];
    switch (context->state)
    {
    case WAIT_ACK:
        int nbytes = recv(sockfd, buf, sizeof buf, 0);

        if (nbytes == 1 && buf[0] == '*') {
            // happy path
            printf("DEBUG: do_recv: received ACK, now in SEND state\n");
            context->state = SEND;
            break;
        }

        if (nbytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // The socket is not *really* ready for recv; wait until it is.
            printf("INFO: do_recv: EAGAIN | EWOULDBLOCK\n");
            break;
        }

        // handle error 
        if (nbytes < 0)
            printf("WARN: do_recv: %s", strerror(errno));
        else if (nbytes == 0)
            printf("WARN: do_recv: server closed connection unexpectedly\n");
        else
            printf("WARN: do_recv: server sent unexpected message other than ACK\n");
        context->state = END;
        break;

    case RECV:

        break;

    default:
        die("Error: do_recv: unexpected state %d", context->state);
    }
    return context->state;
}

int main(int argc, const char **argv)
{

    int portnum = SVRPORT;
    char addr[16] = SVRADDR;
    struct sockaddr_in serv_addr;
    // char buf[BUFSIZE];

    if (argc >= 2)
    {
        strncpy(addr, argv[1], 15);
    }
    if (argc >= 3)
    {
        portnum = atoi(argv[2]);
    }

    // prepare server endpoint structure
    bzero(&serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, addr, &serv_addr.sin_addr) <= 0)
    {
        die("Invalid address / Address not supported %s", addr);
    }
    serv_addr.sin_port = htons(portnum);

    int epollfd = epoll_create1(0);
    if (epollfd < 0)
        die("Error: epoll_create1: %s", strerror(errno));

    // array to hold events for epoll_wait
    struct epoll_event *events = calloc(MAXCONC, sizeof(struct epoll_event));
    if (events == NULL)
        die("Error: calloc: %s", strerror(errno));

    // establish MAXCONC connections
    for (int i = 0; i < MAXCONC; i++)
    {
        add_connection(epollfd, (struct sockaddr *)&serv_addr);
        if (!(i % 1000))
            printf("Connection %d open\n", i);
    }
    printf("All %d connections are open\n", MAXCONC);

    while (1)
    {
        int nready = epoll_wait(epollfd, events, MAXCONC, -1);
        if (nready == -1)
            die("Error: epoll_wait: %s\n", strerror(errno));

        for (int i = 0; i < nready; i++)
        {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;
            if (ev & EPOLLIN)
            {
                // ready for receiving
                state_t state = do_recv(fd);

                if (state == SEND)
                    chg_epoll_mode(epollfd, fd, EPOLLOUT);
                else if (state == END) {
                    /*
                        Si luego de la rutina de recepción pasó a END,
                        cerrar conexión y conectar nuevamente (cicla conexiones)
                    */
                }

            }
            else if (ev & EPOLLOUT)
            {
                // ready for sending
                /*
                    XXX TODO:
                    Llamar a rutina de envío de mensaje:
                     Hay que ver el global_context[fd]
                     Si está SEND, enviar (si aún hay data para enviar)
                     Si terminó de enviar, pasar a RECV.

                     No puede estar en otro estado.

                    Si luego de la rutina de recepción pasó a RECV,
                     modificar en epoll para que espere EPOLLIN
                */
               printf("DEBUG: ready for sending\n");
            }
            else
            {
                die("Error: unexpected event type, ev: %d\n", ev);
            }
        } // for nready
    }     // event loop
}
