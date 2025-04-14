#include <stdlib.h>
#include <assert.h>
#include <uv.h>
#include <memory.h>

#include "../include/udx.h"

#ifdef USE_DRAIN_THREAD

#include "io.h"
#include "internal.h"

#define TRACE 1

#if TRACE
#define tracef(...) \
  do { \
    fprintf(stderr, __VA_ARGS__); \
  } while (0)

static const char *status_str[4] = { "STOP", "INIT", "RUN", "CLOSE" };
static const char *type_str[3] = { "SOCKET_INIT", "SOCKET_REMOVE", "THREAD_STOP" };
#else
#define tracef(...) {}
#endif

#ifndef N_SLOTS
// Allocate on-thread read buffer
#define N_SLOTS 2048
#endif

#define ASSERT_RUNS_ON_THREAD \
  assert(uv_thread_self() == udx->thread.thread_id)

#define ASSERT_RUNS_ON_MAIN \
  assert(uv_thread_self() == udx->thread._main_id)

enum thread_status {
  STOPPED = 0,
  INITIALIZED = 1,
  RUNNING = 2,
  CLOSING = 3,
};

static inline void
set_status(udx_t *udx, enum thread_status status) {
  udx_thread_t *thread = &udx->thread;

#if TRACE
  tracef("thread=%zu udx=%p state transition from=%s to=%s\n", uv_thread_self(), udx, status_str[thread->status], status_str[status]);
#endif

  switch (status) {
    case STOPPED:
      assert(thread->status == CLOSING);
      break;
    case INITIALIZED:
      assert(thread->status == STOPPED);
      break;
    case RUNNING:
      assert(thread->status == INITIALIZED);
      break;
    case CLOSING:
      assert(
        thread->status == INITIALIZED ||
        thread->status == RUNNING
      );
      break;
  }
  thread->status = status;
}

static void
on_drain (uv_async_t *signal) {
  udx_t *udx = signal->data;
  udx_thread_t *thread = &udx->thread;

  ASSERT_RUNS_ON_MAIN;

  thread->perf_load += (thread->buffer_len + thread->cursors.read - thread->cursors.drained) % thread->buffer_len;
  thread->perf_ndrains++;

  udx__drain_slot_t *slot;

  while (thread->cursors.drained != thread->cursors.read) {
    slot = &thread->buffer[thread->cursors.drained];
    udx__drainer__on_packet(slot);
    thread->cursors.drained = (thread->cursors.drained + 1) % thread->buffer_len;
  }
}

static int
thread_poll_update (udx_socket_t *socket);

static void
t_on_uv_poll (uv_poll_t *handle, int status, int events) {
  UDX_UNUSED(status);
  int err;
  udx_socket_t *socket = handle->data;
  udx_t *udx = socket->udx;
  udx_thread_t *thread = &udx->thread;

  ASSERT_RUNS_ON_THREAD;

  if (!(events & UV_READABLE)) goto reset_poll;

  ssize_t size;
  int current;
  udx__drain_slot_t *slot;
  uv_buf_t buf;

  // drain socket buffers
  do {
    if (socket->status & UDX_SOCKET_CLOSED) break;

    current = thread->cursors.read;
    slot = &thread->buffer[current];

    slot->socket = socket;
    memset(&slot->addr, 0, sizeof(slot->addr));
    buf.base = (char *) &slot->buffer;
    buf.len = sizeof(slot->buffer);

    size = udx__recvmsg(socket, &buf, (struct sockaddr *) &slot->addr, sizeof(slot->addr));
    if (size < 0) break;

    slot->len = size;

    if (uv_is_closing((uv_handle_t *) &thread->async_in_drain)) break;

    err = uv_async_send(&thread->async_in_drain);
    assert(err == 0);

    int next = (thread->cursors.read + 1) % thread->buffer_len;

    if (thread->cursors.drained == next) {
      udx->packets_dropped_by_thread++;
      socket->packets_dropped_by_thread++;
      continue;
    }

    thread->cursors.read = next;
  } while(1);

reset_poll:

  err = thread_poll_update(socket);
  assert(err == 0);
}

static int
thread_poll_update (udx_socket_t *socket) {
  udx_t *udx = socket->udx;

  ASSERT_RUNS_ON_THREAD;

  if (socket->status & UDX_SOCKET_CLOSED) {
    assert(!uv_is_active((uv_handle_t *) &socket->poll_drain));
    return 0;
  }

  uv_poll_t *poll = &socket->poll_drain;
  return uv_poll_start(poll, UV_READABLE, t_on_uv_poll);
}

static void
on_poll_stop (uv_async_t *signal) {
  udx_socket_t *socket = signal->data;
  udx_t *udx = socket->udx;

  ASSERT_RUNS_ON_MAIN;

  tracef("thread=%zu udx=%p poll=%p fd=%i POLL STOPPED\n", uv_thread_self(), udx, &socket->poll_drain, socket->_fd);

  assert(!uv_is_closing((uv_handle_t *) signal));
  uv_close((uv_handle_t *) signal, NULL); // close the jump signal

  udx__drainer__on_poll_stop(socket); // return to udx.c
}

static void
t_on_uv_poll_close (uv_handle_t *handle) {
  udx_socket_t *socket = handle->data;
  udx_t *udx = socket->udx;

  ASSERT_RUNS_ON_THREAD;

  int err = uv_async_send(&socket->async_in_poll_stopped); // jump to main
  assert(err == 0);
}

static void
t_on_control (uv_async_t *signal) {
  udx_t *udx = signal->data;
  udx_thread_t *thread = &udx->thread;

  ASSERT_RUNS_ON_THREAD;

  // process queue
  while (thread->tail != thread->head) {
    int next = (thread->tail + 1) % CMD_QUEUE_SIZE;
    udx__tcmd_t *cmd = &thread->queue[next];

    switch (cmd->type) {
      case SOCKET_INIT: {
        int err;
        udx_socket_t *socket = cmd->data;
        assert(socket->udx == udx);

        assert(thread->status == RUNNING);
        assert(socket->poll_initialized == false);

        // assert(!(socket->status & UDX_SOCKET_CLOSED));
        if (socket->status & UDX_SOCKET_CLOSED) break;

        uv_loop_t *subloop = &thread->loop;

        uv_poll_t *poll = &socket->poll_drain;

        err = uv_poll_init_socket(subloop, poll, (uv_os_sock_t) socket->_fd);
        assert(err == 0 && "init read poll failed");

        poll->data = socket;

        err = thread_poll_update(socket);
        assert(err == 0);

        socket->poll_initialized = true;

        tracef("thread=%zu udx=%p poll=%p fd=%i POLL START\n", uv_thread_self(), udx, poll, socket->_fd);
      } break;

      case SOCKET_REMOVE: {
        int err;
        udx_socket_t *socket = cmd->data;

        if (!socket->poll_initialized) {
          err = uv_async_send(&socket->async_in_poll_stopped);
          assert(err == 0);
          break;
        }

        err = uv_poll_stop(&socket->poll_drain);
        assert(err == 0);

        assert(!uv_is_closing((uv_handle_t *) &socket->poll_drain));
        uv_close((uv_handle_t *) &socket->poll_drain, t_on_uv_poll_close);
      } break;

      case THREAD_STOP:
        assert(thread->status == CLOSING);
        assert(!uv_is_closing((uv_handle_t *) &thread->async_out_ctrl));
        // close control handle that keeps subloop active.
        uv_close((uv_handle_t *) &thread->async_out_ctrl, NULL);
        break;

      default:
        assert(0);
    }

    tracef("udx=%p <-- done[%i] cmd=%p t=%s\n", udx, next, cmd, type_str[cmd->type]);

    thread->tail = next;
  }
}

static inline int
run_command (udx_t *udx, enum udx__tcmd_type type, void *data) {
  ASSERT_RUNS_ON_MAIN;

  udx_thread_t *thread = &udx->thread;

  assert(!uv_is_closing((uv_handle_t *) &thread->async_out_ctrl));

  int next = (thread->head + 1) % CMD_QUEUE_SIZE;
  assert(next != thread->tail && "command queue overflow");

  udx__tcmd_t *cmd = &thread->queue[next];
  cmd->type = type;
  cmd->data = data;
  cmd->next = NULL;

  thread->head = next;
  tracef("udx=%p --> push[%i] cmd=%p t=%s\n", udx, next, cmd, type_str[type]);

  if (!uv_is_active((uv_handle_t *) &thread->async_out_ctrl)) {
    return 0; // just queue event, notify triggers on thread run
  } else {
    return uv_async_send(&thread->async_out_ctrl);
  }
}

static void
on_thread_stop(uv_async_t *signal) {
  udx_t *udx = signal->data;
  udx_thread_t *thread = &udx->thread;

  ASSERT_RUNS_ON_MAIN;

  set_status(udx, STOPPED);

  assert(!uv_is_closing((uv_handle_t *) &thread->async_in_drain));
  uv_close((uv_handle_t *) &thread->async_in_drain, NULL);

  assert(!uv_is_closing((uv_handle_t *) signal));
  uv_close((uv_handle_t *) signal, NULL); // release main loop
}

static void
thread_run (void *data) {
  int err;
  udx_t *udx = data;
  udx_thread_t *thread = &udx->thread;

  ASSERT_RUNS_ON_THREAD;

  if (thread->status == CLOSING) return;

  assert(thread->status == INITIALIZED);

  tracef("thread=%zu udx=%p THREAD LAUNCH\n", uv_thread_self(), udx);
  uv_loop_t *subloop = &thread->loop;
  uv_loop_t tmp = {0};
  assert(memcmp(subloop, &tmp, sizeof(tmp)) == 0);

  err = uv_loop_init(subloop);
  assert(err == 0);

  err = uv_async_init(subloop, &thread->async_out_ctrl, t_on_control);
  assert(err == 0);

  thread->async_out_ctrl.data = udx;

  thread->buffer = malloc(sizeof(udx__drain_slot_t) * N_SLOTS);
  thread->buffer_len = N_SLOTS;

  set_status(udx, RUNNING);

  err = uv_async_send(&thread->async_out_ctrl); // queue pending cmds
  assert(err == 0);

  err = uv_run(subloop, UV_RUN_DEFAULT);
  assert(err == 0 && "uv_run(subloop) failed");

  free(thread->buffer);

  tracef("thread=%zu udx=%p THREAD EXIT\n", uv_thread_self(), udx);

  err = uv_async_send(&thread->async_in_thread_stopped); // notify main
  assert(err == 0);
}

static void on_thread_start (uv_async_t *signal) {
  udx_t *udx = signal->data;
  udx_thread_t *thread = &udx->thread;

  ASSERT_RUNS_ON_MAIN;

  assert(!uv_is_closing((uv_handle_t *) signal));
  uv_close((uv_handle_t *) signal, NULL);

  if (thread->status == CLOSING) return;

  assert(thread->status == INITIALIZED);

  int err = uv_thread_create(&thread->thread_id, thread_run, udx);
  assert(err == 0);
}

/// exports

int
udx__thread_init(udx_t *udx) {
  udx_thread_t *thread = &udx->thread;

  if (thread->status != STOPPED) return 0; // do nothing

  assert(thread->_main_id == 0 && "debug thread-restarts");

  thread->_main_id = uv_thread_self();

  ASSERT_RUNS_ON_MAIN;

  int err;
  uv_loop_t *loop = udx->loop;

  err = uv_async_init(loop, &thread->async_in_drain, on_drain);
  assert(err == 0);

  thread->async_in_drain.data = udx;

  err = uv_async_init(loop, &thread->async_in_thread_stopped, on_thread_stop);
  assert(err == 0);

  thread->async_in_thread_stopped.data = udx;

  // queue thread start on main-loop (prevent launch before main loop runs)
  err = uv_async_init(loop, &thread->async_thread_start, on_thread_start);
  assert(err == 0);

  thread->async_thread_start.data = udx;

  set_status(udx, INITIALIZED);

  uv_async_send(&thread->async_thread_start);

  return 0;
}

int
udx__thread_destroy (udx_t *udx) {
  ASSERT_RUNS_ON_MAIN;

  udx_thread_t *thread = &udx->thread;

  if (
    thread->status == STOPPED ||
    thread->status == CLOSING
  ) return 0;

  set_status(udx, CLOSING);

  return run_command(udx, THREAD_STOP, NULL);
}

int
udx__thread_poll_init (udx_socket_t *socket) {
  udx_t *udx = socket->udx;

  ASSERT_RUNS_ON_MAIN;

  int err = uv_async_init(udx->loop, &socket->async_in_poll_stopped, on_poll_stop);
  assert(err == 0);

  socket->async_in_poll_stopped.data = socket;

  err = uv_fileno((const uv_handle_t *) &socket->handle, &socket->_fd);
  assert(err == 0);

  return run_command(udx, SOCKET_INIT, socket);
}

int
udx__thread_poll_destroy (udx_socket_t *socket) {
  udx_t *udx = socket->udx;

  ASSERT_RUNS_ON_MAIN;

  assert(socket->status & UDX_SOCKET_CLOSED);

  return run_command(udx, SOCKET_REMOVE, socket);
}

#undef tracef
#undef N_SLOTS
#undef ASSERT_RUNS_ON_THREAD
#undef ASSERT_RUNS_ON_MAIN
#endif // USE_DRAIN_THREAD
