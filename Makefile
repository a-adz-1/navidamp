# Builds the two Navidrome plugins against the installed Audacious.
#   make                  build
#   sudo make install     install into Audacious's plugin folder
#   sudo make uninstall   remove them again
# (Or use the PKGBUILD: makepkg -si)

CXX      ?= g++
CXXFLAGS ?= -O2 -g
PLUGIN_DIR := $(shell pkg-config --variable=plugin_dir audacious)
DEPS_CFLAGS := $(shell pkg-config --cflags audacious glib-2.0)
DEPS_LIBS   := $(shell pkg-config --libs audacious glib-2.0)
FLAGS = -std=c++17 -fPIC -shared -fvisibility=hidden -Wall \
        -DPACKAGE='"audacious-navidrome"' -DEXPORT='__attribute__((visibility("default")))'

all: navidrome.so navidrome-transport.so

navidrome.so: navidrome.cc
	$(CXX) $(CXXFLAGS) $(FLAGS) $(DEPS_CFLAGS) $< -o $@ $(LDFLAGS) $(DEPS_LIBS)

navidrome-transport.so: navidrome.cc
	$(CXX) $(CXXFLAGS) $(FLAGS) -DNAVIDROME_TRANSPORT $(DEPS_CFLAGS) $< -o $@ $(LDFLAGS) $(DEPS_LIBS)

install: all
	install -Dm755 navidrome.so "$(DESTDIR)$(PLUGIN_DIR)/General/navidrome.so"
	install -Dm755 navidrome-transport.so "$(DESTDIR)$(PLUGIN_DIR)/Transport/navidrome-transport.so"

uninstall:
	rm -f "$(DESTDIR)$(PLUGIN_DIR)/General/navidrome.so" \
	      "$(DESTDIR)$(PLUGIN_DIR)/Transport/navidrome-transport.so"

clean:
	rm -f navidrome.so navidrome-transport.so

.PHONY: all install uninstall clean
