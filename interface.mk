# interface.mk - builds the Winamp Classic Interface (Navidrome) from the audacious-plugins
# source, after navidrome-interface.patch has been applied to it.
#   make -f interface.mk AP_SRC=/path/to/audacious-plugins
#   sudo make -f interface.mk install
# (The PKGBUILD does all of this for you.)

AP_SRC   ?= audacious-plugins
CONFIG_H ?= $(CURDIR)/interface-config.h
SRC      := $(AP_SRC)/src/skins-qt
CXX      ?= g++
CXXFLAGS ?= -O2 -g
PLUGIN_DIR := $(shell pkg-config --variable=plugin_dir audacious)
# Qt 6: from pkg-config if available, otherwise ask qmake6 where it lives
QT_CFLAGS := $(shell pkg-config --cflags Qt6Core Qt6Gui Qt6Widgets 2>/dev/null)
QT_LIBS   := $(shell pkg-config --libs Qt6Core Qt6Gui Qt6Widgets 2>/dev/null)
ifeq ($(strip $(QT_CFLAGS)),)
QT_INC    := $(shell qmake6 -query QT_INSTALL_HEADERS 2>/dev/null)
QT_CFLAGS := -I$(QT_INC) -I$(QT_INC)/QtCore -I$(QT_INC)/QtGui -I$(QT_INC)/QtWidgets
QT_LIBS   := -lQt6Widgets -lQt6Gui -lQt6Core
endif
DEPS_CFLAGS := $(shell pkg-config --cflags audacious glib-2.0) $(QT_CFLAGS)
DEPS_LIBS   := $(shell pkg-config --libs audacious glib-2.0) $(QT_LIBS) -laudqt
FLAGS = -std=gnu++17 -fPIC -fvisibility=hidden -fvisibility-inlines-hidden -Wall \
        -include $(CONFIG_H) -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_CORE_LIB -DQT_NO_DEBUG

SOURCES := actions.cc button.cc dialogs-qt.cc dock.cc drag-handle.cc eq-graph.cc \
           eq-slider.cc equalizer.cc hslider.cc main.cc menurow.cc menu-ops.cc menus.cc \
           monostereo.cc navidrome-win.cc number.cc playlistwin.cc playlist-slider.cc \
           playlist-widget.cc playstatus.cc plugin.cc plugin-window.cc search-select.cc \
           skin.cc skin-ini.cc skins_cfg.cc skins_util.cc skinselector.cc svis.cc \
           textbox.cc view.cc vis-callbacks.cc vis.cc widget.cc window.cc
OBJECTS := $(addprefix build/,$(SOURCES:.cc=.o))

all: skins-navidrome.so

build/%.o: $(SRC)/%.cc $(CONFIG_H)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(FLAGS) $(DEPS_CFLAGS) -I$(SRC) -c $< -o $@

skins-navidrome.so: $(OBJECTS)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(LDFLAGS) $(DEPS_LIBS)

install: all
	install -Dm755 skins-navidrome.so "$(DESTDIR)$(PLUGIN_DIR)/General/skins-navidrome.so"

uninstall:
	rm -f "$(DESTDIR)$(PLUGIN_DIR)/General/skins-navidrome.so"

clean:
	rm -rf build skins-navidrome.so

.PHONY: all install uninstall clean
