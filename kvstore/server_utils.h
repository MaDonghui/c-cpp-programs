#ifndef KVSTORE_UTILS__H
#define KVSTORE_UTILS__H

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "request_dispatcher.h"
#include "common.h"

struct conn_info {
	struct sockaddr_in addr;
	int socket_fd;
};

int server_init(int argc, char *arg[]);
int accept_new_connection(int listen_sock, struct conn_info *conn_info);
int recv_request(int socket, struct request *request);
int connection_ready(int socket);
int receive_header(int socket, struct request *request);
void close_connection(int socket);
struct request *allocate_request();
int read_payload(int socket, struct request *request, size_t expected_len,
		 char *buf);
int check_payload(int socket, struct request *request, size_t expected_len);
#endif
