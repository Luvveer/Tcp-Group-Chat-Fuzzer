#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

int convert(uint8_t *buf, ssize_t buf_size, char *str, ssize_t str_size) {

  if (buf == NULL || str == NULL || buf_size <= 0 ||
      str_size < (buf_size * 2 + 1)) {
    return -1;
  }

  for (int i = 0; i < buf_size; i++)
    sprintf(str + i * 2, "%02X", buf[i]);
  str[buf_size * 2] = '\0';

  return 0;
}
