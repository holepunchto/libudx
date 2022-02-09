#include <node_api.h>
#include <napi-macros.h>
#include <uv.h>

#include "include/udx.h"

#define UDX_NAPI_THROW(err) \
  { \
    napi_throw_error(env, uv_err_name(err), uv_strerror(err)); \
    return NULL; \
  }

#define UDX_NAPI_INTERACTIVE 0
#define UDX_NAPI_NON_INTERACTIVE 1

#define UDX_NAPI_CALLBACK(self, fn, src) \
  napi_env env = self->env; \
  napi_handle_scope scope; \
  napi_open_handle_scope(env, &scope); \
  napi_value ctx; \
  napi_get_reference_value(env, self->ctx, &ctx); \
  napi_value callback; \
  napi_get_reference_value(env, fn, &callback); \
  src \
  napi_close_handle_scope(env, scope);

#define UDX_NAPI_MAKE_ALLOC_CALLBACK(self, env, nil, ctx, cb, n, argv, res) \
  if (napi_make_callback(env, nil, ctx, cb, n, argv, &res) == napi_pending_exception) { \
    napi_value fatal_exception; \
    napi_get_and_clear_last_exception(env, &fatal_exception); \
    napi_fatal_exception(env, fatal_exception); \
    printf("oh no add that realloc\n"); \
  } else { \
    napi_get_buffer_info(env, ret, (void **) &(self->read_buf), &(self->read_buf_free)); \
    self->read_buf_head = self->read_buf; \
  }

typedef struct {
  udx_t udx;

  napi_env env;
  napi_ref ctx;
  napi_ref on_send;
  napi_ref on_message;
  napi_ref on_close;
} udx_napi_t;

typedef struct {
  udx_stream_t stream;

  char *read_buf;
  char *read_buf_head;
  size_t read_buf_free;

  int mode;

  napi_env env;
  napi_ref ctx;
  napi_ref on_data;
  napi_ref on_end;
  napi_ref on_drain;
  napi_ref on_ack;
  napi_ref on_close;
} udx_napi_stream_t;

inline static void
parse_address (struct sockaddr *name, char *ip, int *port) {
  struct sockaddr_in *name_in = (struct sockaddr_in *) name;
  *port = ntohs(name_in->sin_port);
  uv_ip4_name(name_in, ip, 17);
}

static void
on_udx_send (udx_t *self, udx_send_t *req, int failed) {
  udx_napi_t *n = (udx_napi_t *) self;

  UDX_NAPI_CALLBACK(n, n->on_send, {
    napi_value argv[2];
    napi_create_int32(env, req->userid, &(argv[0]));
    napi_create_int32(env, failed, &(argv[1]));
    NAPI_MAKE_CALLBACK(env, NULL, ctx, callback, 2, argv, NULL)
  })
}

static void
on_udx_message (udx_t *self, const char *buf, size_t buf_len, const struct sockaddr *from) {
  udx_napi_t *n = (udx_napi_t *) self;

  int port;
  char ip[17];
  parse_address((struct sockaddr *) from, ip, &port);

  UDX_NAPI_CALLBACK(n, n->on_message, {
    napi_value argv[3];
    napi_create_buffer_copy(n->env, buf_len, buf, NULL, &(argv[0]));
    napi_create_uint32(env, port, &(argv[1]));
    napi_create_string_utf8(env, ip, NAPI_AUTO_LENGTH, &(argv[2]));
    NAPI_MAKE_CALLBACK(env, NULL, ctx, callback, 3, argv, NULL)
  })
}

static void
on_udx_close (udx_t *self) {
  udx_napi_t *n = (udx_napi_t *) self;

  UDX_NAPI_CALLBACK(n, n->on_close, {
    NAPI_MAKE_CALLBACK(env, NULL, ctx, callback, 0, NULL, NULL)
  })
}

static void
on_udx_stream_data (udx_stream_t *stream, const char *buf, const size_t buf_len) {
  udx_napi_stream_t *n = (udx_napi_stream_t *) stream;

  memcpy(n->read_buf_head, buf, buf_len);

  n->read_buf_head += buf_len;
  n->read_buf_free -= buf_len;

  if (n->mode == UDX_NAPI_NON_INTERACTIVE && n->read_buf_free >= UDX_MTU) {
    return;
  }

  size_t read = n->read_buf_head - n->read_buf;

  UDX_NAPI_CALLBACK(n, n->on_data, {
    napi_value ret;
    napi_value argv[1];
    napi_create_uint32(env, read, &(argv[0]));
    UDX_NAPI_MAKE_ALLOC_CALLBACK(n, env, NULL, ctx, callback, 1, argv, ret)
  })
}

static void
on_udx_stream_end (udx_stream_t *stream) {
  udx_napi_stream_t *n = (udx_napi_stream_t *) stream;

  size_t read = n->read_buf_head - n->read_buf;

  UDX_NAPI_CALLBACK(n, n->on_end, {
    napi_value argv[1];
    napi_create_uint32(env, read, &(argv[0]));
    NAPI_MAKE_CALLBACK(env, NULL, ctx, callback, 1, argv, NULL)
  })
}

static void
on_udx_stream_drain (udx_stream_t *stream) {
  udx_napi_stream_t *n = (udx_napi_stream_t *) stream;

  UDX_NAPI_CALLBACK(n, n->on_drain, {
    NAPI_MAKE_CALLBACK(env, NULL, ctx, callback, 0, NULL, NULL)
  })
}

static void
on_udx_stream_ack (udx_stream_t *stream, udx_write_t *req, int status, int unordered) {
  udx_napi_stream_t *n = (udx_napi_stream_t *) stream;

  UDX_NAPI_CALLBACK(n, n->on_ack, {
    napi_value argv[1];
    napi_create_uint32(env, req->userid, &(argv[0]));
    NAPI_MAKE_CALLBACK(env, NULL, ctx, callback, 1, argv, NULL)
  })
}

static void
on_udx_stream_close (udx_stream_t *stream, int hard_close) {
  udx_napi_stream_t *n = (udx_napi_stream_t *) stream;

  UDX_NAPI_CALLBACK(n, n->on_close, {
    napi_value argv[1];
    napi_create_uint32(env, hard_close, &(argv[0]));
    NAPI_MAKE_CALLBACK(env, NULL, ctx, callback, 1, argv, NULL)
  })
}

NAPI_METHOD(udx_napi_init) {
  NAPI_ARGV(5)
  NAPI_ARGV_BUFFER_CAST(udx_napi_t *, self, 0)

  udx_t *udx = (udx_t *) self;

  self->env = env;
  napi_create_reference(env, argv[1], 1, &(self->ctx));
  napi_create_reference(env, argv[2], 1, &(self->on_send));
  napi_create_reference(env, argv[3], 1, &(self->on_message));
  napi_create_reference(env, argv[4], 1, &(self->on_close));

  struct uv_loop_s *loop;
  napi_get_uv_event_loop(env, &loop);

  int err = udx_init(udx, loop);
  if (err < 0) UDX_NAPI_THROW(err)

  udx_set_on_send(udx, on_udx_send);
  udx_set_on_message(udx, on_udx_message);
  udx_set_on_close(udx, on_udx_close);

  return NULL;
}

NAPI_METHOD(udx_napi_bind) {
  NAPI_ARGV(3)
  NAPI_ARGV_BUFFER_CAST(udx_t *, self, 0)
  NAPI_ARGV_UINT32(port, 1)
  NAPI_ARGV_UTF8(ip, 17, 2)

  struct sockaddr_in addr;
  int err = uv_ip4_addr(ip, port, &addr);
  if (err < 0) UDX_NAPI_THROW(err)

  err = udx_bind(self, (const struct sockaddr *) &addr);
  if (err < 0) UDX_NAPI_THROW(err)

  // TODO: move the bottom stuff into another function, start, so error handling is easier

  struct sockaddr name;
  int name_len = sizeof(name);

  // wont error in practice
  err = udx_getsockname(self, &name, &name_len);
  if (err < 0) UDX_NAPI_THROW(err)

  struct sockaddr_in *name_in = (struct sockaddr_in *) &name;
  int local_port = ntohs(name_in->sin_port);

  // wont error in practice
  err = udx_read_start(self);
  if (err < 0) UDX_NAPI_THROW(err)

  NAPI_RETURN_UINT32(local_port)
}

NAPI_METHOD(udx_napi_set_ttl) {
  NAPI_ARGV(2)
  NAPI_ARGV_BUFFER_CAST(udx_t *, self, 0)
  NAPI_ARGV_UINT32(ttl, 1)

  int err = udx_set_ttl(self, ttl);
  if (err < 0) UDX_NAPI_THROW(err)

  return NULL;
}

NAPI_METHOD(udx_napi_recv_buffer_size) {
  NAPI_ARGV(2)
  NAPI_ARGV_BUFFER_CAST(udx_t *, self, 0)
  NAPI_ARGV_INT32(size, 1)

  int err = udx_recv_buffer_size(self, &size);
  if (err < 0) UDX_NAPI_THROW(err)

  NAPI_RETURN_UINT32(size)
}

NAPI_METHOD(udx_napi_send_buffer_size) {
  NAPI_ARGV(2)
  NAPI_ARGV_BUFFER_CAST(udx_t *, self, 0)
  NAPI_ARGV_INT32(size, 1)

  int err = udx_send_buffer_size(self, &size);
  if (err < 0) UDX_NAPI_THROW(err)

  NAPI_RETURN_UINT32(size)
}

NAPI_METHOD(udx_napi_send) {
  NAPI_ARGV(6)
  NAPI_ARGV_BUFFER_CAST(udx_t *, self, 0)
  NAPI_ARGV_BUFFER_CAST(udx_send_t *, req, 1)
  NAPI_ARGV_UINT32(rid, 2)
  NAPI_ARGV_BUFFER(buf, 3)
  NAPI_ARGV_UINT32(port, 4)
  NAPI_ARGV_UTF8(ip, 17, 5)

  req->userid = rid;

  struct sockaddr_in addr;

  int err = uv_ip4_addr((char *) &ip, port, &addr);
  if (err < 0) UDX_NAPI_THROW(err)

  err = udx_send(self, req, buf, buf_len, (const struct sockaddr *) &addr);
  if (err < 0) UDX_NAPI_THROW(err)

  return NULL;
}

NAPI_METHOD(udx_napi_close) {
  NAPI_ARGV(1)
  NAPI_ARGV_BUFFER_CAST(udx_t *, self, 0)

  int err = udx_close(self);
  if (err < 0) UDX_NAPI_THROW(err)

  return NULL;
}

NAPI_METHOD(udx_napi_stream_init) {
  NAPI_ARGV(8)
  NAPI_ARGV_BUFFER_CAST(udx_t *, self, 0)
  NAPI_ARGV_BUFFER_CAST(udx_napi_stream_t *, stream, 1)

  stream->mode = UDX_NAPI_INTERACTIVE;

  stream->read_buf = NULL;
  stream->read_buf_head = NULL;
  stream->read_buf_free = 0;

  stream->env = env;
  napi_create_reference(env, argv[2], 1, &(stream->ctx));
  napi_create_reference(env, argv[3], 1, &(stream->on_data));
  napi_create_reference(env, argv[4], 1, &(stream->on_end));
  napi_create_reference(env, argv[5], 1, &(stream->on_drain));
  napi_create_reference(env, argv[6], 1, &(stream->on_ack));
  napi_create_reference(env, argv[7], 1, &(stream->on_close));

  udx_stream_t *u = (udx_stream_t *) stream;
  uint32_t local_id;

  int err = udx_stream_init(self, u, &local_id);
  if (err < 0) UDX_NAPI_THROW(err)

  udx_stream_set_on_data(u, on_udx_stream_data);
  udx_stream_set_on_end(u, on_udx_stream_end);
  udx_stream_set_on_drain(u, on_udx_stream_drain);
  udx_stream_set_on_ack(u, on_udx_stream_ack);
  udx_stream_set_on_close(u, on_udx_stream_close);

  NAPI_RETURN_UINT32(local_id)
}

NAPI_METHOD(udx_napi_stream_set_mode) {
  NAPI_ARGV(2)
  NAPI_ARGV_BUFFER_CAST(udx_napi_stream_t *, stream, 0)
  NAPI_ARGV_UINT32(mode, 1)

  stream->mode = mode;

  return NULL;
}

NAPI_METHOD(udx_napi_stream_connect) {
  NAPI_ARGV(5)
  NAPI_ARGV_BUFFER_CAST(udx_napi_stream_t *, stream, 0)
  NAPI_ARGV_BUFFER(read_buf, 1)
  NAPI_ARGV_UINT32(remote_id, 2)
  NAPI_ARGV_UINT32(remote_port, 3)
  NAPI_ARGV_UTF8(remote_ip, 17, 4)

  struct sockaddr_in addr;
  int err = uv_ip4_addr(remote_ip, remote_port, &addr);
  if (err < 0) UDX_NAPI_THROW(err)

  stream->read_buf = read_buf;
  stream->read_buf_head = read_buf;
  stream->read_buf_free = read_buf_len;

  udx_stream_connect((udx_stream_t *) stream, remote_id, (const struct sockaddr *) &addr);

  return NULL;
}

NAPI_METHOD(udx_napi_stream_write) {
  NAPI_ARGV(4)
  NAPI_ARGV_BUFFER_CAST(udx_stream_t *, stream, 0)
  NAPI_ARGV_BUFFER_CAST(udx_write_t *, req, 1)
  NAPI_ARGV_UINT32(rid, 2)
  NAPI_ARGV_BUFFER(buf, 3)

  req->userid = rid;

  int err = udx_stream_write(stream, req, buf, buf_len);
  if (err < 0) UDX_NAPI_THROW(err)

  NAPI_RETURN_UINT32(err);
}

NAPI_METHOD(udx_napi_stream_end) {
  NAPI_ARGV(3)
  NAPI_ARGV_BUFFER_CAST(udx_stream_t *, stream, 0)
  NAPI_ARGV_BUFFER_CAST(udx_write_t *, req, 1)
  NAPI_ARGV_UINT32(rid, 2)

  req->userid = rid;

  int err = udx_stream_end(stream, req);
  if (err < 0) UDX_NAPI_THROW(err)

  NAPI_RETURN_UINT32(err);
}

NAPI_METHOD(udx_napi_stream_destroy) {
  NAPI_ARGV(1)
  NAPI_ARGV_BUFFER_CAST(udx_stream_t *, stream, 0)

  int err = udx_stream_destroy(stream);
  if (err < 0) UDX_NAPI_THROW(err)

  NAPI_RETURN_UINT32(err);
}

NAPI_INIT() {
  // TODO: do NOT do this!! either find a better way to get random nums
  // or seed with something more random
  srand(time(0));

  NAPI_EXPORT_OFFSETOF(udx_stream_t, inflight)
  NAPI_EXPORT_OFFSETOF(udx_stream_t, cwnd)
  NAPI_EXPORT_OFFSETOF(udx_stream_t, srtt)
  NAPI_EXPORT_OFFSETOF(udx_stream_t, pkts_waiting)
  NAPI_EXPORT_OFFSETOF(udx_stream_t, pkts_inflight)

  NAPI_EXPORT_SIZEOF(udx_napi_t)
  NAPI_EXPORT_SIZEOF(udx_napi_stream_t)

  NAPI_EXPORT_SIZEOF(udx_send_t)
  NAPI_EXPORT_SIZEOF(udx_write_t)

  NAPI_EXPORT_FUNCTION(udx_napi_init)
  NAPI_EXPORT_FUNCTION(udx_napi_bind)
  NAPI_EXPORT_FUNCTION(udx_napi_set_ttl)
  NAPI_EXPORT_FUNCTION(udx_napi_recv_buffer_size)
  NAPI_EXPORT_FUNCTION(udx_napi_send_buffer_size)
  NAPI_EXPORT_FUNCTION(udx_napi_send)
  NAPI_EXPORT_FUNCTION(udx_napi_close)

  NAPI_EXPORT_FUNCTION(udx_napi_stream_init)
  NAPI_EXPORT_FUNCTION(udx_napi_stream_set_mode)
  NAPI_EXPORT_FUNCTION(udx_napi_stream_connect)
  NAPI_EXPORT_FUNCTION(udx_napi_stream_write)
  NAPI_EXPORT_FUNCTION(udx_napi_stream_end)
  NAPI_EXPORT_FUNCTION(udx_napi_stream_destroy)
}
