#include "../include/udx.h"
#include "helpers.h"
#include <stdlib.h>

udx_stream_write_t *
allocate_write (int nwbufs) {
  udx_stream_write_t *ret = calloc(1, sizeof(udx_stream_write_t) + sizeof(udx_stream_write_buf_t) * nwbufs);
  ret->nwbufs = nwbufs;
}
