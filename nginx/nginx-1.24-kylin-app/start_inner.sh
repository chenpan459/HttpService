#!/bin/bash

# 停止所有正在运行的 Nginx 进程
pkill -9 nginx

# 启动 Nginx 并使用指定的配置文件
/usr/local/nginx/sbin/nginx -c /usr/local/nginx/conf/nginx_inner.conf

# 输出启动状态
if [ $? -eq 0 ]; then
    echo "Inner Nginx started successfully!"
else
    echo "Failed to start Inner Nginx"
fi

