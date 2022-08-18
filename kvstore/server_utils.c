#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <signal.h>
#include <netinet/tcp.h>
#include <dlfcn.h>
#include <assert.h>

#include "parser.h"
#include "server_utils.h"
#include "common.h"
#include "request_dispatcher.h"

#define BACKLOG     10
#define TIMEOUT     60

int debug = 0;
int verbose = 0;

int accept(int sockfd, struct sockaddr *addr, socklen_t * addrlen)
{
    sockfd = sockfd;
    addr = addr;
    addrlen = addrlen;
    pr_debug("Calling accept() is not allowed\n");
    exit(EXIT_FAILURE);
}

struct request *allocate_request()
{
    struct request *r = malloc(sizeof(struct request));
    if (r == NULL) {
        pr_debug("error in memory allocation");
        return NULL;
    }

    return r;
}

void close_connection(int socket)
{
    pr_debug("Closing connection on socket %d\n", socket);
    close(socket);
}

void usage(char *prog)
{
    fprintf(stderr, "Usage %s [--help -h] [--verbose -v] [--debug -d] "
        "[--port -p]\n", prog);
    fprintf(stderr, "--help -h\n\t Print help message\n");
    fprintf(stderr, "--verbose -v\n\t Print info messages\n");
    fprintf(stderr, "--debug -d\n\t Print debug info\n");
    fprintf(stderr,
        "--port -p\n\t Port to bind on. Default: pick the first available port\n");
}

int server_init(int argc, char *argv[])
{
    unsigned int port = PORT;
    int option = 1;
    int socket_fd;
    struct sockaddr_in server_addr;

    const struct option long_options[] = {
        {"help", no_argument, NULL, 'h'},
        {"verbose", no_argument, NULL, 'v'},
        {"debug", no_argument, NULL, 'd'},
        {"port", required_argument, NULL, 'p'},
        {0, 0, 0, 0}
    };

    for (;;) {
        int option_index = 0;
        int c;
        c = getopt_long(argc, argv, "hvdp:", long_options,
                &option_index);
        if (c == -1)
            break;

        switch (c) {
        case 'h':
            usage(argv[0]);
            exit(EXIT_SUCCESS);
        case 'v':
            verbose = 1;
            break;
        case 'd':
            debug = 1;
            break;
        case 'p':
            port = atoi(argv[optind]);
            break;
        default:
            exit(EXIT_SUCCESS);
        }
    }

    /* TCP connection */
    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd == -1) {
        perror("socket cannot be created\n");
        exit(EXIT_FAILURE);
    }

    /* if the port is busy and in the TIME_WAIT state, reuse it anyway. */
    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, (char *)&option,
               sizeof(option)) < 0) {
        close(socket_fd);
        perror("setsockopt failed\n");
        exit(EXIT_FAILURE);
    }

    memset((void *)&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);

    if ((bind(socket_fd, (struct sockaddr *)&server_addr,
          sizeof(server_addr))) < 0) {
        perror("address and port binding failed\n");
        exit(EXIT_FAILURE);
    }

    socklen_t addr_len = sizeof(server_addr);
    if (getsockname(socket_fd, (struct sockaddr *)&server_addr, &addr_len)
        == -1) {
        perror("address and port binding failed\n");
        exit(EXIT_FAILURE);
    }

    pr_info("[%s] Pid:%d bind on socket:%d Port:%d\n", SERVER,
        (int)getpid(), socket_fd, ntohs(server_addr.sin_port));

    if (listen(socket_fd, BACKLOG) < 0) {
        perror("Cannot listen on socket");
        exit(EXIT_FAILURE);
    }

    pr_info("Listenig socket n. %d\n", socket_fd);
    signal(SIGPIPE, SIG_IGN);
    return socket_fd;
}

int orig_accept(int sockfd, struct sockaddr *addr, socklen_t * addrlen)
{
    static int (*_orig_accept) (int, struct sockaddr *, socklen_t *);
    int ret;
    if (!_orig_accept)
        _orig_accept = dlsym(RTLD_NEXT, "accept");
    ret = _orig_accept(sockfd, addr, addrlen);
    assert(ret != -1);
    return ret;
}

/**
 * call accept() on the listening socket for incoming connections
 * @return 0 on success, -1 on error
 */
int accept_new_connection(int listen_sock, struct conn_info *conn_info)
{
    int nodelay = 1;
    socklen_t addrlen = sizeof(conn_info->addr);
    if ((conn_info->socket_fd =
         /* accept(listen_sock, (struct sockaddr *)&conn_info->addr, &addrlen)) < 0) { */
         orig_accept(listen_sock, (struct sockaddr *)&conn_info->addr,
             &addrlen)) < 0) {
        error("Cannot accept new connection\n");
        return -1;
    }

    if (setsockopt
        (conn_info->socket_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay,
         sizeof(nodelay)) < 0) {
        perror("setsockopt TCP_NODELAT");
        return -1;
    }
    return 0;
}

int connection_ready(int socket)
{
    struct timeval timeout;
    timeout.tv_sec = TIMEOUT;
    timeout.tv_usec = 0;
    fd_set rset;

    FD_ZERO(&rset);
    FD_SET(socket, &rset);

    // Close connection after TIMEOUT seconds without requests
    if (select(socket + 1, &rset, NULL, NULL, &timeout) == -1) {
        perror("Select timeout expired with no data\n");
        return -1;
    }

    return 0;
}

int receive_header(int socket, struct request *request)
{
    int recved;
    recved = parse_header(socket, request);
    if (recved == 0)
        return 0;
    if (recved == -1) {
        request->connection_close = 1;
        return -1;
    }
    if (recved == -2) {
        send_response(socket, PARSING_ERROR, 0, NULL);
        request->connection_close = 1;
        return -1;
    }
    return 1;
}

/**
 * Try to read the header line for a request.
 * @return an int representing the request's method, -1 on error.
 * an error can be caused by the socket not ready for I/O or
 * a bad request which cannot be parsed
 */
int recv_request(int socket, struct request *request)
{
    if (connection_ready(socket) == -1) {
        return -1;
    }
    if (receive_header(socket, request) == -1) {
        // Connection closed from client side or error occurred
        free(request->key);
        request->key = NULL;
        return -1;
    }

    request_dispatcher(socket, request);
    return request->method;
}

/**
 * It read 'expected_len' bytes from 'socket' and return them into 'buf'
 * it returns the actual number of read bytes or -1 on error.
 * On error 'request->connection_close' is set to indicate that the connection
 * should be closed from the server side.
 */
int read_payload(int socket, struct request *request, size_t expected_len,
         char *buf)
{
    char tmp;
    int recvd = 0;
    // Still read out the payload so we keep the stream consistent
    for (size_t i = 0; i < expected_len; i++) {
        if (read(socket, &tmp, 1) <= 0) {
            request->connection_close = 1;
            return -1;
        }
        recvd++;
        buf[i] = tmp;
    }

    return recvd;
}

/**
 * Check the payload is well formed and read the last byte which shoudl be '\n'
 * It returns 0 on success, -1 otherwise.
 */
int check_payload(int socket, struct request *request, size_t expected_len)
{
    char tmp;
    int rcved;

    // The payload (if there was any) should be followed by a \n
    if (expected_len &&
        (((rcved = read(socket, &tmp, 1)) <= 0) || tmp != '\n')) {
        error("Corrupted stream (read %d chars, char %c (%#x))\n",
              rcved, tmp, tmp);
        request->connection_close = 1;
        return -1;
    }
    return 0;
}
