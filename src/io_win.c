#include <uv.h>
#include "io.h"

int udx__sendmmsg(udx_socket_t *socket, udx_packet_t *pkts[], unsigned int pkts_len) {
  int npkts = 0;

  for (; npkts < pkts_len; npkts++) {
    udx_packet_t *pkt = pkts[npkts];
    if (pkt == NULL) continue;

    bool adjust_ttl = pkt->ttl > 0 && socket->ttl != pkt->ttl;

    if (adjust_ttl) uv_udp_set_ttl((uv_udp_t *) socket, pkt->ttl);

    int rc = udx__sendmsg(socket, pkt->bufs, pkt->bufs_len, (struct sockaddr *) &(pkt->dest), pkt->dest_len);

    if (adjust_ttl) uv_udp_set_ttl((uv_udp_t *) socket, socket->ttl);

    if (rc < 0) {
      return npkts > 0 ? npkts : rc; /* if it fails on the first call (npkts == 0) return rc to mimic sendmmsg */
    }
  }

  return npkts;
}
ssize_t
udx__sendmsg (udx_socket_t *handle, const uv_buf_t bufs[], unsigned int bufs_len, struct sockaddr *addr, int addr_len) {
  DWORD bytes, flags = 0;

  int result = WSASendTo(
    handle->socket.socket,
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
udx__recvmsg (udx_socket_t *handle, uv_buf_t *buf, struct sockaddr *addr, int addr_len) {
  DWORD bytes, flags = 0;

  int result = WSARecvFrom(
    handle->socket.socket,
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
