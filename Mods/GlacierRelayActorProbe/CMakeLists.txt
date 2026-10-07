cmake_minimum_required(VERSION 3.12)

# M2 Stage B0 read-only runtime probe (glacier-relay docs/research/ACTOR_OUTCOME_ARCHAEOLOGY.md,
# section 12, as tightened by the B0 authorization). Logs the game's own telemetry event stream raw,
# a census of activated actors with the few fields needed for correlation, and a filtered set of
# entity output pins. No wire, no events, no writes to engine state. Reuses the relay's durable log
# and scene observation sources by path; Mods/GlacierRelay itself is not modified.

set(PROBE_RELAY_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/../GlacierRelay/Src/RelayLog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../GlacierRelay/Src/RelayLog.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../GlacierRelay/Src/SceneObservation.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../GlacierRelay/Src/SceneObservation.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../GlacierRelay/Src/SceneState.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../GlacierRelay/Src/MissionObserver.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../GlacierRelay/Src/MissionObserver.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../GlacierRelay/Src/RelayEvent.h
)

add_library(GlacierRelayActorProbe SHARED
        Src/ActorProbe.cpp
        Src/ActorProbe.h
        ${PROBE_RELAY_SOURCES}
)

target_include_directories(GlacierRelayActorProbe PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Src
        ${CMAKE_CURRENT_SOURCE_DIR}/../GlacierRelay/Src
)

target_link_libraries(GlacierRelayActorProbe PRIVATE
        ZHMModSDK
)
