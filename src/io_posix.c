#define _GNU_SOURCE

#if defined(__linux__) || defined(__FreeBSD__)
#define UDX_PLATFORM_HAS_SENDMMSG
#endif

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <uv.h>

#include "../include/udx.h"
#include "internal.h"
#include "io.h"

#if defined(__APPLE__)

int
udx__get_link_mtu (const struct sockaddr *addr) {
  UDX_UNUSED(addr);
  return -1;
}

#else

int
udx__get_link_mtu (const struct sockaddr *addr) {
  assert(addr->sa_family == AF_INET || addr->sa_family == AF_INET6);

  int s = socket(addr->sa_family, SOCK_DGRAM, 0);
  if (s == -1) {
    return -1;
  }

  int rc = connect(s, addr, addr->sa_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6));

  if (rc == -1) {
    return -1;
  }

  int mtu;
  socklen_t mtu_opt_size = sizeof mtu;
  if (addr->sa_family == AF_INET) {
    rc = getsockopt(s, IPPROTO_IP, IP_MTU, &mtu, &mtu_opt_size);
  } else {
    rc = getsockopt(s, IPPROTO_IPV6, IPV6_MTU, &mtu, &mtu_opt_size);
  }
  if (rc == -1) {
    close(s);
    return -1;
  }

  close(s);
  return mtu;
}
#endif

ssize_t
udx__sendmsg (udx_socket_t *handle, const uv_buf_t bufs[], unsigned int bufs_len, struct sockaddr *addr, int addr_len) {
  ssize_t size;
  struct msghdr h;

  memset(&h, 0, sizeof(h));

  h.msg_name = addr;
  h.msg_namelen = addr_len;

  h.msg_iov = (struct iovec *) bufs;
  h.msg_iovlen = bufs_len;

  do {
    size = sendmsg(handle->io_poll.io_watcher.fd, &h, 0);
  } while (size == -1 && errno == EINTR);

  return size == -1 ? uv_translate_sys_error(errno) : size;
}

ssize_t
udx__recvmsg (udx_socket_t *handle, uv_buf_t *buf, struct sockaddr *addr, int addr_len) {
  ssize_t size;
  struct msghdr h;

  memset(&h, 0, sizeof(h));

  h.msg_name = addr;
  h.msg_namelen = addr_len;

  h.msg_iov = (struct iovec *) buf;
  h.msg_iovlen = 1;

  union {
    struct cmsghdr align;
    uint8_t buf[2048];
  } u;

  h.msg_control = u.buf;
  h.msg_controllen = sizeof(u.buf);

  do {
    size = recvmsg(handle->io_poll.io_watcher.fd, &h, 0);
  } while (size == -1 && errno == EINTR);

  // relies on SO_RXQ_OVFL being set
  uint32_t npackets_dropped_since_last_recv = 0;
  uint32_t ttl = 0;

  for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&h); cmsg != NULL; cmsg = CMSG_NXTHDR(&h, cmsg)) {
    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SO_RXQ_OVFL) {
      npackets_dropped_since_last_recv = *(uint32_t *) CMSG_DATA(cmsg);
    }
    if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_TTL) {
      ttl = *(int *) CMSG_DATA(cmsg);
    }
  }

  return size == -1 ? uv_translate_sys_error(errno) : size;
}

#if defined(__linux__)
int
udx__udp_set_rxq_ovfl (int fd) {
  int on = 1;
  return setsockopt(fd, SOL_SOCKET, SO_RXQ_OVFL, &on, sizeof(on));
}
#else
int
udx__udp_set_rxq_ovfl (int fd) {
  return -1;
}
#endif
