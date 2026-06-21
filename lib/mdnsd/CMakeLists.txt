aux_source_directory(. dnssd_src)
set(DIR_SRCS ${dnssd_src})
include_directories(.)
add_library( dnssd 
        STATIC
        ${DIR_SRCS})



