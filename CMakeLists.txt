cmake_minimum_required(VERSION 3.15)
project(gp32recomp LANGUAGES C CXX)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The runtime sources are embedded in the tool (see recomp/embed.cmake).
set(RT_FILES CMakeLists.txt gp32rt.h gp32_ops.h internal.h cpu.c hw.c bios.c smc.c main.c
             platform_null.c platform_win32.c)
set(RT_DEPS "")
foreach(f ${RT_FILES})
  list(APPEND RT_DEPS ${CMAKE_SOURCE_DIR}/runtime/${f})
endforeach()
set(RT_DATA ${CMAKE_BINARY_DIR}/runtime_data.cpp)
add_custom_command(
  OUTPUT ${RT_DATA}
  COMMAND ${CMAKE_COMMAND} "-DFILES=${RT_FILES}" -DDIR=${CMAKE_SOURCE_DIR}/runtime -DOUT=${RT_DATA}
          -P ${CMAKE_SOURCE_DIR}/recomp/embed.cmake
  DEPENDS ${RT_DEPS} ${CMAKE_SOURCE_DIR}/recomp/embed.cmake
  VERBATIM)

add_executable(gp32recomp recomp/main.cpp recomp/gp32recomp.cpp ${RT_DATA})
target_include_directories(gp32recomp PRIVATE recomp)
if(MSVC)
  target_compile_options(gp32recomp PRIVATE /W3 /D_CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(gp32recomp PRIVATE -Wall -Wextra -Wno-unused-function)
endif()
