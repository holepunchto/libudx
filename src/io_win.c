#include <uv.h>

#include "io.h"

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
