cd  tengine-3.1.0
./configure \
  --prefix=/usr/local/tengine \
  --with-http_ssl_module \
  --with-http_v2_module \
  --with-http_gzip_static_module \
  --with-stream \
  --with-stream_ssl_module \
  --with-stream_realip_module \
  --with-stream_ssl_preread_module \
  --with-pcre

make -j8 && make install
cd ../