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
#define MAXCONC 100         // max concurrency

/*
    XXX FIXME:
    extra room in global context (filedescriptors are not always assigned
    sequencially), should use another kind of structure?
    (hashes? malloc and realloc?)
*/
#define GCEXTRA 100

typedef enum { WAIT_ACK, SEND, RECV, END } state_t;

#define SENDBUF_SIZE 1024

typedef struct {
    state_t state;
    uint8_t sendbuf[SENDBUF_SIZE];
    int sendbuf_end;
    int sendptr;
} context_t;

// problema: los file descriptors no se asignan siempre de forma secuencial
// lo que hace que add_connection pinche cuando está llegando al máximo de FDs
// dado que no alcanza el espacio. No sé si lo resuelvo "fácil" agregando
// espacio en global_context, por ahí hay que acudir a otra estructura (un hash)
context_t global_context[MAXCONC+GCEXTRA];

void
add_connection(int epollfd, struct sockaddr* saddr)
{
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1)
        die ("Error on socket: %s\n", strerror(errno));
    
    make_socket_non_blocking(sockfd);
    
    if (connect(sockfd, saddr, sizeof(*saddr)) &&
        errno != EINPROGRESS)
            die("Error connecting: %s\n", strerror(errno));
    
        // set socket for input events
    struct epoll_event event = {
        .data.fd = sockfd,
        .events = EPOLLIN
    };

    // add socket and expected event to epoll context (epollfd)
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, sockfd, &event) < 0)
        die("error with epoll_ctl: %s", strerror(errno));
    
    // initialize context
    context_t* context = &global_context[sockfd];
    context->state = WAIT_ACK;
}

int
main(int argc, const char** argv)
{

    int portnum = SVRPORT;
    char addr[16] = SVRADDR;
    struct sockaddr_in serv_addr;
    //char buf[BUFSIZE];

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
        die("Invalid address / Address not supported %s", addr);
    }
    serv_addr.sin_port = htons(portnum);

    int epollfd = epoll_create1(0);
    if (epollfd < 0)
        die("error with epoll_create1: %s", strerror(errno));
    
    // array to hold events for epoll_wait
    struct epoll_event* events = calloc(MAXCONC, sizeof(struct epoll_event));
    if (events == NULL)
        die("error allocating memory: %s", strerror(errno));
    
    // establish MAXCONC connections
    for (int i = 0; i < MAXCONC; i++) {
        add_connection(epollfd, (struct sockaddr*)&serv_addr);
        if (!(i%1000)) printf("connection %d open\n", i);
    }
    printf("all %d connections are open\n", MAXCONC);

    while (1) {
        int nready = epoll_wait(epollfd, events, MAXCONC, -1);
        if (nready == -1)
            die("epoll_wait error: %s\n", strerror(errno));

        for (int i = 0; i < nready; i++) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;
            if (ev & EPOLLIN) {
                // ready for reading
                /*
                    XXX TODO:
                    Llamar a rutina de recepción de mensaje:
                     Hay que ver el global_context[fd].
                     Si está WAIT_ACK, recibir el '*' (y no debería haber nada mas)
                      y pasar a SEND
                     Si está en RECV, recibir. Si terminó, pasar a END
                     En una primera versión podemos soportar un mensaje por conexión

                     No puede estar en otro estado.

                    Si luego de la rutina de recepción pasó a END,
                     cerrar conexión y conextar nuevamente (cicla conexiones)
                */ 
            }
            else if (ev & EPOLLOUT) {
                // ready for writing
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
            } else {
                // WTF??
                die("no debería llegar aquí, ev: %d\n", ev);
            }
        } // for nready
    } // event loop
}
    
