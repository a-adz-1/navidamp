/* Build settings for the Winamp Classic Interface (Navidrome), matching how
 * Arch builds audacious-plugins. PACKAGE stays "audacious-plugins" so the
 * interface keeps its existing translations. */
#define PACKAGE "audacious-plugins"
#define VERSION "4.6.1"
#define PACKAGE_VERSION VERSION
#define COPYRIGHT "Copyright (C) 2001-2026 Audacious developers and others"
#define EXPORT __attribute__((visibility("default")))
#define PLUGIN_SUFFIX ".so"
#define ICONV_CONST
#define HAVE_GETTEXT 1
#define USE_QT 1
#define USE_GTK_OR_QT 1
#define HAVE_PLAYBACK_HISTORY 1
#define HAVE_PLAYLIST_MANAGER 1
#define HAVE_SEARCH_TOOL 1
