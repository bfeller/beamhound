# Beamhound build.
#   make            -> device build (run on the Cardputer): build/device/beamhound
#   make desktop    -> headless preview build for a PC: build/desktop/beamhound
#   make tools      -> host tools (index builder, tests): build/host/
#   make install    -> install app + data into /usr/share/APPLaunch and /usr/share/beamhound
#
# Needs only make + gcc/g++. LVGL v9.5.0 is expected in deps/lvgl
# (git clone --depth 1 --branch v9.5.0 https://github.com/lvgl/lvgl.git deps/lvgl).

LVGL_DIR ?= deps/lvgl
JOBS ?= 2

CC  ?= gcc
CXX ?= g++
COMMON  := -O2 -g0 -Isrc -Isrc/config -I$(LVGL_DIR) -DLV_CONF_INCLUDE_SIMPLE -MMD -MP
CFLAGS  += $(COMMON)
CXXFLAGS += $(COMMON) -std=c++17 -Wall -Wextra

LVGL_SRCS := $(shell find $(LVGL_DIR)/src -name '*.c' 2>/dev/null)
CORE_SRCS := src/core/ir.cpp src/core/irfile.cpp src/core/index.cpp src/core/matcher.cpp
APP_SRCS  := $(CORE_SRCS) src/platform/hw.cpp src/app/app.cpp src/main.cpp

define objs
$(patsubst %.c,$(1)/%.o,$(filter %.c,$(2))) $(patsubst %.cpp,$(1)/%.o,$(filter %.cpp,$(2)))
endef

DEV_DIR := build/device
DSK_DIR := build/desktop
DEV_OBJS := $(call objs,$(DEV_DIR),$(LVGL_SRCS) $(APP_SRCS))
DSK_OBJS := $(call objs,$(DSK_DIR),$(LVGL_SRCS) $(APP_SRCS))

.PHONY: all device desktop cross deploy tools test install clean
all: device

# Cross-compile the device binary on a PC with Zig: targets glibc 2.36 (Cardputer has 2.41)
# and links Zig's libc++ statically, so the result runs regardless of the PC's newer libs.
ZIG ?= tools/zig
ZTARGET ?= aarch64-linux-gnu.2.36
ZFLAGS ?= -target $(ZTARGET) -mcpu=cortex_a53
cross:
	$(MAKE) device DEV_DIR=build/cross CC="$(ZIG) cc $(ZFLAGS)" CXX="$(ZIG) c++ $(ZFLAGS)" LDFLAGS=-s

# Push the cross-built binary + data to the device and register it with APPLaunch.
DEVICE ?= cardputer
deploy: cross
	ssh $(DEVICE) 'mkdir -p ~/beamhound-staging'
	tar czf - -C build/cross beamhound -C ../.. packaging/beamhound.desktop assets/images/beamhound_100.png -C build data \
	  | ssh $(DEVICE) 'tar xzf - -C ~/beamhound-staging && cd ~/beamhound-staging && \
	    sudo install -D -m 755 beamhound /usr/share/APPLaunch/bin/beamhound && \
	    sudo install -D -m 644 packaging/beamhound.desktop /usr/share/APPLaunch/applications/beamhound.desktop && \
	    sudo install -D -m 644 assets/images/beamhound_100.png /usr/share/APPLaunch/share/images/beamhound.png && \
	    sudo mkdir -p /usr/share/beamhound && sudo rm -rf /usr/share/beamhound/irdb && \
	    sudo cp -r data/index.bin data/irdb /usr/share/beamhound/ && rm -rf ~/beamhound-staging && echo deployed'
device: $(DEV_DIR)/beamhound
desktop: $(DSK_DIR)/beamhound

$(DEV_DIR)/beamhound: $(DEV_OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ -lm -lpthread
$(DSK_DIR)/beamhound: $(DSK_OBJS)
	$(CXX) -o $@ $^ -lm -lpthread

$(DEV_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(DEV_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(DSK_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DBH_DESKTOP -c $< -o $@
$(DSK_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -DBH_DESKTOP -c $< -o $@

tools: build/host/build_index build/host/test_core build/host/sim
build/host/build_index: $(CORE_SRCS) tools/build_index.cpp
	@mkdir -p $(dir $@)
	$(CXX) -std=c++17 -O2 -Isrc $^ -o $@
build/host/test_core: $(CORE_SRCS) tests/test_core.cpp
	@mkdir -p $(dir $@)
	$(CXX) -std=c++17 -O2 -Isrc $^ -o $@
build/host/sim: $(CORE_SRCS) tests/sim_matcher.cpp
	@mkdir -p $(dir $@)
	$(CXX) -std=c++17 -O2 -Isrc $^ -o $@

test: tools
	./build/host/test_core
	./build/host/sim build/data/index.bin 1000

PREFIX_APP  ?= /usr/share/APPLaunch
PREFIX_DATA ?= /usr/share/beamhound
install: $(DEV_DIR)/beamhound
	install -d $(PREFIX_APP)/bin $(PREFIX_APP)/applications $(PREFIX_APP)/share/images $(PREFIX_DATA)
	install -m 755 $(DEV_DIR)/beamhound $(PREFIX_APP)/bin/beamhound
	install -m 644 packaging/beamhound.desktop $(PREFIX_APP)/applications/beamhound.desktop
	install -m 644 assets/images/beamhound_100.png $(PREFIX_APP)/share/images/beamhound.png
	install -m 644 build/data/index.bin $(PREFIX_DATA)/index.bin
	cp -r build/data/irdb $(PREFIX_DATA)/

clean:
	rm -rf build/device build/desktop

-include $(DEV_OBJS:.o=.d) $(DSK_OBJS:.o=.d)
