CMAKE_MINIMUM_REQUIRED(VERSION 3.8)

set(WOLF_INCLUDE wolfmqtt)
set(WOLF_SRC src)

aux_source_directory(${WOLF_SRC} SRCS)
aux_source_directory(${WOLF_INCLUDE} INCLUDE_SRCS)

include_directories(${WOLF_INCLUDE})
include_directories(${WOLF_SRC})

add_compile_options(-Os)
add_compile_options(-Wall -Wmissing-prototypes -Wno-unused-but-set-variable -Wno-unused-variable -Wno-conversion -Wno-sign-conversion)

# Create library
add_library(wolfMQTT STATIC ${SRCS} ${INCLUDE_SRCS})
