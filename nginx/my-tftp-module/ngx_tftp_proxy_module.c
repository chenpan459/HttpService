/*
 * ngx_stream_tftp_proxy_module.c
 *
 * Simplified TFTP UDP proxy module for nginx stream
 *
 * Listens UDP 69, parses RRQ/WRQ, allocates data UDP port for proxying
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_stream.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define NGX_TFTP_PROXY_MIN_PORT 30000
#define NGX_TFTP_PROXY_MAX_PORT 31000
#define NGX_TFTP_PROXY_SESSION_TIMEOUT 60 /* seconds */

typedef struct {
    ngx_uint_t      port_min;
    ngx_uint_t      port_max;
    ngx_uint_t      timeout;
    ngx_str_t       backend;  /* backend IP:port */
} ngx_stream_tftp_proxy_srv_conf_t;

/* Per-session structure */
typedef struct {
    ngx_queue_t     queue;
    ngx_udp_connection_t *client_conn;
    ngx_udp_connection_t *backend_conn;
    ngx_event_t     timer;
    ngx_uint_t      proxy_port;
    ngx_addr_t      client_addr;
    ngx_addr_t      backend_addr;
    ngx_stream_tftp_proxy_srv_conf_t *srv_conf;
} ngx_stream_tftp_proxy_session_t;

/* Main context storing port pool and sessions */
typedef struct {
    ngx_queue_t     free_ports;   /* queue of ngx_uint_t ports */
    ngx_rbtree_t    sessions_rbtree;
    ngx_rbtree_node_t sentinel;
    ngx_event_t     cleanup_ev;
} ngx_stream_tftp_proxy_main_conf_t;

/* Module directives and context */
static void *ngx_stream_tftp_proxy_create_srv_conf(ngx_conf_t *cf);
static char *ngx_stream_tftp_proxy_merge_srv_conf(ngx_conf_t *cf, void *parent, void *child);
static char *ngx_stream_tftp_proxy_pass(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

static ngx_int_t ngx_stream_tftp_proxy_preread_handler(ngx_stream_session_t *s);
static ngx_int_t ngx_stream_tftp_proxy_init(ngx_conf_t *cf);
static ngx_int_t ngx_stream_tftp_proxy_init_main_conf(ngx_cycle_t *cycle);
static void ngx_stream_tftp_proxy_cleanup_handler(ngx_event_t *ev);

static ngx_command_t ngx_stream_tftp_proxy_commands[] = {
    { ngx_string("tftp_proxy_pass"),
      NGX_STREAM_SRV_CONF|NGX_CONF_TAKE1,
      ngx_stream_tftp_proxy_pass,
      NGX_STREAM_SRV_CONF_OFFSET,
      0,
      NULL },
    { ngx_string("tftp_data_port_range"),
      NGX_STREAM_SRV_CONF|NGX_CONF_TAKE2,
      ngx_conf_set_num_slot,
      NGX_STREAM_SRV_CONF_OFFSET,
      offsetof(ngx_stream_tftp_proxy_srv_conf_t, port_min),
      NULL },
    ngx_null_command
};

static ngx_stream_module_t ngx_stream_tftp_proxy_module_ctx = {
    NULL,
    ngx_stream_tftp_proxy_init,
    NULL,
    NULL,
    ngx_stream_tftp_proxy_create_srv_conf,
    ngx_stream_tftp_proxy_merge_srv_conf
};

ngx_module_t ngx_stream_tftp_proxy_module = {
    NGX_MODULE_V1,
    &ngx_stream_tftp_proxy_module_ctx,
    ngx_stream_tftp_proxy_commands,
    NGX_STREAM_MODULE,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NGX_MODULE_V1_PADDING
};

/* Create srv conf */
static void *ngx_stream_tftp_proxy_create_srv_conf(ngx_conf_t *cf) {
    ngx_stream_tftp_proxy_srv_conf_t *conf;
    conf = ngx_pcalloc(cf->pool, sizeof(ngx_stream_tftp_proxy_srv_conf_t));
    if (conf == NULL) {
        return NULL;
    }
    conf->port_min = NGX_CONF_UNSET_UINT;
    conf->port_max = NGX_CONF_UNSET_UINT;
    conf->timeout = NGX_CONF_UNSET_UINT;
    return conf;
}

/* Merge srv conf */
static char *ngx_stream_tftp_proxy_merge_srv_conf(ngx_conf_t *cf, void *parent, void *child) {
    ngx_stream_tftp_proxy_srv_conf_t *prev = parent;
    ngx_stream_tftp_proxy_srv_conf_t *conf = child;
    ngx_conf_merge_uint_value(conf->port_min, prev->port_min, NGX_TFTP_PROXY_MIN_PORT);
    ngx_conf_merge_uint_value(conf->port_max, prev->port_max, NGX_TFTP_PROXY_MAX_PORT);
    ngx_conf_merge_uint_value(conf->timeout, prev->timeout, NGX_TFTP_PROXY_SESSION_TIMEOUT);
    return NGX_CONF_OK;
}

/* tftp_proxy_pass directive handler */
static char *ngx_stream_tftp_proxy_pass(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_stream_tftp_proxy_srv_conf_t *psc = conf;
    ngx_str_t *value = cf->args->elts;
    psc->backend = value[1];
    return NGX_CONF_OK;
}

/* Preread handler: parse incoming UDP packet for RRQ/WRQ */
static ngx_int_t ngx_stream_tftp_proxy_preread_handler(ngx_stream_session_t *s) {
    // TODO: peek packet, parse opcode (1=RRQ,2=WRQ)
    // Allocate UDP port, create proxy session, start forwarding
    ngx_connection_t *c = s->connection;
    u_char buf[512];
    ssize_t n = recv(c->fd, buf, sizeof(buf), MSG_PEEK);
    if (n <= 0) {
        return NGX_OK;
    }
    // Simple check for RRQ/WRQ opcode
    if (n >= 2 && (buf[0] == 0 && (buf[1] == 1 || buf[1] == 2))) {
        ngx_log_error(NGX_LOG_INFO, c->log, 0, "[tftp_proxy] detected RRQ/WRQ");
        // TODO: allocate proxy UDP port, bind socket, create session,
        // start forwarding packets between client and backend
    }
    return NGX_OK;
}

/* Init: register preread handler */
static ngx_int_t ngx_stream_tftp_proxy_init(ngx_conf_t *cf) {
    ngx_stream_core_main_conf_t *cmcf = ngx_stream_conf_get_module_main_conf(cf, ngx_stream_core_module);
    ngx_array_t *phases = &cmcf->phases[NGX_STREAM_PREREAD_PHASE].handlers;
    ngx_stream_handler_pt *h = ngx_array_push(phases);
    if (h == NULL) {
        return NGX_ERROR;
    }
    *h = ngx_stream_tftp_proxy_preread_handler;
    return NGX_OK;
}

/* Module main conf init - setup port pool etc (skeleton) */
static ngx_int_t ngx_stream_tftp_proxy_init_main_conf(ngx_cycle_t *cycle) {
    ngx_stream_tftp_proxy_main_conf_t *mcf;
    ngx_uint_t i;
    ngx_uint_t nports;
    ngx_uint_t port_min = NGX_TFTP_PROXY_MIN_PORT;
    ngx_uint_t port_max = NGX_TFTP_PROXY_MAX_PORT;
    nports = port_max - port_min + 1;

    mcf = ngx_pcalloc(cycle->pool, sizeof(ngx_stream_tftp_proxy_main_conf_t));
    if (mcf == NULL) {
        return NGX_ERROR;
    }

    ngx_rbtree_init(&mcf->sessions_rbtree, &mcf->sentinel, ngx_str_rbtree_insert_value);
    ngx_queue_init(&mcf->free_ports);

    for (i = 0; i < nports; i++) {
        ngx_uint_t *pport = ngx_pcalloc(cycle->pool, sizeof(ngx_uint_t));
        *pport = port_min + i;
        ngx_queue_insert_tail(&mcf->free_ports, (ngx_queue_t *) pport);
    }

    /* schedule cleanup event */
    mcf->cleanup_ev.handler = ngx_stream_tftp_proxy_cleanup_handler;
    mcf->cleanup_ev.log = cycle->log;
    mcf->cleanup_ev.data = mcf;
    ngx_add_timer(&mcf->cleanup_ev, 1000);

    ngx_set_cycle_user_data(cycle, mcf);
    return NGX_OK;
}

/* Periodic cleanup */
static void ngx_stream_tftp_proxy_cleanup_handler(ngx_event_t *ev) {
    ngx_stream_tftp_proxy_main_conf_t *mcf = ev->data;
    // TODO: cleanup expired sessions, free ports
    ngx_add_timer(ev, 1000);
}

/* Usage example nginx.conf snippet:
stream {
    server {
        listen 69 udp;
        tftp_proxy_pass 192.168.1.100:69;
        tftp_data_port_range 30000 31000;
    }
}
*/

/* 需要自己完善：会话管理、UDP socket创建与绑定、数据转发回调、错误与超时处理等 */


