#include <stdlib.h>
#include <assert.h>
#include <uv.h>
#include <memory.h>

#include "../include/udx.h"
#include "internal.h"

#include "debug.h"

static void
on_miso (uv_async_t *signal) {
  UDX_UNUSED(signal);
  printf("miso: message from sub, running on main\n");
}

/*
static void
command_close (uv_handle_t *signal) {
  UDX_UNUSED(signal);
  printf("handle closed\n");
  // if (!--pending) uv_async_send(miso_closed);
}
*/

static int stub_update_poll (udx_socket_t *socket);

/**
 * note to self:
 * There is a low-level protocol in udx (relaying / ordering / retransmission)
 * it includes uv_timers and uv_calls.
 * so it's not possible to simply divert the read flow here.
 * A)
 * - remove the defined update_poll and on_uv_poll functions here.
 * - make the original pollers thread/loop aware.
 * - check out what other uv-objects would have to be moved;
 *
 * B)
 * - or... actually blindly drain the data from kernel here;
 *   and act as a prebuffer/ gracefully return flow back to `udx.c:on_uv_poll`
 *
 *
 * Next up:
 * need a small queue for control signals.
 */

// TODO: remove after verify multithreaded poll works
static void
stub_on_uv_poll (uv_poll_t *handle, int status, int events) {
  UDX_UNUSED(status);
  udx_socket_t *socket = handle->data;

  printf("event: %i, socket->events: %i\n", events, socket->events);
  debugger
  // TODO: uv_async_send(loop, miso_drain);

  assert(stub_update_poll(socket) == 0);
}

// TODO: remove
static int
stub_update_poll (udx_socket_t *socket) {
  uv_poll_t *poll = &socket->drain_poll;
  return uv_poll_start(poll, UV_READABLE, stub_on_uv_poll);
}

static void
socket_init (udx_t *udx, udx_socket_t *socket) {
  uv_loop_t *loop = &udx->worker.loop;

  int err = 0;
  uv_poll_t *poll = &socket->drain_poll;
  uv_os_fd_t fd;

  err = uv_fileno((const uv_handle_t *) &socket->handle, &fd);
  assert(err == 0);

  printf("read_poll_start: %i\n", fd);

  err = uv_poll_init_socket(loop, poll, (uv_os_sock_t) fd);
  assert(err == 0);

  poll->data = socket;

  err = stub_update_poll(socket); // TODO: use main poll updater instead
  assert(err == 0);
}

enum command_id {
  SOCKET_INIT = 0,
  SOCKET_REMOVE,
  CLOSE
};

typedef struct command_s {
  enum command_id type;
  void *data;
  struct command_s *next;
} command_t;

static void
on_control (uv_async_t *signal) {
  udx_t *udx = signal->data;
  // TODO: threadsafe
  command_t *head = udx->worker.commands;
  printf("sub thread: on_control() t: %zu\n", uv_thread_self());
  while (head != NULL) {
    switch (head->type) {
      case SOCKET_INIT:
        socket_init(udx, head->data);
        break;

      case SOCKET_REMOVE:
        printf("thread.c: stop polling not implemented\n");
        break;

      case CLOSE:
        printf("thread.c: close not implemented\n");
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
  printf("run_command(%i) %zu\n", type, uv_thread_self());

  command_t *cmd = malloc(sizeof(command_t));
  cmd->type = type;
  cmd->data = data;
  cmd->next = NULL;

  // TODO: threadsafe
  command_t *head = udx->worker.commands;
  if (head) {
    while (head->next != NULL) head = head->next;
    head->next = cmd;
  } else {
    udx->worker.commands = cmd;
  }

  return uv_async_send(&udx->worker.signal_control);
}

static void reader_thread (void *data) {
  udx_t *udx = data;
  int err;

  udx_reader_t *worker = &(udx->worker);
  uv_loop_t *loop = &(udx->worker.loop);

  printf("background thread started %zu\n", uv_thread_self());
  err = uv_loop_init(loop);
  assert(err == 0);

  err = uv_async_init(loop, &worker->signal_control, on_control);
  assert(err == 0);

  printf("starting loop\n");
  err = uv_run(loop, UV_RUN_DEFAULT);
  assert(err == 0);

  printf("sub loop & thread exit\n");
}

/* =======.
 * exports |
 * ======="*/

int
__udx_read_poll_setup(udx_t *udx) {
  printf("read_poll_setup() %zu\n", uv_thread_self());

  int err;
  err = uv_async_init(udx->loop, &udx->worker.signal_drain, on_miso);
  if (err) return err;

  err = uv_thread_create(&udx->worker.thread_id, reader_thread, udx);
  if (err) return err;

  return 0;
}

int
__udx_read_poll_start (udx_t *udx, udx_socket_t *socket) {
  return run_command(udx, SOCKET_INIT, socket);
}

int
__udx_read_poll_stop (udx_t *udx, udx_socket_t *socket) {
  return run_command(udx, SOCKET_REMOVE, socket);
}

int
__udx_read_poll_destroy (udx_t *udx) {
  UDX_UNUSED(udx);

  // close all handles
  // uv_close((uv_handle_t *) &udx->worker.signals_main.close, reader_signal_close);

  // run on subloop uv_close((uv_handle_t *) &udx->worker.miso, reader_signal_close);

  // TODO: uv_thread_join()
  printf("sub thread end\n");
  return 0;
}
