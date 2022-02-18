#ifndef UDX_IO_H
#define UDX_IO_H

#include "../include/udx.h"

ssize_t
udx__sendmsg(udx_t *self, udx_packet_t *pkt);

ssize_t
udx__recvmsg(udx_t *self, uv_buf_t *buf, struct sockaddr *addr);

#endif // UDX_IO_H
