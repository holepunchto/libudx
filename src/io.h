#ifndef UDX_IO_H
#define UDX_IO_H

#include "../include/udx.h"

int
udx__get_link_mtu (const struct sockaddr *s);

int
udx__udp_set_dontfrag (uv_os_sock_t fd, bool is_ipv6);

#endif // UDX_IO_H
