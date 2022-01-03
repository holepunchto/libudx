#include <node_api.h>
#include <napi-macros.h>
#include <uv.h>
#include "src/ucp.h"

#define UCP_NAPI_THROW(err) \
  { \
    napi_throw_error(env, uv_err_name(err), uv_strerror(err)); \
    return NULL; \
  }

#define UCP_NAPI_CALLBACK(self, fn, src) \
  napi_env env = self->env; \
  napi_handle_scope scope; \
  napi_open_handle_scope(env, &scope); \
  napi_value ctx; \
  napi_get_reference_value(env, self->ctx, &ctx); \
  napi_value callback; \
  napi_get_reference_value(env, fn, &callback); \
  src \
  napi_close_handle_scope(env, scope);

#define UCP_NAPI_MAKE_ALLOC_CALLBACK(self, env, nil, ctx, cb, n, argv, res) \
  if (napi_make_callback(env, nil, ctx, cb, n, argv, &res) == napi_pending_exception) { \
    napi_value fatal_exception; \
    napi_get_and_clear_last_exception(env, &fatal_exception); \
    napi_fatal_exception(env, fatal_exception); \
    printf("oh no add that realloc\n"); \
  } else { \
    napi_get_buffer_info(env, ret, (void **) &(self->read_buf), &(self->read_buf_len)); \
  }

typedef struct {
  ucp_t ucp;

  napi_env env;
  napi_ref ctx;
  napi_ref on_send;
  napi_ref on_message;
} ucp_napi_t;

typedef struct {
  ucp_stream_t stream;

  char *read_buf;
  size_t read_buf_len;

  napi_env env;
  napi_ref ctx;
  napi_ref on_read;
  napi_ref on_end;
  napi_ref on_drain;
  napi_ref on_ack;
  napi_ref on_close;

  uv_timer_t timer;
} ucp_napi_stream_t;

static void
on_uv_interval (uv_timer_t *req) {
  ucp_stream_t *stream = req->data;
  ucp_stream_check_timeouts(stream);
}

inline static void
parse_address (struct sockaddr *name, char *ip, int *port) {
  struct sockaddr_in *name_in = (struct sockaddr_in *) name;
  *port = ntohs(name_in->sin_port);
  uv_ip4_name(name_in, ip, 17);
}

static void
on_send (ucp_t *self, ucp_send_t *req, int status) {
  ucp_napi_t *n = (ucp_napi_t *) self;

  UCP_NAPI_CALLBACK(n, n->on_send, {
    napi_value argv[1];
    napi_create_int32(env, req->userid, &(argv[0]));
    NAPI_MAKE_CALLBACK(env, NULL, ctx, callback, 1, argv, NULL)
  })
}

static void
on_message (ucp_t *self, const char *buf, size_t buf_len, const struct sockaddr *from) {
  ucp_napi_t *n = (ucp_napi_t *) self;

  int port;
  char ip[17];
  parse_address((struct sockaddr *) from, ip, &port);

  UCP_NAPI_CALLBACK(n, n->on_message, {
    napi_value argv[3];
    napi_create_buffer_copy(n->env, buf_len, buf, NULL, &(argv[0]));
    napi_create_uint32(env, port, &(argv[1]));
    napi_create_string_utf8(env, ip, NAPI_AUTO_LENGTH, &(argv[2]));
    NAPI_MAKE_CALLBACK(env, NULL, ctx, callback, 3, argv, NULL)
  })
}

static void
on_read (ucp_stream_t *stream, const char *buf, const size_t buf_len) {
  ucp_napi_stream_t *n = (ucp_napi_stream_t *) stream;

  memcpy(n->read_buf, buf, buf_len);

  n->read_buf += buf_len;
  n->read_buf_len -= buf_len;

  UCP_NAPI_CALLBACK(n, n->on_read, {
    napi_value ret;
    napi_value argv[1];
    napi_create_uint32(env, buf_len, &(argv[0]));
    UCP_NAPI_MAKE_ALLOC_CALLBACK(n, env, NULL, ctx, callback, 1, argv, ret)
  })
}

static void
on_end (ucp_stream_t *stream) {
  ucp_napi_stream_t *n = (ucp_napi_stream_t *) stream;

  UCP_NAPI_CALLBACK(n, n->on_end, {
    NAPI_MAKE_CALLBACK(env, NULL, ctx, callback, 0, NULL, NULL)
  })
}

static void
on_drain (ucp_stream_t *stream) {
  ucp_napi_stream_t *n = (ucp_napi_stream_t *) stream;

  UCP_NAPI_CALLBACK(n, n->on_drain, {
    NAPI_MAKE_CALLBACK(env, NULL, ctx, callback, 0, NULL, NULL)
  })
}

static void
on_ack (ucp_stream_t *stream, ucp_write_t *req, int status, int unordered) {
  ucp_napi_stream_t *n = (ucp_napi_stream_t *) stream;

  UCP_NAPI_CALLBACK(n, n->on_ack, {
    napi_value argv[1];
    napi_create_uint32(env, req->userid, &(argv[0]));
    NAPI_MAKE_CALLBACK(env, NULL, ctx, callback, 1, argv, NULL)
  })
}

static void
on_close (ucp_stream_t *stream) {
  printf("I closed!\n");
}

NAPI_METHOD(ucp_napi_init) {
  NAPI_ARGV(4)
  NAPI_ARGV_BUFFER_CAST(ucp_napi_t *, self, 0)

  ucp_t *ucp = (ucp_t *) self;

  self->env = env;
  napi_create_reference(env, argv[1], 1, &(self->ctx));
  napi_create_reference(env, argv[2], 1, &(self->on_send));
  napi_create_reference(env, argv[3], 1, &(self->on_message));

  struct uv_loop_s *loop;
  napi_get_uv_event_loop(env, &loop);

  int err = ucp_init(ucp, loop);
  if (err < 0) UCP_NAPI_THROW(err)

  ucp_set_callback(ucp, UCP_ON_SEND, on_send);
  ucp_set_callback(ucp, UCP_ON_MESSAGE, on_message);

  return NULL;
}

NAPI_METHOD(ucp_napi_bind) {
  NAPI_ARGV(3)
  NAPI_ARGV_BUFFER_CAST(ucp_t *, self, 0)
  NAPI_ARGV_UINT32(port, 1)
  NAPI_ARGV_UTF8(ip, 17, 2)

  struct sockaddr_in addr;
  int err = uv_ip4_addr(ip, port, &addr);
  if (err < 0) UCP_NAPI_THROW(err)

  err = ucp_bind(self, (const struct sockaddr *) &addr);
  if (err < 0) UCP_NAPI_THROW(err)

  struct sockaddr name;
  int name_len = sizeof(name);

  err = ucp_getsockname(self, &name, &name_len);
  if (err < 0) UCP_NAPI_THROW(err)

  struct sockaddr_in *name_in = (struct sockaddr_in *) &name;
  int local_port = ntohs(name_in->sin_port);

  NAPI_RETURN_UINT32(local_port)
}

NAPI_METHOD(ucp_napi_set_ttl) {
  NAPI_ARGV(2)
  NAPI_ARGV_BUFFER_CAST(ucp_t *, self, 0)
  NAPI_ARGV_UINT32(ttl, 1)

  int err = ucp_set_ttl(self, ttl);
  if (err < 0) UCP_NAPI_THROW(err)

  return NULL;
}

NAPI_METHOD(ucp_napi_recv_buffer_size) {
  NAPI_ARGV(2)
  NAPI_ARGV_BUFFER_CAST(ucp_t *, self, 0)
  NAPI_ARGV_INT32(size, 1)

  int err = ucp_recv_buffer_size(self, &size);
  if (err < 0) UCP_NAPI_THROW(err)

  NAPI_RETURN_UINT32(size)
}

NAPI_METHOD(ucp_napi_send_buffer_size) {
  NAPI_ARGV(2)
  NAPI_ARGV_BUFFER_CAST(ucp_t *, self, 0)
  NAPI_ARGV_INT32(size, 1)

  int err = ucp_send_buffer_size(self, &size);
  if (err < 0) UCP_NAPI_THROW(err)

  NAPI_RETURN_UINT32(size)
}

NAPI_METHOD(ucp_napi_send) {
  NAPI_ARGV(6)
  NAPI_ARGV_BUFFER_CAST(ucp_t *, self, 0)
  NAPI_ARGV_BUFFER_CAST(ucp_send_t *, req, 1)
  NAPI_ARGV_UINT32(rid, 2)
  NAPI_ARGV_BUFFER(buf, 3)
  NAPI_ARGV_UINT32(port, 4)
  NAPI_ARGV_UTF8(ip, 17, 5)

  req->userid = rid;

  struct sockaddr_in addr;

  int err = uv_ip4_addr((char *) &ip, port, &addr);
  if (err < 0) UCP_NAPI_THROW(err)

  err = ucp_send(self, req, buf, buf_len, (const struct sockaddr *) &addr);
  if (err < 0) UCP_NAPI_THROW(err)

  return NULL;
}

NAPI_METHOD(ucp_napi_stream_init) {
  NAPI_ARGV(8)
  NAPI_ARGV_BUFFER_CAST(ucp_t *, self, 0)
  NAPI_ARGV_BUFFER_CAST(ucp_napi_stream_t *, stream, 1)

  stream->read_buf = NULL;
  stream->read_buf_len = 0;

// TODO: move this to the ucp instance
struct uv_loop_s *loop;
uv_timer_t *timer = &(stream->timer);
napi_get_uv_event_loop(env, &loop);
uv_timer_init(loop, timer);
uv_timer_start(timer, on_uv_interval, 20, 20);
timer->data = stream;

  stream->env = env;
  napi_create_reference(env, argv[2], 1, &(stream->ctx));
  napi_create_reference(env, argv[3], 1, &(stream->on_read));
  napi_create_reference(env, argv[4], 1, &(stream->on_end));
  napi_create_reference(env, argv[5], 1, &(stream->on_drain));
  napi_create_reference(env, argv[6], 1, &(stream->on_ack));
  napi_create_reference(env, argv[7], 1, &(stream->on_close));

  ucp_stream_t *u = (ucp_stream_t *) stream;
  uint32_t local_id;

  int err = ucp_stream_init(self, u, &local_id);
  if (err < 0) UCP_NAPI_THROW(err)

  ucp_stream_set_callback(u, UCP_ON_READ, on_read);
  ucp_stream_set_callback(u, UCP_ON_END, on_end);
  ucp_stream_set_callback(u, UCP_ON_DRAIN, on_drain);
  ucp_stream_set_callback(u, UCP_ON_ACK, on_ack);
  ucp_stream_set_callback(u, UCP_ON_CLOSE, on_close);

  NAPI_RETURN_UINT32(local_id)
}

NAPI_METHOD(ucp_napi_stream_connect) {
  NAPI_ARGV(5)
  NAPI_ARGV_BUFFER_CAST(ucp_napi_stream_t *, stream, 0)
  NAPI_ARGV_BUFFER(read_buf, 1)
  NAPI_ARGV_UINT32(remote_id, 2)
  NAPI_ARGV_UINT32(remote_port, 3)
  NAPI_ARGV_UTF8(remote_ip, 17, 4)

  struct sockaddr_in addr;
  int err = uv_ip4_addr(remote_ip, remote_port, &addr);
  if (err < 0) UCP_NAPI_THROW(err)

  stream->read_buf = read_buf;
  stream->read_buf_len = read_buf_len;

  ucp_stream_connect((ucp_stream_t *) stream, remote_id, (const struct sockaddr *) &addr);

  return NULL;
}

NAPI_METHOD(ucp_napi_stream_write) {
  NAPI_ARGV(5)
  NAPI_ARGV_BUFFER_CAST(ucp_stream_t *, stream, 0)
  NAPI_ARGV_BUFFER_CAST(ucp_write_t *, req, 1)
  NAPI_ARGV_UINT32(rid, 2)
  NAPI_ARGV_BUFFER(buf, 3)

  req->userid = rid;

  int err = ucp_stream_write(stream, req, buf, buf_len);
  if (err < 0) UCP_NAPI_THROW(err)

  NAPI_RETURN_UINT32(err);
}

NAPI_METHOD(ucp_napi_stream_end) {
  NAPI_ARGV(5)
  NAPI_ARGV_BUFFER_CAST(ucp_stream_t *, stream, 0)
  NAPI_ARGV_BUFFER_CAST(ucp_write_t *, req, 1)
  NAPI_ARGV_UINT32(rid, 2)

  req->userid = rid;

  int err = ucp_stream_end(stream, req);
  if (err < 0) UCP_NAPI_THROW(err)

  NAPI_RETURN_UINT32(err);
}

NAPI_INIT() {
  // TODO: do NOT do this!! either find a better way to get random nums
  // or seed with something more random
  srand(time(0));

  NAPI_EXPORT_OFFSETOF(ucp_stream_t, inflight)
  NAPI_EXPORT_OFFSETOF(ucp_stream_t, cwnd)
  NAPI_EXPORT_OFFSETOF(ucp_stream_t, srtt)
  NAPI_EXPORT_OFFSETOF(ucp_stream_t, pkts_waiting)
  NAPI_EXPORT_OFFSETOF(ucp_stream_t, pkts_inflight)

  NAPI_EXPORT_SIZEOF(ucp_napi_t)
  NAPI_EXPORT_SIZEOF(ucp_napi_stream_t)

  NAPI_EXPORT_SIZEOF(ucp_send_t)
  NAPI_EXPORT_SIZEOF(ucp_write_t)

  NAPI_EXPORT_FUNCTION(ucp_napi_init)
  NAPI_EXPORT_FUNCTION(ucp_napi_bind)
  NAPI_EXPORT_FUNCTION(ucp_napi_set_ttl)
  NAPI_EXPORT_FUNCTION(ucp_napi_recv_buffer_size)
  NAPI_EXPORT_FUNCTION(ucp_napi_send_buffer_size)
  NAPI_EXPORT_FUNCTION(ucp_napi_send)

  NAPI_EXPORT_FUNCTION(ucp_napi_stream_init)
  NAPI_EXPORT_FUNCTION(ucp_napi_stream_connect)
  NAPI_EXPORT_FUNCTION(ucp_napi_stream_write)
  NAPI_EXPORT_FUNCTION(ucp_napi_stream_end)
}
