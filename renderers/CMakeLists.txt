
if (APPLE )
  set( ENV{PKG_CONFIG_PATH} "/Library/FrameWorks/GStreamer.framework/Libraries/pkgconfig" ) # GStreamer.framework, preferred
  set( ENV{PKG_CONFIG_PATH} "$ENV{PKG_CONFIG_PATH}:/usr/local/lib/pkgconfig" ) # Brew or self-installed gstreamer
  set( ENV{PKG_CONFIG_PATH} "$ENV{PKG_CONFIG_PATH}:/opt/homebrew/lib/pkgconfig" ) # Brew, M1/M2 macs  
  set( ENV{PKG_CONFIG_PATH} "$ENV{PKG_CONFIG_PATH}:$ENV{HOMEBREW_PREFIX}/lib/pkgconfig" ) # Brew, using prefix
  set( ENV{PKG_CONFIG_PATH} "$ENV{PKG_CONFIG_PATH}:/opt/local/lib/pkgconfig/" ) # MacPorts
  message( "PKG_CONFIG_PATH (Apple, renderers) = " $ENV{PKG_CONFIG_PATH} )
  find_program( PKG_CONFIG_EXECUTABLE  pkg-config  PATHS /Library/FrameWorks/GStreamer.framework/Commands )
  set(PKG_CONFIG_EXECUTABLE ${PKG_CONFIG_EXECUTABLE} --define-prefix )
else()
  if ( DEFINED ENV{GSTREAMER_ROOT_DIR} )
     if ( EXISTS "$ENV{GSTREAMER_ROOT_DIR}/pkgconfig" )
       message ( STATUS "*** Using GSTREAMER_ROOT_DIR = " $ENV{GSTREAMER_ROOT_DIR} )
       set( ENV{PKG_CONFIG_PATH} "$ENV{GSTREAMER_ROOT_DIR}/pkgconfig:$ENV{PKG_CONFIG_PATH}" )
     endif()
  endif()
  set( ENV{PKG_CONFIG_PATH} "$ENV{PKG_CONFIG_PATH}:/usr/local/lib/pkgconfig" ) # standard location for self-installed gstreamer
endif()

find_package( PkgConfig REQUIRED )
if ( X11_FOUND )
  message (STATUS "Will use X_DISPLAY_FIX" )
  add_definitions( -DX_DISPLAY_FIX )
  pkg_check_modules (GST120 gstreamer-1.0>=1.20)
  if ( GST120_FOUND )
    message( "--   ZOOMFIX will NOT be applied as Gstreamer version is >= 1.20" )
  else()
    message( "--   Failure to find Gstreamer >= 1.20 is NOT an error!" )
    message( "--   ZOOMFIX will be applied as Gstreamer version is < 1.20" )
    add_definitions( -DZOOM_WINDOW_NAME_FIX )
  endif()
endif()

pkg_check_modules(GST REQUIRED    gstreamer-1.0>=1.4 
                                  gstreamer-sdp-1.0>=1.4
                                  gstreamer-video-1.0>=1.4
                                  gstreamer-app-1.0>=1.4
)

add_library( renderers
             STATIC
             audio_renderer.c
             video_renderer.c
             mux_renderer.c )

target_link_libraries ( renderers PUBLIC airplay )

# hacks to fix cmake confusion due to links in path with macOS FrameWorks

if( GST_INCLUDE_DIRS MATCHES "/Library/FrameWorks/GStreamer.framework/include" )
  set( GST_INCLUDE_DIRS "/Library/FrameWorks/GStreamer.framework/Headers")
  message( STATUS  "GST_INCLUDE_DIRS" ${GST_INCLUDE_DIRS} )
# fix to use -DGST_MACOS for "Official" GStreamer >= 1.22 packages
  pkg_check_modules ( GST122 gstreamer-1.0>=1.22 )
  if ( GST122_FOUND )
    set( GST_MACOS "1" CACHE STRING "define GST_MACOS in uxplay.cpp" )
  endif()
endif()

# set GST_MACOS for all Apple when GStreamer >= 1.24
if ( APPLE AND NOT GST_MACOS )
  pkg_check_modules ( GST124 gstreamer-1.0>=1.24 )
  if ( GST124_FOUND )
     set( GST_MACOS "1" CACHE STRING "define GST_MACOS in uxplay.cpp" )
   endif()
endif()

target_include_directories ( renderers PUBLIC ${GST_INCLUDE_DIRS} )

if( GST_LIBRARY_DIRS MATCHES "/Library/FrameWorks/GStreamer.framework/lib" )
  set( GST_LIBRARY_DIRS "/Library/FrameWorks/GStreamer.framework/Libraries")
  message( STATUS  "GST_LIBRARY_DIRS" ${GST_LIBRARY_DIRS} )
  target_link_libraries( renderers PUBLIC ${GST_LIBRARIES} )
  if( CMAKE_VERSION VERSION_LESS "3.13" )
    message( FATAL_ERROR "This macOS build needs cmake >= 3.13" )
  endif()
  target_link_directories ( renderers  PUBLIC ${GST_LIBRARY_DIRS} )  
elseif( CMAKE_VERSION VERSION_LESS "3.12" )
  target_link_libraries ( renderers  PUBLIC ${GST_LIBRARIES} )  
else()		  
  target_link_libraries( renderers PUBLIC ${GST_LINK_LIBRARIES} )
endif()



