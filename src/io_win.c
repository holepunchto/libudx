#include "io.h"

ssize_t
udx__sendmsg (udx_t *self, udx_packet_t *pkt) {
  DWORD bytes;

  pkt->time_sent = uv_hrtime() / 1e6;

  int result = WSASendTo(
    self->handle.socket,
    (WSABUF *) &(pkt->bufs),
    pkt->bufs_len,
    &bytes,
    0,
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
udx__recvmsg (udx_t *self, uv_buf_t *buf, struct sockaddr *addr) {
  DWORD bytes, flags = 0;

  int addr_len = sizeof(*addr);

  int result = WSARecvFrom(
    self->handle.socket,
    (WSABUF *) &buf,
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
