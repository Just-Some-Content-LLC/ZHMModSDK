cmake_minimum_required(VERSION 3.12)

# Glacier Relay native adapter (glacier-relay docs/design/M1_FIRST_SEMANTIC_EVENT.md).
#
# Two groups of sources, kept apart on purpose:
# - Glacier-facing: includes SDK headers, reads engine state, produces plain values.
# - Engine-independent: the semantic layer, events, envelope, adapter and sinks. These never include
#   an SDK header. GlacierRelayTests builds them without the SDK on its include path, so the compiler
#   enforces the boundary.

set(RELAY_ENGINE_INDEPENDENT_SOURCES
        Src/IRelaySink.h
        Src/Json.cpp
        Src/Json.h
        Src/LogRelaySink.cpp
        Src/LogRelaySink.h
        Src/MissionObserver.cpp
        Src/MissionObserver.h
        Src/RelayAdapter.cpp
        Src/RelayAdapter.h
        Src/RelayEnvelope.cpp
        Src/RelayEnvelope.h
        Src/RelayEvent.h
        Src/RelayLog.cpp
        Src/RelayLog.h
        Src/SceneState.h
)

set(RELAY_GLACIER_FACING_SOURCES
        Src/GlacierRelay.cpp
        Src/GlacierRelay.h
        Src/SceneObservation.cpp
        Src/SceneObservation.h
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

# Unit tests for the engine-independent layers. Deliberately not linked against ZHMModSDK and without
# its include directory, so any SDK include in those sources fails here.
find_package(fmt CONFIG REQUIRED)

add_executable(GlacierRelayTests
        ${RELAY_ENGINE_INDEPENDENT_SOURCES}
        Tests/Main.cpp
        Tests/MissionObserverTests.cpp
        Tests/RelayAdapterTests.cpp
        Tests/TestHarness.h
)

target_include_directories(GlacierRelayTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Src
)

target_link_libraries(GlacierRelayTests PRIVATE
        fmt::fmt
)

enable_testing()
add_test(NAME GlacierRelayTests COMMAND GlacierRelayTests)
