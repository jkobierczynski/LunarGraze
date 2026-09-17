cmake_minimum_required(VERSION 3.16)
project(LunarGraze CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---- core astronomy library, shared by the CLI test harness and the GUI ----
# frames.{hpp,cpp}, time_utils.{hpp,cpp}, star.hpp and vec3.hpp are bundled,
# unmodified copies of the same files from the main moon-graze C++ engine
# (../../include and ../../src) -- this directory is self-contained and
# builds standalone wherever it's extracted, independent of the rest of the
# moon-graze repo.
add_library(graze_core STATIC
    src/areaephem.cpp
    src/starcatalog.cpp
    src/grazecore.cpp
    src/frames.cpp
    src/time_utils.cpp
)
target_include_directories(graze_core PUBLIC src)
if(MSVC)
    target_compile_options(graze_core PRIVATE /W4)
else()
    target_compile_options(graze_core PRIVATE -Wall -Wextra -O2)
endif()

# ---- headless CLI test harness (no Qt) -- used to verify graze_core
#      against tools/graze_finder/graze_finder.py's output before/without
#      needing the GUI at all. ----
add_executable(graze_core_test src/main_cli.cpp)
target_link_libraries(graze_core_test PRIVATE graze_core)

# ---- Qt GUI (optional: only built if Qt6 Widgets+Network are found) ----
find_package(Qt6 QUIET COMPONENTS Widgets Network)
if(Qt6_FOUND)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    set(CMAKE_AUTOUIC ON)

    add_executable(LunarGraze
        src/main.cpp
        src/mainwindow.cpp
        src/mapview.cpp
    )
    target_link_libraries(LunarGraze PRIVATE graze_core Qt6::Widgets Qt6::Network)
    if(NOT MSVC)
        target_compile_options(LunarGraze PRIVATE -Wall -Wextra -O2)
    endif()
    if(WIN32)
        # Windows GUI subsystem -- without this, launching LunarGraze.exe
        # also pops up a blank console window behind the actual window.
        set_target_properties(LunarGraze PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()
else()
    message(STATUS "Qt6 (Widgets+Network) not found -- LunarGraze target skipped; "
                    "graze_core_test still builds. Install e.g. 'qt6-base-dev' "
                    "(Debian/Ubuntu) or 'qt6-qtbase-devel' (Fedora) and re-run cmake.")
endif()
