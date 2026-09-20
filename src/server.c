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

/* write(2) is not guaranteed to send the whole buffer in one call (short
 * writes are common once the socket buffer fills up). Loop until every byte
 * is sent, or the connection is genuinely broken. */
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
 * The original protocol delimited messages with '\n', but the body itself
 * contains binary IP/port bytes that can legitimately contain 0x0A, which
 * desynchronized the parser. A length prefix removes the ambiguity. */
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

int client_id_count = 0;
int type1_count = 0;
int max_client;
struct client_info *info = NULL;
// pthread_mutex_t client_id = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t writing_all_clients = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t type1 = PTHREAD_MUTEX_INITIALIZER;

/* Broadcasting only reaches whoever is registered in info[] *at the moment
 * of the broadcast* — there is no message history/replay. Without this
 * gate, a client that connects (or is simply scheduled) a moment late would
 * silently miss messages sent by faster clients before it joined. All
 * client threads block here until every expected client has connected, so
 * the group is complete before any message can be sent. Paired with
 * writing_all_clients (the mutex that already guards client_id_count) so
 * every read/write of that counter goes through the same lock. */
pthread_cond_t all_connected_cv = PTHREAD_COND_INITIALIZER;

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

  pthread_mutex_lock(&writing_all_clients);
  while (client_id_count < max_client)
    pthread_cond_wait(&all_connected_cv, &writing_all_clients);
  pthread_mutex_unlock(&writing_all_clients);

  uint8_t type;
  char print_buf[1024];
  char msg_buf[1024];
  size_t msg_len;
  int rc;

  while ((rc = read_frame(cfd, msg_buf, sizeof(msg_buf), &msg_len)) > 0) {

    if (msg_len < 1)
      handle_error("Empty message received by server");
    type = msg_buf[0];
    if (type == 0) {
      char send_buf[1024];
      send_buf[0] = (uint8_t)0;
      memcpy(send_buf + 1, &ip, sizeof(uint32_t));
      memcpy(send_buf + 5, &port, sizeof(uint16_t));
      memcpy(send_buf + 7, msg_buf + 1, msg_len - 1);
      size_t send_len = msg_len + 6;

      pthread_mutex_lock(&writing_all_clients);
      for (int i = 0; i < client_id_count; i++) {
        if (info[i].cfd != -1) {
          if (write_frame(info[i].cfd, send_buf, send_len) != 0)
            handle_error("write to client");
        }
      }
      memcpy(print_buf, msg_buf + 1, msg_len - 1);
      print_buf[msg_len - 1] = '\0';
      pthread_mutex_unlock(&writing_all_clients);
      printf("MSG from client %d: %s", client_id, print_buf);

    } else if (type == 1) {

      pthread_mutex_lock(&type1);
      type1_count++;
      if (type1_count == max_client) {
        // Wait for any chat broadcast currently in flight on another
        // client thread to finish before sending termination, otherwise
        // exit() below could kill the process mid-write and drop a
        // message for the other clients.
        pthread_mutex_lock(&writing_all_clients);
        for (int i = 0; i < max_client; i++) {
          char end_msg[2];
          end_msg[0] = (uint8_t)1;
          end_msg[1] = '\n';
          write_frame(info[i].cfd, &end_msg, sizeof(end_msg));
        }
        pthread_mutex_unlock(&writing_all_clients);
        pthread_mutex_unlock(&type1);
        printf("Ending connection for all client\n");
        fflush(stdout);
        exit(0);
      }
      pthread_mutex_unlock(&type1);
    } else {
      handle_error("Issue type received by server");
    }
  }

  if (rc < 0)
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
    if (client_id_count == max_clients)
      pthread_cond_broadcast(&all_connected_cv);
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
