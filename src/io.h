#ifndef UDX_IO_H
#define UDX_IO_H

#include "../include/udx.h"

int
udx__get_link_mtu (const struct sockaddr *s);

int
udx__udp_set_dontfrag (uv_os_sock_t fd, bool is_ipv6);

int
udx__get_socket_ttl (udx_socket_t *socket);

#endif // UDX_IO_H
