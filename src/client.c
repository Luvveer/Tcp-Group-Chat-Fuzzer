#define _DEFAULT_SOURCE
#include "hex_string.h"
#include <arpa/inet.h>
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

int message_count = 0;
char *log_path = NULL;

void *print_messages(void *arg) {

  int sfd = *(int *)arg;
  char buf[1024];
  ssize_t num_read;
  uint8_t type;
  FILE *logf = fopen(log_path, "a");
  if (!logf)
    handle_error("fopen");

  char store_buf[2048];
  int len = 0;

  while ((num_read = read(sfd, buf, sizeof(buf))) > 0) {

    if (len + num_read > (int)sizeof(store_buf))
      handle_error("size overflow in store buf");
    memcpy(store_buf + len, buf, num_read);
    len += num_read;

    while (1) {

      char *nl = memchr(store_buf, '\n', len);
      if (!nl)
        break;
      int msg_len = nl - store_buf + 1;
      type = store_buf[0];
      if (type == 1) {

        printf("Server is ending the chat.\n");
        return NULL;

      } else if (type == 0) {

        uint32_t ip;
        uint16_t port;
        memcpy(&ip, store_buf + 1, 4);
        memcpy(&port, store_buf + 5, 2);
        ip = ntohl(ip);
        port = ntohs(port);

        struct in_addr in;
        memcpy(&in.s_addr, store_buf + 1, 4);
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &in, ip_str, sizeof(ip_str));
        // printf("1\n");

        char print_msg[1024];
        int msg_size = msg_len - 7;
        memcpy(print_msg, store_buf + 7, msg_size);
        print_msg[msg_size] = '\0';
        printf("%-15s%-10u%s", ip_str, port, print_msg);
        fprintf(logf, "%-15s%-10u%s", ip_str, port, print_msg);
        fflush(logf);

      } else {
        handle_error("type error received by client");
      }
      memmove(store_buf, store_buf + msg_len, len - msg_len);
      len -= msg_len;
    }
  }

  fclose(logf);
  if (num_read == -1)
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
    if (write(sfd, send_buf, out_len) != out_len)
      handle_error("write");
  }

  char end_msg[2];
  end_msg[0] = (uint8_t)1;
  end_msg[1] = '\n';
  write(sfd, &end_msg, sizeof(end_msg));
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
