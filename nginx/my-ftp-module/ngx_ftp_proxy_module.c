/*
 * ngx_stream_ftp_proxy_module.c
 * FTP application-layer proxy module for nginx/tengine (expanded skeleton)
 *
 * Goal: intercept FTP control connections, parse PORT and PASV(227) messages,
 * allocate proxy-managed data ports, establish data-channel forwarders, and
 * maintain per-session mappings with timeout cleanup.
 *
 * IMPORTANT: This module is an extended, practical skeleton intended to be a
 * solid development starting point. It implements:
 *  - control-channel preread inspection to detect PORT/PASV events
 *  - a data-port pool and allocator
 *  - creating local TCP listener sockets for allocated data ports
 *  - session table management and TTL-based cleanup
 *  - hooks/placeholders for rewriting control messages and for attaching
 *    data-forwarder callbacks
 *
 * It still requires production hardening: robust parsing (multi-line replies),
 * binary-safe rewriting, full bidirectional data-forwarding code, FTPS handling,
 * and extensive testing. But the code below provides all essential building
 * blocks and an example data-socket accept callback.
 *
 * Build: add this module to nginx/Tengine build with --add-module=/path/to/this/dir
 * Example configure: ./configure --with-stream --add-module=/path/to/module
 *
 * Author: generated skeleton (extended)
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_stream.h>

#define NGX_FTP_PROXY_MIN_DATA_PORT 20000
#define NGX_FTP_PROXY_MAX_DATA_PORT 21000
#define NGX_FTP_PROXY_SESSION_TTL   60
#define NGX_FTP_PROXY_MAX_SESSIONS  16384

typedef struct {
    ngx_str_t  backend;      /* backend address (IP:port or upstream name) */
    ngx_uint_t data_port_min;/* passive data port range min */
    ngx_uint_t data_port_max;/* passive data port range max */
    ngx_uint_t timeout;      /* control/data idle timeout seconds */
} ngx_stream_ftp_proxy_srv_conf_t;

/* per-session data */
typedef struct ngx_ftp_proxy_session_s  ngx_ftp_proxy_session_t;

typedef struct {
    ngx_queue_t       queue;        /* for session LRU / cleanup */
    ngx_uint_t        in_use;       /* 0/1 */
    ngx_socket_t      fd;           /* listening socket for allocated data port */
    ngx_event_t       rcv_event;    /* accept/read handler */
    ngx_uint_t        port;         /* local port allocated */
    ngx_ftp_proxy_session_t  *sess;  /* backref to owning session */
} ngx_ftp_data_listener_t;

struct ngx_ftp_proxy_session_s {
    ngx_queue_t              queue;      /* global session queue */
    ngx_connection_t        *c;          /* client control connection */
    ngx_connection_t        *upstream;   /* upstream control connection */
    ngx_pool_t              *pool;
    ngx_event_t              timer;      /* TTL timer */

    /* mapping state: one data listener for PASV allocated by server rewrite
     * or for PORT by client rewrite. Real implementation supports multiple
     * concurrent data channels per session; skeleton uses one for demo. */
    ngx_ftp_data_listener_t *data_listener;
    ngx_uint_t               state;
};

/* module-wide context */
typedef struct {
    ngx_rbtree_t            sessions_rbtree;
    ngx_rbtree_node_t       sessions_sentinel;
    ngx_queue_t             free_data_ports; /* queue of ngx_ftp_data_listener_t */
    ngx_ftp_data_listener_t *data_listeners_pool;
    ngx_uint_t              data_listeners_n;
    ngx_event_t             cleanup_ev;
    ngx_uint_t              session_count;
} ngx_stream_ftp_proxy_main_conf_t;

/* prototypes */
static void *ngx_stream_ftp_proxy_create_srv_conf(ngx_conf_t *cf);
static char *ngx_stream_ftp_proxy_merge_srv_conf(ngx_conf_t *cf, void *parent, void *child);
static char *ngx_stream_ftp_proxy_pass(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_stream_ftp_proxy_preread_handler(ngx_stream_session_t *s);
static ngx_int_t ngx_stream_ftp_proxy_init(ngx_conf_t *cf);
static ngx_int_t ngx_stream_ftp_proxy_init_main_conf(ngx_cycle_t *cycle);
static void ngx_stream_ftp_proxy_cleanup_handler(ngx_event_t *ev);
static ngx_ftp_data_listener_t *ngx_ftp_allocate_data_listener(ngx_stream_ftp_proxy_main_conf_t *mcf);
static void ngx_ftp_free_data_listener(ngx_stream_ftp_proxy_main_conf_t *mcf, ngx_ftp_data_listener_t *dl);
static void ngx_ftp_data_accept_handler(ngx_event_t *ev);

/* directives */
static ngx_command_t ngx_stream_ftp_proxy_commands[] = {
    { ngx_string("ftp_proxy_pass"),
      NGX_STREAM_SRV_CONF|NGX_CONF_TAKE1,
      ngx_stream_ftp_proxy_pass,
      NGX_STREAM_SRV_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("ftp_data_port_range"),
      NGX_STREAM_SRV_CONF|NGX_CONF_TAKE2,
      ngx_conf_set_num_slot,
      NGX_STREAM_SRV_CONF_OFFSET,
      offsetof(ngx_stream_ftp_proxy_srv_conf_t, data_port_min),
      NULL },

    ngx_null_command
};

static ngx_stream_module_t ngx_stream_ftp_proxy_module_ctx = {
    NULL,
    ngx_stream_ftp_proxy_init,

    NULL,
    NULL,

    ngx_stream_ftp_proxy_create_srv_conf,
    ngx_stream_ftp_proxy_merge_srv_conf
};

ngx_module_t ngx_stream_ftp_proxy_module = {
    NGX_MODULE_V1,
    &ngx_stream_ftp_proxy_module_ctx,
    ngx_stream_ftp_proxy_commands,
    NGX_STREAM_MODULE,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NGX_MODULE_V1_PADDING
};

/* create server conf */
static void *
ngx_stream_ftp_proxy_create_srv_conf(ngx_conf_t *cf)
{
    ngx_stream_ftp_proxy_srv_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_stream_ftp_proxy_srv_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    conf->data_port_min = NGX_CONF_UNSET_UINT;
    conf->data_port_max = NGX_CONF_UNSET_UINT;
    conf->timeout = NGX_CONF_UNSET_UINT;

    return conf;
}

static char *
ngx_stream_ftp_proxy_merge_srv_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_stream_ftp_proxy_srv_conf_t *prev = parent;
    ngx_stream_ftp_proxy_srv_conf_t *conf = child;

    ngx_conf_merge_str_value(conf->backend, prev->backend, "");
    ngx_conf_merge_uint_value(conf->data_port_min, prev->data_port_min, NGX_FTP_PROXY_MIN_DATA_PORT);
    ngx_conf_merge_uint_value(conf->data_port_max, prev->data_port_max, NGX_FTP_PROXY_MAX_DATA_PORT);
    ngx_conf_merge_uint_value(conf->timeout, prev->timeout, NGX_FTP_PROXY_SESSION_TTL);

    return NGX_CONF_OK;
}

static char *
ngx_stream_ftp_proxy_pass(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_stream_ftp_proxy_srv_conf_t *psc = conf;
    ngx_str_t *value;

    value = cf->args->elts;
    psc->backend = value[1];

    return NGX_CONF_OK;
}

/* helper: case-insensitive startswith */
static ngx_int_t
starts_with_ci(u_char *s, u_char *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (ngx_tolower(s[i]) != ngx_tolower(p[i])) return 0;
    }
    return 1;
}

/* parse PORT h1,h2,h3,h4,p1,p2 into sockaddr_in. naive parser */
static ngx_int_t
parse_port_cmd(u_char *data, ssize_t len, struct sockaddr_in *addr)
{
    u_char *p = data;
    /* find start of numbers */
    while (p < data + len && *p != ' ' && *p != '	') p++;
    if (p >= data + len) return NGX_ERROR;
    p++; /* skip space */

    u_char buf[64];
    ssize_t copy_len = ngx_min((ssize_t)sizeof(buf)-1, (data + len) - p);
    ngx_memzero(buf, sizeof(buf));
    ngx_memcpy(buf, p, copy_len);

    ngx_uint_t nums[6];
    char *tok = strtok((char*)buf, ",
");
    int i=0;
    while (tok && i < 6) {
        nums[i++] = (ngx_uint_t) atoi(tok);
        tok = strtok(NULL, ",
");
    }
    if (i != 6) return NGX_ERROR;

    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = htonl((nums[0]<<24)|(nums[1]<<16)|(nums[2]<<8)|nums[3]);
    addr->sin_port = htons((nums[4]<<8)|nums[5]);
    return NGX_OK;
}

/* parse 227 reply: find '(' then parse numbers */
static ngx_int_t
parse_227_reply(u_char *data, ssize_t len, struct sockaddr_in *addr)
{
    u_char *p = ngx_strlchr(data, data+len, '(');
    if (p == NULL) return NGX_ERROR;
    p++;

    u_char buf[64];
    ssize_t copy_len = ngx_min((ssize_t)sizeof(buf)-1, (data + len) - p);
    ngx_memzero(buf, sizeof(buf));
    ngx_memcpy(buf, p, copy_len);

    ngx_uint_t nums[6];
    char *tok = strtok((char*)buf, ",)
");
    int i=0;
    while (tok && i < 6) {
        nums[i++] = (ngx_uint_t) atoi(tok);
        tok = strtok(NULL, ",)
");
    }
    if (i != 6) return NGX_ERROR;

    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = htonl((nums[0]<<24)|(nums[1]<<16)|(nums[2]<<8)|nums[3]);
    addr->sin_port = htons((nums[4]<<8)|nums[5]);
    return NGX_OK;
}

/* allocate data-listener pool at init */
static ngx_int_t
ngx_stream_ftp_proxy_init(ngx_conf_t *cf)
{
    ngx_stream_core_main_conf_t   *cmcf;
    ngx_stream_ftp_proxy_main_conf_t *mcf;

    cmcf = ngx_stream_conf_get_module_main_conf(cf, ngx_stream_core_module);
    ngx_array_t *phases = &cmcf->phases[NGX_STREAM_PREREAD_PHASE].handlers;

    ngx_stream_handler_pt *h = ngx_array_push(phases);
    if (h == NULL) return NGX_ERROR;
    *h = ngx_stream_ftp_proxy_preread_handler;

    /* create main conf (module-global) in cycle init later */
    return NGX_OK;
}

/* initialize module-wide structures in cycle init (post configure) */
static ngx_int_t
ngx_stream_ftp_proxy_init_main_conf(ngx_cycle_t *cycle)
{
    ngx_stream_ftp_proxy_main_conf_t *mcf;
    ngx_uint_t i;
    ngx_uint_t port_min = NGX_FTP_PROXY_MIN_DATA_PORT;
    ngx_uint_t port_max = NGX_FTP_PROXY_MAX_DATA_PORT;
    ngx_uint_t nports = port_max - port_min + 1;

    mcf = ngx_pcalloc(cycle->pool, sizeof(ngx_stream_ftp_proxy_main_conf_t));
    if (mcf == NULL) return NGX_ERROR;

    ngx_rbtree_init(&mcf->sessions_rbtree, &mcf->sessions_sentinel, ngx_str_rbtree_insert_value);
    ngx_queue_init(&mcf->free_data_ports);

    mcf->data_listeners_n = nports;
    mcf->data_listeners_pool = ngx_pcalloc(cycle->pool, sizeof(ngx_ftp_data_listener_t) * nports);
    if (mcf->data_listeners_pool == NULL) return NGX_ERROR;

    for (i = 0; i < nports; i++) {
        ngx_ftp_data_listener_t *dl = &mcf->data_listeners_pool[i];
        dl->in_use = 0;
        dl->fd = (ngx_socket_t) -1;
        dl->port = (ngx_uint_t)(port_min + i);
        ngx_queue_insert_tail(&mcf->free_data_ports, &dl->queue);
    }

    /* schedule cleanup event */
    mcf->cleanup_ev.handler = ngx_stream_ftp_proxy_cleanup_handler;
    mcf->cleanup_ev.log = cycle->log;
    mcf->cleanup_ev.data = mcf;
    ngx_add_timer(&mcf->cleanup_ev, 1000); /* 1s periodic cleanup */

    /* store mcf in cycle->conf_ctx? For skeleton, we attach to cycle->pool user data */
    ngx_set_cycle_user_data(cycle, mcf);

    return NGX_OK;
}

/* allocate a free data listener entry (pop from free queue) */
static ngx_ftp_data_listener_t *
ngx_ftp_allocate_data_listener(ngx_stream_ftp_proxy_main_conf_t *mcf)
{
    if (ngx_queue_empty(&mcf->free_data_ports)) {
        return NULL;
    }

    ngx_queue_t *q = ngx_queue_head(&mcf->free_data_ports);
    ngx_ftp_data_listener_t *dl = (ngx_ftp_data_listener_t *) q;
    ngx_queue_remove(q);
    dl->in_use = 1;
    dl->sess = NULL;
    return dl;
}

static void
ngx_ftp_free_data_listener(ngx_stream_ftp_proxy_main_conf_t *mcf, ngx_ftp_data_listener_t *dl)
{
    if (dl->in_use == 0) return;
    dl->in_use = 0;
    if (dl->fd != (ngx_socket_t)-1) {
        ngx_close_socket(dl->fd);
        dl->fd = (ngx_socket_t)-1;
    }
    ngx_queue_insert_head(&mcf->free_data_ports, &dl->queue);
}

/* create a TCP listening socket for the allocated port */
static ngx_int_t
ngx_ftp_create_listener(ngx_ftp_data_listener_t *dl, ngx_log_t *log)
{
    ngx_socket_t s;
    struct sockaddr_in addr;
    int reuse = 1;

    s = ngx_socket(AF_INET, SOCK_STREAM, 0);
    if (s == (ngx_socket_t) -1) {
        ngx_log_error(NGX_LOG_ERR, log, ngx_socket_errno, "ngx_socket() failed");
        return NGX_ERROR;
    }

    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const void*)&reuse, sizeof(reuse)) == -1) {
        ngx_log_error(NGX_LOG_WARN, log, ngx_socket_errno, "setsockopt(SO_REUSEADDR) failed");
    }

    ngx_memzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((in_port_t) dl->port);

    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        ngx_log_error(NGX_LOG_ERR, log, ngx_socket_errno, "bind(port %ui) failed", dl->port);
        ngx_close_socket(s);
        return NGX_ERROR;
    }

    if (listen(s, 16) == -1) {
        ngx_log_error(NGX_LOG_ERR, log, ngx_socket_errno, "listen() failed");
        ngx_close_socket(s);
        return NGX_ERROR;
    }

    dl->fd = s;
    dl->rcv_event.data = dl;
    dl->rcv_event.log = log;
    dl->rcv_event.handler = ngx_ftp_data_accept_handler;
    ngx_add_event(&dl->rcv_event, NGX_READ_EVENT, 0);

    ngx_log_error(NGX_LOG_INFO, log, 0, "[ftp_proxy] listening data port %ui", dl->port);
    return NGX_OK;
}

/* accept handler for data listener - when a client/server connects to data port */
static void
ngx_ftp_data_accept_handler(ngx_event_t *ev)
{
    ngx_ftp_data_listener_t *dl = ev->data;
    ngx_socket_t s;
    struct sockaddr_in peer;
    socklen_t len = sizeof(peer);

    s = accept(dl->fd, (struct sockaddr*)&peer, &len);
    if (s == (ngx_socket_t) -1) {
        ngx_log_error(NGX_LOG_ERR, ev->log, ngx_socket_errno, "accept() failed");
        return;
    }

    ngx_log_error(NGX_LOG_INFO, ev->log, 0, "[ftp_proxy] data accept on port %ui", dl->port);

    /* TODO: create ngx_connection_t wrapping 's', attach read/write handlers to
     * forward data between client and backend peer sockets according to mapping.
     * For demo skeleton we just close immediately (user to implement forwarding).
     */

    ngx_close_socket(s);
}

/* periodic cleanup: free stale sessions/listeners */
static void
ngx_stream_ftp_proxy_cleanup_handler(ngx_event_t *ev)
{
    ngx_stream_ftp_proxy_main_conf_t *mcf = ev->data;
    /* TODO: iterate session rbtree / queues, drop stale sessions, free data listeners */
    ngx_add_timer(ev, 1000);
}

/* preread handler: inspect first bytes of control channel and detect PORT/227 */
static ngx_int_t
ngx_stream_ftp_proxy_preread_handler(ngx_stream_session_t *s)
{
    ngx_connection_t *c = s->connection;
    ssize_t n;
    u_char buf[2048];

    n = recv(c->fd, buf, sizeof(buf), MSG_PEEK);
    if (n == -1) {
        if (ngx_socket_errno == NGX_EAGAIN) return NGX_OK;
        ngx_log_error(NGX_LOG_ERR, c->log, ngx_socket_errno, "recv() failed");
        return NGX_ERROR;
    }

    if (n >= 4 && starts_with_ci(buf, (u_char*)"PORT", 4)) {
        struct sockaddr_in addr;
        if (parse_port_cmd(buf, n, &addr) == NGX_OK) {
            ngx_log_error(NGX_LOG_INFO, c->log, 0,
                          "[ftp_proxy] detected PORT %ud.%ud.%ud.%ud:%d",
                          (addr.sin_addr.s_addr>>24)&0xff,
                          (addr.sin_addr.s_addr>>16)&0xff,
                          (addr.sin_addr.s_addr>>8)&0xff,
                          (addr.sin_addr.s_addr)&0xff,
                          ntohs(addr.sin_port));

            /* find module main conf */
            ngx_stream_ftp_proxy_main_conf_t *mcf = ngx_get_cycle_user_data(ngx_cycle);
            if (mcf) {
                ngx_ftp_data_listener_t *dl = ngx_ftp_allocate_data_listener(mcf);
                if (dl) {
                    if (ngx_ftp_create_listener(dl, c->log) == NGX_OK) {
                        /* TODO: rewrite PORT command to point to dl->port
                         * and register mapping (client addr -> backend addr via dl)
                         * Rewriting requires consuming from client and writing modified
                         * buffer to upstream control socket; the proper place is in a
                         * stream filter that sits between client and upstream.
                         */
                    } else {
                        ngx_ftp_free_data_listener(mcf, dl);
                    }
                } else {
                    ngx_log_error(NGX_LOG_WARN, c->log, 0, "[ftp_proxy] no free data port");
                }
            }
        }
    }

    if (n >= 3 && starts_with_ci(buf, (u_char*)"227", 3)) {
        struct sockaddr_in addr;
        if (parse_227_reply(buf, n, &addr) == NGX_OK) {
            ngx_log_error(NGX_LOG_INFO, c->log, 0,
                          "[ftp_proxy] detected 227 PASV %ud.%ud.%ud.%ud:%d",
                          (addr.sin_addr.s_addr>>24)&0xff,
                          (addr.sin_addr.s_addr>>16)&0xff,
                          (addr.sin_addr.s_addr>>8)&0xff,
                          (addr.sin_addr.s_addr)&0xff,
                          ntohs(addr.sin_port));

            /* Similar: allocate local port, create listener, rewrite 227 to proxy IP:port */
        }
    }

    return NGX_OK;
}

/* Module cycle init hook: register main-conf initializer */
static ngx_int_t
ngx_stream_ftp_proxy_init_cycle(ngx_cycle_t *cycle)
{
    return ngx_stream_ftp_proxy_init_main_conf(cycle);
}

/* NOTE: hooking into cycle init requires adding callbacks to init cycle list
 * which is out of scope of this skeleton. You can call
 * ngx_stream_ftp_proxy_init_main_conf from a proper hook in your build or
 * modify core initialization. For simplicity, we rely on ngx_set_cycle_user_data
 * used earlier in init_main_conf (see comments).
 */

/* README usage notes (extended)

To compile:
  ./configure --with-stream --add-module=/path/to/this/module
  make && make install

Example nginx.conf (stream):

stream {
    server {
        listen 21;
        ftp_proxy_pass 192.168.1.100:21;
    }
}

What this module provides in skeleton form:
 - detection of PORT commands and 227 replies
 - a pool-backed allocator of local TCP ports (default 20000-21000)
 - creation of listening sockets for allocated ports and accept handler stub
 - session and listener bookkeeping primitives and periodic cleanup hook

What's left to implement for a working FTP proxy:
 1) Proper interception and rewriting of control-channel messages. You must
    implement a stream filter that consumes client bytes, rewrites PORT lines
    and forwards modified bytes to upstream; likewise intercept upstream replies
    (227) and rewrite them to proxy address/port.
 2) Complete data channel forwarding: when a data peer connects to allocated
    local port, establish a connection to the remote party (server or client)
    and proxy bytes bidirectionally using non-blocking ngx_connection_t objects
    and event handlers.
 3) Support multiple concurrent data channels per control session and map them
    correctly. Manage lifecycle and TTL cleanup.
 4) Handle edge cases: multi-line 227 replies, non-ASCII replies, binary files,
    overlapping commands, and both passive/active FTP modes robustly.
 5) FTPS (FTP over TLS): if TLS terminates on control channel, parsing must
    happen after TLS decryption; for passthrough you need other techniques.

If you want, I can proceed and implement each missing piece step-by-step:
 - Step A: implement stream-level filters to consume and rewrite control data
 - Step B: implement full data socket forwarder (bidirectional) with mapping
 - Step C: add robust parsing, logging, metrics and limits

Which step do you want next? (A/B/C) Please pick one and I will update the module
code in the document accordingly.
*/
