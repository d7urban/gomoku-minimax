cmake_minimum_required(VERSION 3.20)

project(gomoku_minimax LANGUAGES CXX)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the build type." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(gomoku_engine
    src/engine/AnalystAI.cpp
    src/engine/ClubAI.cpp
    src/engine/ExpertAI.cpp
    src/engine/GameState.cpp
    src/engine/Match.cpp
    src/engine/OpeningBook.cpp
    src/engine/PatternAnalysis.cpp
    src/engine/ProofSearch.cpp
    src/engine/Replay.cpp
    src/engine/RookieAI.cpp
    src/engine/Rules.cpp
    src/engine/Search.cpp
    src/engine/TacticalAI.cpp
    src/engine/ThreatSearch.cpp
    src/engine/Threats.cpp
    src/engine/TimeGovernor.cpp
    src/engine/Types.cpp
)

target_include_directories(gomoku_engine PUBLIC include)
target_compile_features(gomoku_engine PUBLIC cxx_std_20)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(gomoku_engine PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(gomoku_cli src/cli/main.cpp)
target_link_libraries(gomoku_cli PRIVATE gomoku_engine)

add_executable(gomoku_selfplay src/tools/selfplay.cpp)
target_link_libraries(gomoku_selfplay PRIVATE gomoku_engine)

add_executable(gomoku_bench src/tools/benchmark.cpp)
target_link_libraries(gomoku_bench PRIVATE gomoku_engine)

add_executable(gomoku_book src/tools/opening_book.cpp)
target_link_libraries(gomoku_book PRIVATE gomoku_engine)

add_executable(gomoku_proof_bench src/tools/proof_benchmark.cpp)
target_link_libraries(gomoku_proof_bench PRIVATE gomoku_engine)

add_executable(gomoku_gomocup src/tools/gomoku_gomocup.cpp)
target_link_libraries(gomoku_gomocup PRIVATE gomoku_engine)

add_executable(gomoku_tournament src/tools/tournament.cpp)
target_link_libraries(gomoku_tournament PRIVATE gomoku_engine)

option(GOMOKU_BUILD_SFML_UI "Build the SFML graphical UI" ON)

if(GOMOKU_BUILD_SFML_UI)
    find_package(SFML 2.5 COMPONENTS graphics window system CONFIG QUIET)

    if(TARGET SFML::Graphics AND TARGET SFML::Window AND TARGET SFML::System)
        add_executable(gomoku_ui src/ui/main.cpp)
        target_link_libraries(gomoku_ui PRIVATE gomoku_engine SFML::Graphics SFML::Window SFML::System)
    elseif(TARGET sfml-graphics AND TARGET sfml-window AND TARGET sfml-system)
        add_executable(gomoku_ui src/ui/main.cpp)
        target_link_libraries(gomoku_ui PRIVATE gomoku_engine sfml-graphics sfml-window sfml-system)
    else()
        message(WARNING "SFML was not found. The graphical gomoku_ui target is disabled. Install libsfml-dev to build Checkpoint 1 UI locally.")
    endif()
endif()

include(CTest)

if(BUILD_TESTING)
    add_executable(gomoku_smoke tests/smoke.cpp)
    target_link_libraries(gomoku_smoke PRIVATE gomoku_engine)
    add_test(NAME gomoku_smoke COMMAND gomoku_smoke)

    add_executable(gomoku_tactical_regressions tests/tactical_regressions.cpp)
    target_link_libraries(gomoku_tactical_regressions PRIVATE gomoku_engine)
    add_test(NAME gomoku_tactical_regressions COMMAND gomoku_tactical_regressions)

    add_executable(gomoku_search_enhancements tests/search_enhancements.cpp)
    target_link_libraries(gomoku_search_enhancements PRIVATE gomoku_engine)
    add_test(NAME gomoku_search_enhancements COMMAND gomoku_search_enhancements)

    add_executable(gomoku_seeded_regressions tests/seeded_regressions.cpp)
    target_link_libraries(gomoku_seeded_regressions PRIVATE gomoku_engine)
    add_test(NAME gomoku_seeded_regressions COMMAND gomoku_seeded_regressions)

    add_executable(gomoku_time_governor tests/time_governor.cpp)
    target_link_libraries(gomoku_time_governor PRIVATE gomoku_engine)
    add_test(NAME gomoku_time_governor COMMAND gomoku_time_governor)
endif()
