// Threaded socket server - accepting multiple clients concurrently, by creating
// a new thread for each connecting client.
//
// Eli Bendersky [http://eli.thegreenplace.net]
// This code is in the public domain.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>

#include "utils.h"

#define LOGLEVEL INFO		// log from this level
#define STATSFRQ 10000

typedef struct { int sockfd; } thread_config_t;

typedef enum { WAIT_FOR_MSG, IN_MSG } ProcessingState;

void serve_connection(int sockfd) {
  logger(DEBUG, "server_connection");
  if (send(sockfd, "*", 1, 0) < 1) {
    logger(ERR, "serve_connection: sending ACK (%d)", errno);
  }

  ProcessingState state = WAIT_FOR_MSG;

  while (1) {
    logger(DEBUG, "serve_connection: while");
    uint8_t buf[1024];
    int len = recv(sockfd, buf, sizeof buf, 0);
    logger(DEBUG, "serve_connection: recv: %d", len);
    if (len < 0) {
      if (errno == ECONNRESET) {
        logger(INFO, "serve_connection: recv: (%d)", errno);
        break;
      }
      logdie("serve_connection: recv: (%d)", errno);
    } else if (len == 0) {
      logger(WARN, "serve_connection: recv: len 0");
      break;
    }

    uint8_t* sbuf = buf;
    ssize_t slen = 0;
    for (int i = 0; i < len; ++i) {
      switch (state) {
      case WAIT_FOR_MSG:
        if (buf[i] == '^') {
          sbuf = &buf[i+1];
          state = IN_MSG;
          logger(DEBUG, "serve_connection: change to IN_MSG");
        }
        break;

      case IN_MSG:
        if (buf[i] == '$') {
          logger(DEBUG, "serve_connection: end of message, len: %d", slen);
          // end of message
          while(slen) {
            logger(DEBUG, "serve_connection: bytes to send: %d", slen);
            int nsent = send(sockfd, sbuf, slen, MSG_NOSIGNAL);
            logger(DEBUG, "serve_connection: bytes sent: %d", nsent);
            if (nsent < 0) {
              logger(ERR, "serve_connection: send: %d", errno);
              break;
            }
            if (nsent == 0) {
              logger(WARN, "serve_connection: sent 0 bytes");
            }
            slen = slen - nsent;
            sbuf+= nsent;
            assert(slen > 0);
            if (slen) logger(WARN, "serve_connection: need to send more than once");
          }
          logger(DEBUG, "serve_connection: change to WAIT_FOR_MSG");
          state = WAIT_FOR_MSG;
        } else {
          slen++;
        }
        break;
      }
    }
    break; // XXX BUG, escapo forzando por ahora 1 solo ciclo x server
  }

  close(sockfd);
}

void* server_thread(void* arg) {
  thread_config_t* config = (thread_config_t*)arg;
  int sockfd = config->sockfd;
  free(config);

  // This cast will work for Linux, but in general casting pthread_id to an
  // integral type isn't portable.
  unsigned long id = (unsigned long)pthread_self();
  logger(DEBUG, "Thread %lu created to handle connection with socket %d", id,
         sockfd);
  serve_connection(sockfd);
  logger(DEBUG, "Thread %lu done", id);
  return 0;
}

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);

  set_loglevel(LOGLEVEL);

  int portnum = 9090;
  if (argc >= 2) {
    portnum = atoi(argv[1]);
  }
  logger(INFO, "Serving on port %d\n", portnum);
  fflush(stdout);

  int sockfd = listen_inet_socket(portnum);

  int conn = 0;
  while (1) {
    struct sockaddr_in peer_addr;
    socklen_t peer_addr_len = sizeof(peer_addr);

    int newsockfd =
        accept(sockfd, (struct sockaddr*)&peer_addr, &peer_addr_len);

    conn++;

    if (newsockfd < 0) {
      logdie("accept: %d");
    }

    if (!(conn % STATSFRQ)) report_peer_connected(&peer_addr, peer_addr_len);
    
    pthread_t the_thread;

    thread_config_t* config = (thread_config_t*)malloc(sizeof(*config));
    if (!config) {
      logdie("OOM");
    }
    config->sockfd = newsockfd;
    pthread_create(&the_thread, NULL, server_thread, config);

    // Detach the thread - when it's done, its resources will be cleaned up.
    // Since the main thread lives forever, it will outlive the serving threads.
    pthread_detach(the_thread);
  }

  return 0;
}
