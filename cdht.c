#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdarg.h>

#define BUFLEN 512
#define NPACK 10
#define PORT 9930
#define LOCALHOST 0x7f000001


void diep(char *s)
{
    perror(s);
    exit(1);
}

int me;
int successor[2] = {0};
int s;
volatile int failed_pings = 0;

void receive_message(int *id, char *buf, int buffer_length)
{
    struct sockaddr_in sockaddr_other = { 0 };
    int slen = sizeof(struct sockaddr_in);
    if (recvfrom(s, buf, buffer_length, 0, (struct sockaddr * __restrict__) &sockaddr_other, &slen)
	== -1) {
	diep("recvfrom()");
    }
    *id = ntohs(sockaddr_other.sin_port) - 50000;
}


void send_message(int id, char *format, ...)
{
    va_list args;
    va_start(args, format);
    struct sockaddr_in sockaddr_other;
    sockaddr_other.sin_family = AF_INET;
    sockaddr_other.sin_port = htons(50000 + id);
    sockaddr_other.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    char buf[BUFLEN];

    vsnprintf(buf, BUFLEN, format, args);

    va_end(args);

    if (sendto
	(s, buf, strlen(buf) + 1, 0, (const struct sockaddr *) &sockaddr_other,
	 sizeof(struct sockaddr_in)) == -1) {
	diep("sendto()");
    }
}



void split_space(char *message, int max, char *result[])
{
    int count = 0;
    char reset = 1;
    while (*message && count < max - 1) {
	if (reset) {
	    result[count++] = message;
	    reset = 0;
	}
	if (*message == ' ') {
	    *message = '\0';
	    reset = 1;
	}
	if (*message == '\n') {
	    *message = '\0';
	    return;
	}
	message++;
    }
}

void *listener_thread(void *arg)
{
    printf("Listener starting...\n");
    char buf[BUFLEN];
    char reply[BUFLEN];
    char *split[5];
    int id = 0;
    while (1) {
	receive_message(&id, buf, BUFLEN);
	split_space(buf, sizeof(split) / sizeof(*split), split);
	if (!strcmp(split[0], "ping")) {
	    printf("A ping request message was received from Peer %d.\n", id);
	    send_message(id, "pong");
	} else if (!strcmp(split[0], "pong")) {
	    printf("A ping response message was received from Peer %d.\n", id);
	    if (id == successor[0]) {
		failed_pings = 0;
	    }
	} else if (!strcmp(split[0], "request")) {
	    int file_id = atoi(split[2]);
	    int peer_id = file_id % 256;
	    if ((peer_id <= me || id > me) && peer_id > id) {
		printf("File %s is here.\n", split[2]);
		printf("A response message destined for peer %s, has been sent.\n", split[1]);
		send_message(atoi(split[1]), "file %s", split[2]);
	    } else {
		printf("File %s is not stored here.\n", split[2]);
		printf("File request message has been forwarded to my successor.\n", split[1]);
		send_message(successor[0], "request %s %s", split[1], split[2]);
	    }
	} else if (!strcmp(split[0], "file")) {
	    printf("Received a response message from peer %d, which has the file %s.\n", id, split[1]);
	} else if (!strcmp(split[0], "successor")) {
	    send_message(id, "successor-response %d", successor[0]);
	} else if (!strcmp(split[0], "successor-response")) {
	    if (successor[0] == id) {
		successor[1] = atoi(split[1]);
		printf("My second successor is now peer %s.\n", split[1]);
	    }
        } else if (!strcmp(split[0], "quit")) {
	    printf("Recieved quit message for %s from %d.\n", split[1], id);
	    int quit_id = atoi(split[1]);
	    if (successor[0] == quit_id) {
		successor[0] = successor[1];
		printf("My first successor is now peer %d.\n", successor[0]);
		successor[1] = 0;
	    } else if (successor[1] == quit_id) {
		successor[1] = 0;
		send_message(successor[0], "quit %s", split[1]);
	    } else if ((successor[0] - me) % 255 >= (quit_id - me) % 255) {
		send_message(successor[0], "quit %s", split[1]);
	    }
	} else {
	    printf("Unknown message received: %s\n", split[0]);
	}
    }
}

void *user_thread(void *arg)
{
    printf("User thread stating...\n");
    char buf[BUFLEN];
    char *split[5];
    while (fgets(buf, 512, stdin) != NULL) {
	split_space(buf, sizeof(split) / sizeof(*split), split);
	if (!strcmp(split[0], "request")) {
	    // request <origin> <filename>
	    printf("File request message for %s has been sent to my successor.\n", split[1]);
	    send_message(successor[0], "request %d %s", me, split[1]);
	} else if (!strcmp(split[0], "quit")) {
	    printf("Peer %d will depart from the network.\n", me);
	    send_message(successor[0], "quit %d", me);
	    exit(0);
	} else if (!strcmp(split[0], "die")) {
	    exit(-1);
	}
    }
}

void *active_thread()
{
    printf("Active thread starting...\n");
    while (1) {
	if (successor[1] == 0) {
	    send_message(successor[0], "successor");
	}
	if (failed_pings == 3) {
	    printf("%d not responding.\n", successor[0]);
	    send_message(successor[1], "quit %d", successor[0]);
	    successor[0] = successor[1];
	    printf("My first successor is now peer %d.\n", successor[0]);
	    send_message(successor[0], "successor");
	}
	send_message(successor[0], "ping");
	failed_pings ++;
	sleep(5);
    }
}




int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IOLBF, 512);

    // setup socket

    if (argc != 4) {
	fprintf(stderr, "Usage: %s id peer1 peer2\n", argv[0]);
	exit(-1);
    }

    me = atoi(argv[1]);
    successor[0] = atoi(argv[2]);
    successor[1] = atoi(argv[3]);

    struct sockaddr_in sockaddr_me;

    sockaddr_me.sin_family = AF_INET;
    sockaddr_me.sin_port = htons(50000 + me);
    sockaddr_me.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == -1) {
	diep("socket");
    }

    if (bind(s, (const struct sockaddr *) &sockaddr_me, sizeof(sockaddr_me)) == -1) {
	diep("bind");
    }

    int error;


    // setup listener thread

    pthread_t tinfo;

    if ((error = pthread_create(&tinfo, NULL, &listener_thread, NULL)) != 0) {
	diep("pthread_create");
    }

    if ((error = pthread_create(&tinfo, NULL, &user_thread, NULL)) != 0) {
	diep("pthread_create");
    }

    active_thread();
}
