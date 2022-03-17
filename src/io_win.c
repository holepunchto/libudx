#include "io.h"

ssize_t
udx__sendmsg (udx_t *handle, udx_packet_t *pkt) {
  DWORD bytes;

  pkt->time_sent = uv_hrtime() / 1e6;

  int result = WSASendTo(
    handle->socket,
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
udx__recvmsg (udx_t *handle, uv_buf_t *buf, struct sockaddr *addr) {
  DWORD bytes, flags = 0;

  int result = WSARecvFrom(
    handle->socket,
    (WSABUF *) &buf,
    1,
    &bytes,
    &flags,
    addr,
    sizeof(*addr),
    NULL,
    NULL
  );

  if (result != 0) {
    return uv_translate_sys_error(WSAGetLastError());
  }

  return bytes;
}
