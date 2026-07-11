cmake_minimum_required(VERSION 3.19)

project(hyprtester DESCRIPTION "Hyprland test suite")

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

find_package(PkgConfig REQUIRED)

pkg_check_modules(hyprtester_deps REQUIRED IMPORTED_TARGET hyprutils>=0.5.0)

file(GLOB_RECURSE SRCFILES CONFIGURE_DEPENDS "src/*.cpp")

add_executable(hyprtester ${SRCFILES})
add_custom_command(
  TARGET hyprtester
  POST_BUILD
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/plugin/build.sh "${LUA_INCLUDE_DIRS}"
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/plugin)

target_link_libraries(hyprtester PUBLIC PkgConfig::hyprtester_deps)

# OpenXR integration suite (docs/openxr/06-testing.md). Gated by compile
# definition, not file exclusion — hyprtester GLOBs all of src/*.cpp and every
# file under src/xr/ + src/tests/xr/ wraps its contents in #ifdef WITH_XR_TESTS.
if(WITH_XR_TESTS)
  target_compile_definitions(hyprtester PRIVATE WITH_XR_TESTS)
  # Repo root for discovering the vendored Monado build (subprojects/monado/build)
  # and the tracked/local test configs — see MonadoOrchestrator::resolveBinary().
  get_filename_component(HYPRTESTER_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
  target_compile_definitions(hyprtester PRIVATE HYPRTESTER_SOURCE_ROOT="${HYPRTESTER_REPO_ROOT}")
  message(STATUS "hyprtester: OpenXR integration suite enabled (WITH_XR_TESTS)")

  # Convenience wrapper around the one-time vendored-Monado build. Deliberately NOT part of
  # ALL / not a dependency of hyprtester: Monado is a multi-minute external build needed only
  # to RUN the XR suite, and its own CMake project can't be add_subdirectory'd into ours.
  # Invoke explicitly: cmake --build build-debug --target monado
  add_custom_target(monado
    COMMAND "${HYPRTESTER_REPO_ROOT}/scripts/build-monado.sh"
    USES_TERMINAL
    COMMENT "Building vendored Monado (subprojects/monado) via scripts/build-monado.sh")
endif()

install(TARGETS hyprtester)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/test.lua
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/hypr)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/lua-require
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/hypr)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/plugin/hyprtestplugin.so
        DESTINATION ${CMAKE_INSTALL_PREFIX}/lib)

file(WRITE ${CMAKE_CURRENT_SOURCE_DIR}/src/tests/clients/build.hpp
  "#include <string>\n"
  "static const std::string binaryDir = \"${CMAKE_CURRENT_BINARY_DIR}\";"
)

######## wayland protocols testing stuff

if(CMAKE_BUILD_TYPE MATCHES Debug OR CMAKE_BUILD_TYPE MATCHES DEBUG)
  set(CMAKE_EXECUTABLE_ENABLE_EXPORTS TRUE)
endif()

find_package(hyprwayland-scanner 0.4.0 REQUIRED)
pkg_check_modules(
  protocols_deps
  REQUIRED
  IMPORTED_TARGET
  hyprutils>=0.8.0
  wayland-client
)

# Keep in sync with the top-level CMakeLists.txt: use the system
# wayland-protocols when new enough, otherwise fall back to the pinned
# subproject (subprojects/wayland-protocols). Frozen distro mirrors may ship an
# older release than we require.
set(WAYLAND_SERVER_PROTOCOLS_MINIMUM_VERSION 1.49)
pkg_check_modules(wayland_protocols_dep QUIET wayland-protocols)
if(wayland_protocols_dep_FOUND
   AND NOT wayland_protocols_dep_VERSION VERSION_LESS
           ${WAYLAND_SERVER_PROTOCOLS_MINIMUM_VERSION})
  pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
  message(STATUS "Found wayland-protocols at ${WAYLAND_PROTOCOLS_DIR}")
elseif(EXISTS "${CMAKE_SOURCE_DIR}/subprojects/wayland-protocols/stable")
  set(WAYLAND_PROTOCOLS_DIR "${CMAKE_SOURCE_DIR}/subprojects/wayland-protocols")
  message(
    STATUS "system wayland-protocols too old, using vendored subproject")
  message(STATUS "Found wayland-protocols at ${WAYLAND_PROTOCOLS_DIR}")
else()
  message(
    FATAL_ERROR
      "wayland-protocols >= ${WAYLAND_SERVER_PROTOCOLS_MINIMUM_VERSION} not found and subprojects/wayland-protocols is missing; run: git submodule update --init subprojects/wayland-protocols"
  )
endif()
pkg_get_variable(WAYLAND_SCANNER_PKGDATA_DIR wayland-scanner pkgdatadir)
message(STATUS "Found wayland-scanner pkgdatadir at ${WAYLAND_SCANNER_PKGDATA_DIR}")

# gen core wayland stuff
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/protocols/wayland.cpp
         ${CMAKE_CURRENT_SOURCE_DIR}/protocols/wayland.hpp
  COMMAND hyprwayland-scanner --wayland-enums --client
          ${WAYLAND_SCANNER_PKGDATA_DIR}/wayland.xml ${CMAKE_CURRENT_SOURCE_DIR}/protocols/
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

function(protocolNew protoPath protoName external)
  if(external)
    set(path ${CMAKE_CURRENT_SOURCE_DIR}/${protoPath})
  else()
    set(path ${WAYLAND_PROTOCOLS_DIR}/${protoPath})
  endif()

  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/protocols/${protoName}.cpp
           ${CMAKE_CURRENT_SOURCE_DIR}/protocols/${protoName}.hpp
    COMMAND hyprwayland-scanner --client ${path}/${protoName}.xml
            ${CMAKE_CURRENT_SOURCE_DIR}/protocols/
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endfunction()
function(clientNew sourceName)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "PROTOS")

  add_executable(${sourceName} clients/${sourceName}.cpp)

  target_include_directories(${sourceName} BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/protocols")
  target_link_libraries(${sourceName} PUBLIC PkgConfig::protocols_deps)

  target_sources(${sourceName} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/protocols/wayland.cpp ${CMAKE_CURRENT_SOURCE_DIR}/protocols/wayland.hpp)

  foreach(protoName IN LISTS ARG_PROTOS)
    target_sources(${sourceName} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/protocols/${protoName}.cpp
                                         ${CMAKE_CURRENT_SOURCE_DIR}/protocols/${protoName}.hpp)
  endforeach()
endfunction()

protocolnew("staging/pointer-warp" "pointer-warp-v1" false)
protocolnew("staging/fractional-scale" "fractional-scale-v1" false)
protocolnew("stable/xdg-shell" "xdg-shell" false)
protocolnew("unstable/keyboard-shortcuts-inhibit" "keyboard-shortcuts-inhibit-unstable-v1" false)
# docs/openxr/06-testing.md §6 row 8 (WP12): idle-notify test client.
protocolnew("staging/ext-idle-notify" "ext-idle-notify-v1" false)

clientNew("pointer-warp" PROTOS "pointer-warp-v1" "xdg-shell")
clientNew("surface-scale-transform" PROTOS "fractional-scale-v1" "xdg-shell")
clientNew("pointer-scroll" PROTOS "xdg-shell")
clientNew("child-window" PROTOS "xdg-shell")
clientNew("xdg-interactive" PROTOS "xdg-shell")
clientNew("shortcut-inhibitor" PROTOS "xdg-shell" "keyboard-shortcuts-inhibit-unstable-v1")
clientNew("keyboard-modifiers" PROTOS "xdg-shell")
clientNew("idle-notify" PROTOS "ext-idle-notify-v1")
