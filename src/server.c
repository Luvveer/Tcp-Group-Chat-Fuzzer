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

int client_id_count = 0;
int type1_count = 0;
int max_client;
struct client_info *info = NULL;
// pthread_mutex_t client_id = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t writing_all_clients = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t type1 = PTHREAD_MUTEX_INITIALIZER;

struct client_info {
  int cfd;
  int client_id;
  struct sockaddr_in addr;
};

void *client_function(void *arg) {
  int *client_idd = (int *)arg;
  struct client_info *client_details = &info[*client_idd];
  int cfd = client_details->cfd;
  int client_id = client_details->client_id;
  uint16_t port = client_details->addr.sin_port;
  uint32_t ip = client_details->addr.sin_addr.s_addr;
  free(client_idd);
  client_idd = NULL;

  uint8_t type;
  int num_read;
  char buf[1024];
  char print_buf[1024];
  char store_buf[65536];
  int len = 0;

  while ((num_read = read(cfd, buf, sizeof(buf))) > 0) {

    if (len + num_read > (int)sizeof(store_buf))
      handle_error("MEssage too large");
    memcpy(store_buf + len, buf, num_read);
    len += num_read;

    while (1) {

      char *nl = memchr(store_buf, '\n', len);
      if (!nl)
        break;
      int msg_len = nl - store_buf + 1;
      type = store_buf[0];
      if (type == 0) {
        char send_buf[1024];
        send_buf[0] = (uint8_t)0;
        memcpy(send_buf + 1, &ip, sizeof(uint32_t));
        memcpy(send_buf + 5, &port, sizeof(uint16_t));
        memcpy(send_buf + 7, store_buf + 1, msg_len - 1);
        size_t send_len = msg_len + 6;

        pthread_mutex_lock(&writing_all_clients);
        for (int i = 0; i < client_id_count; i++) {
          if (info[i].cfd != -1) {
            write(info[i].cfd, send_buf, send_len);
          }
        }
        memcpy(print_buf, store_buf + 1, msg_len - 1);
        print_buf[msg_len - 1] = '\0';
        pthread_mutex_unlock(&writing_all_clients);
        printf("MSG from client %d: %s", client_id, print_buf);

      } else if (type == 1) {

        pthread_mutex_lock(&type1);
        type1_count++;
        // printf("\ntype1: %d", type1_count);
        // printf("\nmax_client: %d\n", max_client);
        if (type1_count == max_client) {
          // printf("\nreached\n");
          for (int i = 0; i < max_client; i++) {
            char end_msg[2];
            end_msg[0] = (uint8_t)1;
            end_msg[1] = '\n';
            write(info[i].cfd, &end_msg, sizeof(end_msg));
          }
          pthread_mutex_unlock(&type1);
          printf("Ending connection for all client\n");
          return NULL;
        }
        pthread_mutex_unlock(&type1);
        // printf("Error with the if loop");
      } else {
        handle_error("Issue type received by server");
      }
      memmove(store_buf, store_buf + msg_len, len - msg_len);
      len -= msg_len;
    }
  }

  if (num_read == -1)
    handle_error("read");

  printf("Ending connection for client %d\n", client_id);
  return NULL;
}

int main(int argc, char *argv[]) {

  if (argc != 3)
    handle_error("Issue with input arguments");
  int port = atoi(argv[1]);
  int max_clients = atoi(argv[2]);
  max_client = max_clients;
  printf("port: %d\n", port);
  printf("max_clients: %d\n", max_clients);

  struct client_info client_details[max_clients];
  info = client_details;

  struct sockaddr_in addr;
  int sfd;
  sfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sfd == -1)
    handle_error("socket");

  memset(&addr, 0, sizeof(struct sockaddr_in));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(sfd, (struct sockaddr *)&addr, sizeof(struct sockaddr_in)) == -1)
    handle_error("bind");

  if (listen(sfd, 32) == -1)
    handle_error("listen");

  for (;;) {

    int a = client_id_count;
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_cfd = accept(sfd, (struct sockaddr *)&client_addr, &addr_len);

    if (client_cfd == -1)
      handle_error("accept");

    if (client_id_count >= max_clients)
      handle_error("Exceeding_Max_client");

    pthread_mutex_lock(&writing_all_clients);
    client_details[a].addr = client_addr;
    client_details[a].cfd = client_cfd;
    client_details[a].client_id = client_id_count;
    client_id_count++;
    pthread_mutex_unlock(&writing_all_clients);

    printf("New Client created. ID %d on docket FD %d\n",
           client_details[a].client_id, client_details[a].cfd);

    int *arg = malloc(sizeof(int));
    *arg = client_id_count - 1;

    pthread_t pid;
    if (pthread_create(&pid, NULL, client_function, arg) != 0) {
      handle_error("pthread_create");
    }

    pthread_detach(pid);
  }

  for (int i = 0; i < max_clients; i++) {
    if (close(info[i].cfd) == -1)
      handle_error("Close in type 1");
    info[i].cfd = -1;
  }

  if (close(sfd) == -1)
    handle_error("close");

  exit(0);
}
