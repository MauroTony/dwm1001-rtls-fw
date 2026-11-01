zephyr_library()
zephyr_library_sources(host_if_codec.c)
zephyr_library_sources_ifdef(CONFIG_RTLS_HOST_IF host_if.c)
