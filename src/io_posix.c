#define _GNU_SOURCE

#if defined(__APPLE__)
#define __APPLE_USE_RFC_3542
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

int
udx__get_socket_ttl (udx_socket_t *socket) {
  uv_os_fd_t fd;
  uv_fileno((uv_handle_t *) &socket->uv_udp, &fd);

  int ttl;
  socklen_t ttl_opt_size = sizeof ttl;
  int rc;
  if (socket->family == 4) {
    rc = getsockopt((int) fd, IPPROTO_IP, IP_TTL, &ttl, &ttl_opt_size);
  } else {
    rc = getsockopt((int) fd, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &ttl, &ttl_opt_size);
  }

  if (rc == -1) {
    return -1;
  }

  return ttl;
}

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
