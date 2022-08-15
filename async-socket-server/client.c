#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "utils.h"

#define SVRPORT 9090
#define SVRADDR "127.0.0.1"
#define BUFSIZE 1024

int main(int argc, const char** argv) {

    int portnum = SVRPORT;
    char addr[16] = SVRADDR;
    struct sockaddr_in serv_addr;
    int sockfd, nread;
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

    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        die ("Error on socket: %s\n", strerror(errno));
    }
    if (connect(sockfd, (struct sockaddr*) &serv_addr, sizeof(serv_addr))) {
        die("Error connecting: %s\n", strerror(errno));
    }

    // at this point the client has to receive '*' as ACK
    nread = read(sockfd, buf, BUFSIZE);
    if (nread == -1) {
        die("Error reading: %s\n", strerror(errno));
    
    }
    else if (nread == 0) {
        die("server closed connection unexpectedly\n");
    }
    if (nread != 1 || buf[0] != '*') {
        die("server sent unexpected ack\n");
    }
    printf("server sent * as expected\n");
    return 0;
}

