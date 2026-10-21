zephyr_library()
zephyr_library_sources(twr_math.c)
zephyr_library_sources_ifdef(CONFIG_RTLS_TWR
  twr_common.c
  twr_initiator.c
  twr_responder.c
)
