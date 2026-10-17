#ifndef UDX_IO_H
#define UDX_IO_H

#include "../include/udx.h"

int
udx__get_link_mtu (const struct sockaddr *s);

int
udx__udp_set_dontfrag (uv_os_sock_t fd, bool is_ipv6);

int
udx__get_socket_ttl (udx_socket_t *socket);

void
udx__platform_init (udx_t *udx);

void
udx__platform_uninit (udx_t *udx);

// max credit (ms) the pacer may accumulate when a send is late.
// should cover the platform's timer granularity
extern const uint64_t udx__pacing_max_credit_ms;

#endif // UDX_IO_H
