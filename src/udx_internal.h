#ifndef __udx__internal_h__
#define __udx__internal_h__

#include "../include/udx.h"

#define UDX_PACKET_CALLBACK     (UDX_PACKET_STREAM_SEND | UDX_PACKET_STREAM_DESTROY | UDX_PACKET_SEND)
#define UDX_PACKET_FREE_ON_SEND (UDX_PACKET_STREAM_STATE | UDX_PACKET_STREAM_DESTROY)

void addr_to_v6(struct sockaddr_in *addr);
void trigger_send_callback(udx_socket_t *socket, udx_packet_t *packet);
void close_handles(udx_socket_t *socket);

#endif
