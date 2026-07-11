#!/bin/sh

make clean
# $2: optional vendored hyprutils include tree that must shadow the (broken)
# system headers — see the hyprutils block in hyprtester/CMakeLists.txt.
make all LUA_INCLUDES="${1}" HYPRUTILS_SHADOW_INCLUDE="${2}"
