include_directories( playfair llhttp )

message( STATUS "*** CFLAGS \"" ${CMAKE_C_FLAGS} "\" from build environment will be postpended to CMAKE_CFLAGS" )

# Common x86/x86_64 cflags
if( NOT NO_MARCH_NATIVE AND CMAKE_SYSTEM_PROCESSOR MATCHES "(x86)|(X86)|(amd64)|(AMD64)" )
  set( CMAKE_C_FLAGS "-Ofast -march=native ${CMAKE_C_FLAGS}" )
  message( STATUS "Using CFLAGS with  -march=native" )
  message( STATUS "*** ONLY USE THIS WHEN COMPILING ON THE MACHINE THAT WILL RUN UXPLAY" )
  message( STATUS "  run \"cmake -DNO_MARCH_NATIVE=ON\" to switch off this compiler option" )
else()
  message( STATUS "Not using -march=native" )
  set( CMAKE_C_FLAGS "-O2 ${CMAKE_C_FLAGS}" )
endif()

# Common Linux cflags
if ( UNIX  AND NOT APPLE )
  set( CMAKE_C_FLAGS "-DSTANDALONE -D__STDC_CONSTANT_MACROS -D__STDC_LIMIT_MACROS -DTARGET_POSIX -D_LINUX -fPIC -DPIC -D_REENTRANT -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64  -Wall ${CMAKE_C_FLAGS}" )
endif()

if ( WIN32 )
  message( STATUS "Building for Windows " )
  set( CMAKE_C_FLAGS "-DSTANDALONE -D__STDC_CONSTANT_MACROS -D__STDC_LIMIT_MACROS -DTARGET_POSIX -D_WIN32 -fPIC -DPIC -D_REENTRANT -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -Wall ${CMAKE_C_FLAGS}" )
endif()

message( STATUS "using CMAKE_CFLAGS: " ${CMAKE_C_FLAGS} )

#activate the NOHOLD feature to drop existing connections if a third connection is made 
add_definitions( -DNOHOLD )

INCLUDE (CheckIncludeFiles)
if( WIN32 )
CHECK_INCLUDE_FILES ("winsock2.h" WINSOCK2 )
else()
# for BSD Unix (e.g. FreeBSD)
CHECK_INCLUDE_FILES ("sys/endian.h" BSD )
if ( BSD )
  add_definitions( -DSYS_ENDIAN_H )
endif ( BSD )
endif()

if( APPLE )
  set( ENV{PKG_CONFIG_PATH} "/usr/local/lib/pkgconfig" ) # standard location, and Brew
  set( ENV{PKG_CONFIG_PATH} "$ENV{PKG_CONFIG_PATH}:/opt/homebrew/lib/pkgconfig" ) # Brew for M1 macs
  set( ENV{PKG_CONFIG_PATH} "$ENV{PKG_CONFIG_PATH}:$ENV{HOMEBREW_PREFIX}/lib/pkgconfig" ) # Brew using prefix
  set( ENV{PKG_CONFIG_PATH} "$ENV{PKG_CONFIG_PATH}:/opt/local/lib/pkgconfig/" ) # MacPorts
  set( ENV{PKG_CONFIG_PATH} "$ENV{PKG_CONFIG_PATH}:/usr/local/opt/openssl@3/lib/pkgconfig" ) # Brew openssl
  set( ENV{PKG_CONFIG_PATH} "$ENV{PKG_CONFIG_PATH}:/opt/homebrew/opt/openssl@3/lib/pkgconfig" ) # Brew M1 openssl
  set( ENV{PKG_CONFIG_PATH} "$ENV{PKG_CONFIG_PATH}:$ENV{HOMEBREW_PREFIX}/opt/openssl@3/lib/pkgconfig" ) # Brew using prefix  openssl
  message( "PKG_CONFIG_PATH (Apple, lib) = " $ENV{PKG_CONFIG_PATH} )
  find_program( PKG_CONFIG_EXECUTABLE  pkg-config  PATHS /Library/FrameWorks/GStreamer.framework/Commands )
  message( "PKG_CONFIG_EXECUTABLE " ${PKG_CONFIG_EXECUTABLE} )
endif()
find_package(PkgConfig REQUIRED)

aux_source_directory(. play_src)
set(DIR_SRCS ${play_src})

add_library( airplay  STATIC
             ${DIR_SRCS}
           )

if ( APPLE )
  target_link_libraries( airplay 
                         pthread
                         playfair
                         llhttp )
elseif( WIN32 )
  target_link_libraries( airplay 
                         pthread
                         playfair
                         llhttp
                         wsock32
			 iphlpapi
                         ws2_32 )
else()
  target_link_libraries( airplay PUBLIC
          pthread
          playfair
          llhttp )
endif()

# libplist

if( APPLE )
   # use static linking
   pkg_search_module(PLIST REQUIRED libplist-2.0)
   find_library( LIBPLIST libplist-2.0.a REQUIRED )
   message( STATUS "(Static linking) LIBPLIST " ${LIBPLIST} )
   target_link_libraries ( airplay  ${LIBPLIST} )
elseif( WIN32)
pkg_search_module(PLIST REQUIRED libplist-2.0)
  find_library( LIBPLIST ${PLIST_LIBRARIES} PATH ${PLIST_LIBDIR} )
  target_link_libraries ( airplay ${LIBPLIST} )
else ()
  pkg_search_module(PLIST libplist>=2.0)
  if(NOT PLIST_FOUND)
    pkg_search_module(PLIST REQUIRED libplist-2.0)
  endif()
  find_library( LIBPLIST ${PLIST_LIBRARIES} PATH ${PLIST_LIBDIR} )
  target_link_libraries ( airplay PUBLIC ${LIBPLIST} )
endif()
if ( PLIST_FOUND )
   message( STATUS "found libplist-${PLIST_VERSION}")
   pkg_check_modules ( PLIST_21 libplist-2.0>=2.1.0 )
   if ( PLIST_21_FOUND )
     add_definitions( -DPLIST_210 )
   endif()
   pkg_check_modules ( PLIST_23 libplist-2.0>=2.3.0 )
   if ( PLIST_23_FOUND )
     add_definitions( -DPLIST_230 )
   endif()
endif()
target_include_directories( airplay PRIVATE ${PLIST_INCLUDE_DIRS} )

#libcrypto
if( APPLE )
  # use static linking
  # can either compile Openssl 1.1.1 or 3.0.0  from source (install_dev  to /usr/local) or use Macports or Brew
  # MacPorts needs zlib with it. Brew has a "keg-only" installation in /usr/local/opt/openssl@3
  # Brew on M1 macs puts this in  /opt/homebrew/opt/openssl@3
  pkg_check_modules( OPENSSL REQUIRED openssl>=1.1.1)
  message( "OPENSSL_LIBRARY_DIRS " ${OPENSSL_LIBRARY_DIRS} )
  message( "OPENSSL_INCLUDE_DIRS " ${OPENSSL_INCLUDE_DIRS} )
  find_library( LIBCRYPTO libcrypto.a PATHS ${OPENSSL_LIBRARY_DIRS} REQUIRED )
  message( "(Static linking) LIBCRYPTO "  ${LIBCRYPTO}  )
  target_link_libraries( airplay ${LIBCRYPTO} )
  if( LIBCRYPTO MATCHES "/opt/local/lib/libcrypto.a" ) #MacPorts openssl
    find_library( LIBZ libz.a)  # needed by MacPorts openssl
    message("(MacPorts) LIBZ= " ${LIBZ} )
    target_link_libraries( airplay  ${LIBZ} )
  endif()
  target_include_directories( airplay PRIVATE ${OPENSSL_INCLUDE_DIRS} )
elseif( WIN32 )
  find_package(OpenSSL 1.1.1 REQUIRED)
  target_compile_definitions( airplay PUBLIC OPENSSL_API_COMPAT=0x10101000L )
  target_link_libraries( airplay  OpenSSL::Crypto )
else()
  find_package(OpenSSL 1.1.1 REQUIRED)
  target_compile_definitions( airplay PUBLIC OPENSSL_API_COMPAT=0x10101000L )
  target_link_libraries( airplay PUBLIC OpenSSL::Crypto )
endif()

#dns_sd 
if ( NOT APPLE )
  pkg_search_module(AVAHI_DNSSD avahi-compat-libdns_sd)
  if (AVAHI_DNSSD_FOUND)
    target_include_directories( airplay PRIVATE ${AVAHI_DNSSD_INCLUDE_DIRS} )
    find_library( DNSSD ${AVAHI_DNSSD_LIBRARIES} PATH ${AVAHI_DNSSD_LIBDIR})
    target_link_libraries(airplay PUBLIC ${DNSSD} )
  else()  # can also build if mDNSResponder or another implementation of dns_sd is present instead of Avahi
    if ( WIN32 )
      if (DEFINED ENV{BONJOUR_SDK_HOME}) 
        set(BONJOUR_SDK "$ENV{BONJOUR_SDK_HOME}" )
      else()
        set(BONJOUR_SDK "C:\\Program Files\\Bonjour SDK")
      endif()
      message( STATUS "BONJOUR_SDK_HOME " ${BONJOUR_SDK} )
      set(DNSSD "${BONJOUR_SDK}/Lib/x64/dnssd.lib")
      target_link_libraries(airplay ${DNSSD} )
      message( STATUS "dns_sd: using " ${DNSSD} )
      find_path(DNSSD_INCLUDE_DIR dns_sd.h HINTS ${BONJOUR_SDK}/Include )
    else()
      find_library( DNSSD dns_sd )
      if( NOT DNSSD )
        message( FATAL_ERROR "libdns_sd missing; can be provided by avahi_compat-libdns_sd or mDNSResponder." )
      else()
        message( STATUS "dns_sd:  found" ${DNSSD} )
      endif()
      target_link_libraries(airplay PUBLIC ${DNSSD} ) 
      find_path(DNSSD_INCLUDE_DIR dns_sd.h HINTS ${CMAKE_INSTALL_INCLUDEDIR} )
    endif()
    if ( NOT DNSSD_INCLUDE_DIR )
       message( FATAL_ERROR " dns_sd.h not found ")
     else()
        message( STATUS "found dns_sd.h in " ${DNSSD_INCLUDE_DIR} )
     endif()
    target_include_directories( airplay PRIVATE ${DNSSD_INCLUDE_DIR} )
  endif()
endif()	
