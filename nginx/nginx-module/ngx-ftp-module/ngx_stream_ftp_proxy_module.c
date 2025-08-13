#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_stream.h>


// server 配置结构
typedef struct {
    ngx_url_t                 *upstream;
} ngx_stream_ftp_proxy_srv_conf_t;


// 指令解析函数
static char *
ngx_stream_ftp_proxy_pass(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_stream_ftp_proxy_srv_conf_t *pscf = conf;
    ngx_str_t                        *value;
    ngx_url_t                        *u;

    value = cf->args->elts;

    u = ngx_pcalloc(cf->pool, sizeof(ngx_url_t));
    if (u == NULL) {
        return NGX_CONF_ERROR;
    }

    u->url = value[1];
    u->default_port = 21; // FTP 控制端口
    u->no_resolve = 0;

    ngx_conf_log_error(NGX_LOG_NOTICE, cf, 0, "ftp_proxy_pass backend: %V", &value[1]);    
    // ngx_conf_log_error(NGX_LOG_NOTICE, cf, 0, "ftp_proxy_pass set to: %V", &psc->backend);

    if (ngx_parse_url(cf->pool, u) != NGX_OK) {
        if (u->err) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "%s in upstream \"%V\"", u->err, &u->url);
        }
        return NGX_CONF_ERROR;
    }

    pscf->upstream = u;
ngx_conf_log_error(NGX_LOG_NOTICE, cf, 0, "ngx_stream_ftp_proxy_pass end");  
    return NGX_CONF_OK;
}


// 连接处理函数（后续可以在这里解析 FTP 控制通道）
static ngx_int_t
ngx_stream_ftp_proxy_handler(ngx_stream_session_t *s)
{
    ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
                  "FTP proxy handler: client %V", &s->connection->addr_text);

    // TODO: 建立 upstream 连接并双向转发
    ngx_stream_finalize_session(s, NGX_OK);
    return NGX_OK;
}


// postconfiguration 注册 handler
static ngx_int_t
ngx_stream_ftp_proxy_init(ngx_conf_t *cf)
{
    ngx_stream_core_main_conf_t *cmcf;
    ngx_stream_handler_pt       *h;
    ngx_conf_log_error(NGX_LOG_NOTICE, cf, 0, "ngx_stream_ftp_proxy_init start");    
    
    cmcf = ngx_stream_conf_get_module_main_conf(cf, ngx_stream_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_STREAM_PREREAD_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_stream_ftp_proxy_handler;
    ngx_conf_log_error(NGX_LOG_NOTICE, cf, 0, "ngx_stream_ftp_proxy_init end");    
    
    return NGX_OK;
}


// 创建 server 配置
static void *
ngx_stream_ftp_proxy_create_srv_conf(ngx_conf_t *cf)
{
    ngx_stream_ftp_proxy_srv_conf_t *conf;
 ngx_conf_log_error(NGX_LOG_NOTICE, cf, 0, "ngx_stream_ftp_proxy_create_srv_conf start");    
    
    conf = ngx_pcalloc(cf->pool, sizeof(ngx_stream_ftp_proxy_srv_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    // if (conf->handler == NULL) {
    //    ngx_conf_log_error(NGX_LOG_NOTICE, cf, 0, "conf->handler == NULL");                   
    //     // return NGX_CONF_ERROR;
    // }

    ngx_conf_log_error(NGX_LOG_NOTICE, cf, 0, "ngx_stream_ftp_proxy_create_srv_conf end");    
    
    return conf;
}


static void
ngx_stream_ftp_content_handler(ngx_stream_session_t *s)
{
    ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
                  "FTP proxy handler: client %V", &s->connection->addr_text);

    // TODO: 建立 upstream 连接并双向转发
    ngx_stream_finalize_session(s, NGX_OK);
    // return NGX_OK;
}
// 添加这个函数用于设置 server 的 handler
static char *
ngx_stream_ftp_proxy_merge_srv_conf(ngx_conf_t *cf, void *parent, void *child)
{
    // ngx_stream_ftp_proxy_srv_conf_t *conf = child;
    ngx_stream_core_srv_conf_t      *cscf;
    ngx_log_error(NGX_LOG_INFO, cf->log, 0,
                  "ngx_stream_ftp_proxy_merge_srv_conf");

    cscf = ngx_stream_conf_get_module_srv_conf(cf, ngx_stream_core_module);

    if (cscf->handler == NULL) {
        cscf->handler = ngx_stream_ftp_content_handler;
    }

    return NGX_CONF_OK;
}

// 模块指令
static ngx_command_t ngx_stream_ftp_proxy_commands[] = {
    {
        ngx_string("ftp_proxy_pass"),        
        NGX_STREAM_MAIN_CONF|NGX_STREAM_SRV_CONF|NGX_CONF_TAKE1,// NGX_STREAM_SRV_CONF|NGX_CONF_TAKE1,
        ngx_stream_ftp_proxy_pass,
        NGX_STREAM_SRV_CONF_OFFSET,
        0,
        NULL
    },

     
    ngx_null_command
};


// 模块上下文
static ngx_stream_module_t ngx_stream_ftp_proxy_module_ctx = {
    NULL,                              /* preconfiguration */
    ngx_stream_ftp_proxy_init,         /* postconfiguration */
    NULL,                              /* create main configuration */
    NULL,                              /* init main configuration */
    ngx_stream_ftp_proxy_create_srv_conf, /* create server configuration */
    ngx_stream_ftp_proxy_merge_srv_conf  /* merge server configuration */
};


// 模块定义
ngx_module_t ngx_stream_ftp_proxy_module = {
    NGX_MODULE_V1,
    &ngx_stream_ftp_proxy_module_ctx,  /* module context */
    ngx_stream_ftp_proxy_commands,     /* module directives */
    NGX_STREAM_MODULE,                 /* module type */
    NULL,                              /* init master */
    NULL,                              /* init module */
    NULL,                              /* init process */
    NULL,                              /* init thread */
    NULL,                              /* exit thread */
    NULL,                              /* exit process */
    NULL,                              /* exit master */
    NGX_MODULE_V1_PADDING
};
