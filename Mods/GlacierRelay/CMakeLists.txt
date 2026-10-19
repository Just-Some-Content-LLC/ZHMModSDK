cmake_minimum_required(VERSION 3.12)

# Glacier Relay native adapter (glacier-relay docs/design/M1_FIRST_SEMANTIC_EVENT.md).
#
# Two groups of sources, kept apart on purpose:
# - Glacier-facing: includes SDK headers, reads engine state, produces plain values.
# - Engine-independent: the semantic layer, events, envelope, adapter and sinks. These never include
#   an SDK header. GlacierRelayTests builds them without the SDK on its include path, so the compiler
#   enforces the boundary.

set(RELAY_ENGINE_INDEPENDENT_SOURCES
        Src/Attestation.cpp
        Src/Attestation.h
        Src/CandidateObservation.cpp
        Src/CandidateObservation.h
        Src/CommandJson.cpp
        Src/CommandJson.h
        Src/CommandLink.cpp
        Src/CommandLink.h
        Src/CommandProtocol.cpp
        Src/CommandProtocol.h
        Src/CommandQueue.h
        Src/CommandScheduler.cpp
        Src/CommandScheduler.h
        Src/FloatWords.cpp
        Src/FloatWords.h
        Src/IOutfitApplier.h
        Src/ITransformApplier.h
        Src/IRelaySink.h
        Src/Json.cpp
        Src/Json.h
        Src/LifecycleTee.cpp
        Src/LifecycleTee.h
        Src/LogRelaySink.cpp
        Src/LogRelaySink.h
        Src/MissionObserver.cpp
        Src/MissionObserver.h
        Src/MotionLink.cpp
        Src/MotionLink.h
        Src/MotionProtocol.cpp
        Src/MotionProtocol.h
        Src/OutfitApply.cpp
        Src/OutfitApply.h
        Src/OutfitObservation.cpp
        Src/OutfitObservation.h
        Src/PlayerObservation.cpp
        Src/PlayerObservation.h
        Src/PreconditionEvaluator.cpp
        Src/PreconditionEvaluator.h
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
        Src/TransformApply.cpp
        Src/TransformApply.h
        Src/TransformObservation.cpp
        Src/TransformObservation.h
)

set(RELAY_GLACIER_FACING_SOURCES
        Src/GlacierRelay.cpp
        Src/GlacierRelay.h
        Src/OutfitApplier.cpp
        Src/OutfitApplier.h
        Src/SceneObservation.cpp
        Src/SceneObservation.h
        Src/TelemetryIntake.cpp
        Src/TelemetryIntake.h
        Src/TransformApplier.cpp
        Src/TransformApplier.h
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
        Tests/AttestationTests.cpp
        Tests/CommandLinkTests.cpp
        Tests/CommandProtocolTests.cpp
        Tests/CommandQueueTests.cpp
        Tests/CommandSchedulerTests.cpp
        Tests/OutfitApplyTests.cpp
        Tests/OutfitInspectionTests.cpp
        Tests/PlayerInspectionTests.cpp
        Tests/InspectionBoundaryTests.cpp
        Tests/CandidateInspectionTests.cpp
        Tests/PropertyGuardTests.cpp
        Tests/TransformObservationTests.cpp
        Tests/TransformApplyTests.cpp
        Tests/FakeTransformApplier.h
        Tests/TestListener.h
        Tests/ContractLifecycleTests.cpp
        Tests/DisguiseTelemetryTests.cpp
        Tests/ItemTelemetryTests.cpp
        Tests/ObjectiveTelemetryTests.cpp
        Tests/PlayerStateTelemetryTests.cpp
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
        Tests/Fixtures/B0PlayerState.h
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

# M5-B: standalone Windows command probe — the real command link, queue and scheduler with a fake
# frame source, no game, for the BEAM <-> native offline end-to-end gate.
add_executable(GlacierRelayCommandProbe
        ${RELAY_ENGINE_INDEPENDENT_SOURCES}
        Tests/CommandProbe.cpp
)

target_include_directories(GlacierRelayCommandProbe PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Src
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests
)

target_link_libraries(GlacierRelayCommandProbe PRIVATE
        fmt::fmt
        ws2_32
)
