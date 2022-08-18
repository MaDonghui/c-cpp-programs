#ifndef KVSTORE_REQUEST_DISPATCHER_H
#define KVSTORE_REQUEST_DISPATCHER_H

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common.h"

int send_response(int sock, int code, int payload_len, char *payload);
void request_dispatcher(int socket, struct request *request);

#endif
