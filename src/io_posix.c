#define _GNU_SOURCE

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

int
udx__udp_set_dontfrag (uv_os_sock_t fd, bool is_ipv6) {
  int val = 1;
  int rc;
  if (is_ipv6) {
    rc = setsockopt(fd, IPPROTO_IPV6, IPV6_DONTFRAG, &val, sizeof(val));
  } else {
    rc = setsockopt(fd, IPPROTO_IP, IP_DONTFRAG, &val, sizeof(val));
  }

  return rc;
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

int
udx__udp_set_dontfrag (uv_os_sock_t fd, bool is_ipv6) {
  int rc;
  if (is_ipv6) {
    int val = IPV6_PMTUDISC_PROBE;
    rc = setsockopt(fd, IPPROTO_IPV6, IPV6_MTU_DISCOVER, &val, sizeof(val));
  } else {
    int val = IP_PMTUDISC_PROBE;
    rc = setsockopt(fd, IPPROTO_IP, IP_MTU_DISCOVER, &val, sizeof(val));
  }

  return rc;
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

#if defined(__linux__)

  if (size != -1 && h.msg_controllen) {

    // relies on SO_RXQ_OVFL being set
    uint32_t packets_dropped_by_kernel = 0;

    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&h); cmsg != NULL; cmsg = CMSG_NXTHDR(&h, cmsg)) {
      if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SO_RXQ_OVFL) {
        memcpy(&packets_dropped_by_kernel, CMSG_DATA(cmsg), sizeof(packets_dropped_by_kernel));
      }
    }

    if (packets_dropped_by_kernel) {
      uint32_t delta = packets_dropped_by_kernel - handle->packets_dropped_by_kernel;
      handle->udx->packets_dropped_by_kernel += delta;
      handle->packets_dropped_by_kernel = packets_dropped_by_kernel;
    }
  }

#endif

  return size == -1 ? uv_translate_sys_error(errno) : size;
}

#if defined(__linux__)
int
udx__udp_set_rxq_ovfl (uv_os_sock_t fd) {
  int on = 1;
  return setsockopt(fd, SOL_SOCKET, SO_RXQ_OVFL, &on, sizeof(on));
}
#else
int
udx__udp_set_rxq_ovfl (uv_os_sock_t fd) {
  UDX_UNUSED(fd);
  return -1;
}
#endif
