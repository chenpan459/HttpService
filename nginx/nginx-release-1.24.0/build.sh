make clean
./auto/configure --prefix=/usr/local/nginx \
            --with-stream \
            --add-module=../nginx-module/nginx-upload-module \
            --add-module=../nginx-module/ngx-ftp-module \
            --with-http_stub_status_module \
            --with-http_ssl_module


make -j8 && make install
