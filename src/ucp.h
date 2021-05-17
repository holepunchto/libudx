#include <uv.h>
#include "fifo.h"

typedef struct ucp {
  uv_udp_t handle;
  uv_poll_t io_poll;
  uv_loop_t *loop;
  ucp_fifo_t send_queue;

  int bound;

  struct sockaddr_in on_message_addr;
  void (*on_message)(struct ucp *self, const char *buf, ssize_t buf_len, const struct sockaddr_in *from);
} ucp_t;

typedef struct {
  struct msghdr h;
  struct iovec buf;
  struct sockaddr dest;
} ucp_send_t;

enum UCP_TYPE {
  UCP_ON_MESSAGE = 1
};

int
ucp_init (ucp_t *self, uv_loop_t *loop);

int
ucp_bind (ucp_t *self, const struct sockaddr *addr);

int
ucp_send (ucp_t *self, ucp_send_t *req, const char *buf, size_t buf_len, const struct sockaddr *addr);

int
ucp_set_callback(ucp_t *self, enum UCP_TYPE name, void *fn);
