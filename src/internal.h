#ifndef UDX_INTERNAL_H
#define UDX_INTERNAL_H

#include "../include/udx.h"

#define UDX_PACKET_CALLBACK     (UDX_PACKET_TYPE_STREAM_SEND | UDX_PACKET_TYPE_STREAM_DESTROY | UDX_PACKET_TYPE_SOCKET_SEND)
#define UDX_PACKET_FREE_ON_SEND (UDX_PACKET_TYPE_STREAM_STATE | UDX_PACKET_TYPE_STREAM_DESTROY | UDX_PACKET_TYPE_STREAM_RELAY)

#define UDX_UNUSED(x) ((void) (x))

static inline void
addr_to_v6 (struct sockaddr_in *addr) {
  struct sockaddr_in6 in;
  memset(&in, 0, sizeof(in));

  in.sin6_family = AF_INET6;
  in.sin6_port = addr->sin_port;
#ifdef SIN6_LEN
  in.sin6_len = sizeof(struct sockaddr_in6);
#endif

  in.sin6_addr.s6_addr[10] = 0xff;
  in.sin6_addr.s6_addr[11] = 0xff;

  // Copy the IPv4 address to the last 4 bytes of the IPv6 address.
  memcpy(&(in.sin6_addr.s6_addr[12]), &(addr->sin_addr), 4);

  memcpy(addr, &in, sizeof(in));
}

void
udx__close_handles (udx_socket_t *socket);

// TODO: hmm..

static inline bool
is_addr_v4_mapped (const struct sockaddr *addr) {
  return addr->sa_family == AF_INET6 && IN6_IS_ADDR_V4MAPPED(&(((struct sockaddr_in6 *) addr)->sin6_addr));
}

static inline void
addr_to_v4 (struct sockaddr_in6 *addr) {
  struct sockaddr_in in;
  memset(&in, 0, sizeof(in));

  in.sin_family = AF_INET;
  in.sin_port = addr->sin6_port;
#ifdef SIN6_LEN
  in.sin_len = sizeof(struct sockaddr_in);
#endif

  // Copy the IPv4 address from the last 4 bytes of the IPv6 address.
  memcpy(&(in.sin_addr), &(addr->sin6_addr.s6_addr[12]), 4);

  memcpy(addr, &in, sizeof(in));
}

#ifdef THREADED_DRAIN

int
process_packet (udx_socket_t *socket, char *buf, ssize_t buf_len, struct sockaddr *addr);

#endif

#endif // UDX_INTERNAL_H
