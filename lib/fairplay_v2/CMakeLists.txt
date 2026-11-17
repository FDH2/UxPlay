aux_source_directory(. fairplay_v2_src)
set(DIR_SRCS ${fairplay_v2_src})
include_directories(.)
add_library( fairplay_v2
        STATIC
        ${DIR_SRCS})
