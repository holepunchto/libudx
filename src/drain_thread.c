#include <stdlib.h>
#include <assert.h>
#include <uv.h>
#include <memory.h>

#include "../include/udx.h"

#ifdef USE_DRAIN_THREAD

#include "debug.h"
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
  udx_reader_t *thread = &udx->thread;

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
thread_update_read_poll (udx_socket_t *socket);

static void
thread_on_uv_read_poll (uv_poll_t *handle, int status, int events) {
  UDX_UNUSED(status);
  udx_socket_t *socket = handle->data;

  udx_t *udx = socket->udx;
  udx_reader_t *thread = &udx->thread;

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

    int err = uv_async_send(&thread->signal_drain);
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
  assert(thread_update_read_poll(socket) == 0);
}

static int
thread_update_read_poll (udx_socket_t *socket) {
  if (socket->status & UDX_SOCKET_CLOSED) return 0;

  uv_poll_t *poll = &socket->drain_poll;
  return uv_poll_start(poll, UV_READABLE, thread_on_uv_read_poll);
}

static void
_on_poll_stopped_main (uv_async_t *signal) { // main loop
  debug_printf("drain thread=%zu socket poll stopped\n", uv_thread_self());
  udx_socket_t *socket = signal->data;

  uv_close((uv_handle_t *) signal, NULL); // close the jump signal

  udx__drainer__on_poll_stop(socket); // return to udx.c
}

static void
thread_read_poll_start (udx_socket_t *socket) { // aka drainer_socket_init
  int err = 0;

  assert(socket->udx->thread.status == RUNNING);
  assert(socket->drain_poll_initialized == false);

  if (socket->status & UDX_SOCKET_CLOSED) return;

  udx_t *udx = socket->udx;
  uv_loop_t *subloop = &udx->thread.loop;

  uv_poll_t *poll = &socket->drain_poll;
  uv_os_fd_t fd;

  err = uv_fileno((const uv_handle_t *) &socket->handle, &fd);
  assert(err == 0);

  debug_printf("drain thread=%zu socket poll fd=%i start\n", uv_thread_self(), fd);

  err = uv_poll_init_socket(subloop, poll, (uv_os_sock_t) fd);
  assert(err == 0);

  poll->data = socket;

  err = thread_update_read_poll(socket);
  assert(err == 0);

  socket->drain_poll_initialized = true;
}

static void
thread_on_uv_poll_close (uv_handle_t *handle) { // sub loop
  udx_socket_t *socket = handle->data;
  int err = uv_async_send(&socket->signal_poll_stopped);
  assert(err == 0);
}

static inline void
thread_read_poll_stop (udx_socket_t *socket) { // sub loop
  int err;

  if (!socket->drain_poll_initialized) {
    err = uv_async_send(&socket->signal_poll_stopped);
    assert(err == 0);
    return;
  }

  err = uv_poll_stop(&socket->drain_poll);
  assert(err == 0);

  uv_close((uv_handle_t *) &socket->drain_poll, thread_on_uv_poll_close);
}

static void
thread_on_control (uv_async_t *signal) {
  udx_t *udx = signal->data;

  command_t *head = atomic_exchange(&udx->thread.ctrl_queue, NULL);
  assert(head != NULL);

  // process queue
  while (head != NULL) {
    switch (head->type) {
      case SOCKET_INIT:
        thread_read_poll_start(head->data);
        break;

      case SOCKET_REMOVE:
        thread_read_poll_stop(head->data);
        break;

      case THREAD_STOP:
        // close control handle that keeps subloop active.
        uv_close((uv_handle_t *) &udx->thread.signal_control, NULL);
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

  command_t *head = atomic_load(&udx->thread.ctrl_queue);

  if (head == NULL) {
    atomic_store(&udx->thread.ctrl_queue, cmd);
  } else {
    while (head->next != NULL) head = head->next;
    head->next = cmd;
  }

  if (udx->thread.status != RUNNING) {
    return 0;
  } else {
    return uv_async_send(&udx->thread.signal_control);
  }
}

static void
on_thread_stop(uv_async_t *signal) { // main loop
  udx_t *udx = signal->data;

  assert(udx->thread.status == RUNNING);
  udx->thread.status = STOPPED;

  uv_close((uv_handle_t *) signal, NULL); // release main loop
}

static void
reader_thread (void *data) {
  debug_printf("drain thread=%zu start\n", uv_thread_self());
  udx_t *udx = data;
  int err;

  uv_loop_t *subloop = &udx->thread.loop;

  err = uv_loop_init(subloop);
  assert(err == 0);

  err = uv_async_init(subloop, &udx->thread.signal_control, thread_on_control);
  assert(err == 0);
  udx->thread.signal_control.data = udx;


  udx->thread.buffer = malloc(sizeof(udx__drain_slot_t) * N_SLOTS);
  udx->thread.buffer_len = N_SLOTS;

  udx->thread.status = RUNNING;

  err = uv_async_send(&udx->thread.signal_control); // queue pending cmds
  assert(err == 0);

  err = uv_run(subloop, UV_RUN_DEFAULT);
  assert(err == 0);

  free(udx->thread.buffer);

  debug_printf("drain thread=%zu exit\n", uv_thread_self());

  err = uv_async_send(&udx->thread.signal_thread_stopped); // notify main
  assert(err == 0);
}

static void launch_thread (uv_async_t *signal) {
  udx_t *udx = signal->data;
  uv_close((uv_handle_t *) signal, NULL);

  int err = uv_thread_create(&udx->thread.thread_id, reader_thread, udx);
  assert(err == 0);

  debug_printf("thread launched, id=%zu loop=%p handles{ drain=%p, ctrl=%p }\n", udx->thread.thread_id, &udx->thread.loop, &udx->thread.signal_drain, &udx->thread.signal_control);
}

/// exports

int
udx__drainer_init(udx_t *udx) {
  if (udx->thread.status != STOPPED) return 0; // do nothing
  int err;
  uv_loop_t *loop = udx->loop;

  err = uv_async_init(loop, &udx->thread.signal_drain, on_drain);
  assert(err == 0);
  udx->thread.signal_drain.data = udx;

  err = uv_async_init(loop, &udx->thread.signal_thread_stopped, on_thread_stop);
  assert(err == 0);
  udx->thread.signal_thread_stopped.data = udx;

  // queue thread start onto main loop (don't spin up subloop before main loop starts)
  err = uv_async_init(loop, &udx->thread.launch_thread, launch_thread); // mainloop
  assert(err == 0);
  udx->thread.launch_thread.data = udx;

  udx->thread.status = INITIALIZED;

  uv_async_send(&udx->thread.launch_thread);

  return 0;
}

int
udx__drainer_socket_init (udx_socket_t *socket) {
  udx_t *udx = socket->udx;
  uv_os_fd_t fd;
  assert(0 == uv_fileno((uv_handle_t *) &socket->handle, &fd));

  // initialize poll close signal
  int err = uv_async_init(udx->loop, &socket->signal_poll_stopped, _on_poll_stopped_main);
  assert(err == 0);
  socket->signal_poll_stopped.data = socket;

  return run_command(udx, SOCKET_INIT, socket);
}

int
udx__drainer_socket_stop (udx_socket_t *socket) {
  udx_t *udx = socket->udx;
  return run_command(udx, SOCKET_REMOVE, socket);
}

int
udx__drainer_destroy (udx_t *udx) {
  if (udx->thread.status == STOPPED) return 0;

  uv_close((uv_handle_t *) &udx->thread.signal_drain, NULL);

  return run_command(udx, THREAD_STOP, NULL);
}
#endif // USE_DRAIN_THREAD
