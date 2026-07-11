CXXFLAGS = -shared -fPIC -g -std=c++2b -Wno-c++11-narrowing
INCLUDES = `pkg-config --cflags pixman-1 libdrm pangocairo libinput libudev wayland-server xkbcommon`
LIBS = `pkg-config --libs pangocairo`
LUA_FLAGS = $(if $(strip $(LUA_INCLUDES)),-I$(LUA_INCLUDES),)
# Optional vendored hyprutils include tree shadowing broken system headers (must
# come first) — see the hyprutils block in hyprtester/CMakeLists.txt.
HYPRUTILS_SHADOW_FLAGS = $(if $(strip $(HYPRUTILS_SHADOW_INCLUDE)),-I$(HYPRUTILS_SHADOW_INCLUDE),)

SRC = src/main.cpp
TARGET = hyprtestplugin.so

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(HYPRUTILS_SHADOW_FLAGS) -I../.. -I../../src -I../../protocols $(INCLUDES) $(LUA_FLAGS) $^ -o $@ $(LIBS) -O2

clean:
	rm -f ./$(TARGET)

.PHONY: all clean
