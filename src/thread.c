#include <stdlib.h>
#include <assert.h>
#include <uv.h>
#include <memory.h>

#include "../include/udx.h"
#include "io.h"
#include "internal.h"

#define debugger __builtin_debugtrap();

/**
 * note to self:
 *
 * Next up:
 * - [x] need a small queue for control signals.
 * - [x] how to thread-safe without stdatomic.h? (atomic allowed, msvc<2017 unsupported)
 * - [ ] threadsafe malloc/free ctrl queue
 * - [x] drain kernel buffer / prove multi-threaded poll works
 * - [x] stash data into buffer
 * - [x] notify drain available
 * - [ ] patch buffer back into on_recv/process_packet()
 */

// TODO: maybe move to udx.c instead of importing multiple deps via internal.h
static void
on_drain (uv_async_t *signal) {
  udx_t *udx = signal->data;
  udx_reader_t *worker = &udx->worker;

  struct sockaddr_storage addr = {0};

  udx__drain_slot_t *slot;
  udx_socket_t *socket;

  while (worker->cursors.drained != worker->cursors.read) {
    slot = &worker->buffer[worker->cursors.drained];

    socket = slot->socket;

    if (!process_packet(socket, slot->buffer, slot->len, (struct sockaddr *) &addr) && socket->on_recv != NULL) {
      if (is_addr_v4_mapped((struct sockaddr *) &addr)) {
        addr_to_v4((struct sockaddr_in6 *) &addr);
      }

      uv_buf_t buf = {
        .base = slot->buffer,
        .len = slot->len
      };

      // TODO: ask, what is the difference between read_len and buf_len?

      socket->on_recv(socket, slot->len, &buf, (struct sockaddr *) &addr);
    }
    worker->cursors.drained = (worker->cursors.drained + 1) % worker->buffer_len;
  }
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

// borrowed from udx.c:on_uv_poll()
static void
stub_on_uv_poll (uv_poll_t *handle, int status, int events) {
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
    // TODO: skip/'contine' when size == 0 or break or process anyway?
    slot->len = size;

    uv_async_send(&worker->signal_drain);

    int next = (worker->cursors.read + 1) % worker->buffer_len;

    if (worker->cursors.drained == next) {
      printf("buffer full, packet dropped");
      continue;
    }

    worker->cursors.read = next;
  } while(1);

reset_poll:
  assert(stub_update_poll(socket) == 0);
}

static int
stub_update_poll (udx_socket_t *socket) {
  if (socket->status & UDX_SOCKET_CLOSED) return 0;

  uv_poll_t *poll = &socket->drain_poll;
  return uv_poll_start(poll, UV_READABLE, stub_on_uv_poll);
}

static void
read_poll_start (udx_t *udx, udx_socket_t *socket) {
  uv_loop_t *loop = &udx->worker.loop;

  int err = 0;
  uv_poll_t *poll = &socket->drain_poll;
  uv_os_fd_t fd;

  err = uv_fileno((const uv_handle_t *) &socket->handle, &fd);
  assert(err == 0);

  printf("read_poll_start(fd: %i) tid: %zu\n", fd, uv_thread_self());

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
  // udx_queue_node_t queue; replaces/reuses next/ompfh; assuming udx_queue is not thread safe
} command_t;

static void
on_control (uv_async_t *signal) {
  udx_t *udx = signal->data;

  command_t *head;
  // TODO: not threadsafe
  head = udx->worker.commands;
  udx->worker.commands = NULL;

  // process queue
  while (head != NULL) {
    printf("sub thread: on_control(%i) t: %zu\n", head->type, uv_thread_self());
    switch (head->type) {
      case SOCKET_INIT:
        read_poll_start(udx, head->data);
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
  printf("run_command(%i) \t\ttid: %zu\n", type, uv_thread_self());

  // TODO: try to use queue.c instead?
  command_t *cmd = malloc(sizeof(command_t));
  cmd->type = type;
  cmd->data = data;
  cmd->next = NULL;

  // TODO: not threadsafe
  command_t *head = udx->worker.commands;
  if (head != NULL) {
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

  uv_loop_t *loop = &udx->worker.loop;

  printf("uv_run(worker) \t\ttid: %zu\n", uv_thread_self());

  err = uv_run(loop, UV_RUN_DEFAULT);
  assert(err == 0);

  printf("sub loop & thread exit\n");
}

/* =======.
 * exports |
 * ======="*/

int
__udx_read_poll_setup(udx_t *udx) {
  printf("read_poll_setup() main \ttid: %zu\n", uv_thread_self());

  int err;
  err = uv_async_init(udx->loop, &udx->worker.signal_drain, on_drain);
  if (err) return err;

  // not sure where to allocate drain buffer
  uint16_t n_slots = 32; // let's do 32 packets for now.
  udx->worker.buffer = malloc(sizeof(udx__drain_slot_t) * n_slots);
  udx->worker.buffer_len = n_slots;

  udx->worker.signal_drain.data = udx;

  err = uv_loop_init(&udx->worker.loop);
  assert(err == 0);

  err = uv_async_init(&udx->worker.loop, &udx->worker.signal_control, on_control);
  assert(err == 0);

  udx->worker.signal_control.data = udx;

  err = uv_thread_create(&udx->worker.thread_id, reader_thread, udx);
  if (err) return err;

  return 0;
}

int
__udx_read_poll_start (udx_t *udx, udx_socket_t *socket) {
  uv_os_fd_t fd;
  assert(0 == uv_fileno((uv_handle_t *) &socket->handle, &fd));
  printf("__poll_start fd: %i \ttid: %zu\n", fd, uv_thread_self());
  return run_command(udx, SOCKET_INIT, socket);
}

int
__udx_read_poll_stop (udx_t *udx, udx_socket_t *socket) {
  return run_command(udx, SOCKET_REMOVE, socket);
}

int
__udx_read_poll_destroy (udx_t *udx) {
  UDX_UNUSED(udx);
  free(udx->worker.buffer);
  // close all handles
  // uv_close((uv_handle_t *) &udx->worker.signals_main.close, reader_signal_close);

  // run on subloop uv_close((uv_handle_t *) &udx->worker.miso, reader_signal_close);

  // TODO: uv_thread_join()
  printf("sub thread end\n");
  return 0;
}
