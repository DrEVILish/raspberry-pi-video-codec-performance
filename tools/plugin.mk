# Shared build rules for the GStreamer plugins in this repository.
#
# Each plugin folder has a three-line Makefile that sets PLUGIN, SRCS, PKGS and ELEMENT and includes this file.
#   make                  build the plugin (.so) in this folder
#   make check            load it with gst-inspect-1.0 and show the element
#   make install          copy it to ~/.local/share/gstreamer-1.0/plugins (your user only, no sudo)
#   sudo make install-system
#                         copy it to the system GStreamer plugin directory (every user, every service)
#   make uninstall / make uninstall-system / make clean
# Needs: a C compiler, pkg-config and the GStreamer development headers
#   (Debian/Raspberry Pi OS: sudo apt install build-essential pkg-config libgstreamer1.0-dev \
#    libgstreamer-plugins-base1.0-dev)
CFLAGS ?= -O2 -g
override CFLAGS += -fPIC -Wall -Wno-unused-function -DSTANDALONE_PLUGIN $(DEFS) $(shell pkg-config --cflags $(PKGS))
LDLIBS += $(shell pkg-config --libs $(PKGS)) -ldl -lpthread -lm
USER_PLUGINDIR ?= $(HOME)/.local/share/gstreamer-1.0/plugins
SYSTEM_PLUGINDIR ?= $(shell pkg-config --variable=pluginsdir gstreamer-1.0)

all: $(PLUGIN)

$(PLUGIN): $(SRCS)
	$(CC) $(CFLAGS) -shared -o $@ $(SRCS) $(LDLIBS)

check: $(PLUGIN)
	GST_PLUGIN_PATH=$(CURDIR) gst-inspect-1.0 $(ELEMENT) | head -n 30

install: $(PLUGIN)
	install -D -m 644 $(PLUGIN) $(DESTDIR)$(USER_PLUGINDIR)/$(PLUGIN)

install-system: $(PLUGIN)
	install -D -m 644 $(PLUGIN) $(DESTDIR)$(SYSTEM_PLUGINDIR)/$(PLUGIN)

uninstall:
	rm -f $(DESTDIR)$(USER_PLUGINDIR)/$(PLUGIN)

uninstall-system:
	rm -f $(DESTDIR)$(SYSTEM_PLUGINDIR)/$(PLUGIN)

clean:
	rm -f $(PLUGIN)

.PHONY: all check install install-system uninstall uninstall-system clean
