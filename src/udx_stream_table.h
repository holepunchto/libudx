#ifndef udx_stream_table_h_INCLUDED
#define udx_stream_table_h_INCLUDED

#include <stdint.h>

#include "../include/udx.h"

udx_stream_entry_t *
udx_stream_entry_get (udx_t *udx, uint32_t id);

// return entry that was replaced or NULL if no prior entry
// new->id must be set
udx_stream_entry_t *
udx_stream_entry_set (udx_t *udx, udx_stream_entry_t *new);

// return entry that was removed or NULL if no entry found
udx_stream_entry_t *
udx_stream_entry_remove (udx_t *udx, uint32_t id);

#endif // udx_stream_table_h_INCLUDED
