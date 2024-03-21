#ifndef TEST_HELPERS_H
#define TEST_HELPERS_H

#define HASH_INIT 5381

static inline uint64_t
hash (uint64_t prev, uint8_t *data, int len) {
  uint64_t hash = prev;

  for (int i = 0; i < len; i++) {
    hash = ((hash << 5) + hash) + data[i];
  }

  return hash;
}

udx_stream_write_t *
allocate_write (int nbufs);

#endif // TEST_HELPERS_H
