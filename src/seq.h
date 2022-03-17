#ifndef UDX_SEQ_H
#define UDX_SEQ_H

#include "../include/udx.h"

uint32_t
udx__seq_add (uint32_t a, uint32_t b);

uint32_t
udx__seq_inc (uint32_t seq);

int32_t
udx__seq_diff (uint32_t a, uint32_t b);

int
udx__seq_compare (uint32_t a, uint32_t b);

#endif // UDX_SEQ_H
