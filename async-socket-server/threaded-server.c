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

#define LOGLEVEL ERR	// log from this level
#define STATSFRQ 10000
#define DUMMY_WORK 2000 // times calling wait
#define DUMMY_WAIT 5000 // microseconds to wait

typedef struct { int sockfd; } thread_config_t;

typedef enum { WAIT_FOR_MSG, IN_MSG } ProcessingState;

void do_something_dumb()
{
	int i=0;
	ssize_t n;
	// CPU intensive...
	for (i=0; i < DUMMY_WORK; i++) {
		n = write(1, "", 0);
	}
	n = 1;
	// rest...
	usleep(DUMMY_WAIT);
	if (n) return;
	
}

void serve_connection(int sockfd, unsigned long id) {
  logger(DEBUG, "server_connection(%lu)", id);
  if (send(sockfd, "*", 1, 0) < 1) {
    logger(ERR, "serve_connection(%lu): sending ACK (%d)", id, errno);
  }

  ProcessingState state = WAIT_FOR_MSG;

  while (1) {
    logger(DEBUG, "serve_connection(%lu): while", id);
    uint8_t buf[1024];
    int len = recv(sockfd, buf, sizeof buf, 0);
    logger(DEBUG, "serve_connection(%lu): recv: %d", id, len);
    if (len < 0) {
      if (errno == ECONNRESET) {
        logger(WARN, "serve_connection(%lu): recv: ECONNRESET", id);
        break;
      }
      logdie("serve_connection(%lu): recv: (%d)", id, errno);
    } else if (len == 0) {
      logger(WARN, "serve_connection(%lu): recv: len 0", id);
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
          logger(DEBUG, "serve_connection(%lu): change to IN_MSG", id);
        }
        break;

      case IN_MSG:
        if (buf[i] == '$') {
          logger(DEBUG, "serve_connection(%lu): end of message, len: %d", id, slen);
          // end of message
          while(slen) {
            do_something_dumb();
            logger(DEBUG, "serve_connection(%lu): bytes to send: %d", id, slen);
            int nsent = send(sockfd, sbuf, slen, MSG_NOSIGNAL);
            logger(DEBUG, "serve_connection(%lu): bytes sent: %d", id, nsent);
            if (nsent < 0) {
              logger(ERR, "serve_connection(%lu): send: %d", id, errno);
              break;
            }
            if (nsent == 0) {
              logger(WARN, "serve_connection(%lu): sent 0 bytes", id);
            }
            slen = slen - nsent;
            sbuf+= nsent;
            assert(slen > 0);
            if (slen) logger(WARN, "serve_connection(%lu): need to send more than once",id);
          }
          logger(DEBUG, "serve_connection(%lu): change to WAIT_FOR_MSG", id);
          state = WAIT_FOR_MSG;
        } else {
          slen++;
        }
        break;
      }
    }
    //break; // XXX BUG, escapo forzando por ahora 1 solo ciclo x server
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
  serve_connection(sockfd, id);
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
      logdie("accept: %d", errno);
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

