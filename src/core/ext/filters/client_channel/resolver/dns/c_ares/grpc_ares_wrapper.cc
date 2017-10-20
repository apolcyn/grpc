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
#if GRPC_ARES == 1 && !defined(GRPC_UV)

#include "src/core/ext/filters/client_channel/resolver/dns/c_ares/grpc_ares_wrapper.h"
#include "src/core/lib/iomgr/sockaddr.h"
#include "src/core/lib/iomgr/socket_utils_posix.h"

#include <string.h>
#include <sys/types.h>
#include <errno.h>
#include <netinet/in.h>

#include <ares.h>
#include <grpc/support/alloc.h>
#include <grpc/support/host_port.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/time.h>
#include <grpc/support/useful.h>

#include "src/core/ext/filters/client_channel/parse_address.h"
#include "src/core/ext/filters/client_channel/resolver/dns/c_ares/grpc_ares_ev_driver.h"
#include "src/core/lib/iomgr/error.h"
#include "src/core/lib/iomgr/executor.h"
#include "src/core/lib/iomgr/iomgr_internal.h"
#include "src/core/lib/iomgr/nameser.h"
#include "src/core/lib/iomgr/sockaddr_utils.h"
#include "src/core/lib/support/string.h"

grpc_tracer_flag grpc_trace_cares_address_sorting = GRPC_TRACER_INITIALIZER(false, "cares_address_sorting");

static gpr_once g_basic_init = GPR_ONCE_INIT;
static gpr_mu g_init_mu;

struct grpc_ares_request {
  /** indicates the DNS server to use, if specified */
  struct ares_addr_port_node dns_server_addr;
  /** following members are set in grpc_resolve_address_ares_impl */
  /** closure to call when the request completes */
  grpc_closure *on_done;
  /** the pointer to receive the resolved addresses */
  grpc_lb_addresses **lb_addrs_out;
  /** the pointer to receive the service config in JSON */
  char **service_config_json_out;
  /** the evernt driver used by this request */
  grpc_ares_ev_driver *ev_driver;
  /** number of ongoing queries */
  gpr_refcount pending_queries;

  /** mutex guarding the rest of the state */
  gpr_mu mu;
  /** is there at least one successful query, set in on_done_cb */
  bool success;
  /** the errors explaining the request failure, set in on_done_cb */
  grpc_error *error;
};

typedef struct grpc_ares_hostbyname_request {
  /** following members are set in create_hostbyname_request */
  /** the top-level request instance */
  grpc_ares_request *parent_request;
  /** host to resolve, parsed from the name to resolve */
  char *host;
  /** port to fill in sockaddr_in, parsed from the name to resolve */
  uint16_t port;
  /** is it a grpclb address */
  bool is_balancer;
} grpc_ares_hostbyname_request;

static void do_basic_init(void) { gpr_mu_init(&g_init_mu); }

static uint16_t strhtons(const char *port) {
  if (strcmp(port, "http") == 0) {
    return htons(80);
  } else if (strcmp(port, "https") == 0) {
    return htons(443);
  }
  return htons((unsigned short)atoi(port));
}

static void grpc_ares_request_ref(grpc_ares_request *r) {
  gpr_ref(&r->pending_queries);
}

static void grpc_ares_request_unref(grpc_exec_ctx *exec_ctx,
                                    grpc_ares_request *r) {
  /* If there are no pending queries, invoke on_done callback and destroy the
     request */
  if (gpr_unref(&r->pending_queries)) {
    /* TODO(zyc): Sort results with RFC6724 before invoking on_done. */
    grpc_ares_wrapper_rfc_6724_sort(*(r->lb_addrs_out));
    if (exec_ctx == NULL) {
      /* A new exec_ctx is created here, as the c-ares interface does not
         provide one in ares_host_callback. It's safe to schedule on_done with
         the newly created exec_ctx, since the caller has been warned not to
         acquire locks in on_done. ares_dns_resolver is using combiner to
         protect resources needed by on_done. */
      grpc_exec_ctx new_exec_ctx = GRPC_EXEC_CTX_INIT;
      GRPC_CLOSURE_SCHED(&new_exec_ctx, r->on_done, r->error);
      grpc_exec_ctx_finish(&new_exec_ctx);
    } else {
      GRPC_CLOSURE_SCHED(exec_ctx, r->on_done, r->error);
    }
    gpr_mu_destroy(&r->mu);
    grpc_ares_ev_driver_destroy(r->ev_driver);
    gpr_free(r);
  }
}

static grpc_ares_hostbyname_request *create_hostbyname_request(
    grpc_ares_request *parent_request, char *host, uint16_t port,
    bool is_balancer) {
  grpc_ares_hostbyname_request *hr = (grpc_ares_hostbyname_request *)gpr_zalloc(
      sizeof(grpc_ares_hostbyname_request));
  hr->parent_request = parent_request;
  hr->host = gpr_strdup(host);
  hr->port = port;
  hr->is_balancer = is_balancer;
  grpc_ares_request_ref(parent_request);
  return hr;
}

static void destroy_hostbyname_request(grpc_exec_ctx *exec_ctx,
                                       grpc_ares_hostbyname_request *hr) {
  grpc_ares_request_unref(exec_ctx, hr->parent_request);
  gpr_free(hr->host);
  gpr_free(hr);
}

static void on_hostbyname_done_cb(void *arg, int status, int timeouts,
                                  struct hostent *hostent) {
  grpc_ares_hostbyname_request *hr = (grpc_ares_hostbyname_request *)arg;
  grpc_ares_request *r = hr->parent_request;
  gpr_mu_lock(&r->mu);
  if (status == ARES_SUCCESS) {
    GRPC_ERROR_UNREF(r->error);
    r->error = GRPC_ERROR_NONE;
    r->success = true;
    grpc_lb_addresses **lb_addresses = r->lb_addrs_out;
    if (*lb_addresses == NULL) {
      *lb_addresses = grpc_lb_addresses_create(0, NULL);
    }
    size_t prev_naddr = (*lb_addresses)->num_addresses;
    size_t i;
    for (i = 0; hostent->h_addr_list[i] != NULL; i++) {
    }
    (*lb_addresses)->num_addresses += i;
    (*lb_addresses)->addresses = (grpc_lb_address *)gpr_realloc(
        (*lb_addresses)->addresses,
        sizeof(grpc_lb_address) * (*lb_addresses)->num_addresses);
    for (i = prev_naddr; i < (*lb_addresses)->num_addresses; i++) {
      switch (hostent->h_addrtype) {
        case AF_INET6: {
          size_t addr_len = sizeof(struct sockaddr_in6);
          struct sockaddr_in6 addr;
          memset(&addr, 0, addr_len);
          memcpy(&addr.sin6_addr, hostent->h_addr_list[i - prev_naddr],
                 sizeof(struct in6_addr));
          addr.sin6_family = (sa_family_t)hostent->h_addrtype;
          addr.sin6_port = hr->port;
          grpc_lb_addresses_set_address(
              *lb_addresses, i, &addr, addr_len,
              hr->is_balancer /* is_balancer */,
              hr->is_balancer ? hr->host : NULL /* balancer_name */,
              NULL /* user_data */);
          char output[INET6_ADDRSTRLEN];
          ares_inet_ntop(AF_INET6, &addr.sin6_addr, output, INET6_ADDRSTRLEN);
          gpr_log(GPR_DEBUG,
                  "c-ares resolver gets a AF_INET6 result: \n"
                  "  addr: %s\n  port: %d\n  sin6_scope_id: %d\n",
                  output, ntohs(hr->port), addr.sin6_scope_id);
          break;
        }
        case AF_INET: {
          size_t addr_len = sizeof(struct sockaddr_in);
          struct sockaddr_in addr;
          memset(&addr, 0, addr_len);
          memcpy(&addr.sin_addr, hostent->h_addr_list[i - prev_naddr],
                 sizeof(struct in_addr));
          addr.sin_family = (sa_family_t)hostent->h_addrtype;
          addr.sin_port = hr->port;
          grpc_lb_addresses_set_address(
              *lb_addresses, i, &addr, addr_len,
              hr->is_balancer /* is_balancer */,
              hr->is_balancer ? hr->host : NULL /* balancer_name */,
              NULL /* user_data */);
          char output[INET_ADDRSTRLEN];
          ares_inet_ntop(AF_INET, &addr.sin_addr, output, INET_ADDRSTRLEN);
          gpr_log(GPR_DEBUG,
                  "c-ares resolver gets a AF_INET result: \n"
                  "  addr: %s\n  port: %d\n",
                  output, ntohs(hr->port));
          break;
        }
      }
    }
  } else if (!r->success) {
    char *error_msg;
    gpr_asprintf(&error_msg, "C-ares status is not ARES_SUCCESS: %s",
                 ares_strerror(status));
    grpc_error *error = GRPC_ERROR_CREATE_FROM_COPIED_STRING(error_msg);
    gpr_free(error_msg);
    if (r->error == GRPC_ERROR_NONE) {
      r->error = error;
    } else {
      r->error = grpc_error_add_child(error, r->error);
    }
  }
  gpr_mu_unlock(&r->mu);
  destroy_hostbyname_request(NULL, hr);
}

static void on_srv_query_done_cb(void *arg, int status, int timeouts,
                                 unsigned char *abuf, int alen) {
  grpc_ares_request *r = (grpc_ares_request *)arg;
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  gpr_log(GPR_DEBUG, "on_query_srv_done_cb");
  if (status == ARES_SUCCESS) {
    gpr_log(GPR_DEBUG, "on_query_srv_done_cb ARES_SUCCESS");
    struct ares_srv_reply *reply;
    const int parse_status = ares_parse_srv_reply(abuf, alen, &reply);
    if (parse_status == ARES_SUCCESS) {
      ares_channel *channel = grpc_ares_ev_driver_get_channel(r->ev_driver);
      for (struct ares_srv_reply *srv_it = reply; srv_it != NULL;
           srv_it = srv_it->next) {
        if (grpc_ipv6_loopback_available()) {
          grpc_ares_hostbyname_request *hr = create_hostbyname_request(
              r, srv_it->host, htons(srv_it->port), true /* is_balancer */);
          ares_gethostbyname(*channel, hr->host, AF_INET6,
                             on_hostbyname_done_cb, hr);
        }
        grpc_ares_hostbyname_request *hr = create_hostbyname_request(
            r, srv_it->host, htons(srv_it->port), true /* is_balancer */);
        ares_gethostbyname(*channel, hr->host, AF_INET, on_hostbyname_done_cb,
                           hr);
        grpc_ares_ev_driver_start(&exec_ctx, r->ev_driver);
      }
    }
    if (reply != NULL) {
      ares_free_data(reply);
    }
  } else if (!r->success) {
    char *error_msg;
    gpr_asprintf(&error_msg, "C-ares status is not ARES_SUCCESS: %s",
                 ares_strerror(status));
    grpc_error *error = GRPC_ERROR_CREATE_FROM_COPIED_STRING(error_msg);
    gpr_free(error_msg);
    if (r->error == GRPC_ERROR_NONE) {
      r->error = error;
    } else {
      r->error = grpc_error_add_child(error, r->error);
    }
  }
  grpc_ares_request_unref(&exec_ctx, r);
  grpc_exec_ctx_finish(&exec_ctx);
}

static const char g_service_config_attribute_prefix[] = "grpc_config=";

static void on_txt_done_cb(void *arg, int status, int timeouts,
                           unsigned char *buf, int len) {
  gpr_log(GPR_DEBUG, "on_txt_done_cb");
  char *error_msg;
  grpc_ares_request *r = (grpc_ares_request *)arg;
  const size_t prefix_len = sizeof(g_service_config_attribute_prefix) - 1;
  struct ares_txt_ext *result = NULL;
  struct ares_txt_ext *reply = NULL;
  grpc_error *error = GRPC_ERROR_NONE;
  gpr_mu_lock(&r->mu);
  if (status != ARES_SUCCESS) goto fail;
  status = ares_parse_txt_reply_ext(buf, len, &reply);
  if (status != ARES_SUCCESS) goto fail;
  // Find service config in TXT record.
  for (result = reply; result != NULL; result = result->next) {
    if (result->record_start &&
        memcmp(result->txt, g_service_config_attribute_prefix, prefix_len) ==
            0) {
      break;
    }
  }
  // Found a service config record.
  if (result != NULL) {
    size_t service_config_len = result->length - prefix_len;
    *r->service_config_json_out = (char *)gpr_malloc(service_config_len + 1);
    memcpy(*r->service_config_json_out, result->txt + prefix_len,
           service_config_len);
    for (result = result->next; result != NULL && !result->record_start;
         result = result->next) {
      *r->service_config_json_out = (char *)gpr_realloc(
          *r->service_config_json_out, service_config_len + result->length + 1);
      memcpy(*r->service_config_json_out + service_config_len, result->txt,
             result->length);
      service_config_len += result->length;
    }
    (*r->service_config_json_out)[service_config_len] = '\0';
    gpr_log(GPR_INFO, "found service config: %s", *r->service_config_json_out);
  }
  // Clean up.
  ares_free_data(reply);
  goto done;
fail:
  gpr_asprintf(&error_msg, "C-ares TXT lookup status is not ARES_SUCCESS: %s",
               ares_strerror(status));
  error = GRPC_ERROR_CREATE_FROM_COPIED_STRING(error_msg);
  gpr_free(error_msg);
  if (r->error == GRPC_ERROR_NONE) {
    r->error = error;
  } else {
    r->error = grpc_error_add_child(error, r->error);
  }
done:
  gpr_mu_unlock(&r->mu);
  grpc_ares_request_unref(NULL, r);
}

static grpc_ares_request *grpc_dns_lookup_ares_impl(
    grpc_exec_ctx *exec_ctx, const char *dns_server, const char *name,
    const char *default_port, grpc_pollset_set *interested_parties,
    grpc_closure *on_done, grpc_lb_addresses **addrs, bool check_grpclb,
    char **service_config_json) {
  grpc_error *error = GRPC_ERROR_NONE;
  grpc_ares_hostbyname_request *hr = NULL;
  grpc_ares_request *r = NULL;
  ares_channel *channel = NULL;
  /* TODO(zyc): Enable tracing after #9603 is checked in */
  /* if (grpc_dns_trace) {
      gpr_log(GPR_DEBUG, "resolve_address (blocking): name=%s, default_port=%s",
              name, default_port);
     } */

  /* parse name, splitting it into host and port parts */
  char *host;
  char *port;
  gpr_split_host_port(name, &host, &port);
  if (host == NULL) {
    error = grpc_error_set_str(
        GRPC_ERROR_CREATE_FROM_STATIC_STRING("unparseable host:port"),
        GRPC_ERROR_STR_TARGET_ADDRESS, grpc_slice_from_copied_string(name));
    goto error_cleanup;
  } else if (port == NULL) {
    if (default_port == NULL) {
      error = grpc_error_set_str(
          GRPC_ERROR_CREATE_FROM_STATIC_STRING("no port in name"),
          GRPC_ERROR_STR_TARGET_ADDRESS, grpc_slice_from_copied_string(name));
      goto error_cleanup;
    }
    port = gpr_strdup(default_port);
  }

  grpc_ares_ev_driver *ev_driver;
  error = grpc_ares_ev_driver_create(&ev_driver, interested_parties);
  if (error != GRPC_ERROR_NONE) goto error_cleanup;

  r = (grpc_ares_request *)gpr_zalloc(sizeof(grpc_ares_request));
  gpr_mu_init(&r->mu);
  r->ev_driver = ev_driver;
  r->on_done = on_done;
  r->lb_addrs_out = addrs;
  r->service_config_json_out = service_config_json;
  r->success = false;
  r->error = GRPC_ERROR_NONE;
  channel = grpc_ares_ev_driver_get_channel(r->ev_driver);

  // If dns_server is specified, use it.
  if (dns_server != NULL) {
    gpr_log(GPR_INFO, "Using DNS server %s", dns_server);
    grpc_resolved_address addr;
    if (grpc_parse_ipv4_hostport(dns_server, &addr, false /* log_errors */)) {
      r->dns_server_addr.family = AF_INET;
      struct sockaddr_in *in = (struct sockaddr_in *)addr.addr;
      memcpy(&r->dns_server_addr.addr.addr4, &in->sin_addr,
             sizeof(struct in_addr));
      r->dns_server_addr.tcp_port = grpc_sockaddr_get_port(&addr);
      r->dns_server_addr.udp_port = grpc_sockaddr_get_port(&addr);
    } else if (grpc_parse_ipv6_hostport(dns_server, &addr,
                                        false /* log_errors */)) {
      r->dns_server_addr.family = AF_INET6;
      struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)addr.addr;
      memcpy(&r->dns_server_addr.addr.addr6, &in6->sin6_addr,
             sizeof(struct in6_addr));
      r->dns_server_addr.tcp_port = grpc_sockaddr_get_port(&addr);
      r->dns_server_addr.udp_port = grpc_sockaddr_get_port(&addr);
    } else {
      error = grpc_error_set_str(
          GRPC_ERROR_CREATE_FROM_STATIC_STRING("cannot parse authority"),
          GRPC_ERROR_STR_TARGET_ADDRESS, grpc_slice_from_copied_string(name));
      gpr_free(r);
      goto error_cleanup;
    }
    int status = ares_set_servers_ports(*channel, &r->dns_server_addr);
    if (status != ARES_SUCCESS) {
      char *error_msg;
      gpr_asprintf(&error_msg, "C-ares status is not ARES_SUCCESS: %s",
                   ares_strerror(status));
      error = GRPC_ERROR_CREATE_FROM_COPIED_STRING(error_msg);
      gpr_free(error_msg);
      gpr_free(r);
      goto error_cleanup;
    }
  }
  gpr_ref_init(&r->pending_queries, 1);
  if (grpc_ipv6_loopback_available()) {
    hr = create_hostbyname_request(r, host, strhtons(port),
                                   false /* is_balancer */);
    ares_gethostbyname(*channel, hr->host, AF_INET6, on_hostbyname_done_cb, hr);
  }
  hr = create_hostbyname_request(r, host, strhtons(port),
                                 false /* is_balancer */);
  ares_gethostbyname(*channel, hr->host, AF_INET, on_hostbyname_done_cb, hr);
  if (check_grpclb) {
    /* Query the SRV record */
    grpc_ares_request_ref(r);
    char *service_name;
    gpr_asprintf(&service_name, "_grpclb._tcp.%s", host);
    ares_query(*channel, service_name, ns_c_in, ns_t_srv, on_srv_query_done_cb,
               r);
    gpr_free(service_name);
  }
  if (service_config_json != NULL) {
    grpc_ares_request_ref(r);
    ares_search(*channel, hr->host, ns_c_in, ns_t_txt, on_txt_done_cb, r);
  }
  /* TODO(zyc): Handle CNAME records here. */
  grpc_ares_ev_driver_start(exec_ctx, r->ev_driver);
  grpc_ares_request_unref(exec_ctx, r);
  gpr_free(host);
  gpr_free(port);
  return r;

error_cleanup:
  GRPC_CLOSURE_SCHED(exec_ctx, on_done, error);
  gpr_free(host);
  gpr_free(port);
  return NULL;
}

grpc_ares_request *(*grpc_dns_lookup_ares)(
    grpc_exec_ctx *exec_ctx, const char *dns_server, const char *name,
    const char *default_port, grpc_pollset_set *interested_parties,
    grpc_closure *on_done, grpc_lb_addresses **addrs, bool check_grpclb,
    char **service_config_json) = grpc_dns_lookup_ares_impl;

void grpc_cancel_ares_request(grpc_exec_ctx *exec_ctx, grpc_ares_request *r) {
  if (grpc_dns_lookup_ares == grpc_dns_lookup_ares_impl) {
    grpc_ares_ev_driver_shutdown(exec_ctx, r->ev_driver);
  }
}

grpc_error *grpc_ares_init(void) {
  gpr_once_init(&g_basic_init, do_basic_init);
  gpr_mu_lock(&g_init_mu);
  int status = ares_library_init(ARES_LIB_INIT_ALL);
  gpr_mu_unlock(&g_init_mu);

  if (status != ARES_SUCCESS) {
    char *error_msg;
    gpr_asprintf(&error_msg, "ares_library_init failed: %s",
                 ares_strerror(status));
    grpc_error *error = GRPC_ERROR_CREATE_FROM_COPIED_STRING(error_msg);
    gpr_free(error_msg);
    return error;
  }
  return GRPC_ERROR_NONE;
}

void grpc_ares_cleanup(void) {
  gpr_mu_lock(&g_init_mu);
  ares_library_cleanup();
  gpr_mu_unlock(&g_init_mu);
}

/*
 * grpc_resolve_address_ares related structs and functions
 */

typedef struct grpc_resolve_address_ares_request {
  /** the pointer to receive the resolved addresses */
  grpc_resolved_addresses **addrs_out;
  /** currently resolving lb addresses */
  grpc_lb_addresses *lb_addrs;
  /** closure to call when the resolve_address_ares request completes */
  grpc_closure *on_resolve_address_done;
  /** a closure wrapping on_dns_lookup_done_cb, which should be invoked when the
      grpc_dns_lookup_ares operation is done. */
  grpc_closure on_dns_lookup_done;
} grpc_resolve_address_ares_request;

static void on_dns_lookup_done_cb(grpc_exec_ctx *exec_ctx, void *arg,
                                  grpc_error *error) {
  grpc_resolve_address_ares_request *r =
      (grpc_resolve_address_ares_request *)arg;
  grpc_resolved_addresses **resolved_addresses = r->addrs_out;
  if (r->lb_addrs == NULL || r->lb_addrs->num_addresses == 0) {
    *resolved_addresses = NULL;
  } else {
    *resolved_addresses =
        (grpc_resolved_addresses *)gpr_zalloc(sizeof(grpc_resolved_addresses));
    (*resolved_addresses)->naddrs = r->lb_addrs->num_addresses;
    (*resolved_addresses)->addrs = (grpc_resolved_address *)gpr_zalloc(
        sizeof(grpc_resolved_address) * (*resolved_addresses)->naddrs);
    for (size_t i = 0; i < (*resolved_addresses)->naddrs; i++) {
      GPR_ASSERT(!r->lb_addrs->addresses[i].is_balancer);
      memcpy(&(*resolved_addresses)->addrs[i],
             &r->lb_addrs->addresses[i].address, sizeof(grpc_resolved_address));
    }
  }
  GRPC_CLOSURE_SCHED(exec_ctx, r->on_resolve_address_done,
                     GRPC_ERROR_REF(error));
  grpc_lb_addresses_destroy(exec_ctx, r->lb_addrs);
  gpr_free(r);
}

/*
 * structs and functions related to sorting addresses per RFC 6724
 */

static int default_socket_factory_socket(grpc_ares_wrapper_socket_factory *factory, int domain, int type, int protocol) {
  return socket(domain, type, protocol);
}

static int default_socket_factory_connect(grpc_ares_wrapper_socket_factory *factory, int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
  return connect(sockfd, addr, addrlen);
}

static int default_socket_factory_getsockname(grpc_ares_wrapper_socket_factory *factory, int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
  return getsockname(sockfd, addr, addrlen);
}

static int default_socket_factory_close(grpc_ares_wrapper_socket_factory *factory, int sockfd) {
  return close(sockfd);
}

static const grpc_ares_wrapper_socket_factory_vtable default_socket_factory_vtable = {
  default_socket_factory_socket,
  default_socket_factory_connect,
  default_socket_factory_getsockname,
  default_socket_factory_close,
};
static grpc_ares_wrapper_socket_factory default_socket_factory = {&default_socket_factory_vtable};
static grpc_ares_wrapper_socket_factory *current_socket_factory = &default_socket_factory;

void grpc_ares_wrapper_set_socket_factory(grpc_ares_wrapper_socket_factory *factory) {
  current_socket_factory = factory;
}

int grpc_ares_wrapper_socket(int domain, int type, int protocol) {
  GPR_ASSERT(domain == AF_INET || domain == AF_INET6);
  return current_socket_factory->vtable->socket(current_socket_factory, domain, type, protocol);
}

int grpc_ares_wrapper_connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
  return current_socket_factory->vtable->connect(current_socket_factory, sockfd, addr, addrlen);
}

int grpc_ares_wrapper_getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
  return current_socket_factory->vtable->getsockname(current_socket_factory, sockfd, addr, addrlen);
}

int grpc_ares_wrapper_close(int sockfd) {
  return current_socket_factory->vtable->close(current_socket_factory, sockfd);
}

struct sortable_address {
  grpc_lb_address lb_addr;
  struct sockaddr_in6 dest_addr;
  struct sockaddr_in6 source_addr;
  size_t original_index;
  bool src_addr_exists;
};

struct rfc_6724_table_entry {
  uint8_t prefix[16];
  size_t prefix_len;
  int precedence;
  int label;
};

rfc_6724_table_entry rfc_6724_default_policy_table[9] = {
  {
    { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x1},
    128,
    50,
    0,
  },
  {
    { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0},
    0,
    40,
    1,
  },
  {
    { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xff, 0xff, 0xff, 0xff, 0x0, 0x0},
    96,
    35,
    4,
  },
  {
    { 0x20, 0x02, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0},
    16,
    30,
    2,
  },
  {
    { 0x20, 0x01, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0},
    32,
    5,
    5,
  },
  {
    { 0xfc, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0},
    7,
    3,
    13,
  },
  {
    { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0},
    96,
    1,
    3,
  },
  {
    { 0xfe, 0xc0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0},
    10,
    1,
    11,
  },
  {
    { 0x3f, 0xfe, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0},
    16,
    1,
    12,
  },
};

static rfc_6724_table_entry *rfc_6724_policy_table = rfc_6724_default_policy_table;
static size_t rfc_6724_policy_table_size = 9;

static int ipv6_prefix_match_length(unsigned char *a, unsigned char *b) {
  int cur_bit = 0;
  while (cur_bit < 128) {
    int a_val = a[cur_bit / CHAR_BIT] & (1 << (cur_bit % CHAR_BIT));
    int b_val = b[cur_bit / CHAR_BIT] & (1 << (cur_bit % CHAR_BIT));
    if (a_val == b_val) {
      cur_bit++;
    } else {
      break;
    }
  }
  return cur_bit;
}

rfc_6724_table_entry *lookup_policy_table_match(sockaddr_in6 *s_addr) {
  rfc_6724_table_entry *best_match  = NULL;
  size_t best_match_index = -1;
  for (size_t i = 0; i < rfc_6724_policy_table_size; i++) {
    size_t prefix_match = ipv6_prefix_match_length((unsigned char*)rfc_6724_policy_table[i].prefix, (unsigned char*)&s_addr->sin6_addr.s6_addr);
    if (prefix_match >= rfc_6724_policy_table[i].prefix_len) {
      if (best_match == NULL || rfc_6724_policy_table[i].prefix_len > best_match->prefix_len) {
        best_match = &rfc_6724_policy_table[i];
        best_match_index = i;
      }
    }
  }
  GPR_ASSERT(best_match);
  if (GRPC_TRACER_ON(grpc_trace_cares_address_sorting)) {
    gpr_log(GPR_INFO, "Looked up best match in policy table. Index: %" PRIdPTR ". Label: %d. Precedence: %d", best_match_index, best_match->label, best_match->precedence);
  }
  return best_match;
}

static int get_label_value(sockaddr_in6 *s_addr) {
  rfc_6724_table_entry *entry = lookup_policy_table_match(s_addr);
  GPR_ASSERT(entry != NULL);
  gpr_log(GPR_INFO, "returning label: %d", entry->label);
  return entry->label;
}

static int get_precedence_value(sockaddr_in6 *s_addr) {
  rfc_6724_table_entry *entry = lookup_policy_table_match(s_addr);
  GPR_ASSERT(entry != NULL);
  return entry->precedence;
}

#define IPV6_ADDR_SCOPE_GLOBAL 0x0e
#define IPV6_ADDR_SCOPE_LINKLOCAL 0x02
#define IPV6_ADDR_SCOPE_SITELOCAL 0x05

static int sockaddr_get_scope(sockaddr_in6 *s_addr) {
  switch (s_addr->sin6_family) {
  case AF_INET:
    gpr_log(GPR_INFO, "ipv4 so global scope");
    return IPV6_ADDR_SCOPE_GLOBAL;
  case AF_INET6:
    if (IN6_IS_ADDR_LOOPBACK(&s_addr->sin6_addr) || IN6_IS_ADDR_LINKLOCAL(&s_addr->sin6_addr)) {
      gpr_log(GPR_INFO, "found link local scope");
      return IPV6_ADDR_SCOPE_LINKLOCAL;
    }
    if (IN6_IS_ADDR_SITELOCAL(&s_addr->sin6_addr)) {
      gpr_log(GPR_INFO, "found site local scope");
      return IPV6_ADDR_SCOPE_SITELOCAL;
    }
    gpr_log(GPR_INFO, "found global scope");
    return IPV6_ADDR_SCOPE_GLOBAL;
  default:
    gpr_log(GPR_ERROR, "Unknown socket family %d in grpc_sockaddr_get_port", s_addr->sin6_family);
    return 0;
  }
}

static int compare_src_addr_exists(sortable_address *sa, sortable_address *sb) {
  if (sa->src_addr_exists != sb->src_addr_exists) {
    gpr_log(GPR_INFO, "src addrs not equal");
    return sa->src_addr_exists ? -1 : 1;
  }
  gpr_log(GPR_INFO, "src addrs both not there or there");
  return 0;
}

static int compre_src_dst_scope_matches(sortable_address *sa, sortable_address *sb) {
  int a_src_dst_scope_matches = false;
  if (sockaddr_get_scope((sockaddr_in6*)&sa->dest_addr) == sockaddr_get_scope(&sa->source_addr)) {
    gpr_log(GPR_INFO, "a src and dst scopes match");
    a_src_dst_scope_matches = true;
  }
  int b_src_dst_scope_matches = false;
  if (sockaddr_get_scope((sockaddr_in6*)&sb->dest_addr) == sockaddr_get_scope(&sb->source_addr)) {
    gpr_log(GPR_INFO, "b src and dst scopes match");
    b_src_dst_scope_matches = true;
  }
  if (a_src_dst_scope_matches != b_src_dst_scope_matches) {
    return a_src_dst_scope_matches ? -1 : 1;
  }
  gpr_log(GPR_INFO, "matching of scopes matches");
  return 0;
}

static int compare_src_dst_labels_match(sortable_address *sa, sortable_address *sb) {
  int a_label_matches = false;
  if (get_label_value((sockaddr_in6*)&sa->dest_addr) == get_label_value(&sa->source_addr)) {
    a_label_matches = true;
  }
  int b_label_matches = false;
  if (get_label_value((sockaddr_in6*)&sb->dest_addr) == get_label_value(&sb->source_addr)) {
    b_label_matches = true;
  }
  if (a_label_matches != b_label_matches) {
    gpr_log(GPR_INFO, "labels dont match");
    return a_label_matches ? -1 : 1;
  }
  gpr_log(GPR_INFO, "labels match");
  return 0;
}

static int compare_dst_precedence(sortable_address *sa, sortable_address *sb) {
  if (get_precedence_value(&sa->dest_addr) != get_precedence_value(&sb->dest_addr)) {
    return get_precedence_value(&sb->dest_addr) - get_precedence_value(&sa->dest_addr);
  }
  gpr_log(GPR_INFO, "precedence of both destinations match");
  return 0;
}

static int compare_dst_scope(sortable_address *sa, sortable_address *sb) {
  if (sockaddr_get_scope(&sa->dest_addr) != sockaddr_get_scope(&sb->dest_addr)) {
    return sockaddr_get_scope(&sa->dest_addr) - sockaddr_get_scope(&sb->dest_addr);
  }
  return 0;
}

static int compare_src_dst_prefix_match_lengths(sortable_address *sa, sortable_address *sb) {
  if (grpc_sockaddr_get_family(&sa->lb_addr.address) == grpc_sockaddr_get_family(&sb->lb_addr.address) && grpc_sockaddr_get_family(&sa->lb_addr.address) == AF_INET6) {
    int a_match = ipv6_prefix_match_length((unsigned char*)&sa->source_addr.sin6_addr.s6_addr, (unsigned char*)&sa->dest_addr.sin6_addr.s6_addr);
    int b_match = ipv6_prefix_match_length((unsigned char*)&sb->source_addr.sin6_addr.s6_addr, (unsigned char*)&sb->dest_addr.sin6_addr.s6_addr);
    if (a_match != b_match) {
      return a_match - b_match;
    }
  }
  return 0;
}

static int rfc_6724_compare(const void *a, const void *b) {
  sortable_address* sa = (sortable_address*)a;
  sortable_address* sb = (sortable_address*)b;

  int out = 0;
  if ((out = compare_src_addr_exists(sa, sb))) {
    return out;
  } else if ((out = compre_src_dst_scope_matches(sa, sb))) {
    return out;
  } else if ((out = compare_src_dst_labels_match(sa, sb))) {
    return out;
  // TODO: avoid deprecated addresses
  // TODO: avoid temporary addresses
  } else if ((out = compare_dst_precedence(sa, sb))) {
    return out;
  // TODO: prefer native transport
  } else if ((out = compare_dst_scope(sa, sb))) {
    return out;
  } else if ((out = compare_src_dst_prefix_match_lengths(sa, sb))) {
    return out;
  }
  // Prefer that the sort be stable otherwise
  return sa->original_index - sb->original_index;
}

static void update_maybe_v4map(grpc_resolved_address *resolved_addr, sockaddr_in6 *to_update) {
  grpc_resolved_address v4_mapped;
  if (grpc_sockaddr_to_v4mapped(resolved_addr, &v4_mapped)) {
    memcpy(to_update, &v4_mapped.addr, sizeof(struct sockaddr_in6));
  } else {
    memcpy(to_update, &resolved_addr->addr, sizeof(struct sockaddr_in6));
  }
}

void grpc_ares_wrapper_rfc_6724_sort(grpc_lb_addresses *resolved_lb_addrs) {
  sortable_address* sortable = (sortable_address*)gpr_zalloc(resolved_lb_addrs->num_addresses * sizeof(sortable_address));
  for (size_t i = 0; i < resolved_lb_addrs->num_addresses; i++) {
    sortable[i].lb_addr = resolved_lb_addrs->addresses[i];
    sortable[i].src_addr_exists = false;
    sortable[i].original_index = i;
    update_maybe_v4map(&resolved_lb_addrs->addresses[i].address, &sortable[i].dest_addr);
    int address_family = grpc_sockaddr_get_family(&resolved_lb_addrs->addresses[i].address);
    // TODO: reset already-created sockets when possible, if needed
    int s = grpc_ares_wrapper_socket(address_family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (address_family == AF_INET6) {
      sockaddr_in6 *s_addr = (sockaddr_in6*)&resolved_lb_addrs->addresses[i].address.addr;
      char ntop_buf[INET6_ADDRSTRLEN + 1];
      ntop_buf[INET6_ADDRSTRLEN] = 0;
      gpr_log(GPR_INFO, "ares wrapper lb addr addr string");
      inet_ntop(AF_INET6, &s_addr->sin6_addr, ntop_buf, sizeof(ntop_buf));
      gpr_log(GPR_INFO, "lb addr str: %s", ntop_buf);
    }
    if (s != -1) {
      struct sockaddr *dest = (struct sockaddr*)resolved_lb_addrs->addresses[i].address.addr;
      if (grpc_ares_wrapper_connect(s, dest, resolved_lb_addrs->addresses[i].address.len) != -1) {
        grpc_resolved_address src_addr;
        if (grpc_ares_wrapper_getsockname(s, (struct sockaddr*)&src_addr.addr, (socklen_t*)&src_addr.len) != -1) {
          sortable[i].src_addr_exists = true;
          update_maybe_v4map(&src_addr, &sortable[i].source_addr);
          // Do logging
          char *dst_str;
          GPR_ASSERT(grpc_sockaddr_to_string(&dst_str, &resolved_lb_addrs->addresses[i].address, true));
          char *src_str;
          GPR_ASSERT(grpc_sockaddr_to_string(&src_str, &src_addr, true));
          gpr_log(GPR_INFO, "Resolved destination %s and found source address candidate %s", dst_str, src_str);
          gpr_free(dst_str);
          gpr_free(src_str);
        } else {
          char *addr_str;
          GPR_ASSERT(grpc_sockaddr_to_string(&addr_str, &resolved_lb_addrs->addresses[i].address, true));
          gpr_log(GPR_INFO, "Resolved destination %s but getsockname after connect failed with %d, so de-prioritizing it", addr_str, errno);
          gpr_free(addr_str);
        }
      } else {
        char *addr_str;
        GPR_ASSERT(grpc_sockaddr_to_string(&addr_str, &resolved_lb_addrs->addresses[i].address, true));
        gpr_log(GPR_INFO, "Resolved destination %s but connect failed with %d, so de-prioritizing it", addr_str, errno);
        gpr_free(addr_str);
      }
      grpc_ares_wrapper_close(s);
    }
  }
  qsort(sortable, resolved_lb_addrs->num_addresses, sizeof(sortable_address), rfc_6724_compare);
  grpc_lb_address *sorted_lb_addrs = (grpc_lb_address*)gpr_zalloc(resolved_lb_addrs->num_addresses * sizeof(grpc_lb_address));
  for (size_t i = 0; i < resolved_lb_addrs->num_addresses; i++) {
    sorted_lb_addrs[i] = sortable[i].lb_addr;
    char *src_str;
    GPR_ASSERT(grpc_sockaddr_to_string(&src_str, &sorted_lb_addrs[i].address, false));
    gpr_log(GPR_INFO, "Adding sorted address: %s", src_str);
    gpr_free(src_str);
  }
  gpr_free(sortable);
  gpr_free(resolved_lb_addrs->addresses);
  resolved_lb_addrs->addresses = sorted_lb_addrs;
}

static void grpc_resolve_address_ares_impl(grpc_exec_ctx *exec_ctx,
                                           const char *name,
                                           const char *default_port,
                                           grpc_pollset_set *interested_parties,
                                           grpc_closure *on_done,
                                           grpc_resolved_addresses **addrs) {
  grpc_resolve_address_ares_request *r =
      (grpc_resolve_address_ares_request *)gpr_zalloc(
          sizeof(grpc_resolve_address_ares_request));
  r->addrs_out = addrs;
  r->on_resolve_address_done = on_done;
  GRPC_CLOSURE_INIT(&r->on_dns_lookup_done, on_dns_lookup_done_cb, r,
                    grpc_schedule_on_exec_ctx);
  grpc_dns_lookup_ares(exec_ctx, NULL /* dns_server */, name, default_port,
                       interested_parties, &r->on_dns_lookup_done, &r->lb_addrs,
                       false /* check_grpclb */,
                       NULL /* service_config_json */);
}

void (*grpc_resolve_address_ares)(
    grpc_exec_ctx *exec_ctx, const char *name, const char *default_port,
    grpc_pollset_set *interested_parties, grpc_closure *on_done,
    grpc_resolved_addresses **addrs) = grpc_resolve_address_ares_impl;

#endif /* GRPC_ARES == 1 && !defined(GRPC_UV) */
