# UxPlay unit tests.
#
# No external test framework is used (no gtest/Catch2): each test is a small,
# dependency-light executable that asserts and returns 0 on success, non-zero
# on failure, registered with CTest via add_test().

add_executable( test_audio_advertise_flag
                 test_audio_advertise_flag.cpp
                 ${CMAKE_SOURCE_DIR}/audio_advertise.cpp )
target_include_directories( test_audio_advertise_flag PRIVATE ${CMAKE_SOURCE_DIR} )
add_test( NAME audio_advertise_flag COMMAND test_audio_advertise_flag )

# Compiles lib/dnssd.c directly (no GStreamer/OpenSSL/network backend needed:
# dnssd_init()/dnssd_private_init() are never called by these tests).
add_executable( test_dnssd_features
                 test_dnssd_features.c
                 ${CMAKE_SOURCE_DIR}/lib/dnssd.c )
target_include_directories( test_dnssd_features PRIVATE ${CMAKE_SOURCE_DIR}/lib )
add_test( NAME dnssd_features COMMAND test_dnssd_features )

# Black-box check that "-h" documents the new flag. "-h" exits 0 via printf+exit(0)
# before any GStreamer/dnssd initialization, so this is safe/fast on all platforms.
if ( TARGET uxplay )
  add_test( NAME help_text COMMAND uxplay -h )
  set_tests_properties( help_text PROPERTIES PASS_REGULAR_EXPRESSION "no-audio-advertise" )
endif()
