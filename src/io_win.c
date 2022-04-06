#include <uv.h>

#include "io.h"

ssize_t
udx__sendmsg (udx_t *self, udx_packet_t *pkt) {
  DWORD bytes, flags = 0;

  pkt->time_sent = uv_hrtime() / 1e6;

  int result = WSASendTo(
    self->socket.socket,
    (WSABUF *) &(pkt->bufs),
    pkt->bufs_len,
    &bytes,
    flags,
    &(pkt->dest),
    sizeof(pkt->dest),
    NULL,
    NULL
  );

  if (result != 0) {
    return uv_translate_sys_error(WSAGetLastError());
  }

  return bytes;
}

ssize_t
udx__recvmsg (udx_t *self, uv_buf_t *buf, struct sockaddr *addr, int addr_len) {
  DWORD bytes, flags = 0;

  int result = WSARecvFrom(
    self->socket.socket,
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
