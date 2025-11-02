if ( APPLE )
  cmake_minimum_required( VERSION 3.13 )
else ()
  cmake_minimum_required( VERSION 3.10 )
endif ()

project( uxplay )

message( STATUS "Project name: " ${PROJECT_NAME} )

include(GNUInstallDirs)

set ( CMAKE_CXX_STANDARD 11 )

if (ZOOMFIX )
  message (STATUS "cmake option ZOOMFIX is no longer used (if needed, ZOOMFIX is automatically applied if X11 libraries are present)" )
endif()

if ( ( UNIX AND NOT APPLE ) OR USE_X11 )
  if ( NOT NO_X11_DEPS )
  find_package( X11  )
    if ( X11_FOUND )
      message (STATUS "Will compile using X11 Libraries  (use cmake option -DNO_X11_DEPS=ON if X11 dependence is not wanted)" )
      link_libraries( ${X11_LIBRARIES} )
      include_directories( ${X11_INCLUDE_DIR} )
    else ()
      message (STATUS "X11 libraries not found, will compile without X11 dependence" )
    endif ()
  else()
    message (STATUS "will compile without X11 dependence" )
  endif()
endif()

if( UNIX AND NOT APPLE )
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(DBUS dbus-1>=1.4.12)
  if (DBUS_FOUND )
      add_definitions(-DDBUS )
      include_directories(${DBUS_INCLUDE_DIRS})
  endif()
endif()

if( UNIX AND NOT APPLE )
  add_definitions( -DSUPPRESS_AVAHI_COMPAT_WARNING )
  # convert AirPlay colormap 1:3:7:1 to sRGB (1:1:7:1), needed on Linux and BSD
  add_definitions( -DFULL_RANGE_RGB_FIX )
else()
  set( CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE )
endif()

add_subdirectory( lib/llhttp )
add_subdirectory( lib/playfair )
add_subdirectory( lib )
add_subdirectory( renderers )

if  ( GST_MACOS )
     add_definitions( -DGST_MACOS )
     message ( STATUS "define GST_MACOS" )
endif()

add_executable( uxplay uxplay.cpp )
target_link_libraries( uxplay
                   renderers
                   airplay
		   )
if (DBUS_FOUND)
target_link_libraries( uxplay
		     ${DBUS_LIBRARIES}
		     )
endif()

install( TARGETS  uxplay RUNTIME DESTINATION bin )
install( FILES uxplay.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1 )
install( FILES README.md README.txt README.html LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR} ) 
install( FILES lib/llhttp/LICENSE-MIT DESTINATION ${CMAKE_INSTALL_DOCDIR}/llhttp )
install( FILES uxplay.service DESTINATION ${CMAKE_INSTALL_DOCDIR}/systemd ) 

if (DBUS_FOUND)
install( FILES Bluetooth_LE_beacon/dbus/uxplay-beacon.py
         DESTINATION bin
         PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ GROUP_EXECUTE GROUP_READ  WORLD_EXECUTE WORLD_READ)
install( FILES Bluetooth_LE_beacon/dbus/uxplay-beacon.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1 )
endif()

if (WIN32)
install( FILES Bluetooth_LE_beacon/winrt/uxplay-beacon.py
         DESTINATION bin
         PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ GROUP_EXECUTE GROUP_READ  WORLD_EXECUTE WORLD_READ)
install( FILES Bluetooth_LE_beacon/winrt/uxplay-beacon.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1 )
endif()



# uninstall target
if(NOT TARGET uninstall)
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake_uninstall.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
    IMMEDIATE @ONLY)

  add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake)
endif()
