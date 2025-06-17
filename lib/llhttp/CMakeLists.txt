aux_source_directory(. llhttp_src)
set(DIR_SRCS ${llhttp_src})
include_directories(.)
add_library( llhttp
        STATIC
        ${DIR_SRCS})
