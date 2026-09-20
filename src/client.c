#define _DEFAULT_SOURCE
#include "hex_string.h"
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define handle_error(msg)                                                      \
  do {                                                                         \
    perror(msg);                                                               \
    exit(EXIT_FAILURE);                                                        \
  } while (0)

/* write(2) may perform a short write; retry until the whole buffer is sent. */
static int write_all(int fd, const void *buf, size_t len) {
  const char *p = buf;
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = write(fd, p + sent, len - sent);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    sent += (size_t)n;
  }
  return 0;
}

/* Mirror of write_all() for reads: read(2) can also return short.
 * Returns 1 on success, 0 on a clean EOF before any bytes of this call were
 * read, -1 on error. */
static int read_all(int fd, void *buf, size_t len) {
  char *p = buf;
  size_t got = 0;
  while (got < len) {
    ssize_t n = read(fd, p + got, len - got);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (n == 0)
      return got == 0 ? 0 : -1;
    got += (size_t)n;
  }
  return 1;
}

/* Frames on the wire are [4-byte big-endian length][length bytes of body].
 * Framing used to rely on scanning for '\n', but the binary IP/port header
 * bytes can legitimately contain 0x0A, which desynchronized the parser and
 * caused intermittent message loss. A length prefix removes that ambiguity. */
static int write_frame(int fd, const void *body, size_t body_len) {
  uint32_t net_len = htonl((uint32_t)body_len);
  if (write_all(fd, &net_len, sizeof(net_len)) != 0)
    return -1;
  return write_all(fd, body, body_len);
}

/* Returns 1 on a frame read into buf (*out_len set), 0 on clean EOF, -1 on
 * error or a frame larger than buf_cap. */
static int read_frame(int fd, char *buf, size_t buf_cap, size_t *out_len) {
  uint32_t net_len;
  int rc = read_all(fd, &net_len, sizeof(net_len));
  if (rc <= 0)
    return rc;
  uint32_t body_len = ntohl(net_len);
  if (body_len > buf_cap)
    return -1;
  rc = read_all(fd, buf, body_len);
  if (rc <= 0)
    return -1;
  *out_len = body_len;
  return 1;
}

int message_count = 0;
char *log_path = NULL;

void *print_messages(void *arg) {

  int sfd = *(int *)arg;
  uint8_t type;
  FILE *logf = fopen(log_path, "a");
  if (!logf)
    handle_error("fopen");

  char msg_buf[1024];
  size_t msg_len;
  int rc;

  while ((rc = read_frame(sfd, msg_buf, sizeof(msg_buf), &msg_len)) > 0) {

    if (msg_len < 1)
      handle_error("Empty message received by client");
    type = msg_buf[0];
    if (type == 1) {

      printf("Server is ending the chat.\n");
      fclose(logf);
      return NULL;

    } else if (type == 0) {

      uint32_t ip;
      uint16_t port;
      memcpy(&ip, msg_buf + 1, 4);
      memcpy(&port, msg_buf + 5, 2);
      ip = ntohl(ip);
      port = ntohs(port);

      struct in_addr in;
      memcpy(&in.s_addr, msg_buf + 1, 4);
      char ip_str[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &in, ip_str, sizeof(ip_str));

      char print_msg[1024];
      int msg_size = msg_len - 7;
      memcpy(print_msg, msg_buf + 7, msg_size);
      print_msg[msg_size] = '\0';
      printf("%-15s%-10u%s", ip_str, port, print_msg);
      fprintf(logf, "%-15s%-10u%s", ip_str, port, print_msg);
      fflush(logf);

    } else {
      handle_error("type error received by client");
    }
  }

  fclose(logf);
  if (rc < 0)
    handle_error("read error from server");

  return NULL;
}

int main(int argc, char *argv[]) {

  if (argc != 5)
    handle_error("Issue with input arguments.");
  char *IP = argv[1];
  int port = atoi(argv[2]);
  int no_message = atoi(argv[3]);
  char *log_p = argv[4];
  log_path = log_p;
  printf("IP: %s\n", IP);
  printf("Port: %d\n", port);
  printf("No of messages: %d\n", no_message);
  printf("Log file path: %s\n", log_p);

  struct sockaddr_in addr;
  int sfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sfd == -1)
    handle_error("socket");
  memset(&addr, 0, sizeof(struct sockaddr_in));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, IP, &addr.sin_addr) <= 0)
    handle_error("inet_pton");

  int res = connect(sfd, (struct sockaddr *)&addr, sizeof(struct sockaddr_in));
  if (res == -1)
    handle_error("connect");

  FILE *logf = fopen(log_path, "w");
  if (!logf)
    handle_error("fopen log file");

  pthread_t tid;
  pthread_create(&tid, NULL, print_messages, &sfd);

  ssize_t num_read;
  uint8_t buf1[249];
  uint8_t buf2[249];
  char send_buf[1024];

  for (int i = 0; i < no_message; i++) {

    char *send_ptr = NULL;
    num_read = getentropy(buf1, sizeof(buf1));
    if (num_read != 0)
      handle_error("Error in getentropy");
    num_read = getentropy(buf2, sizeof(buf2));
    if (num_read != 0)
      handle_error("Error in getentropy");

    send_buf[0] = (uint8_t)0;
    send_ptr = send_buf + 1;
    if (convert(buf1, sizeof(buf1), send_ptr, 1024 - (send_ptr - send_buf)) !=
        0)
      handle_error("convert failed");
    send_ptr += sizeof(buf1) * 2;
    if (convert(buf2, sizeof(buf2), send_ptr, 1024 - (send_ptr - send_buf)) !=
        0)
      handle_error("convert failed");

    send_ptr += sizeof(buf2) * 2;
    *send_ptr = '\n';
    send_ptr++;
    int out_len = send_ptr - send_buf;
    if (write_frame(sfd, send_buf, out_len) != 0)
      handle_error("write");
  }

  char end_msg[2];
  end_msg[0] = (uint8_t)1;
  end_msg[1] = '\n';
  write_frame(sfd, &end_msg, sizeof(end_msg));
  // printf("\nDone with messages.\n");

  // while ((num_read = read(STDIN_FILENO, buf, 1024)) > 0) {
  //   message_count++;
  //   if (message_count <= no_message) {
  //     char send_buf[1024 + 1];
  //     send_buf[0] = 0;
  //  memcpy(send_buf + 1, &IP, sizeof(uint32_t));
  //  memcpy(send_buf + 5, &port, sizeof(uint16_t));
  //    memcpy(send_buf + 1, buf, num_read);
  //    if (write(sfd, send_buf, num_read + 1) != num_read + 1)
  //      handle_error("write");
  //    // printf("Just sent %zd bytes.\n", num_read);
  //  } else {

  //    uint8_t end_msg = 1;
  //    write(sfd, &end_msg, 1);
  //    handle_error("No of messages exceeding.");
  //  }
  //}

  // if (num_read == -1)
  //   handle_error("read error from terminal");
  pthread_join(tid, NULL);

  close(sfd);
  exit(EXIT_SUCCESS);
}
