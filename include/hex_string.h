#ifndef HEX_STRING_H
#define HEX_STRING_H

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

int convert(uint8_t *buf, ssize_t buf_size, char *str, ssize_t str_size);

#endif
