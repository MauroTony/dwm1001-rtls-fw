cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(rtls_unit_tests)

target_sources(app PRIVATE
  src/test_twr_math.c
  src/test_frames.c
  src/test_loc.c
  src/test_host_if.c
)
