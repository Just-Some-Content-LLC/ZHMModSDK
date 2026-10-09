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
        Src/RelayFrame.cpp
        Src/RelayFrame.h
        Src/RelayLog.cpp
        Src/RelayLog.h
        Src/RepositoryId.cpp
        Src/RepositoryId.h
        Src/SceneState.h
        Src/TcpRelaySink.cpp
        Src/TcpRelaySink.h
        Src/TelemetryNormalizer.cpp
        Src/TelemetryNormalizer.h
        Src/TelemetryObservation.h
        Src/TelemetryQueue.h
)

set(RELAY_GLACIER_FACING_SOURCES
        Src/GlacierRelay.cpp
        Src/GlacierRelay.h
        Src/SceneObservation.cpp
        Src/SceneObservation.h
        Src/TelemetryIntake.cpp
        Src/TelemetryIntake.h
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
        Tests/ContractLifecycleTests.cpp
        Tests/DisguiseTelemetryTests.cpp
        Tests/ItemTelemetryTests.cpp
        Tests/ObjectiveTelemetryTests.cpp
        Tests/MissionObserverTests.cpp
        Tests/RelayAdapterTests.cpp
        Tests/RelayFrameTests.cpp
        Tests/RepositoryIdTests.cpp
        Tests/TcpRelaySinkTests.cpp
        Tests/TelemetryNormalizerTests.cpp
        Tests/TelemetryQueueTests.cpp
        Tests/TestHarness.h
        Tests/TestJson.h
        Tests/Fixtures/B0ActorOutcomes.h
        Tests/Fixtures/B0ContractLifecycle.h
        Tests/Fixtures/B0Disguise.h
        Tests/Fixtures/B0DisguiseBytes.h
        Tests/Fixtures/B0ItemBytes.h
        Tests/Fixtures/B0Items.h
        Tests/Fixtures/B0Objectives.h
)

target_include_directories(GlacierRelayTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Src
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests
)

target_link_libraries(GlacierRelayTests PRIVATE
        fmt::fmt
        ws2_32
)

enable_testing()
add_test(NAME GlacierRelayTests COMMAND GlacierRelayTests)

# Standalone Windows client for the Windows -> WSL2 -> BEAM integration test (no game involved).
add_executable(GlacierRelayWireProbe
        ${RELAY_ENGINE_INDEPENDENT_SOURCES}
        Tests/WireProbe.cpp
)

target_include_directories(GlacierRelayWireProbe PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Src
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests
)

target_link_libraries(GlacierRelayWireProbe PRIVATE
        fmt::fmt
        ws2_32
)
