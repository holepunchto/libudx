#include <assert.h>

#include "../include/udx.h"

int
main () {
  int e;

  uv_loop_t loop;
  uv_loop_init(&loop);

  udx_t udx;
  e = udx_init(&loop, &udx);
  assert(e == 0);

  udx_stream_t stream;
  e = udx_stream_init(&udx, &stream, 1);
  assert(e == 0);

  e = udx_stream_destroy(&stream);
  assert(e == 0);

  uv_loop_close(&loop);

  return 0;
}
