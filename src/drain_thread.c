#include <stdlib.h>
#include <assert.h>
#include <uv.h>
#include <memory.h>

#include "../include/udx.h"
#include "debug.h"

#ifdef USE_DRAIN_THREAD

#include "io.h"
#include "internal.h"

// upfront allocated receive buffer size in amount of packets
// size = N_SLOTS * sizeof(udx__drain_slot_t)
#define N_SLOTS 2048 // ~4MB

enum thread_status {
  STOPPED = 0,
  INITIALIZED = 1,
  RUNNING = 2,
};

enum command_id {
  SOCKET_INIT = 0,
  SOCKET_REMOVE,
  THREAD_STOP
};

typedef struct command_s {
  enum command_id type;
  void *data;
  struct command_s *next;
} command_t;

static void
on_drain (uv_async_t *signal) {
  udx_t *udx = signal->data;
  udx_reader_t *worker = &udx->worker;

  worker->perf_load += (worker->buffer_len + worker->cursors.read - worker->cursors.drained) % worker->buffer_len;
  worker->perf_ndrains++;

  udx__drain_slot_t *slot;

  while (worker->cursors.drained != worker->cursors.read) {
    slot = &worker->buffer[worker->cursors.drained];
    udx__drainer__on_packet(slot);
    worker->cursors.drained = (worker->cursors.drained + 1) % worker->buffer_len;
  }
}

static int
update_read_poll (udx_socket_t *socket);

static void
on_uv_read_poll (uv_poll_t *handle, int status, int events) {
  UDX_UNUSED(status);
  udx_socket_t *socket = handle->data;

  udx_t *udx = socket->udx;
  udx_reader_t *worker = &udx->worker;

  if (!(events & UV_READABLE)) goto reset_poll;

  ssize_t size;
  int current;
  udx__drain_slot_t *slot;
  uv_buf_t buf;

  // drain socket buffers
  do {
    if (socket->status & UDX_SOCKET_CLOSED) break;

    current = worker->cursors.read;
    slot = &worker->buffer[current];

    slot->socket = socket;
    memset(&slot->addr, 0, sizeof(slot->addr));
    buf.base = (char *) &slot->buffer;
    buf.len = sizeof(slot->buffer);

    size = udx__recvmsg(socket, &buf, (struct sockaddr *) &slot->addr, sizeof(slot->addr));
    if (size < 0) break;

    slot->len = size;

    int err = uv_async_send(&worker->signal_drain);
    assert(err == 0);

    int next = (worker->cursors.read + 1) % worker->buffer_len;

    if (worker->cursors.drained == next) {
      udx->packets_dropped_by_worker++;
      socket->packets_dropped_by_worker++;
      continue;
    }

    worker->cursors.read = next;
  } while(1);

reset_poll:
  assert(update_read_poll(socket) == 0);
}

static int
update_read_poll (udx_socket_t *socket) {
  if (socket->status & UDX_SOCKET_CLOSED) return 0;

  uv_poll_t *poll = &socket->drain_poll;
  return uv_poll_start(poll, UV_READABLE, on_uv_read_poll);
}

static void
_on_jump_handle_close (uv_handle_t *handle) { // main loop
  udx_socket_t *socket = handle->data;

  debug_printf("drain thread=%zu socket poll stopped\n", uv_thread_self());
  udx__drainer__on_poll_stop(socket); // return to udx.c
}

static void
_on_stop_jump_main (uv_async_t *signal) { // main loop
  // close the jump signal
  uv_close((uv_handle_t *) signal, _on_jump_handle_close);
}

static void
read_poll_start (udx_socket_t *socket) { // aka drainer_socket_init
  udx_t *udx = socket->udx;
  uv_loop_t *loop = &udx->worker.loop;

  int err = 0;
  uv_poll_t *poll = &socket->drain_poll;
  uv_os_fd_t fd;

  err = uv_fileno((const uv_handle_t *) &socket->handle, &fd);
  assert(err == 0);

  debug_printf("drain thread=%zu socket poll fd=%i start\n", uv_thread_self(), fd);

  err = uv_poll_init_socket(loop, poll, (uv_os_sock_t) fd);
  assert(err == 0);

  poll->data = socket;

  err = update_read_poll(socket);
  assert(err == 0);

  // initialize poll close signal
  err = uv_async_init(socket->udx->loop, &socket->signal_poll_stopped, _on_stop_jump_main);
  assert(err == 0);

  socket->signal_poll_stopped.data = socket;
}

static void
on_uv_poll_close (uv_handle_t *handle) { // sub loop
  udx_socket_t *socket = handle->data;
  int err = uv_async_send(&socket->signal_poll_stopped);
  assert(err == 0);
}

static inline void
read_poll_stop (udx_socket_t *socket) { // sub loop
  int err;

  err = uv_poll_stop(&socket->drain_poll);
  assert(err == 0);

  uv_close((uv_handle_t *) &socket->drain_poll, on_uv_poll_close);
}

static void
on_control (uv_async_t *signal) {
  udx_t *udx = signal->data;

  command_t *head = atomic_exchange(&udx->worker.ctrl_queue, NULL);
  assert(head != NULL);

  // process queue
  while (head != NULL) {
    switch (head->type) {
      case SOCKET_INIT:
        read_poll_start(head->data);
        break;

      case SOCKET_REMOVE:
        read_poll_stop(head->data);
        break;

      case THREAD_STOP:
        // close control handle that keeps subloop active.
        uv_close((uv_handle_t *) &udx->worker.signal_control, NULL);
        break;

      default:
        assert(0);
    }

    command_t *prev = head;
    head = prev->next;
    free(prev);
  }
}

static inline int
run_command (udx_t *udx, enum command_id type, void *data) {
  command_t *cmd = malloc(sizeof(command_t));
  cmd->type = type;
  cmd->data = data;
  cmd->next = NULL;

  command_t *head = atomic_load(&udx->worker.ctrl_queue);

  if (head == NULL) {
    atomic_store(&udx->worker.ctrl_queue, cmd);
  } else {
    while (head->next != NULL) head = head->next;
    head->next = cmd;
  }

  return uv_async_send(&udx->worker.signal_control);
}

static void
on_thread_stop(uv_async_t *signal) {
  udx_t *udx = signal->data;

  assert(udx->worker.status == RUNNING);
  udx->worker.status = STOPPED;

  uv_close((uv_handle_t *) signal, NULL); // release main loop
}

static void
reader_thread (void *data) {
  debug_printf("drain thread=%zu start\n", uv_thread_self());
  udx_t *udx = data;
  int err;

  uv_loop_t *loop = &udx->worker.loop;

  udx->worker.buffer = malloc(sizeof(udx__drain_slot_t) * N_SLOTS);
  udx->worker.buffer_len = N_SLOTS;

  udx->worker.status = RUNNING;
  err = uv_run(loop, UV_RUN_DEFAULT);
  assert(err == 0);

  free(udx->worker.buffer);

  debug_printf("drain thread=%zu exit\n", uv_thread_self());

  err = uv_async_send(&udx->worker.signal_thread_stopped); // notify main
  assert(err == 0);
}

/// exports

int
udx__drainer_init(udx_t *udx) {
  if (udx->worker.status != STOPPED) return 0; // do nothing
  int err;

  err = uv_async_init(udx->loop, &udx->worker.signal_drain, on_drain); // mainloop
  assert(err == 0);
  udx->worker.signal_drain.data = udx;

  err = uv_async_init(udx->loop, &udx->worker.signal_thread_stopped, on_thread_stop); // mainloop
  assert(err == 0);
  udx->worker.signal_thread_stopped.data = udx;

  err = uv_loop_init(&udx->worker.loop);
  assert(err == 0);

  err = uv_async_init(&udx->worker.loop, &udx->worker.signal_control, on_control); // subloop
  assert(err == 0);
  udx->worker.signal_control.data = udx;

  udx->worker.status = INITIALIZED;

  err = uv_thread_create(&udx->worker.thread_id, reader_thread, udx);
  if (err) return err;

  debug_printf("thread launched, id=%zu loop=%p handles{ drain=%p, ctrl=%p }\n", udx->worker.thread_id, &udx->worker.loop, &udx->worker.signal_drain, &udx->worker.signal_control);
  return 0;
}

int
udx__drainer_socket_init (udx_socket_t *socket) {
  udx_t *udx = socket->udx;
  uv_os_fd_t fd;
  assert(0 == uv_fileno((uv_handle_t *) &socket->handle, &fd));
  return run_command(udx, SOCKET_INIT, socket);
}

int
udx__drainer_socket_stop (udx_socket_t *socket) {
  udx_t *udx = socket->udx;
  return run_command(udx, SOCKET_REMOVE, socket);
}

int
udx__drainer_destroy (udx_t *udx) {
  uv_close((uv_handle_t *) &udx->worker.signal_drain, NULL);

  return run_command(udx, THREAD_STOP, NULL);
}
#endif // USE_DRAIN_THREAD
