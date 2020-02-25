make -f Makefile.std make-common
make -f Makefile.std make-server

Building on Mac, install openssl and libwebsockets with brew:

brew install openssl libwebsockets

export PKG_CONFIG_PATH=/usr/local/opt/libwebsockets/lib/pkgconfig:/usr/local/opt/openssl@1.1/lib/pkgconfig


