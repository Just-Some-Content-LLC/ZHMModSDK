cmake_minimum_required(VERSION 3.12)

# Glacier Relay native adapter (glacier-relay docs/design/M1_FIRST_SEMANTIC_EVENT.md).
#
# Two groups of sources, kept apart on purpose:
# - Glacier-facing: includes SDK headers, reads engine state, produces plain values.
# - Engine-independent: the semantic layer, events, envelope, adapter and sinks. These never include
#   an SDK header. GlacierRelayTests builds them without the SDK on its include path, so the compiler
#   enforces the boundary.

set(RELAY_ENGINE_INDEPENDENT_SOURCES
        Src/RelayLog.cpp
        Src/RelayLog.h
)

set(RELAY_GLACIER_FACING_SOURCES
        Src/GlacierRelay.cpp
        Src/GlacierRelay.h
)

add_library(GlacierRelay SHARED
        ${RELAY_ENGINE_INDEPENDENT_SOURCES}
        ${RELAY_GLACIER_FACING_SOURCES}
)

target_include_directories(GlacierRelay PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Src
)

target_link_libraries(GlacierRelay PRIVATE
        ZHMModSDK
)
