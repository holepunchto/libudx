#include <assert.h>
#include <uv.h>

#include "fifo.h"
#include "internal.h"
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

void
udx__on_writable (udx_socket_t *socket) {
  while (socket->send_queue.len > 0) {
    udx_packet_t *pkt = (udx_packet_t *) udx__fifo_shift(&(socket->send_queue));
    if (pkt == NULL) continue;

    bool adjust_ttl = pkt->ttl > 0 && socket->ttl != pkt->ttl;

    if (adjust_ttl) uv_udp_set_ttl((uv_udp_t *) socket, pkt->ttl);

    if (socket->family == 6 && pkt->dest.ss_family == AF_INET) {
      addr_to_v6((struct sockaddr_in *) &(pkt->dest));
      pkt->dest_len = sizeof(struct sockaddr_in6);
    }

    ssize_t size = udx__sendmsg(socket, pkt->bufs, pkt->bufs_len, (struct sockaddr *) &(pkt->dest), pkt->dest_len);

    if (adjust_ttl) uv_udp_set_ttl((uv_udp_t *) socket, socket->ttl);

    if (size == UV_EAGAIN) {
      udx__fifo_undo(&(socket->send_queue));
      break;
    }

    assert(pkt->status == UDX_PACKET_SENDING);
    pkt->status = UDX_PACKET_INFLIGHT;
    pkt->transmits++;
    pkt->time_sent = uv_hrtime() / 1e6;

    int type = pkt->type;

    if (type & UDX_PACKET_CALLBACK) {
      udx__trigger_send_callback(socket, pkt);
      // TODO: watch for re-entry here!
    }

    if (type & UDX_PACKET_FREE_ON_SEND) {
      free(pkt);
    }

    // queue another write, might be able to do this smarter...
    if (socket->send_queue.len > 0) continue;

    // if the socket is under closure, we need to trigger shutdown now since no important writes are pending
    if (socket->status & UDX_SOCKET_CLOSING) {
      udx__close_handles(socket);
      return;
    }
  }
}
