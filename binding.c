#include <node_api.h>
#include <napi-macros.h>
#include <string.h>
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
    napi_get_buffer_info(env, res, (void **) &(self->read_buf), &(self->read_buf_free)); \
    self->read_buf_head = self->read_buf; \
  }

typedef struct {
  udx_socket_t udx;

  napi_env env;
  napi_ref ctx;
  napi_ref on_send;
  napi_ref on_message;
  napi_ref on_close;
} udx_napi_socket_t;

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
  napi_ref on_send;
  napi_ref on_message;
  napi_ref on_close;
  napi_ref on_firewall;
} udx_napi_stream_t;

inline static void
parse_address (struct sockaddr *name, char *ip, int *port) {
  struct sockaddr_in *name_in = (struct sockaddr_in *) name;
  *port = ntohs(name_in->sin_port);
  uv_ip4_name(name_in, ip, 17);
}

static void
on_udx_send (udx_socket_send_t *req, int status) {
  udx_napi_socket_t *n = (udx_napi_socket_t *) req->handle;

  UDX_NAPI_CALLBACK(n, n->on_send, {
    napi_value argv[2];
    napi_create_int32(env, (uintptr_t) req->data, &(argv[0]));
    napi_create_int32(env, status, &(argv[1]));
    NAPI_MAKE_CALLBACK(env, NULL, ctx, callback, 2, argv, NULL)
  })
}

static void
on_udx_message (udx_socket_t *self, ssize_t read_len, const uv_buf_t *buf, const struct sockaddr *from) {
  udx_napi_socket_t *n = (udx_napi_socket_t *) self;

  int port;
  char ip[17];
  parse_address((struct sockaddr *) from, ip, &port);

  UDX_NAPI_CALLBACK(n, n->on_message, {
    napi_value argv[3];
    napi_create_buffer_copy(n->env, buf->len, buf->base, NULL, &(argv[0]));
    napi_create_uint32(env, port, &(argv[1]));
    napi_create_string_utf8(env, ip, NAPI_AUTO_LENGTH, &(argv[2]));
    NAPI_MAKE_CALLBACK(env, NULL, ctx, callback, 3, argv, NULL)
  })
}

static void
on_udx_close (udx_socket_t *self) {
  udx_napi_socket_t *n = (udx_napi_socket_t *) self;

  UDX_NAPI_CALLBACK(n, n->on_close, {
    NAPI_MAKE_CALLBACK(env, NULL, ctx, callback, 0, NULL, NULL)
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
on_udx_stream_read (udx_stream_t *stream, ssize_t read_len, const uv_buf_t *buf) {
  if (read_len == UV_EOF) return on_udx_stream_end(stream);

  udx_napi_stream_t *n = (udx_napi_stream_t *) stream;

  memcpy(n->read_buf_head, buf->base, buf->len);

  n->read_buf_head += buf->len;
  n->read_buf_free -= buf->len;

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
on_udx_stream_drain (udx_stream_t *stream) {
  udx_napi_stream_t *n = (udx_napi_stream_t *) stream;

  UDX_NAPI_CALLBACK(n, n->on_drain, {
    NAPI_MAKE_CALLBACK(env, NULL, ctx, callback, 0, NULL, NULL)
  })
}

static void
on_udx_stream_ack (udx_stream_write_t *req, int status, int unordered) {
  udx_napi_stream_t *n = (udx_napi_stream_t *) req->handle;

  UDX_NAPI_CALLBACK(n, n->on_ack, {
    napi_value argv[1];
    napi_create_uint32(env, (uintptr_t) req->data, &(argv[0]));
    NAPI_MAKE_CALLBACK(env, NULL, ctx, callback, 1, argv, NULL)
  })
}

static void
on_udx_stream_send (udx_stream_send_t *req, int status) {
  udx_napi_stream_t *n = (udx_napi_stream_t *) req->handle;

  UDX_NAPI_CALLBACK(n, n->on_send, {
    napi_value argv[2];
    napi_create_int32(env, (uintptr_t) req->data, &(argv[0]));
    napi_create_int32(env, status, &(argv[1]));
    NAPI_MAKE_CALLBACK(env, NULL, ctx, callback, 2, argv, NULL)
  })
}

static void
on_udx_stream_recv (udx_stream_t *stream, ssize_t read_len, const uv_buf_t *buf) {
  udx_napi_stream_t *n = (udx_napi_stream_t *) stream;

  UDX_NAPI_CALLBACK(n, n->on_message, {
    napi_value argv[1];
    napi_create_buffer_copy(n->env, buf->len, buf->base, NULL, &(argv[0]));
    NAPI_MAKE_CALLBACK(env, NULL, ctx, callback, 1, argv, NULL)
  })
}

static void
on_udx_stream_close (udx_stream_t *stream, int status) {
  udx_napi_stream_t *n = (udx_napi_stream_t *) stream;

  UDX_NAPI_CALLBACK(n, n->on_close, {
    napi_value argv[1];
    napi_create_int32(env, status, &(argv[0]));
    NAPI_MAKE_CALLBACK(env, NULL, ctx, callback, 1, argv, NULL)
  })
}

static int
on_udx_stream_firewall (udx_stream_t *stream, udx_socket_t *socket, const struct sockaddr *from) {
  udx_napi_stream_t *n = (udx_napi_stream_t *) stream;
  udx_napi_socket_t *s = (udx_napi_socket_t *) socket;

  uint32_t fw = 1; // assume error means firewall it, whilst reporting the uncaught

  int port;
  char ip[17];
  parse_address((struct sockaddr *) from, ip, &port);

  UDX_NAPI_CALLBACK(n, n->on_firewall, {
    napi_value res;
    napi_value argv[3];

    napi_get_reference_value(env, s->ctx, &(argv[0]));
    napi_create_uint32(env, port, &(argv[1]));
    napi_create_string_utf8(env, ip, NAPI_AUTO_LENGTH, &(argv[2]));

    if (napi_make_callback(env, NULL, ctx, callback, 3, argv, &res) == napi_pending_exception) {
      napi_value fatal_exception;
      napi_get_and_clear_last_exception(env, &fatal_exception);
      napi_fatal_exception(env, fatal_exception);
    } else {
      napi_get_value_uint32(env, res, &fw);
    }
  })

  return fw;
}

NAPI_METHOD(udx_napi_init) {
  NAPI_ARGV(1)
  NAPI_ARGV_BUFFER_CAST(udx_t *, self, 0)

  struct uv_loop_s *loop;
  napi_get_uv_event_loop(env, &loop);

  udx_init(loop, self);

  return NULL;
}

NAPI_METHOD(udx_napi_socket_init) {
  NAPI_ARGV(6)
  NAPI_ARGV_BUFFER_CAST(udx_t *, udx, 0)
  NAPI_ARGV_BUFFER_CAST(udx_napi_socket_t *, self, 1)

  udx_socket_t *socket = (udx_socket_t *) self;

  self->env = env;
  napi_create_reference(env, argv[2], 1, &(self->ctx));
  napi_create_reference(env, argv[3], 1, &(self->on_send));
  napi_create_reference(env, argv[4], 1, &(self->on_message));
  napi_create_reference(env, argv[5], 1, &(self->on_close));

  int err = udx_socket_init(udx, socket);
  if (err < 0) UDX_NAPI_THROW(err)

  return NULL;
}

NAPI_METHOD(udx_napi_socket_bind) {
  NAPI_ARGV(3)
  NAPI_ARGV_BUFFER_CAST(udx_socket_t *, self, 0)
  NAPI_ARGV_UINT32(port, 1)
  NAPI_ARGV_UTF8(ip, 17, 2)

  struct sockaddr_in addr;
  int err = uv_ip4_addr(ip, port, &addr);
  if (err < 0) UDX_NAPI_THROW(err)

  err = udx_socket_bind(self, (const struct sockaddr *) &addr);
  if (err < 0) UDX_NAPI_THROW(err)

  // TODO: move the bottom stuff into another function, start, so error handling is easier

  struct sockaddr name;
  int name_len = sizeof(name);

  // wont error in practice
  err = udx_socket_getsockname(self, &name, &name_len);
  if (err < 0) UDX_NAPI_THROW(err)

  struct sockaddr_in *name_in = (struct sockaddr_in *) &name;
  int local_port = ntohs(name_in->sin_port);

  // wont error in practice
  err = udx_socket_recv_start(self, on_udx_message);
  if (err < 0) UDX_NAPI_THROW(err)

  NAPI_RETURN_UINT32(local_port)
}

NAPI_METHOD(udx_napi_socket_set_ttl) {
  NAPI_ARGV(2)
  NAPI_ARGV_BUFFER_CAST(udx_socket_t *, self, 0)
  NAPI_ARGV_UINT32(ttl, 1)

  int err = udx_socket_set_ttl(self, ttl);
  if (err < 0) UDX_NAPI_THROW(err)

  return NULL;
}

NAPI_METHOD(udx_napi_socket_recv_buffer_size) {
  NAPI_ARGV(2)
  NAPI_ARGV_BUFFER_CAST(udx_socket_t *, self, 0)
  NAPI_ARGV_INT32(size, 1)

  int err = udx_socket_recv_buffer_size(self, &size);
  if (err < 0) UDX_NAPI_THROW(err)

  NAPI_RETURN_UINT32(size)
}

NAPI_METHOD(udx_napi_socket_send_buffer_size) {
  NAPI_ARGV(2)
  NAPI_ARGV_BUFFER_CAST(udx_socket_t *, self, 0)
  NAPI_ARGV_INT32(size, 1)

  int err = udx_socket_send_buffer_size(self, &size);
  if (err < 0) UDX_NAPI_THROW(err)

  NAPI_RETURN_UINT32(size)
}

NAPI_METHOD(udx_napi_socket_send_ttl) {
  NAPI_ARGV(7)
  NAPI_ARGV_BUFFER_CAST(udx_socket_t *, self, 0)
  NAPI_ARGV_BUFFER_CAST(udx_socket_send_t *, req, 1)
  NAPI_ARGV_UINT32(rid, 2)
  NAPI_ARGV_BUFFER(buf, 3)
  NAPI_ARGV_UINT32(port, 4)
  NAPI_ARGV_UTF8(ip, 17, 5)
  NAPI_ARGV_UINT32(ttl, 6)

  req->data = (void *)((uintptr_t) rid);

  struct sockaddr_in addr;

  int err = uv_ip4_addr((char *) &ip, port, &addr);
  if (err < 0) UDX_NAPI_THROW(err)

  uv_buf_t b = uv_buf_init(buf, buf_len);

  udx_socket_send_ttl(req, self, &b, 1, (const struct sockaddr *) &addr, ttl, on_udx_send);

  if (err < 0) UDX_NAPI_THROW(err)

  return NULL;
}

NAPI_METHOD(udx_napi_socket_close) {
  NAPI_ARGV(1)
  NAPI_ARGV_BUFFER_CAST(udx_socket_t *, self, 0)

  int err = udx_socket_close(self, on_udx_close);
  if (err < 0) UDX_NAPI_THROW(err)

  return NULL;
}

NAPI_METHOD(udx_napi_stream_init) {
  NAPI_ARGV(12)
  NAPI_ARGV_BUFFER_CAST(udx_t *, udx, 0)
  NAPI_ARGV_BUFFER_CAST(udx_napi_stream_t *, stream, 1)
  NAPI_ARGV_UINT32(id, 2)

  stream->mode = UDX_NAPI_INTERACTIVE;

  stream->read_buf = NULL;
  stream->read_buf_head = NULL;
  stream->read_buf_free = 0;

  stream->env = env;
  napi_create_reference(env, argv[3], 1, &(stream->ctx));
  napi_create_reference(env, argv[4], 1, &(stream->on_data));
  napi_create_reference(env, argv[5], 1, &(stream->on_end));
  napi_create_reference(env, argv[6], 1, &(stream->on_drain));
  napi_create_reference(env, argv[7], 1, &(stream->on_ack));
  napi_create_reference(env, argv[8], 1, &(stream->on_send));
  napi_create_reference(env, argv[9], 1, &(stream->on_message));
  napi_create_reference(env, argv[10], 1, &(stream->on_close));
  napi_create_reference(env, argv[11], 1, &(stream->on_firewall));

  int err = udx_stream_init(udx, (udx_stream_t *) stream, id);
  if (err < 0) UDX_NAPI_THROW(err)

  udx_stream_firewall((udx_stream_t *) stream, on_udx_stream_firewall);
  udx_stream_write_resume((udx_stream_t *) stream, on_udx_stream_drain);

  return NULL;
}

NAPI_METHOD(udx_napi_stream_set_mode) {
  NAPI_ARGV(2)
  NAPI_ARGV_BUFFER_CAST(udx_napi_stream_t *, stream, 0)
  NAPI_ARGV_UINT32(mode, 1)

  stream->mode = mode;

  return NULL;
}

NAPI_METHOD(udx_napi_stream_recv_start) {
  NAPI_ARGV(2)
  NAPI_ARGV_BUFFER_CAST(udx_napi_stream_t *, stream, 0)
  NAPI_ARGV_BUFFER(read_buf, 1)

  stream->read_buf = read_buf;
  stream->read_buf_head = read_buf;
  stream->read_buf_free = read_buf_len;

  udx_stream_read_start((udx_stream_t *) stream, on_udx_stream_read);
  udx_stream_recv_start((udx_stream_t *) stream, on_udx_stream_recv);

  return NULL;
}

NAPI_METHOD(udx_napi_stream_connect) {
  NAPI_ARGV(5)
  NAPI_ARGV_BUFFER_CAST(udx_napi_stream_t *, stream, 0)
  NAPI_ARGV_BUFFER_CAST(udx_socket_t *, socket, 1)
  NAPI_ARGV_UINT32(remote_id, 2)
  NAPI_ARGV_UINT32(remote_port, 3)
  NAPI_ARGV_UTF8(remote_ip, 17, 4)

  struct sockaddr_in addr;
  int err = uv_ip4_addr(remote_ip, remote_port, &addr);
  if (err < 0) UDX_NAPI_THROW(err)

  udx_stream_connect((udx_stream_t *) stream, socket, remote_id, (const struct sockaddr *) &addr, on_udx_stream_close);

  return NULL;
}

NAPI_METHOD(udx_napi_stream_send) {
  NAPI_ARGV(4)
  NAPI_ARGV_BUFFER_CAST(udx_stream_t *, stream, 0)
  NAPI_ARGV_BUFFER_CAST(udx_stream_send_t *, req, 1)
  NAPI_ARGV_UINT32(rid, 2)
  NAPI_ARGV_BUFFER(buf, 3)

  req->data = (void *)((uintptr_t) rid);

  uv_buf_t b = uv_buf_init(buf, buf_len);

  int err = udx_stream_send(req, stream, &b, 1, on_udx_stream_send);
  if (err < 0) UDX_NAPI_THROW(err)

  NAPI_RETURN_UINT32(err);
}

NAPI_METHOD(udx_napi_stream_write) {
  NAPI_ARGV(4)
  NAPI_ARGV_BUFFER_CAST(udx_stream_t *, stream, 0)
  NAPI_ARGV_BUFFER_CAST(udx_stream_write_t *, req, 1)
  NAPI_ARGV_UINT32(rid, 2)
  NAPI_ARGV_BUFFER(buf, 3)

  req->data = (void *)((uintptr_t) rid);

  uv_buf_t b = uv_buf_init(buf, buf_len);

  int err = udx_stream_write(req, stream, &b, 1, on_udx_stream_ack);
  if (err < 0) UDX_NAPI_THROW(err)

  NAPI_RETURN_UINT32(err);
}

NAPI_METHOD(udx_napi_stream_write_end) {
  NAPI_ARGV(4)
  NAPI_ARGV_BUFFER_CAST(udx_stream_t *, stream, 0)
  NAPI_ARGV_BUFFER_CAST(udx_stream_write_t *, req, 1)
  NAPI_ARGV_UINT32(rid, 2)
  NAPI_ARGV_BUFFER(buf, 3)

  req->data = (void *)((uintptr_t) rid);

  uv_buf_t b = uv_buf_init(buf, buf_len);

  int err = udx_stream_write_end(req, stream, &b, 1, on_udx_stream_ack);
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
  NAPI_EXPORT_OFFSETOF(udx_stream_t, inflight)
  NAPI_EXPORT_OFFSETOF(udx_stream_t, cwnd)
  NAPI_EXPORT_OFFSETOF(udx_stream_t, srtt)
  NAPI_EXPORT_OFFSETOF(udx_stream_t, pkts_waiting)
  NAPI_EXPORT_OFFSETOF(udx_stream_t, pkts_inflight)

  NAPI_EXPORT_SIZEOF(udx_t)
  NAPI_EXPORT_SIZEOF(udx_napi_socket_t)
  NAPI_EXPORT_SIZEOF(udx_napi_stream_t)

  NAPI_EXPORT_SIZEOF(udx_socket_send_t)
  NAPI_EXPORT_SIZEOF(udx_stream_write_t)
  NAPI_EXPORT_SIZEOF(udx_stream_send_t)

  NAPI_EXPORT_FUNCTION(udx_napi_init)

  NAPI_EXPORT_FUNCTION(udx_napi_socket_init)
  NAPI_EXPORT_FUNCTION(udx_napi_socket_bind)
  NAPI_EXPORT_FUNCTION(udx_napi_socket_set_ttl)
  NAPI_EXPORT_FUNCTION(udx_napi_socket_recv_buffer_size)
  NAPI_EXPORT_FUNCTION(udx_napi_socket_send_buffer_size)
  NAPI_EXPORT_FUNCTION(udx_napi_socket_send_ttl)
  NAPI_EXPORT_FUNCTION(udx_napi_socket_close)

  NAPI_EXPORT_FUNCTION(udx_napi_stream_init)
  NAPI_EXPORT_FUNCTION(udx_napi_stream_set_mode)
  NAPI_EXPORT_FUNCTION(udx_napi_stream_connect)
  NAPI_EXPORT_FUNCTION(udx_napi_stream_send)
  NAPI_EXPORT_FUNCTION(udx_napi_stream_recv_start)
  NAPI_EXPORT_FUNCTION(udx_napi_stream_write)
  NAPI_EXPORT_FUNCTION(udx_napi_stream_write_end)
  NAPI_EXPORT_FUNCTION(udx_napi_stream_destroy)
}
