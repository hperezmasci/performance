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
#define BUFSIZE 1024        // send / receive buffer size
#define MSGSIZE 512         // menssage size
#define MAXCONC 1           // max concurrency

int
main(int argc, const char** argv)
{

    int portnum = SVRPORT;
    char addr[16] = SVRADDR;
    struct sockaddr_in serv_addr;
    char buf[BUFSIZE];

    if (argc >= 2) {
        strncpy(addr, argv[1], 15);
    }
    if (argc >= 3) {
        portnum = atoi(argv[2]);
    }

    bzero(&serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, addr, &serv_addr.sin_addr) <= 0) {
        die("Invalid address / Address not supported %s", addr);
    }
    serv_addr.sin_port = htons(portnum);

    printf("connecting to %s:%d\n",
           inet_ntop(AF_INET, &serv_addr.sin_addr, addr, 16),
           ntohs(serv_addr.sin_port));

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1)
        die ("Error on socket: %s\n", strerror(errno));

    make_socket_non_blocking(sockfd);
    
    int epollfd = epoll_create1(0);
    if (epollfd < 0)
        die("error with epoll_create1: %s", strerror(errno));
    
    if (connect(sockfd, (struct sockaddr*) &serv_addr, sizeof(serv_addr)) &&
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
    
    // array to hold events for epoll_wait
    struct epoll_event* events = calloc(MAXCONC, sizeof(struct epoll_event));
    if (events == NULL)
        die("error allocating memory: %s", strerror(errno));

    /*  wait for events in file descriptores defined in epoll context epollfd,
        and register in events */
    int nready = epoll_wait(epollfd, events, MAXCONC, -1);

    printf("sockets ready: %d\n", nready);

    // at this point the client has to receive '*' as ACK
    ssize_t n = read(sockfd, buf, BUFSIZE);
    if (n == -1)
        die("Error reading: %s\n", strerror(errno));
    else if (n == 0)
        die("server closed connection unexpectedly\n");
    
    if (n != 1 || buf[0] != '*')
        die("server sent unexpected ack\n");

    printf("received '*' from server\n");

    // set socket for output events
    event.events = EPOLLOUT;

    // add socket and expected event to epoll context (epollfd)
    if (epoll_ctl(epollfd, EPOLL_CTL_MOD, sockfd, &event) < 0)
        die("error with epoll_ctl: %s", strerror(errno));
    
    /*  wait for events in file descriptores defined in epoll context epollfd,
        and register in events */
    nready = epoll_wait(epollfd, events, MAXCONC, -1);
    
    printf("sockets ready: %d\n", nready);

    // sending message
    buf[0] = '^';
    buf[MSGSIZE+1] = '$';
    bzero(buf+1, MSGSIZE);

    size_t len = MSGSIZE+2;
    while(len) {
        n = write(sockfd, buf, len);
        if (n == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            die("error writing: %s\n", strerror(errno));
        }
        len -= n;
        assert(len>=0);
        if (n < len)
            printf("partial message sent (%d bytes)\n", (int)n);
    }
    printf("message sent (%d bytes)\n", MSGSIZE);

    // set socket for input events
    event.events = EPOLLIN;

    // add socket and expected event to epoll context (epollfd)
    if (epoll_ctl(epollfd, EPOLL_CTL_MOD, sockfd, &event) < 0)
        die("error with epoll_ctl: %s", strerror(errno));

    /*  wait for events in file descriptores defined in epoll context epollfd,
        and register in events */
    nready = epoll_wait(epollfd, events, MAXCONC, -1);
    
    printf("sockets ready: %d\n", nready);

    len = MSGSIZE;
    while(len) {
        n = read(sockfd, buf, len);
        if (n == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            die("error writing: %s\n", strerror(errno));
        }
        len -= n;
        assert(len>=0);
        printf("received %d bytes\n", (int)n);
    }

    return 0;
}

