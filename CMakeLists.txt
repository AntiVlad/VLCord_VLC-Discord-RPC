cmake_minimum_required(VERSION 3.15)
project(vlc_discord_rpc C)

set(CMAKE_C_STANDARD 99)

# Paths
set(VLC_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(VLC_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib")

include_directories(
    "${VLC_SRC_DIR}"
    "${VLC_SRC_DIR}/vlc/plugins"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

link_directories("${VLC_LIB_DIR}")

add_library(discord_rpc_plugin MODULE
    src/plugin.c
    src/discord_rpc.c
    src/artwork.c
    src/presence.c
    src/json.c
)

set_target_properties(discord_rpc_plugin PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "discord_rpc_plugin"
    SUFFIX ".dll"
)

target_compile_definitions(discord_rpc_plugin PRIVATE
    __PLUGIN__
    _WIN32_WINNT=0x0601
)

target_link_libraries(discord_rpc_plugin PRIVATE
    vlccore
    winhttp
    ws2_32
    -static-libgcc
)
