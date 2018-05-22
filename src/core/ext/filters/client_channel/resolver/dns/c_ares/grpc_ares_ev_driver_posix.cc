/*
 *
 * Copyright 2016 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */
#include <grpc/support/port_platform.h>

#include "src/core/lib/iomgr/port.h"
#if GRPC_ARES == 1 && defined(GRPC_POSIX_SOCKET)

#include <ares.h>
#include <string.h>
#include <sys/ioctl.h>

#include "src/core/ext/filters/client_channel/resolver/dns/c_ares/grpc_ares_ev_driver.h"

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/time.h>
#include "src/core/ext/filters/client_channel/resolver/dns/c_ares/grpc_ares_wrapper.h"
#include "src/core/lib/gpr/string.h"
#include "src/core/lib/iomgr/ev_posix.h"
#include "src/core/lib/iomgr/iomgr_internal.h"
#include "src/core/lib/iomgr/sockaddr_utils.h"

typedef struct fd_node {
  /** the owner of this fd node */
  grpc_ares_ev_driver* ev_driver;
  /** a closure wrapping on_readable_locked, which should be
     invoked when the grpc_fd in this node becomes readable. */
  grpc_closure read_closure;
  /** a closure wrapping on_writable_locked, which should be
     invoked when the grpc_fd in this node becomes writable. */
  grpc_closure write_closure;
  /** next fd node in the list */
  struct fd_node* next;

  /** the grpc_fd owned by this fd node */
  grpc_fd* fd;
  /** if the readable closure has been registered */
  bool readable_registered;
  /** if the writable closure has been registered */
  bool writable_registered;
  /** if the fd has been shutdown yet from grpc iomgr perspective */
  bool already_shutdown;
  /** is c-ares interested in events/data on this fd */
  bool ares_library_is_interested;
} fd_node;

struct grpc_ares_ev_driver {
  /** the ares_channel owned by this event driver */
  ares_channel channel;
  /** pollset set for driving the IO events of the channel */
  grpc_pollset_set* pollset_set;
  /** refcount of the event driver */
  gpr_refcount refs;

  /** combiner to synchronize c-ares and I/O callbacks on */
  grpc_combiner* combiner;
  /** a list of grpc_fd that this event driver is currently using. */
  fd_node* fds;
  /** is this event driver currently working? */
  bool working;
  /** is this event driver being shut down */
  bool shutting_down;
};

static void grpc_ares_notify_on_event_locked(grpc_ares_ev_driver* ev_driver);

static grpc_ares_ev_driver* grpc_ares_ev_driver_ref(
    grpc_ares_ev_driver* ev_driver) {
  gpr_log(GPR_DEBUG, "Ref ev_driver %" PRIuPTR, (uintptr_t)ev_driver);
  gpr_ref(&ev_driver->refs);
  return ev_driver;
}

static void grpc_ares_ev_driver_unref(grpc_ares_ev_driver* ev_driver) {
  gpr_log(GPR_DEBUG, "Unref ev_driver %" PRIuPTR, (uintptr_t)ev_driver);
  if (gpr_unref(&ev_driver->refs)) {
    gpr_log(GPR_DEBUG, "destroy ev_driver %" PRIuPTR, (uintptr_t)ev_driver);
    GPR_ASSERT(ev_driver->fds == nullptr);
    GRPC_COMBINER_UNREF(ev_driver->combiner, "free ares event driver");
    ares_destroy(ev_driver->channel);
    gpr_free(ev_driver);
  }
}

static void fd_node_destroy_locked(fd_node* fdn) {
  gpr_log(GPR_DEBUG, "delete fd: %d", grpc_fd_wrapped_fd(fdn->fd));
  GPR_ASSERT(!fdn->readable_registered);
  GPR_ASSERT(!fdn->writable_registered);
  GPR_ASSERT(!fdn->ares_library_is_interested);
  GPR_ASSERT(fdn->already_shutdown);
  /* TODO: we need to pass a non-null "release_fd" parameter to
   * grpc_fd_orphan because "epollsig" iomgr will close the fd
   * even if "already_closed" is true, and it only leaves it open
   * if "release_fd" is non-null. This is unlike the rest of the
   * pollers, should this be changed within epollsig? */
  int dummy_release_fd;
  /* c-ares library has closed the fd inside grpc_fd. This fd may be picked up
     immediately by another thread, and should not be closed by the following
     grpc_fd_orphan. */
  grpc_fd_orphan(fdn->fd, nullptr, &dummy_release_fd, true /* already_closed */,
                 "c-ares query finished");
  gpr_free(fdn);
}

static void fd_node_shutdown_locked(fd_node* fdn, const char* reason) {
  if (!fdn->already_shutdown) {
    fdn->already_shutdown = true;
    grpc_fd_shutdown(
        fdn->fd, GRPC_ERROR_CREATE_FROM_STATIC_STRING("c-ares fd shutdown"));
  }
}

grpc_error* grpc_ares_ev_driver_create(grpc_ares_ev_driver** ev_driver,
                                       grpc_pollset_set* pollset_set,
                                       grpc_combiner* combiner) {
  *ev_driver = static_cast<grpc_ares_ev_driver*>(
      gpr_malloc(sizeof(grpc_ares_ev_driver)));
  ares_options opts;
  memset(&opts, 0, sizeof(opts));
  opts.flags |= ARES_FLAG_STAYOPEN;
  int status = ares_init_options(&(*ev_driver)->channel, &opts, ARES_OPT_FLAGS);
  gpr_log(GPR_DEBUG, "grpc_ares_ev_driver_create");
  if (status != ARES_SUCCESS) {
    char* err_msg;
    gpr_asprintf(&err_msg, "Failed to init ares channel. C-ares error: %s",
                 ares_strerror(status));
    grpc_error* err = GRPC_ERROR_CREATE_FROM_COPIED_STRING(err_msg);
    gpr_free(err_msg);
    gpr_free(*ev_driver);
    return err;
  }
  (*ev_driver)->combiner = GRPC_COMBINER_REF(combiner, "ares event driver");
  gpr_ref_init(&(*ev_driver)->refs, 1);
  (*ev_driver)->pollset_set = pollset_set;
  (*ev_driver)->fds = nullptr;
  (*ev_driver)->working = false;
  (*ev_driver)->shutting_down = false;
  return GRPC_ERROR_NONE;
}

void grpc_ares_ev_driver_destroy_locked(grpc_ares_ev_driver* ev_driver) {
  // We mark the event driver as being shut down. If the event driver
  // is working, grpc_ares_notify_on_event_locked will shut down the
  // fds; if it's not working, there are no fds to shut down.
  ev_driver->shutting_down = true;
  grpc_ares_ev_driver_unref(ev_driver);
}

void grpc_ares_ev_driver_shutdown_locked(grpc_ares_ev_driver* ev_driver) {
  ev_driver->shutting_down = true;
  fd_node* fn = ev_driver->fds;
  while (fn != nullptr) {
    fd_node_shutdown_locked(fn, "grpc_ares_ev_driver_shutdown");
    fn = fn->next;
  }
}

// Search fd in the fd_node list head. This is an O(n) search, the max possible
// value of n is ARES_GETSOCK_MAXNUM (16). n is typically 1 - 2 in our tests.
static fd_node* pop_fd_node_locked(fd_node** head, int fd) {
  fd_node dummy_head;
  dummy_head.next = *head;
  fd_node* node = &dummy_head;
  while (node->next != nullptr) {
    if (grpc_fd_wrapped_fd(node->next->fd) == fd) {
      fd_node* ret = node->next;
      node->next = node->next->next;
      *head = dummy_head.next;
      return ret;
    }
    node = node->next;
  }
  return nullptr;
}

/* Check if \a fd is still readable */
static bool grpc_ares_is_fd_still_readable_locked(
    grpc_ares_ev_driver* ev_driver, int fd) {
  size_t bytes_available = 0;
  return ioctl(fd, FIONREAD, &bytes_available) == 0 && bytes_available > 0;
}

static void on_readable_locked(void* arg, grpc_error* error) {
  fd_node* fdn = static_cast<fd_node*>(arg);
  grpc_ares_ev_driver* ev_driver = fdn->ev_driver;
  const int fd = grpc_fd_wrapped_fd(fdn->fd);
  fdn->readable_registered = false;
  if (!fdn->ares_library_is_interested && !fdn->writable_registered) {
    fd_node_destroy_locked(fdn);
    grpc_ares_ev_driver_unref(ev_driver);
    return;
  }

  gpr_log(GPR_DEBUG, "readable on %d", fd);
  if (error == GRPC_ERROR_NONE) {
    do {
      ares_process_fd(ev_driver->channel, fd, ARES_SOCKET_BAD);
    } while (grpc_ares_is_fd_still_readable_locked(ev_driver, fd));
  } else {
    // If error is not GRPC_ERROR_NONE, it means the fd has been shutdown or
    // timed out. The pending lookups made on this ev_driver will be cancelled
    // by the following ares_cancel() and the on_done callbacks will be invoked
    // with a status of ARES_ECANCELLED. The remaining file descriptors in this
    // ev_driver will be cleaned up in the follwing
    // grpc_ares_notify_on_event_locked().
    ares_cancel(ev_driver->channel);
  }
  grpc_ares_notify_on_event_locked(ev_driver);
  grpc_ares_ev_driver_unref(ev_driver);
}

static void on_writable_locked(void* arg, grpc_error* error) {
  fd_node* fdn = static_cast<fd_node*>(arg);
  grpc_ares_ev_driver* ev_driver = fdn->ev_driver;
  const int fd = grpc_fd_wrapped_fd(fdn->fd);
  fdn->writable_registered = false;
  if (!fdn->ares_library_is_interested && !fdn->readable_registered) {
    fd_node_destroy_locked(fdn);
    grpc_ares_ev_driver_unref(ev_driver);
    return;
  }

  gpr_log(GPR_DEBUG, "writable on %d", fd);
  if (error == GRPC_ERROR_NONE) {
    ares_process_fd(ev_driver->channel, ARES_SOCKET_BAD, fd);
  } else {
    // If error is not GRPC_ERROR_NONE, it means the fd has been shutdown or
    // timed out. The pending lookups made on this ev_driver will be cancelled
    // by the following ares_cancel() and the on_done callbacks will be invoked
    // with a status of ARES_ECANCELLED. The remaining file descriptors in this
    // ev_driver will be cleaned up in the follwing
    // grpc_ares_notify_on_event_locked().
    ares_cancel(ev_driver->channel);
  }
  grpc_ares_notify_on_event_locked(ev_driver);
  grpc_ares_ev_driver_unref(ev_driver);
}

ares_channel* grpc_ares_ev_driver_get_channel(grpc_ares_ev_driver* ev_driver) {
  return &ev_driver->channel;
}

grpc_combiner* grpc_ares_ev_driver_get_combiner(
    grpc_ares_ev_driver* ev_driver) {
  return ev_driver->combiner;
}

// Get the file descriptors used by the ev_driver's ares channel, register
// driver_closure with these filedescriptors.
static void grpc_ares_notify_on_event_locked(grpc_ares_ev_driver* ev_driver) {
  fd_node* new_list = nullptr;
  if (!ev_driver->shutting_down) {
    ares_socket_t socks[ARES_GETSOCK_MAXNUM];
    int socks_bitmask =
        ares_getsock(ev_driver->channel, socks, ARES_GETSOCK_MAXNUM);
    for (size_t i = 0; i < ARES_GETSOCK_MAXNUM; i++) {
      if (ARES_GETSOCK_READABLE(socks_bitmask, i) ||
          ARES_GETSOCK_WRITABLE(socks_bitmask, i)) {
        fd_node* fdn = pop_fd_node_locked(&ev_driver->fds, socks[i]);
        // Create a new fd_node if sock[i] is not in the fd_node list.
        if (fdn == nullptr) {
          char* fd_name;
          gpr_asprintf(&fd_name, "ares_ev_driver-%" PRIuPTR, i);
          fdn = static_cast<fd_node*>(gpr_malloc(sizeof(fd_node)));
          gpr_log(GPR_DEBUG, "new fd: %d", socks[i]);
          fdn->fd = grpc_fd_create(socks[i], fd_name);
          fdn->ev_driver = ev_driver;
          fdn->readable_registered = false;
          fdn->writable_registered = false;
          fdn->ares_library_is_interested = true;
          fdn->already_shutdown = false;
          GRPC_CLOSURE_INIT(&fdn->read_closure, on_readable_locked, fdn,
                            grpc_combiner_scheduler(ev_driver->combiner));
          GRPC_CLOSURE_INIT(&fdn->write_closure, on_writable_locked, fdn,
                            grpc_combiner_scheduler(ev_driver->combiner));
          grpc_pollset_set_add_fd(ev_driver->pollset_set, fdn->fd);
          gpr_free(fd_name);
        }
        fdn->next = new_list;
        new_list = fdn;
        // Register read_closure if the socket is readable and read_closure has
        // not been registered with this socket.
        if (ARES_GETSOCK_READABLE(socks_bitmask, i) &&
            !fdn->readable_registered) {
          grpc_ares_ev_driver_ref(ev_driver);
          gpr_log(GPR_DEBUG, "notify read on: %d", grpc_fd_wrapped_fd(fdn->fd));
          grpc_fd_notify_on_read(fdn->fd, &fdn->read_closure);
          fdn->readable_registered = true;
        }
        // Register write_closure if the socket is writable and write_closure
        // has not been registered with this socket.
        if (ARES_GETSOCK_WRITABLE(socks_bitmask, i) &&
            !fdn->writable_registered) {
          gpr_log(GPR_DEBUG, "notify write on: %d",
                  grpc_fd_wrapped_fd(fdn->fd));
          grpc_ares_ev_driver_ref(ev_driver);
          grpc_fd_notify_on_write(fdn->fd, &fdn->write_closure);
          fdn->writable_registered = true;
        }
      }
    }
  }
  // Any remaining fds in ev_driver->fds were not returned by ares_getsock() and
  // are therefore no longer in use, so they can be shut down and removed from
  // the list.
  while (ev_driver->fds != nullptr) {
    fd_node* cur = ev_driver->fds;
    ev_driver->fds = ev_driver->fds->next;
    cur->ares_library_is_interested = false;
    fd_node_shutdown_locked(cur, "c-ares fd shutdown");
    if (!cur->readable_registered && !cur->writable_registered) {
      fd_node_destroy_locked(cur);
    }
  }
  ev_driver->fds = new_list;
  // If the ev driver has no working fd, all the tasks are done.
  if (new_list == nullptr) {
    ev_driver->working = false;
    gpr_log(GPR_DEBUG, "ev driver stop working");
  }
}

void grpc_ares_ev_driver_start_locked(grpc_ares_ev_driver* ev_driver) {
  if (!ev_driver->working) {
    ev_driver->working = true;
    grpc_ares_notify_on_event_locked(ev_driver);
  }
}

#endif /* GRPC_ARES == 1 && defined(GRPC_POSIX_SOCKET) */
