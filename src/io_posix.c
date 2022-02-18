#include "io.h"

ssize_t
udx__sendmsg(udx_t *self, udx_packet_t *pkt) {
  ssize_t size;
  struct msghdr h;

  memset(&h, 0, sizeof(h));

  h.msg_name = &(pkt->dest);
  h.msg_namelen = sizeof(pkt->dest);

  h.msg_iov = (struct iovec *) &(pkt->buf);
  h.msg_iovlen = pkt->buf[1].len ? 2 : 1;

  do {
    pkt->time_sent = uv_hrtime() / 1e6;
    size = sendmsg(self->io_poll.io_watcher.fd, &h, 0);
  } while (size == -1 && errno == EINTR);

  return size;
}

ssize_t
udx__recvmsg(udx_t *self, uv_buf_t *buf, struct sockaddr *addr) {
  ssize_t size;
  struct msghdr h;

  memset(&h, 0, sizeof(h));

  h.msg_name = addr;
  h.msg_namelen = sizeof(*addr);

  h.msg_iov = (struct iovec *) buf;
  h.msg_iovlen = 1;

  do {
    size = recvmsg(self->io_poll.io_watcher.fd, &h, 0);
  } while (size == -1 && errno == EINTR);

  return size;
}
