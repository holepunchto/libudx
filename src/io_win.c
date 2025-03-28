#include <assert.h>
#include <uv.h>

#include "internal.h"
#include "io.h"

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
  int mtu_opt_size = sizeof mtu;

  if (addr->sa_family == AF_INET) {
    rc = getsockopt(s, IPPROTO_IP, IP_MTU, (char *) &mtu, &mtu_opt_size);
  } else {
    rc = getsockopt(s, IPPROTO_IPV6, IPV6_MTU, (char *) &mtu, &mtu_opt_size);
  }
  if (rc == -1) {
    closesocket(s);
    return -1;
  }

  closesocket(s);
  return mtu;
}

ssize_t
udx__sendmsg (udx_socket_t *socket, const uv_buf_t bufs[], unsigned int bufs_len, struct sockaddr *addr, int addr_len) {
  DWORD bytes, flags = 0;

  int result = WSASendTo(
    socket->handle.socket,
    (WSABUF *) bufs,
    bufs_len,
    &bytes,
    flags,
    addr,
    addr_len,
    NULL,
    NULL
  );

  if (result != 0) {
    return uv_translate_sys_error(WSAGetLastError());
  }

  return bytes;
}

ssize_t
udx__recvmsg (udx_socket_t *socket, uv_buf_t *buf, struct sockaddr *addr, int addr_len) {
  DWORD bytes, flags = 0;

  int result = WSARecvFrom(
    socket->handle.socket,
    (WSABUF *) buf,
    1,
    &bytes,
    &flags,
    addr,
    &addr_len,
    NULL,
    NULL
  );

  if (result != 0) {
    return uv_translate_sys_error(WSAGetLastError());
  }

  return bytes;
}

int
udx__udp_set_rxq_ovfl (uv_os_sock_t fd) {
  UDX_UNUSED(fd);
  return -1;
}

int
udx__udp_set_dontfrag (uv_os_sock_t fd, bool is_ipv6) {
  int rc;
  int val = IP_PMTUDISC_PROBE;
  if (is_ipv6) {
    rc = setsockopt(fd, IPPROTO_IPV6, IPV6_MTU_DISCOVER, &val, sizeof(val));
  } else {
    rc = setsockopt(fd, IPPROTO_IP, IP_MTU_DISCOVER, &val, sizeof(val));
  }

  return rc;
}
