meson setup build \
    --buildtype=release \
    -DPISTACHE_USE_SSL=false \
    -DPISTACHE_BUILD_EXAMPLES=false \
    -DPISTACHE_BUILD_TESTS=false \
    -DPISTACHE_BUILD_DOCS=false


cd build && sudo ninja uninstall  && sudo ninja install
