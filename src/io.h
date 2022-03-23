#ifndef UDX_IO_H
#define UDX_IO_H

#include "../include/udx.h"

ssize_t
udx__sendmsg (udx_t *handle, udx_packet_t *pkt);

ssize_t
udx__recvmsg (udx_t *handle, uv_buf_t *buf, struct sockaddr *addr, int addr_len);

#endif // UDX_IO_H
