#ifndef KVSTORE_PARSER_H
#define KVSTORE_PARSER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"

int parse_header(int fd, struct request *request);
enum method method_to_enum(const char *str);
const char *method_to_str(enum method code);
int read_line(int fd, char *ptr, int maxlen);
ssize_t send_on_socket(int fd, const void *buf, size_t n);

#endif
