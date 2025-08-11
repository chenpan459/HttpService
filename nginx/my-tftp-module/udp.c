#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_stream.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define NGX_TFTP_PROXY_SESSION_TIMEOUT 60  // 超时时间秒
#define NGX_TFTP_PROXY_MIN_PORT 30000
#define NGX_TFTP_PROXY_MAX_PORT 31000
#define MAX_UDP_BUF 1500

typedef struct ngx_stream_tftp_proxy_session_s ngx_stream_tftp_proxy_session_t;

/* 会话结构 */
struct ngx_stream_tftp_proxy_session_s {
    ngx_rbtree_node_t            node;             // 用client_addr做rbtree key，方便查找
    ngx_addr_t                   client_addr;      // 客户端地址
    ngx_addr_t                   backend_addr;     // 后端服务器地址

    ngx_udp_connection_t        *client_udp;       // 监听客户端数据的UDP连接（69端口）
    ngx_udp_connection_t        *proxy_udp;        // 代理分配的UDP端口的监听连接
    ngx_udp_connection_t        *backend_udp;      // 连接后端服务器UDP

    ngx_event_t                  timeout_ev;       // 超时定时器事件

    ngx_uint_t                  proxy_port;        // 分配给该会话的UDP端口

    ngx_queue_t                 queue;             // 会话队列
};

typedef struct {
    ngx_rbtree_t                sessions_rbtree;
    ngx_rbtree_node_t           sentinel;

    ngx_queue_t                 free_ports;        // 端口池队列，元素是 ngx_uint_t*，保存空闲端口号

    ngx_queue_t                 sessions_queue;    // 活跃会话双向链表，用于超时管理

    ngx_log_t                  *log;
    ngx_pool_t                 *pool;
} ngx_stream_tftp_proxy_main_conf_t;

/* 全局会话管理结构（示范） */
static ngx_stream_tftp_proxy_main_conf_t  *proxy_main_conf = NULL;

/* 创建UDP连接 */
static ngx_udp_connection_t *
ngx_stream_tftp_proxy_create_udp_connection(ngx_log_t *log, ngx_pool_t *pool, struct sockaddr *sockaddr, socklen_t socklen, void *data, ngx_udp_recv_pt recv_handler)
{
    int s;
    ngx_connection_t *c;
    ngx_udp_connection_t *uc;

    s = socket(sockaddr->sa_family, SOCK_DGRAM, 0);
    if (s == -1) {
        ngx_log_error(NGX_LOG_ERR, log, ngx_socket_errno, "socket() failed");
        return NULL;
    }

    if (bind(s, sockaddr, socklen) == -1) {
        ngx_log_error(NGX_LOG_ERR, log, ngx_socket_errno, "bind() failed");
        close(s);
        return NULL;
    }

    if (ngx_nonblocking(s) == -1) {
        ngx_log_error(NGX_LOG_ERR, log, ngx_socket_errno, "ngx_nonblocking() failed");
        close(s);
        return NULL;
    }

    c = ngx_get_connection(s, log);
    if (c == NULL) {
        close(s);
        return NULL;
    }

    uc = ngx_pcalloc(pool, sizeof(ngx_udp_connection_t));
    if (uc == NULL) {
        ngx_close_connection(c);
        return NULL;
    }

    c->data = data;
    c->read->handler = (ngx_event_handler_pt) recv_handler;
    c->read->log = log;
    c->write->handler = NULL;

    uc->connection = c;
    uc->sockaddr = sockaddr;
    uc->socklen = socklen;
    uc->recv = recv_handler;

    if (ngx_add_event(c->read, NGX_READ_EVENT, 0) != NGX_OK) {
        ngx_close_connection(c);
        return NULL;
    }

    return uc;
}

/* 端口池初始化 */
static ngx_int_t
ngx_stream_tftp_proxy_init_port_pool(ngx_pool_t *pool, ngx_log_t *log)
{
    ngx_uint_t port;
    ngx_uint_t *pport;

    proxy_main_conf = ngx_pcalloc(pool, sizeof(ngx_stream_tftp_proxy_main_conf_t));
    if (proxy_main_conf == NULL) {
        return NGX_ERROR;
    }

    ngx_rbtree_init(&proxy_main_conf->sessions_rbtree, &proxy_main_conf->sentinel, ngx_str_rbtree_insert_value);
    ngx_queue_init(&proxy_main_conf->free_ports);
    ngx_queue_init(&proxy_main_conf->sessions_queue);

    proxy_main_conf->log = log;
    proxy_main_conf->pool = pool;

    for (port = NGX_TFTP_PROXY_MIN_PORT; port <= NGX_TFTP_PROXY_MAX_PORT; port++) {
        pport = ngx_pcalloc(pool, sizeof(ngx_uint_t));
        *pport = port;
        ngx_queue_insert_tail(&proxy_main_conf->free_ports, (ngx_queue_t *) pport);
    }

    return NGX_OK;
}

/* 从端口池取端口 */
static ngx_uint_t
ngx_stream_tftp_proxy_alloc_port()
{
    if (ngx_queue_empty(&proxy_main_conf->free_ports)) {
        return 0;
    }

    ngx_queue_t *q = ngx_queue_head(&proxy_main_conf->free_ports);
    ngx_uint_t *pport = (ngx_uint_t *) q;
    ngx_uint_t port = *pport;
    ngx_queue_remove(q);
    return port;
}

/* 释放端口到端口池 */
static void
ngx_stream_tftp_proxy_free_port(ngx_uint_t port)
{
    ngx_uint_t *pport = ngx_pcalloc(proxy_main_conf->pool, sizeof(ngx_uint_t));
    if (pport == NULL) {
        return;
    }
    *pport = port;
    ngx_queue_insert_tail(&proxy_main_conf->free_ports, (ngx_queue_t *) pport);
}

/* 查找会话：用客户端ip和port作为key */
static ngx_stream_tftp_proxy_session_t *
ngx_stream_tftp_proxy_find_session(ngx_addr_t *client_addr)
{
    // 简单遍历链表查找，生产可用rbtree优化
    ngx_queue_t *q;
    ngx_stream_tftp_proxy_session_t *session;

    for (q = ngx_queue_head(&proxy_main_conf->sessions_queue);
         q != ngx_queue_sentinel(&proxy_main_conf->sessions_queue);
         q = ngx_queue_next(q))
    {
        session = ngx_queue_data(q, ngx_stream_tftp_proxy_session_t, queue);
        if (ngx_cmp_sockaddr(&client_addr->sockaddr, &session->client_addr.sockaddr, client_addr->socklen) == NGX_OK) {
            return session;
        }
    }

    return NULL;
}

/* 会话超时回调 */
static void
ngx_stream_tftp_proxy_session_timeout(ngx_event_t *ev)
{
    ngx_stream_tftp_proxy_session_t *session = ev->data;
    ngx_log_error(NGX_LOG_INFO, session->client_udp->connection->log, 0, "tftp proxy session timeout");

    // 关闭UDP连接
    if (session->proxy_udp) {
        ngx_close_connection(session->proxy_udp->connection);
        session->proxy_udp = NULL;
    }
    if (session->backend_udp) {
        ngx_close_connection(session->backend_udp->connection);
        session->backend_udp = NULL;
    }

    // 释放端口
    ngx_stream_tftp_proxy_free_port(session->proxy_port);

    // 从会话链表移除
    ngx_queue_remove(&session->queue);

    // 释放内存
    // 此处pool的释放由整体上下文决定，这里示范省略
}

/* UDP数据转发回调 */
static void
ngx_stream_tftp_proxy_udp_recv_handler(ngx_udp_connection_t *uc, ngx_udp_recv_t *recv, ngx_buf_t *buf)
{
    ngx_connection_t *c = uc->connection;
    ngx_stream_tftp_proxy_session_t *session = c->data;
    struct sockaddr *from = (struct sockaddr *) recv->sockaddr;

    ssize_t n;

    if (ngx_cmp_sockaddr(from, &session->client_addr.sockaddr, session->client_addr.socklen) == NGX_OK) {
        // 来自客户端，转发给后端
        n = sendto(session->backend_udp->connection->fd, buf->pos, buf->last - buf->pos, 0,
                   (struct sockaddr *) &session->backend_addr.sockaddr, session->backend_addr.socklen);
        if (n == -1) {
            ngx_log_error(NGX_LOG_ERR, c->log, ngx_socket_errno, "sendto backend failed");
        }
    } else if (ngx_cmp_sockaddr(from, &session->backend_addr.sockaddr, session->backend_addr.socklen) == NGX_OK) {
        // 来自后端，转发给客户端
        n = sendto(session->client_udp->connection->fd, buf->pos, buf->last - buf->pos, 0,
                   (struct sockaddr *) &session->client_addr.sockaddr, session->client_addr.socklen);
        if (n == -1) {
            ngx_log_error(NGX_LOG_ERR, c->log, ngx_socket_errno, "sendto client failed");
        }
    } else {
        ngx_log_error(NGX_LOG_WARN, c->log, 0, "tftp proxy unknown UDP packet source");
    }

    // 重置超时定时器
    ngx_add_timer(&session->timeout_ev, NGX_TFTP_PROXY_SESSION_TIMEOUT * 1000);
}

/* 创建会话 */
static ngx_stream_tftp_proxy_session_t *
ngx_stream_tftp_proxy_create_session(ngx_addr_t *client_addr, ngx_addr_t *backend_addr, ngx_udp_connection_t *client_udp, ngx_log_t *log, ngx_pool_t *pool)
{
    ngx_stream_tftp_proxy_session_t *session;
    ngx_uint_t port;
    struct sockaddr_in sin;

    session = ngx_pcalloc(pool, sizeof(ngx_stream_tftp_proxy_session_t));
    if (session == NULL) {
        return NULL;
    }

    session->client_addr = *client_addr;
    session->backend_addr = *backend_addr;
    session->client_udp = client_udp;

    port = ngx_stream_tftp_proxy_alloc_port();
    if (port == 0) {
        ngx_log_error(NGX_LOG_ERR, log, 0, "no available ports for tftp proxy session");
        return NULL;
    }
    session->proxy_port = port;

    ngx_memzero(&sin, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = INADDR_ANY;

    session->proxy_udp = ngx_stream_tftp_proxy_create_udp_connection(log, pool, (struct sockaddr *) &sin, sizeof(sin), session, ngx_stream_tftp_proxy_udp_recv_handler);
    if (session->proxy_udp == NULL) {
        ngx_stream_tftp_proxy_free_port(port);
        return NULL;
    }

    // 连接后端UDP（非绑定，发包时自动完成）
    session->backend_udp = ngx_pcalloc(pool, sizeof(ngx_udp_connection_t));
    if (session->backend_udp == NULL) {
        ngx_close_connection(session->proxy_udp->connection);
        ngx_stream_tftp_proxy_free_port(port);
        return NULL;
    }

    session->backend_udp->connection = NULL;  // 不绑定，发包时用sendto地址即可
    session->backend_udp->sockaddr = &session->backend_addr.sockaddr;
    session->backend_udp->socklen = session->backend_addr.socklen;

    // 初始化超时定时器
    session->timeout_ev.handler = ngx_stream_tftp_proxy_session_timeout;
    session->timeout_ev.data = session;
    session->timeout_ev.log = log;
    ngx_add_timer(&session->timeout_ev, NGX_TFTP_PROXY_SESSION_TIMEOUT * 1000);

    // 加入会话链表
    ngx_queue_insert_tail(&proxy_main_conf->sessions_queue, &session->queue);

    return session;
}

/* 模块初始化示例 */
static ngx_int_t
ngx_stream_tftp_proxy_init(ngx_conf_t *cf)
{
    ngx_pool_t *pool = cf->pool;
    ngx_log_t *log = cf->log;

    if (ngx_stream_tftp_proxy_init_port_pool(pool, log) != NGX_OK) {
        return NGX_ERROR;
    }

    // 监听UDP 69端口、注册preread handler等初始化操作...

    return NGX_OK;
}
