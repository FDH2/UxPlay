aux_source_directory(. playfair_src)
set(DIR_SRCS ${playfair_src})
include_directories(.)
add_library( playfair
        STATIC
        ${DIR_SRCS})
