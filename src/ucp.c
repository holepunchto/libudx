#include <uv.h>
#include <stdio.h>
#include <stdlib.h>
#include "ucp.h"
#include "fifo.h"

#define UCP_POLL_FLAGS(self) \
  (self->send_queue.len > 0 ? UV_WRITABLE : 0) | (self->on_message == NULL ? 0 : UV_READABLE)

static void
on_uv_poll (uv_poll_t *handle, int status, int events) {
  ucp_t *self = handle->data;
  uv_poll_t *poll = &(self->io_poll);

  if (self->send_queue.len > 0 && events & UV_WRITABLE) {
    ssize_t size;
    const struct msghdr *h = (const struct msghdr *) ucp_fifo_shift(&(self->send_queue));
    const struct sockaddr_in *d = h->msg_name;

    do {
      size = sendmsg(handle->io_watcher.fd, h, 0);
    } while (size == -1 && errno == EINTR);

    // queue another write, might be able to do this smarter...
    if (self->send_queue.len > 0) return;
  }

  if (events & UV_READABLE) {
    ssize_t size;
    struct msghdr h;
    struct iovec buf;

    char b[4096];
    buf.iov_base = &b;
    buf.iov_len = 4096;

    h.msg_name = &(self->on_message_addr);
    h.msg_namelen = sizeof(struct sockaddr_in);
    h.msg_iov = &buf;
    h.msg_iovlen = 1;

    do {
      size = recvmsg(handle->io_watcher.fd, &h, 0);
    } while (size == -1 && errno == EINTR);

    if (self->on_message != NULL) {
      self->on_message(self, b, size, h.msg_name);
      return;
    }
  }

  printf("update polling\n");
  uv_poll_start(poll, UCP_POLL_FLAGS(self), on_uv_poll);
}

int
ucp_init (ucp_t *self, uv_loop_t *loop) {
  int err;

  self->bound = 0;
  self->loop = loop;
  self->on_message = NULL;

  ucp_fifo_init(&(self->send_queue), 16);

  err = uv_udp_init(loop, &(self->handle));
  return err;
}

int
ucp_bind (ucp_t *self, const struct sockaddr *addr) {
  int err;

  if (self->bound) return -1;

  uv_udp_t *handle = &(self->handle);
  uv_poll_t *poll = &(self->io_poll);
  uv_os_fd_t fd;

  err = uv_udp_bind(handle, addr, 0);
  if (err) return err;

  err = uv_fileno((const uv_handle_t *) handle, &fd);
  if (err) return err;

  err = uv_poll_init(self->loop, poll, fd);
  if (err) return err;

  self->bound = 1;
  poll->data = self;

  err = uv_poll_start(poll, UCP_POLL_FLAGS(self), on_uv_poll);
  return err;
}

int
ucp_send (ucp_t *self, ucp_send_t *req, const char *buf, size_t buf_len, const struct sockaddr *dest) {
  int err;

  memset(req, 0, sizeof(struct msghdr));

  req->dest = *dest;
  req->buf.iov_base = (void *) buf;
  req->buf.iov_len = buf_len;

  req->h.msg_name = &(req->dest);
  req->h.msg_namelen = sizeof(struct sockaddr_in);
  req->h.msg_iov = &(req->buf);
  req->h.msg_iovlen = 1;

  ucp_fifo_push(&(self->send_queue), req);

  err = uv_poll_start(&(self->io_poll), UCP_POLL_FLAGS(self), on_uv_poll);

  return err;
}

int
ucp_set_callback (ucp_t *self, enum UCP_TYPE name, void *fn) {
  switch (name) {
    case UCP_ON_MESSAGE: {
      self->on_message = fn;
      return self->bound ? uv_poll_start(&(self->io_poll), UCP_POLL_FLAGS(self), on_uv_poll) : 0;
    }
  }

  return -1;
}
