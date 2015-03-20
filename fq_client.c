/*
 * Copyright (c) 2013 OmniTI Computer Consulting, Inc.
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <alloca.h>
#include <string.h>
#include <unistd.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <poll.h>
#include <ck_fifo.h>
#include <assert.h>
#include <uuid/uuid.h>

#include "fq.h"

#define CONNERR_S(c) do { \
  if(c->errorlog) c->errorlog(c, c->error); \
} while(0)

#define CONNERR(c, s) do { \
  strncpy(c->error, s, sizeof(c->error)); \
  if(c->errorlog) c->errorlog(c, c->error); \
} while(0)

static inline int
fq_client_wfrw_internal(int fd, int needs_read, int needs_write,
                        uint64_t ms, int *mask) {
  int rv;
  struct pollfd pfd;
  pfd.fd = fd;
  pfd.events = 0;
  if(needs_read) pfd.events = POLLIN;
  if(needs_write) pfd.events = POLLOUT;
  rv = poll(&pfd, 1, ms);
  if(mask) *mask = pfd.revents;
  return rv;
}

struct fq_conn_s {
  struct         sockaddr_in remote;
  char           error[128];
  char          *user;
  char          *pass;
  char          *queue;
  char          *queue_type;
  fq_rk          key;
  int            cmd_fd;
  int            cmd_hb_needed;
  unsigned short cmd_hb_ms;
  hrtime_t       cmd_hb_last;
  int            peermode;
  int            data_fd;
  pthread_t      worker;
  pthread_t      data_worker;
  int            stop;
  ck_fifo_mpmc_t cmdq;
  ck_fifo_mpmc_t q;
  ck_fifo_mpmc_t backq;
  uint32_t       qlen;
  uint32_t       qmaxlen;
  uint32_t       q_stall_time;
  bool           non_blocking;
  int            connected;
  int            data_ready;
  fq_msg        *tosend;
  int            tosend_offset;
  int            sync_hooks; /* should they run in the calling thread */
  void         (*auth_hook)(fq_client, int);
  void         (*bind_hook)(fq_client, fq_bind_req *);
  void         (*unbind_hook)(fq_client, fq_unbind_req *);

  void         (*errorlog)(fq_client, const char *);
  ck_fifo_mpmc_entry_t *cmdqhead;
  ck_fifo_mpmc_entry_t *qhead;
  ck_fifo_mpmc_entry_t *backqhead;
};
typedef struct fq_conn_s fq_conn_s;

typedef struct {
  unsigned short cmd;
  union {
    struct {
      uint16_t       ms;
    } heartbeat;
    struct {
      void (*callback)(char *, uint32_t, void *);
      void *closure;
    } status;
    fq_bind_req *bind;
    fq_unbind_req *unbind;
    int return_value;
  } data;
} cmd_instr;

typedef enum {
  AUTH_HOOK_TYPE,
  CMD_HOOK_TYPE
} hook_req_type_t;

typedef struct {
  hook_req_type_t type;
  cmd_instr *entry;
} hook_req_t;

static void
fq_client_signal(fq_client conn, cmd_instr *e) {
  fq_conn_s *conn_s = conn;
  ck_fifo_mpmc_entry_t *fifo_entry;
  fifo_entry = malloc(sizeof(ck_fifo_mpmc_entry_t));
  ck_fifo_mpmc_enqueue(&conn_s->cmdq, fifo_entry, e);
}

static int
fq_socket_connect(fq_conn_s *conn_s) {
  int fd, rv, on = 1;
  fd = socket(AF_INET, SOCK_STREAM, 0);
  if(fd == -1) return -1;
  rv = connect(fd, (struct sockaddr *)&conn_s->remote,
               sizeof(conn_s->remote));
  if(rv == -1) {
    snprintf(conn_s->error, sizeof(conn_s->error),
             "socket: %s", strerror(errno));
    CONNERR_S(conn_s);
    close(fd);
    return -1;
  }
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
  return fd;
}

static void
fq_client_disconnect_internal(fq_conn_s *conn_s) {
  if(conn_s->cmd_fd >= 0) {
#ifdef DEBUG
    fq_debug(FQ_DEBUG_CONN, "close(cmd_fd)\n");
#endif
    close(conn_s->cmd_fd);
    conn_s->cmd_fd = -1;
  }
  if(conn_s->data_fd >= 0) {
#ifdef DEBUG
    fq_debug(FQ_DEBUG_CONN, "close(data_fd)\n");
#endif
    close(conn_s->data_fd);
    conn_s->data_fd = -1;
  }
  conn_s->data_ready = 0;
  if(conn_s->cmd_hb_ms) {
    unsigned short hb;
    hb = conn_s->cmd_hb_ms;
    conn_s->cmd_hb_ms = 0;
    fq_client_heartbeat(conn_s, hb);
    conn_s->cmd_hb_last = 0;
  }
}

static int
fq_client_do_auth(fq_conn_s *conn_s) {
  int len;
  uint16_t cmd;
  char error[1024];
  char *queue_composed;
  if(fq_write_uint16(conn_s->cmd_fd, FQ_PROTO_AUTH_CMD)) return -1;
  if(fq_write_uint16(conn_s->cmd_fd, FQ_PROTO_AUTH_PLAIN)) return -2;
  if(fq_write_short_cmd(conn_s->cmd_fd, strlen(conn_s->user), conn_s->user) < 0)
    return -3;
  len = strlen(conn_s->queue) +
        1 + strlen(conn_s->queue_type);
  queue_composed = alloca(len+1);
  memcpy(queue_composed, conn_s->queue, strlen(conn_s->queue)+1); /* include null terminator */
  memcpy(queue_composed + strlen(conn_s->queue) + 1, conn_s->queue_type, strlen(conn_s->queue_type));
  if(fq_write_short_cmd(conn_s->cmd_fd, len, queue_composed) < 0)
    return -4;
  if(fq_write_short_cmd(conn_s->cmd_fd, strlen(conn_s->pass), conn_s->pass) < 0)
    return -5;
  if(fq_read_uint16(conn_s->cmd_fd, &cmd)) return -6;
  switch(cmd) {
    case FQ_PROTO_ERROR:
      len = fq_read_short_cmd(conn_s->cmd_fd, sizeof(error)-1, error);
      if(conn_s->errorlog) {
        if(len > (int)sizeof(error)-1) len = sizeof(error)-1;
        if(len < 0) conn_s->errorlog(conn_s, "error reading error");
        else conn_s->errorlog(conn_s,error);
      }
      return -7;
    case FQ_PROTO_AUTH_RESP:
      len = fq_read_short_cmd(conn_s->cmd_fd,
                              sizeof(conn_s->key.name), conn_s->key.name);
      if(len < 0 || len > (int)sizeof(conn_s->key.name)) return -8;
      conn_s->key.len = len;
#ifdef DEBUG
      {
        char hex[260];
        if(fq_rk_to_hex(hex, sizeof(hex), &conn_s->key) >= 0)
          fq_debug(FQ_DEBUG_CONN, "client keyed:\n%s\n", hex);
      }
#endif
      conn_s->data_ready = 1;
      break;
    default:
      if(conn_s->errorlog) {
        snprintf(error, sizeof(error),
                 "server auth response 0x%04x unknown\n", cmd);
        conn_s->errorlog(conn_s, error);
      }
      return -9;
  }
  return 0;
}

static int
fq_client_data_connect_internal(fq_conn_s *conn_s) {
  int flags;
  uint32_t cmd = htonl(conn_s->peermode ? FQ_PROTO_PEER_MODE
                                        : FQ_PROTO_DATA_MODE);
  /* We don't support data connections when the cmd connection is down */
  if(conn_s->cmd_fd < 0) return -1;

  if(conn_s->data_fd >= 0) {
    close(conn_s->data_fd);
    conn_s->data_fd = -1;
  }
  conn_s->data_fd = fq_socket_connect(conn_s);
  if(conn_s->data_fd < 0) goto shutdown;
  if(write(conn_s->data_fd, &cmd, sizeof(cmd)) != sizeof(cmd))
    goto shutdown;
#ifdef DEBUG
      {
        char hex[260];
        if(fq_rk_to_hex(hex, sizeof(hex), &conn_s->key) >= 0)
          fq_debug(FQ_DEBUG_CONN, "client keying:\n%s\n", hex);
      }
#endif
  if(fq_write_short_cmd(conn_s->data_fd,
                        conn_s->key.len, conn_s->key.name) < 0) {
    goto shutdown;
  }
  conn_s->tosend_offset = 0;
  if(((flags = fcntl(conn_s->data_fd, F_GETFL, 0)) == -1) ||
     (fcntl(conn_s->data_fd, F_SETFL, flags | O_NONBLOCK) == -1))
    goto shutdown;

  return 0;

 shutdown:
  if(conn_s->data_fd >= 0) {
    close(conn_s->data_fd);
    conn_s->data_fd = -1;
  }
  return -1;
}

/* This is dastardly... we know the ptr has to be aligned,
 * when we see that it isn't on dequeue, we know it is a hook_req_t
 */

#define MARKED_HOOK_REQ_PTR(p) ((void *)(((uintptr_t)p)|1))
#define CHECK_HOOK_REQ_PTR(p) (((uintptr_t)p)&1)
#define UNMARKED_HOOK_REQ_PTR(p) ((void *)(((uintptr_t)p)&~1))

static void
enqueue_auth_hook_req(fq_conn_s *conn_s, int rv) {
  ck_fifo_mpmc_entry_t *fifo_entry;
  hook_req_t *hreq = calloc(1,sizeof(*hreq));
  hreq->type = AUTH_HOOK_TYPE;
  hreq->entry = calloc(1, sizeof(*hreq->entry));
  hreq->entry->data.return_value = 0;
  fifo_entry = malloc(sizeof(ck_fifo_mpmc_entry_t));
  ck_fifo_mpmc_enqueue(&conn_s->backq, fifo_entry, MARKED_HOOK_REQ_PTR(hreq));
}

static void
enqueue_cmd_hook_req(fq_conn_s *conn_s, cmd_instr *e) {
  ck_fifo_mpmc_entry_t *fifo_entry;
  hook_req_t *hreq = calloc(1,sizeof(*hreq));
  hreq->type = CMD_HOOK_TYPE;
  hreq->entry = e;
  fifo_entry = malloc(sizeof(ck_fifo_mpmc_entry_t));
  ck_fifo_mpmc_enqueue(&conn_s->backq, fifo_entry, MARKED_HOOK_REQ_PTR(hreq));
}

static int
fq_client_connect_internal(fq_conn_s *conn_s) {
  int rv = -1;
  uint32_t cmd = htonl(FQ_PROTO_CMD_MODE);
  fq_client_disconnect_internal(conn_s);
  conn_s->cmd_fd = fq_socket_connect(conn_s);
  if(conn_s->cmd_fd < 0) goto shutdown;
  if(write(conn_s->cmd_fd, &cmd, sizeof(cmd)) != sizeof(cmd))
    goto shutdown;
  if((rv = fq_client_do_auth(conn_s)) < 0) {
#ifdef DEBUG
    fq_debug(FQ_DEBUG_CONN, "fq_client_do_auth -> %d\n", rv);
#endif
    goto shutdown;
  }
  if(conn_s->auth_hook) {
    if(conn_s->sync_hooks) enqueue_auth_hook_req(conn_s, 0);
    else conn_s->auth_hook((fq_client)conn_s, 0);
  }
  return 0;

 shutdown:
  if(conn_s->cmd_fd >= 0) {
    close(conn_s->cmd_fd);
    conn_s->cmd_fd = -1;
  }
  if(conn_s->auth_hook) {
    if(conn_s->sync_hooks) enqueue_auth_hook_req(conn_s, rv);
    else conn_s->auth_hook((fq_client)conn_s, rv);
  }
  return -1;
}

static void
fq_client_read_complete(void *closure, fq_msg *msg) {
  ck_fifo_mpmc_entry_t *fifo_entry;
  fq_conn_s *conn_s = (fq_conn_s *)closure;

  fifo_entry = malloc(sizeof(ck_fifo_mpmc_entry_t));
  ck_fifo_mpmc_enqueue(&conn_s->backq, fifo_entry, msg);
}
static void
fq_data_worker_loop(fq_conn_s *conn_s) {
  buffered_msg_reader *ctx = NULL;
  ctx = fq_buffered_msg_reader_alloc(conn_s->data_fd, 1);
  while(conn_s->cmd_fd >= 0 && conn_s->stop == 0) {
    int rv;
    int wait_ms = 500, needs_write = 0, mask, write_rv;
    ck_fifo_mpmc_entry_t *garbage;
    if(conn_s->tosend) goto the_thick_of_it;
    while(ck_fifo_mpmc_dequeue(&conn_s->q, &conn_s->tosend, &garbage) == true) {
      conn_s->tosend_offset = 0;
      ck_pr_dec_uint(&conn_s->qlen);
      free(garbage);
     the_thick_of_it:
#ifdef DEBUG
      fq_debug(FQ_DEBUG_MSG, "dequeue message to submit to server\n");
#endif
      write_rv = fq_client_write_msg(conn_s->data_fd, conn_s->peermode,
                                     conn_s->tosend, conn_s->tosend_offset, NULL);
      if(write_rv > 0) {
        conn_s->tosend_offset += write_rv;
        break;
      }
      if(write_rv < 0) {
        if(errno == EAGAIN) {
          needs_write = 1;
          break;
        }
        if(conn_s->errorlog) {
          char errbuf[128];
          snprintf(errbuf, sizeof(errbuf), "data write error: %s\n", strerror(errno));
          conn_s->errorlog(conn_s, errbuf);
        }
        goto finish;
      }
      fq_msg_deref(conn_s->tosend);
      conn_s->tosend = NULL;
      conn_s->tosend_offset = 0;
      wait_ms = 0;
    }
    rv = fq_client_wfrw_internal(conn_s->data_fd, 1, needs_write, wait_ms, &mask);
    if(rv < 0) {
      if(conn_s->errorlog) {
        char errbuf[128];
        snprintf(errbuf, sizeof(errbuf), "data read error: %s\n", strerror(errno));
        conn_s->errorlog(conn_s, errbuf);
      }
      goto finish;
    }
    if(rv > 0 && (mask & POLLIN)) {
      if(fq_buffered_msg_read(ctx, fq_client_read_complete, conn_s) < 0) {
        if(conn_s->errorlog) conn_s->errorlog(conn_s, "data read: end-of-line\n");
        goto finish;
      }
    }
  }
finish:
  if(ctx) fq_buffered_msg_reader_free(ctx);
#ifdef DEBUG
  fq_debug(FQ_DEBUG_CONN, "cmd_fd -> %d, stop -> %d\n", conn_s->cmd_fd, conn_s->stop);
#endif
}
static void *
fq_data_worker(void *u) {
  int backoff = 0;
  fq_conn_s *conn_s = (fq_conn_s *)u;
  while(conn_s->stop == 0) {
    if(conn_s->data_ready) {
      if(fq_client_data_connect_internal(conn_s) == 0) {
        backoff = 0; /* we're good, restart our backoff */
      }

      fq_data_worker_loop(conn_s);
#ifdef DEBUG
      fq_debug(FQ_DEBUG_IO, "[data] connection failed: %s\n", conn_s->error);
#endif
    }
    if(backoff) usleep(backoff + (4096 - (lrand48()%8192)));  /* +/- 4ms */
    else backoff = 16384;
    if(backoff < 1000000) backoff += (backoff >> 4);
  }
  if(conn_s->data_fd >= 0) {
    close(conn_s->data_fd);
    conn_s->data_fd = -1;
  }
  return (void *)NULL;
}
static void *
fq_conn_worker(void *u) {
  int backoff = 0;
  fq_conn_s *conn_s = (fq_conn_s *)u;
  cmd_instr *last_entry = NULL;
  uint16_t expect;
  cmd_instr *entry;
  ck_fifo_mpmc_entry_t *garbage;

  while(conn_s->stop == 0) {
    expect = 0;
    if(fq_client_connect_internal(conn_s) == 0) {
      backoff = 0; /* we're good, restart our backoff */
    }

    while(conn_s->data_ready) {
      hrtime_t t;
      unsigned long long hb_us;
      struct timeval tv;
      int rv;

      while(ck_fifo_mpmc_dequeue(&conn_s->cmdq, &entry, &garbage) == true) {
        free(garbage);
#ifdef DEBUG
        fq_debug(FQ_DEBUG_CONN, "client acting on user req 0x%04x\n", entry->cmd);
#endif
        switch(entry->cmd) {
          case FQ_PROTO_STATUSREQ:
            if(expect != 0) {
              if(conn_s->errorlog) conn_s->errorlog(conn_s, "protocol violation");
              goto restart;
            }
            fq_debug(FQ_DEBUG_CONN, "sending status request\n");
            if(fq_write_uint16(conn_s->cmd_fd, entry->cmd)) {
              free(entry);
              goto restart;
            }
            expect = FQ_PROTO_STATUS;
            last_entry = entry;
            break;
          case FQ_PROTO_HBREQ:
            fq_debug(FQ_DEBUG_CONN, "sending heartbeat request\n");
            if(fq_write_uint16(conn_s->cmd_fd, entry->cmd) ||
               fq_write_uint16(conn_s->cmd_fd, entry->data.heartbeat.ms)) {
              free(entry);
              goto restart;
            }
            conn_s->cmd_hb_ms = entry->data.heartbeat.ms;
            tv.tv_sec = (unsigned long)entry->data.heartbeat.ms / 1000;
            tv.tv_usec = 1000UL * (entry->data.heartbeat.ms % 1000);
            if(setsockopt(conn_s->cmd_fd, SOL_SOCKET, SO_RCVTIMEO,
                          &tv, sizeof(tv)))
              CONNERR(conn_s, strerror(errno));
            tv.tv_sec = (unsigned long)entry->data.heartbeat.ms / 1000;
            tv.tv_usec = 1000UL * (entry->data.heartbeat.ms % 1000);
            if(setsockopt(conn_s->cmd_fd, SOL_SOCKET, SO_SNDTIMEO,
                          &tv, sizeof(tv)))
              CONNERR(conn_s, strerror(errno));
            conn_s->cmd_hb_last = fq_gethrtime();
            free(entry);
            break;
          case FQ_PROTO_BINDREQ:
            {
              unsigned short flags = entry->data.bind->flags;
              if(expect != 0) {
                if(conn_s->errorlog) conn_s->errorlog(conn_s, "protocol violation");
                goto restart;
              }
              if(fq_write_uint16(conn_s->cmd_fd, entry->cmd) ||
                 fq_write_uint16(conn_s->cmd_fd, flags) ||
                 fq_write_short_cmd(conn_s->cmd_fd,
                                    entry->data.bind->exchange.len,
                                    entry->data.bind->exchange.name) < 0 ||
                 fq_write_short_cmd(conn_s->cmd_fd,
                                    strlen(entry->data.bind->program),
                                    entry->data.bind->program) < 0) {
                goto restart;
              }
              expect = FQ_PROTO_BIND;
              last_entry = entry;
            }
            break;
          case FQ_PROTO_UNBINDREQ:
            {
              if(expect != 0) {
                if(conn_s->errorlog) conn_s->errorlog(conn_s, "protocol violation");
                goto restart;
              }
              if(fq_write_uint16(conn_s->cmd_fd, entry->cmd) ||
                 fq_write_uint32(conn_s->cmd_fd, entry->data.unbind->route_id) ||
                 fq_write_short_cmd(conn_s->cmd_fd,
                                    entry->data.unbind->exchange.len,
                                    entry->data.unbind->exchange.name) < 0) {
                goto restart;
              }
              expect = FQ_PROTO_UNBIND;
              last_entry = entry;
            }
            break;
          default:
            if(conn_s->errorlog) conn_s->errorlog(conn_s, "unknown user-side cmd");
            free(entry);
        }
      }

      if(conn_s->cmd_hb_needed) {
#ifdef DEBUG
          fq_debug(FQ_DEBUG_CONN, "-> heartbeat\n");
#endif
        if(fq_write_uint16(conn_s->cmd_fd, FQ_PROTO_HB)) break;
        conn_s->cmd_hb_needed = 0;
      }

      rv = fq_client_wfrw_internal(conn_s->cmd_fd, 1, 0, 50, NULL);
      t = fq_gethrtime();
      hb_us = (unsigned long long)conn_s->cmd_hb_ms * 3 * 1000000ULL;
      if(conn_s->cmd_hb_last && hb_us &&
         conn_s->cmd_hb_last < (unsigned int) (t - hb_us)) {
        char errbuf[256];
        snprintf(errbuf, sizeof(errbuf), "heartbeat failed [%llu - %llu = %llu]",
                 (unsigned long long)t, (unsigned long long)conn_s->cmd_hb_last,
                 (unsigned long long)(t - conn_s->cmd_hb_last));
        CONNERR(conn_s, errbuf);
        break;
      }
      if(rv < 0) {
        CONNERR(conn_s, strerror(errno));
        break;
      }
      if(rv > 0) {
        uint16_t hb;
        if(fq_read_uint16(conn_s->cmd_fd, &hb)) break;
        switch(hb) {
          case FQ_PROTO_HB:
#ifdef DEBUG
            fq_debug(FQ_DEBUG_CONN, "<- heartbeat\n");
#endif
            conn_s->cmd_hb_last = fq_gethrtime();
            conn_s->cmd_hb_needed = 1;
            break;
          case FQ_PROTO_STATUS:
            if(expect != FQ_PROTO_STATUS) {
              if(conn_s->errorlog) conn_s->errorlog(conn_s, "protocol violation");
              goto restart;
            }
            if(fq_read_status(conn_s->cmd_fd,
                              last_entry->data.status.callback,
                              last_entry->data.status.closure))
              goto restart;
            free(last_entry);
            last_entry = NULL;
            expect = 0;
            break;
          case FQ_PROTO_BIND:
            if(expect != FQ_PROTO_BIND) {
              if(conn_s->errorlog) conn_s->errorlog(conn_s, "protocol violation");
              goto restart;
            }
            if(fq_read_uint32(conn_s->cmd_fd,
                              &last_entry->data.bind->out__route_id)) {
              if(conn_s->bind_hook) {
                if(conn_s->sync_hooks) {
                  enqueue_cmd_hook_req(conn_s, last_entry);
                  last_entry = NULL;
                }
                else conn_s->bind_hook((fq_client)conn_s, last_entry->data.bind);
              }
              goto restart;
            }
            if(conn_s->bind_hook) {
              if(conn_s->sync_hooks) {
                enqueue_cmd_hook_req(conn_s, last_entry);
                last_entry = NULL;
              }
              else conn_s->bind_hook((fq_client)conn_s, last_entry->data.bind);
            }
            if(last_entry) {
              free(last_entry);
              last_entry = NULL;
            }
            expect = 0;
            break;
          case FQ_PROTO_UNBIND:
            if(expect != FQ_PROTO_UNBIND) {
              if(conn_s->errorlog) conn_s->errorlog(conn_s, "protocol violation");
              goto restart;
            }
            if(fq_read_uint32(conn_s->cmd_fd,
                              &last_entry->data.unbind->out__success)) {
              if(conn_s->unbind_hook) {
                if(conn_s->sync_hooks) {
                  enqueue_cmd_hook_req(conn_s, last_entry);
                  last_entry = NULL;
                }
                conn_s->unbind_hook((fq_client)conn_s, last_entry->data.unbind);
              }
              goto restart;
            }
            if(conn_s->unbind_hook) {
              if(conn_s->sync_hooks) {
                enqueue_cmd_hook_req(conn_s, last_entry);
                last_entry = NULL;
              }
              conn_s->unbind_hook((fq_client)conn_s, last_entry->data.unbind);
            }
            if(last_entry) {
              free(last_entry);
              last_entry = NULL;
            }
            expect = 0;
            break;
          default:
            if(conn_s->errorlog) conn_s->errorlog(conn_s, "protocol violation");
            goto restart;
            break;
        }
      }
    }

#ifdef DEBUG
    fq_debug(FQ_DEBUG_CONN, "[cmd] connection failed: %s\n", conn_s->error);
#endif
    if(backoff) usleep(backoff + (4096 - (lrand48()%8192)));  /* +/- 4ms */
    else backoff = 16384;
    if(backoff < 1000000) backoff += (backoff >> 4);
   restart:
    /* drain the queue.. we're going to make a new connection */
#ifdef DEBUG
    fq_debug(FQ_DEBUG_CONN, "[cmd] draining cmds\n");
#endif
    if(last_entry) {
      free(last_entry);
      last_entry = NULL;
    }
    while(ck_fifo_mpmc_dequeue(&conn_s->cmdq, &entry, &garbage) == true) {
      free(garbage);
      free(entry);
    }
  }
  fq_client_disconnect_internal(conn_s);
  return (void *)NULL;
}

int
fq_client_init(fq_client *conn_ptr, int peermode,
               void (*logger)(fq_client, const char *)) {
  fq_conn_s *conn_s;
  conn_s = *conn_ptr = calloc(1, sizeof(*conn_s));
  if(!conn_s) return -1;
  /* make the sockets as disconnected */
  conn_s->cmd_fd = conn_s->data_fd = -1;
  conn_s->peermode = peermode;
  conn_s->errorlog = logger;
  return 0;
}

int
fq_client_hooks(fq_client conn, fq_hooks *hooks) {
  fq_conn_s *conn_s = (fq_conn_s *)conn;
  switch(hooks->version) {
    case FQ_HOOKS_V3:
      conn_s->sync_hooks = hooks->sync;
    case FQ_HOOKS_V2:
      conn_s->unbind_hook = hooks->unbind;
    case FQ_HOOKS_V1:
      conn_s->auth_hook = hooks->auth;
      conn_s->bind_hook = hooks->bind;
      break;
    default:
      return -1;
  }
  return 0;
}

int
fq_client_creds(fq_client conn, const char *host, unsigned short port,
                const char *sender, const char *pass) {
  char qname[39];
  fq_conn_s *conn_s;
  conn_s = conn;

  /* make the sockets as disconnected */
  conn_s->cmd_fd = conn_s->data_fd = -1;

  /* parse the user info */
  conn_s->user = strdup(sender);
  conn_s->queue = strchr(conn_s->user, '/');
  if(conn_s->queue) {
    *conn_s->queue++ = '\0';
    conn_s->queue_type = strchr(conn_s->queue, '/');
    if(conn_s->queue_type) {
      *conn_s->queue_type++ = '\0';
    }
  }
  if(!conn_s->queue || conn_s->queue[0] == '\0') {
    char *cp;
    uuid_t out;
    uuid_generate(out);
    qname[0] = 'q'; qname[1] = '-';
    uuid_unparse(out, qname+2);
    for(cp=qname;*cp;cp++) *cp = tolower(*cp);
    conn_s->queue = qname;
  }
  conn_s->queue_type = strdup(conn_s->queue_type ?
                                conn_s->queue_type :
                                FQ_DEFAULT_QUEUE_TYPE);
  conn_s->queue = strdup(conn_s->queue);
  conn_s->pass = strdup(pass);

  /* determine our endpoint */
  conn_s->remote.sin_family = AF_INET;
  conn_s->remote.sin_port = htons(port);
  if(inet_pton(AF_INET, host, &conn_s->remote.sin_addr) != 1) {
#ifdef HAVE_GETHOSTBYNAME_R
    struct hostent hostbuf, *hp;
    struct in_addr **addr_list;
    int buflen = 1024, herr;
    char *buf;
    if((buf = malloc(buflen)) == NULL) {
      CONNERR(conn_s, "out of memory");
      return -1;
    }
    while((hp = gethostbyname_r(host, &hostbuf, buf, buflen, &herr)) == NULL &&
          errno == ERANGE) {
      buflen *= 2;
      if((buf = realloc(buf, buflen)) == NULL) {
        CONNERR(conn_s, "out of memory");
        return -1;
      }
    }
    if(!hp) {
      CONNERR(conn_s, "host lookup failed");
      return -1;
    }
    addr_list = (struct in_addr **)hp->h_addr_list;
    if(*addr_list == 0) {
      CONNERR(conn_s, "no address for host");
      return -1;
    }
    memcpy(&conn_s->remote.sin_addr, *addr_list, sizeof(struct in_addr));
    free(buf);
#else
    struct hostent *hp;
    struct in_addr **addr_list;
    hp = gethostbyname(host);
    if(!hp) {
      CONNERR(conn_s, "host lookup failed");
      return -1;
    }
    addr_list = (struct in_addr **)hp->h_addr_list;
    if(*addr_list == 0) {
      CONNERR(conn_s, "no address for host");
      return -1;
    }
    memcpy(&conn_s->remote.sin_addr, *addr_list, sizeof(struct in_addr));
#endif
  }
  conn_s->cmdqhead = malloc(sizeof(ck_fifo_mpmc_entry_t));
  ck_fifo_mpmc_init(&conn_s->cmdq, conn_s->cmdqhead);

  conn_s->qhead = malloc(sizeof(ck_fifo_mpmc_entry_t));
  ck_fifo_mpmc_init(&conn_s->q, conn_s->qhead);

  conn_s->backqhead = malloc(sizeof(ck_fifo_mpmc_entry_t));
  ck_fifo_mpmc_init(&conn_s->backq, conn_s->backqhead);

  return 0;
}

void
fq_client_status(fq_client conn,
                 void (*f)(char *, uint32_t, void *), void *c) {
  cmd_instr *e;
  e = malloc(sizeof(*e));
  e->cmd = FQ_PROTO_STATUSREQ;
  e->data.status.callback = f;
  e->data.status.closure = c;
  fq_client_signal(conn, e);
}
void
fq_client_heartbeat(fq_client conn, unsigned short heartbeat_ms) {
  cmd_instr *e;
  e = malloc(sizeof(*e));
  e->cmd = FQ_PROTO_HBREQ;
  e->data.heartbeat.ms = heartbeat_ms;
  fq_client_signal(conn, e);
}
void
fq_client_bind(fq_client conn, fq_bind_req *req) {
  cmd_instr *e;
  e = malloc(sizeof(*e));
  e->cmd = FQ_PROTO_BINDREQ;
  e->data.bind = req;
  fq_client_signal(conn, e);
}
void
fq_client_unbind(fq_client conn, fq_unbind_req *req) {
  cmd_instr *e;
  e = malloc(sizeof(*e));
  e->cmd = FQ_PROTO_UNBINDREQ;
  e->data.unbind = req;
  fq_client_signal(conn, e);
}

void
fq_client_set_backlog(fq_client conn, uint32_t len, uint32_t stall) {
  fq_conn_s *conn_s = conn;
  conn_s->qmaxlen = len;
  conn_s->q_stall_time = stall;
}

void
fq_client_set_nonblock(fq_client conn, bool nonblock) {
  fq_conn_s *conn_s = conn;
  conn_s->non_blocking = nonblock;
}

int
fq_client_connect(fq_client conn) {
  fq_conn_s *conn_s = conn;
  if(conn_s->connected != 0) return -1;

  conn_s->connected = 1;
  if(pthread_create(&conn_s->worker, NULL, fq_conn_worker, conn_s) != 0) {
      CONNERR(conn_s, "could not start command thread");
    return -1;
  }
  if(pthread_create(&conn_s->data_worker, NULL, fq_data_worker, conn_s) != 0) {
      CONNERR(conn_s, "could not start data thread");
    return -1;
  }
  return 0;
}

int
fq_client_data_backlog(fq_client conn) {
  fq_conn_s *conn_s = conn;
  return ck_pr_load_uint(&conn_s->qlen);
}
int
fq_client_publish(fq_client conn, fq_msg *msg) {
  fq_conn_s *conn_s = conn;
  ck_fifo_mpmc_entry_t *fifo_entry;
  while(conn_s->qlen >= conn_s->qmaxlen) {
    if(conn_s->non_blocking) return -1;
    if(conn_s->q_stall_time > 0) usleep(conn_s->q_stall_time);
    else ck_pr_stall();
  }
  fifo_entry = malloc(sizeof(ck_fifo_mpmc_entry_t));
  fq_msg_ref(msg);
  ck_fifo_mpmc_enqueue(&conn_s->q, fifo_entry, msg);
  ck_pr_inc_uint(&conn_s->qlen);
  return 1;
}
fq_msg *fq_client_receive(fq_client conn) {
  fq_conn_s *conn_s = conn;
  fq_msg *m = NULL;
  ck_fifo_mpmc_entry_t *garbage;

  if(ck_fifo_mpmc_dequeue(&conn_s->backq, &m, &garbage) == true) {
    free(garbage);
  }
  if(m && CHECK_HOOK_REQ_PTR(m)) {
    hook_req_t *hreq = UNMARKED_HOOK_REQ_PTR(m);
    m = NULL;
    cmd_instr *entry = hreq->entry;
    switch(hreq->type) {
      case AUTH_HOOK_TYPE:
        if(conn_s->sync_hooks && conn_s->auth_hook)
          conn_s->auth_hook(conn_s, entry->data.return_value);
        break;
      case CMD_HOOK_TYPE:
        switch(entry->cmd) {
          case FQ_PROTO_BINDREQ:
            if(conn_s->sync_hooks && conn_s->bind_hook)
              conn_s->bind_hook(conn_s, entry->data.bind);
            break;
          case FQ_PROTO_UNBINDREQ:
            if(conn_s->sync_hooks && conn_s->unbind_hook)
              conn_s->unbind_hook(conn_s, entry->data.unbind);
            break;
          default:
            fprintf(stderr, "sync cmd feedback unknown: %x\n", entry->cmd);
        }
        break;
    }
    free(entry);
    free(hreq);
  }
  return m;
}

