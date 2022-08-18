#include <errno.h>
#include "parser.h"
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "common.h"

/*
 * @param fd file descriptor
 * @param ptr pointer to a buffer
 * @param maxlen max size for line's lenght
 * @return On success the number of read bytes, -1 on error
 */
int read_line(int fd, char *buf, int maxlen)
{
    int i;
    char c;

    for (i = 0; i < maxlen - 1; i++) {
        ssize_t r;
        if ((r = read(fd, &c, 1)) == -1) {
            return r;
        }

        if (r == 0)
            return -1;  /* EOF */

        if (c == '\n')
            break;
        *buf++ = c;
    }
    if (c != '\n')
        return -2;
    *buf = '\0';
    return i;
}

ssize_t send_on_socket(int fd, const void *buf, size_t n)
{
    size_t nleft;
    ssize_t nwritten;
    const char *ptr;

    ptr = buf;
    nleft = n;
    while (nleft > 0) {
        if ((nwritten = write(fd, ptr, nleft)) <= 0) {
            if ((nwritten < 0) && (errno == EINTR))
                nwritten = 0;
            else
                return (-1);
        }
        nleft -= nwritten;
        ptr += nwritten;
    }
    return (n - nleft);
}

enum method method_to_enum(const char *str)
{
    const size_t nmethods = sizeof(method_conversion) /
        sizeof(method_conversion[0]);
    size_t i;
    for (i = 0; i < nmethods; i++) {
        if (!strcmp(str, method_conversion[i].str)) {
            return method_conversion[i].val;
        }
    }
    return UNK;
}

const char *method_to_str(enum method code)
{
    const size_t nmethods = sizeof(method_conversion) /
        sizeof(method_conversion[0]);
    size_t i;
    for (i = 0; i < nmethods; i++) {
        if (method_conversion[i].val == code) {
            return method_conversion[i].str;
        }
    }
    return "UNK";
}

int parse_header(int fd, struct request *request)
{
    int nread;
    char line[MSG_SIZE], *token;
    char *saveptr;
    char delim[] = " ";

    request->method = UNK;
    request->key = NULL;
    request->key_len = 0;
    request->msg_len = 0;

    bzero(line, MSG_SIZE);
    if ((nread = read_line(fd, line, MSG_SIZE)) <= 0) {
        return nread;
    }
    // Method
    if ((token = strtok_r(line, delim, &saveptr)) == NULL)
        return nread;

    if ((request->method = method_to_enum(token)) == UNK) {
        error("Unknown method '%s'\n", token);
        return nread;
    }
    // Key (optional)
    if ((token = strtok_r(NULL, delim, &saveptr)) == NULL)
        return nread;

    request->key_len = strlen(token);
    request->key = malloc(request->key_len + 1);
    strcpy(request->key, token);

    // Payload len (optional)
    if ((token = strtok_r(NULL, delim, &saveptr)) == NULL)
        return nread;

    request->msg_len = strtoul(token, NULL, 10);
    if (errno != 0) {
        pr_debug("Cannot parse payload len (%s)\n", token);
        return -1;
    }
    return nread;
}
