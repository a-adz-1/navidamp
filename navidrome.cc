/*
 * Navidrome for Audacious
 *
 * Plays a whole Navidrome (Subsonic API) library inside Audacious, on shuffle.
 * Built twice from this one file:
 *
 *   navidrome.so            General plugin: settings page + menu items that load,
 *                           sort and refresh the "Navidrome" playlist.
 *   navidrome-transport.so  Transport plugin: plays navidrome://raw/ID.flac style
 *                           addresses by streaming from the server, with retries.
 *                           (built with -DNAVIDROME_TRANSPORT)
 *
 * Track addresses carry a file extension, so Audacious picks the right decoder
 * straight away instead of guessing from the stream - that was the cause of the
 * "file format could not be determined" errors with plain stream URLs.
 *
 * Copyright (C) 2026 a-adz-1
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef NAVIDROME_TRANSPORT
// before the libaudcore headers, whose translation macros clash with Qt's
#include <QDesktopServices>
#include <QMessageBox>
#include <QUrl>
#include <libaudqt/libaudqt.h>
#endif

#include <glib.h>

#include <libaudcore/audstrings.h>
#include <libaudcore/hook.h>
#include <libaudcore/i18n.h>
#include <libaudcore/interface.h>
#include <libaudcore/mainloop.h>
#include <libaudcore/playlist.h>
#include <libaudcore/plugin.h>
#include <libaudcore/plugins.h>
#include <libaudcore/preferences.h>
#include <libaudcore/runtime.h>
#include <libaudcore/tuple.h>
#include <libaudcore/vfs.h>

#ifndef EXPORT
#define EXPORT __attribute__((visibility("default")))
#endif

/* ------------------------------------------------------------------ */
/* Shared: settings and Subsonic API addresses                         */
/* ------------------------------------------------------------------ */

static const char * const CFG = "navidrome";
static const char * const cfg_defaults[] = {
    "url", "", "user", "", "password", "", "refresh_on_start", "FALSE",
    "order", "folder", "source", "", "source_name", "", nullptr};

static std::string cfg(const char * name)
{
    return std::string((const char *)aud_get_str(CFG, name));
}

static std::string url_encode(const std::string & s)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s)
    {
        if (g_ascii_isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += (char)c;
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

static std::string base_url()
{
    std::string u = cfg("url");
    while (!u.empty() && (u.back() == '/' || u.back() == ' '))
        u.pop_back();
    return u;
}

/* Fresh token + salt on every request, as the Subsonic API recommends. */
static std::string api_url(const char * endpoint, const std::string & params)
{
    static std::mutex rng_lock;
    static std::mt19937_64 rng{std::random_device{}()};
    char salt[17];
    {
        std::lock_guard<std::mutex> lock(rng_lock);
        snprintf(salt, sizeof salt, "%016llx", (unsigned long long)rng());
    }
    std::string secret = cfg("password") + salt;
    gchar * token = g_compute_checksum_for_string(G_CHECKSUM_MD5, secret.c_str(), -1);
    std::string url = base_url() + "/rest/" + endpoint + "?u=" + url_encode(cfg("user")) +
                      "&t=" + token + "&s=" + salt + "&v=1.16.1&c=audacious-navidrome&" + params;
    g_free(token);
    return url;
}

#ifdef NAVIDROME_TRANSPORT

/* ------------------------------------------------------------------ */
/* Transport plugin: navidrome://raw/ID.ext and navidrome://mp3/ID.mp3 */
/* ------------------------------------------------------------------ */

// Not just "schemes": inside the class that name means TransportPlugin::schemes itself.
static const char * const navidrome_schemes[] = {"navidrome"};

class NavidromeTransport : public TransportPlugin
{
public:
    static constexpr PluginInfo info = {
        N_("Navidrome Streaming"), "audacious-navidrome",
        N_("Streams tracks from a Navidrome server for the Navidrome plugin.\n"
           "Settings are under the Navidrome general plugin.")};

    constexpr NavidromeTransport() : TransportPlugin(info, navidrome_schemes) {}

    bool init() override
    {
        aud_config_set_defaults(CFG, cfg_defaults);
        return true;
    }

    VFSImpl * fopen(const char * filename, const char * mode, String & error) override;

    VFSFileTest test_file(const char * filename, VFSFileTest test, String & error) override
    {
        return VFSFileTest(test & VFS_IS_REGULAR);
    }
};

EXPORT NavidromeTransport aud_plugin_instance;

/* Read-only pass-through to the underlying HTTP stream (Audacious's own
 * HTTP plugin does the actual transfer, seeking and buffering). */
class NavidromeFile : public VFSImpl
{
public:
    explicit NavidromeFile(VFSFile && file) : m_file(std::move(file)) {}

    int64_t fread(void * ptr, int64_t size, int64_t nmemb) override
    {
        return m_file.fread(ptr, size, nmemb);
    }
    int fseek(int64_t offset, VFSSeekType whence) override
    {
        return m_file.fseek(offset, whence);
    }
    int64_t ftell() override { return m_file.ftell(); }
    int64_t fsize() override { return m_file.fsize(); }
    bool feof() override { return m_file.feof(); }

    int64_t fwrite(const void *, int64_t, int64_t) override { return 0; }
    int ftruncate(int64_t) override { return -1; }
    int fflush() override { return 0; }

    String get_metadata(const char * field) override { return m_file.get_metadata(field); }

private:
    VFSFile m_file;
};

/* Some MP3s stream fine but won't decode: an "unsynchronised" ID3 tag (an
 * old, rarely supported format) or a very large tag (usually embedded cover
 * art) stops the decoder finding the audio. Peek at the start of the file and
 * say if Navidrome should convert it instead. Leaves the file rewound. */
static bool mp3_needs_converting(VFSFile & file, std::string & why)
{
    unsigned char h[10];
    bool got = file.fread(h, 1, sizeof h) == (int64_t)sizeof h;
    if (file.fseek(0, VFS_SEEK_SET) != 0)
    {
        why = "couldn't rewind the stream";
        return true;
    }
    if (!got)
        return false;

    if (h[0] == 'I' && h[1] == 'D' && h[2] == '3')
    {
        int size = (h[6] << 21) | (h[7] << 14) | (h[8] << 7) | h[9];
        if (h[5] & 0x80)
        {
            why = "unsynchronised ID3 tag";
            return true;
        }
        if (size > 128 * 1024)
        {
            why = "large ID3 tag (" + std::to_string(size / 1024) + " KB, usually cover art)";
            return true;
        }
        return false;
    }
    if (h[0] == 0xFF && (h[1] & 0xE0) == 0xE0) // a plain MPEG audio frame
        return false;

    why = "doesn't start like an MP3";
    return true;
}

VFSImpl * NavidromeTransport::fopen(const char * filename, const char * mode, String & error)
{
    if (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+'))
    {
        error = String(_("Navidrome tracks are read-only"));
        return nullptr;
    }

    // navidrome://<raw|mp3>/<id>.<ext>
    const char * path = strstr(filename, "://");
    path = path ? path + 3 : filename;
    const char * slash = strchr(path, '/');
    if (!slash)
    {
        error = String(_("Not a Navidrome track address"));
        return nullptr;
    }
    std::string kind(path, slash - path);
    std::string id(slash + 1), ext;
    size_t dot = id.rfind('.');
    if (dot != std::string::npos)
    {
        ext = id.substr(dot + 1);
        id.resize(dot);
    }

    std::string params = "id=" + url_encode(id) +
                         (kind == "raw" ? "&format=raw" : "&format=mp3&maxBitRate=320");

    std::string last;
    bool converted = false;
    for (int attempt = 1; attempt <= 3; attempt++)
    {
        VFSFile file(api_url("stream", params).c_str(), "r");
        if (file)
        {
            String ctype = file.get_metadata("content-type");
            if (!ctype || (!strstr(ctype, "json") && !strstr(ctype, "xml")))
            {
                // The converted stream is an MP3 too, so this only helps .mp3 files
                // (Audacious picks the decoder from the extension).
                std::string why;
                if (kind == "raw" && ext == "mp3" && !converted && mp3_needs_converting(file, why))
                {
                    AUDINFO("navidrome: track %s: %s - asking Navidrome to convert it\n",
                            id.c_str(), why.c_str());
                    params = "id=" + url_encode(id) + "&format=mp3&maxBitRate=320";
                    converted = true;
                    attempt = 0; // a fresh set of attempts for the converted stream
                    continue;
                }
                return new NavidromeFile(std::move(file));
            }

            // Subsonic errors come back as a JSON/XML body instead of audio;
            // pull out just the message, e.g. "Data not found".
            char buf[600] = {};
            int64_t n = file.fread(buf, 1, sizeof buf - 1);
            std::string body(buf, n > 0 ? n : 0), msg = body;
            size_t k = body.find("message");
            if (k != std::string::npos)
            {
                size_t a = body.find_first_of("\"'", body.find_first_of(":=", k) + 1);
                size_t b = a == std::string::npos ? a : body.find_first_of("\"'", a + 1);
                if (b != std::string::npos)
                    msg = body.substr(a + 1, b - a - 1);
            }
            last = "Navidrome couldn't play this track: " + msg;
        }
        else
            last = file.error() ? file.error() : "could not connect to Navidrome";

        AUDWARN("navidrome: track %s, attempt %d failed: %s\n", id.c_str(), attempt, last.c_str());
        if (attempt < 3)
            std::this_thread::sleep_for(std::chrono::milliseconds(500 * attempt));
    }

    error = String(last.c_str());
    return nullptr;
}

#else /* general plugin */

/* ------------------------------------------------------------------ */
/* Minimal JSON reader (enough for Subsonic responses)                  */
/* ------------------------------------------------------------------ */

struct JVal
{
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;

    const JVal * get(const char * key) const
    {
        if (type == Obj)
            for (auto & kv : obj)
                if (kv.first == key)
                    return &kv.second;
        return nullptr;
    }
    std::string str(const char * key) const
    {
        const JVal * v = get(key);
        return (v && v->type == Str) ? v->s : std::string();
    }
    int num(const char * key) const
    {
        const JVal * v = get(key);
        return (v && v->type == Num) ? (int)v->n : 0;
    }
};

class JParser
{
public:
    JParser(const char * p, const char * end) : p(p), end(end) {}

    bool parse(JVal & out)
    {
        if (!value(out, 0))
            return false;
        ws();
        return p == end;
    }

private:
    const char * p;
    const char * end;

    void ws()
    {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            p++;
    }
    bool lit(const char * w)
    {
        size_t n = strlen(w);
        if ((size_t)(end - p) < n || strncmp(p, w, n))
            return false;
        p += n;
        return true;
    }
    static void utf8(std::string & s, unsigned cp)
    {
        if (cp < 0x80)
            s += (char)cp;
        else if (cp < 0x800)
        {
            s += (char)(0xC0 | (cp >> 6));
            s += (char)(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            s += (char)(0xE0 | (cp >> 12));
            s += (char)(0x80 | ((cp >> 6) & 0x3F));
            s += (char)(0x80 | (cp & 0x3F));
        }
        else
        {
            s += (char)(0xF0 | (cp >> 18));
            s += (char)(0x80 | ((cp >> 12) & 0x3F));
            s += (char)(0x80 | ((cp >> 6) & 0x3F));
            s += (char)(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(unsigned & v)
    {
        if (end - p < 4)
            return false;
        v = 0;
        for (int i = 0; i < 4; i++)
        {
            int d = g_ascii_xdigit_value(*p++);
            if (d < 0)
                return false;
            v = v * 16 + d;
        }
        return true;
    }
    bool string(std::string & s)
    {
        p++; // opening quote
        while (p < end && *p != '"')
        {
            if (*p != '\\')
            {
                s += *p++;
                continue;
            }
            if (++p >= end)
                return false;
            char c = *p++;
            switch (c)
            {
            case '"': case '\\': case '/': s += c; break;
            case 'b': s += '\b'; break;
            case 'f': s += '\f'; break;
            case 'n': s += '\n'; break;
            case 'r': s += '\r'; break;
            case 't': s += '\t'; break;
            case 'u':
            {
                unsigned cp;
                if (!hex4(cp))
                    return false;
                if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u')
                {
                    p += 2;
                    unsigned lo;
                    if (!hex4(lo))
                        return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(s, cp);
                break;
            }
            default: return false;
            }
        }
        if (p >= end)
            return false;
        p++; // closing quote
        return true;
    }
    bool value(JVal & v, int depth)
    {
        if (depth > 64)
            return false;
        ws();
        if (p >= end)
            return false;
        switch (*p)
        {
        case '{':
        {
            v.type = JVal::Obj;
            p++;
            ws();
            if (p < end && *p == '}')
            {
                p++;
                return true;
            }
            while (true)
            {
                ws();
                if (p >= end || *p != '"')
                    return false;
                std::string key;
                if (!string(key))
                    return false;
                ws();
                if (p >= end || *p++ != ':')
                    return false;
                v.obj.emplace_back(std::move(key), JVal());
                if (!value(v.obj.back().second, depth + 1))
                    return false;
                ws();
                if (p < end && *p == ',')
                {
                    p++;
                    continue;
                }
                if (p < end && *p == '}')
                {
                    p++;
                    return true;
                }
                return false;
            }
        }
        case '[':
        {
            v.type = JVal::Arr;
            p++;
            ws();
            if (p < end && *p == ']')
            {
                p++;
                return true;
            }
            while (true)
            {
                v.arr.emplace_back();
                if (!value(v.arr.back(), depth + 1))
                    return false;
                ws();
                if (p < end && *p == ',')
                {
                    p++;
                    continue;
                }
                if (p < end && *p == ']')
                {
                    p++;
                    return true;
                }
                return false;
            }
        }
        case '"':
            v.type = JVal::Str;
            return string(v.s);
        case 't':
            v.type = JVal::Bool;
            v.b = true;
            return lit("true");
        case 'f':
            v.type = JVal::Bool;
            return lit("false");
        case 'n':
            return lit("null");
        default:
        {
            char * stop;
            std::string num(p, std::min<size_t>(end - p, 64));
            v.n = g_ascii_strtod(num.c_str(), &stop);
            if (stop == num.c_str())
                return false;
            v.type = JVal::Num;
            p += stop - num.c_str();
            return true;
        }
        }
    }
};

/* ------------------------------------------------------------------ */
/* Library fetching                                                     */
/* ------------------------------------------------------------------ */

struct Song
{
    std::string id, title, artist, album, path, suffix, album_id, artist_id;
    int track = 0, disc = 0, duration = 0;
};

/* Calls a Subsonic endpoint; returns the "subsonic-response" object or sets err. */
static bool api_call(const char * endpoint, const std::string & params, JVal & doc,
                     const JVal *& resp, std::string & err)
{
    std::string url = api_url(endpoint, "f=json&" + params);
    VFSFile file(url.c_str(), "r");
    if (!file)
    {
        err = std::string("Couldn't reach Navidrome at ") + base_url() + ": " +
              (file.error() ? file.error() : "connection failed");
        return false;
    }
    Index<char> data = file.read_all();
    if (!JParser(data.begin(), data.end()).parse(doc) || !(resp = doc.get("subsonic-response")))
    {
        err = "Navidrome sent a reply this plugin couldn't read. Is the address right?";
        return false;
    }
    if (resp->str("status") != "ok")
    {
        const JVal * e = resp->get("error");
        err = "Navidrome said: " + (e ? e->str("message") : std::string("request failed"));
        return false;
    }
    return true;
}

static void add_songs(const JVal & list, std::vector<Song> & songs)
{
    for (auto & s : list.arr)
    {
        Song song;
        song.id = s.str("id");
        if (song.id.empty())
            continue;
        song.title = s.str("title");
        song.artist = s.str("artist");
        song.album = s.str("album");
        song.path = s.str("path");
        song.suffix = s.str("suffix");
        song.track = s.num("track");
        song.disc = s.num("discNumber");
        song.duration = s.num("duration");
        song.album_id = s.str("albumId");
        song.artist_id = s.str("artistId");
        songs.push_back(std::move(song));
    }
}

static void set_status(const std::string & text); // defined with the jobs below

/* "46,809" */
static std::string with_commas(int n)
{
    std::string s = std::to_string(n);
    for (int i = (int)s.size() - 3; i > 0; i -= 3)
        s.insert(i, ",");
    return s;
}

/* How many songs Navidrome has, for the progress display (0 if it doesn't say). */
static int library_size()
{
    JVal doc;
    const JVal * resp = nullptr;
    std::string err;
    if (!api_call("getScanStatus", "", doc, resp, err))
        return 0;
    const JVal * st = resp->get("scanStatus");
    return st ? st->num("count") : 0;
}

/* The whole library, 500 songs per request, four requests at a time.
 * Progress goes to the status line as it arrives. */
static bool fetch_songs(std::vector<Song> & songs, std::string & err)
{
    const int page = 500, workers = 4;
    const int total = library_size();

    std::mutex lock;                         // guards pages, err, failed
    std::vector<std::vector<Song>> pages;
    bool failed = false;
    std::atomic<int> next{0}, have{0}, last{INT_MAX};

    auto report = [&](int n) {
        char buf[96];
        if (total > 0)
            snprintf(buf, sizeof buf, "%s / %s (%d%%)", with_commas(n).c_str(),
                     with_commas(total).c_str(), aud::min(100, (int)(100.0 * n / total)));
        else
            snprintf(buf, sizeof buf, "%s songs so far", with_commas(n).c_str());
        set_status(buf);
    };

    auto work = [&]() {
        while (true)
        {
            int i = next++;
            if (i > last)
                return;
            {
                std::lock_guard<std::mutex> g(lock);
                if (failed)
                    return;
            }

            JVal doc;
            const JVal * resp = nullptr;
            std::string my_err;
            std::string params = "query=&artistCount=0&albumCount=0&songCount=" +
                                 std::to_string(page) + "&songOffset=" + std::to_string(i * page);
            if (!api_call("search3", params, doc, resp, my_err))
            {
                std::lock_guard<std::mutex> g(lock);
                if (!failed)
                    failed = true, err = my_err;
                return;
            }
            const JVal * result = resp->get("searchResult3");
            const JVal * list = result ? result->get("song") : nullptr;
            std::vector<Song> got;
            if (list && list->type == JVal::Arr)
                add_songs(*list, got);

            int n = (int)got.size();
            if (n < page) // the end of the library: nobody needs pages after this one
            {
                int cur = last;
                while (i < cur && !last.compare_exchange_weak(cur, i))
                    ;
            }
            {
                std::lock_guard<std::mutex> g(lock);
                if ((int)pages.size() <= i)
                    pages.resize(i + 1);
                pages[i] = std::move(got);
            }
            report(have += n);
        }
    };

    report(0);
    std::vector<std::thread> pool;
    for (int w = 0; w < workers; w++)
        pool.emplace_back(work);
    for (auto & th : pool)
        th.join();

    if (failed)
        return false;
    for (int i = 0; i < (int)pages.size() && i <= last; i++)
        for (auto & s : pages[i])
            songs.push_back(std::move(s));
    return true;
}

struct NdPlaylist
{
    std::string id, name;
    int count = 0;
};

static bool fetch_playlists(std::vector<NdPlaylist> & out, std::string & err)
{
    JVal doc;
    const JVal * resp = nullptr;
    if (!api_call("getPlaylists", "", doc, resp, err))
        return false;
    const JVal * pls = resp->get("playlists");
    const JVal * list = pls ? pls->get("playlist") : nullptr;
    if (list && list->type == JVal::Arr)
        for (auto & p : list->arr)
            if (!p.str("id").empty())
                out.push_back({p.str("id"), p.str("name"), p.num("songCount")});
    return true;
}

static bool fetch_playlist_songs(const std::string & id, std::string & name,
                                 std::vector<Song> & songs, std::string & err)
{
    JVal doc;
    const JVal * resp = nullptr;
    if (!api_call("getPlaylist", "id=" + url_encode(id), doc, resp, err))
        return false;
    const JVal * pl = resp->get("playlist");
    if (!pl)
    {
        err = "that playlist no longer exists";
        return false;
    }
    name = pl->str("name");
    const JVal * entries = pl->get("entry");
    if (entries && entries->type == JVal::Arr)
        add_songs(*entries, songs);
    return true;
}

/* ------------------------------------------------------------------ */
/* Building the playlist                                                */
/* ------------------------------------------------------------------ */

enum class Order { Folder, Artist, Title, Random };

static std::string fold(const std::string & s)
{
    gchar * f = g_utf8_casefold(s.c_str(), -1);
    std::string out = f ? f : s;
    g_free(f);
    return out;
}

static void sort_songs(std::vector<Song> & songs, Order order)
{
    if (order == Order::Random)
    {
        std::shuffle(songs.begin(), songs.end(), std::mt19937{std::random_device{}()});
        return;
    }
    struct Keyed
    {
        std::string a, b, c;
        int disc, track;
        size_t idx;
    };
    std::vector<Keyed> keys;
    keys.reserve(songs.size());
    for (size_t i = 0; i < songs.size(); i++)
    {
        const Song & s = songs[i];
        if (order == Order::Folder)
            keys.push_back({fold(s.path), "", "", 0, 0, i});
        else if (order == Order::Artist)
            keys.push_back({fold(s.artist), fold(s.album), fold(s.title), s.disc, s.track, i});
        else
            keys.push_back({fold(s.title), fold(s.artist), "", 0, 0, i});
    }
    std::stable_sort(keys.begin(), keys.end(), [](const Keyed & x, const Keyed & y) {
        if (x.a != y.a) return x.a < y.a;
        if (x.b != y.b) return x.b < y.b;
        if (x.disc != y.disc) return x.disc < y.disc;
        if (x.track != y.track) return x.track < y.track;
        return x.c < y.c;
    });
    std::vector<Song> sorted;
    sorted.reserve(songs.size());
    for (auto & k : keys)
        sorted.push_back(std::move(songs[k.idx]));
    songs = std::move(sorted);
}

/* Formats Audacious decodes out of the box; anything else is converted to
 * MP3 by Navidrome - unless the FFmpeg plugin is on, which plays everything. */
static bool play_raw(const std::string & suffix)
{
    static const char * const native[] = {"mp3", "flac", "ogg", "oga", "opus", "wav"};
    std::string s = fold(suffix);
    for (const char * n : native)
        if (s == n)
            return true;
    PluginHandle * ff = aud_plugin_lookup_basename("ffaudio");
    return ff && aud_plugin_get_enabled(ff) && !s.empty();
}

static std::string track_uri(const Song & s)
{
    if (play_raw(s.suffix))
        return "navidrome://raw/" + s.id + "." + fold(s.suffix);
    return "navidrome://mp3/" + s.id + ".mp3";
}

/* The song id inside a navidrome:// address, or "" for anything else. */
static std::string uri_id(const char * uri)
{
    if (!uri || strncmp(uri, "navidrome://", 12))
        return std::string();
    const char * slash = strchr(uri + 12, '/');
    if (!slash)
        return std::string();
    std::string id(slash + 1);
    size_t dot = id.rfind('.');
    if (dot != std::string::npos)
        id.resize(dot);
    return id;
}

/* Where a track's page is in Navidrome's web interface: its album, or for
 * tracks with no real album ("[Unknown Album]" is Navidrome's catch-all),
 * its artist. "" = just open Navidrome. */
static std::string web_page(const std::string & album_id, const std::string & album,
                            const std::string & artist_id)
{
    bool real_album = !album_id.empty() && !album.empty() && album != "[Unknown Album]";
    if (real_album)
        return "#/album/" + album_id + "/show";
    if (!artist_id.empty())
        return "#/artist/" + artist_id + "/show";
    if (!album_id.empty())
        return "#/album/" + album_id + "/show";
    return "";
}

// song id -> its page in Navidrome, for "open in Navidrome" (main thread only)
static std::unordered_map<std::string, std::string> s_page_of;

static PlaylistAddItem make_item(const Song & s)
{
    s_page_of[s.id] = web_page(s.album_id, s.album, s.artist_id);
    std::string uri = track_uri(s);
    Tuple t;
    t.set_filename(uri.c_str());
    if (!s.title.empty()) t.set_str(Tuple::Title, s.title.c_str());
    if (!s.artist.empty()) t.set_str(Tuple::Artist, s.artist.c_str());
    if (!s.album.empty()) t.set_str(Tuple::Album, s.album.c_str());
    if (s.track > 0) t.set_int(Tuple::Track, s.track);
    if (s.disc > 0) t.set_int(Tuple::Disc, s.disc);
    if (s.duration > 0) t.set_int(Tuple::Length, s.duration * 1000);
    t.set_state(Tuple::Valid); // details come from Navidrome; no need to scan each stream
    return {String(uri.c_str()), std::move(t), nullptr};
}

static const char * const PLAYLIST_PREFIX = "Navidrome";

static Playlist find_playlist()
{
    int n = Playlist::n_playlists();
    for (int i = 0; i < n; i++)
    {
        Playlist p = Playlist::by_index(i);
        String title = p.get_title();
        if (title && str_has_prefix_nocase(title, PLAYLIST_PREFIX))
            return p;
    }
    return Playlist();
}


/* ------------------------------------------------------------------ */
/* Jobs: fetch on worker threads, touch the playlist on the main one   */
/* ------------------------------------------------------------------ */

// Results posted from worker threads run here, in order, on the main thread.
// (A single QueuedFunc would drop an earlier callback when a later one arrives.)
static std::mutex s_post_lock;
static std::vector<std::function<void()>> s_posted;
static QueuedFunc s_drain;

static void drain_posted()
{
    std::vector<std::function<void()>> todo;
    {
        std::lock_guard<std::mutex> lock(s_post_lock);
        todo.swap(s_posted);
    }
    for (auto & f : todo)
        f();
}

static void post_main(std::function<void()> f)
{
    {
        std::lock_guard<std::mutex> lock(s_post_lock);
        s_posted.push_back(std::move(f));
    }
    s_drain.queue(drain_posted);
}

static std::atomic<bool> s_busy{false};    // a library/playlist load or refresh is running
static std::thread s_worker;
static QueuedFunc s_wait_add;  // waits for Audacious to finish adding entries
static QueuedFunc s_startup;   // optional refresh shortly after Audacious starts
static QueuedFunc s_pl_startup;

// Fetching the list of Navidrome playlists. Every request gets a new number and
// only the newest one's answer is used, so asking again always works, even if
// an earlier request is still out.
static std::atomic<int> s_pl_gen{0};
static std::atomic<int> s_pl_threads{0};  // requests still out (waited for on shutdown)
static std::atomic<bool> s_pl_busy{false};

// Main thread only:
static String s_status;                    // library: loading progress or the last result
static String s_pl_status;                 // playlists: loading, how many, or the error
static std::vector<NdPlaylist> s_playlists;
static bool s_playlists_loaded = false;
static bool s_playlists_failed = false;   // the last fetch didn't work

/* "09:41" - status lines say when something happened, not that it's current */
static std::string clock_now()
{
    GDateTime * now = g_date_time_new_now_local();
    gchar * s = g_date_time_format(now, "%H:%M");
    std::string out = s ? s : "";
    g_free(s);
    g_date_time_unref(now);
    return out;
}

static void set_status_now(const std::string & text)
{
    s_status = String(text.c_str());
    hook_call("navidrome status", nullptr);
}

static void set_pl_status_now(const std::string & text)
{
    s_pl_status = String(text.c_str());
    hook_call("navidrome playlists", nullptr);
}

static void set_status(const std::string & text) // from any thread
{
    post_main([text]() { set_status_now(text); });
}

static void fail(const std::string & msg)
{
    aud_ui_show_error(("Navidrome: " + msg).c_str());
    set_status("Error: " + msg);
    s_busy = false;
}

/* What the Navidrome playlist currently holds: "" for the whole library,
 * "playlist:<id>" for one of your Navidrome playlists. Remembered across
 * restarts so Refresh knows what to refresh. */
static std::string source_playlist_id(const std::string & source)
{
    return source.compare(0, 9, "playlist:") == 0 ? source.substr(9) : std::string();
}

static void set_label_title(Playlist p, const std::string & label, int count,
                            const char * extra = "")
{
    char title[300];
    if (label.empty())
        snprintf(title, sizeof title, "%s - %d tracks%s", PLAYLIST_PREFIX, count, extra);
    else
        snprintf(title, sizeof title, "%s - %s - %d tracks%s", PLAYLIST_PREFIX,
                 label.c_str(), count, extra);
    p.set_title(title);
}

static void finish_load(Playlist p)
{
    // Entries are added in the background; start playback once they're all in.
    s_wait_add.start(250, [p]() {
        if (p.add_in_progress())
            return;
        s_wait_add.stop();
        int n = p.n_entries();
        if (n > 0)
        {
            static std::mt19937 rng{std::random_device{}()};
            aud_set_bool(nullptr, "shuffle", true);
            p.activate();
            p.set_position(std::uniform_int_distribution<int>(0, n - 1)(rng)); // random first track
            p.start_playback();
        }
        s_busy = false;
        set_status_now("Loaded " + clock_now());
    });
}

/* Main thread: replace the Navidrome playlist with these (already ordered) songs. */
static void apply_load(const std::vector<Song> & songs, const std::string & label,
                       const std::string & source)
{
    Index<PlaylistAddItem> items;
    for (auto & s : songs)
        items.append(make_item(s));

    Playlist p = find_playlist();
    if (!p.exists())
        p = Playlist::new_playlist();
    set_status_now("Adding " + with_commas(items.len()) + " tracks...");
    p.remove_all_entries();
    set_label_title(p, label, items.len());
    aud_set_str(CFG, "source", source.c_str());
    aud_set_str(CFG, "source_name", label.c_str());
    hook_call("navidrome source", nullptr);
    p.insert_items(0, std::move(items), false);
    p.activate();
    finish_load(p);
}

static void load_job(Order order)
{
    std::vector<Song> songs;
    std::string err;
    if (!fetch_songs(songs, err))
        return fail(err);
    if (songs.empty())
        return fail("the library is empty");
    sort_songs(songs, order);

    auto shared = std::make_shared<std::vector<Song>>(std::move(songs));
    post_main([shared]() { apply_load(*shared, "", ""); });
}

static void playlist_job(std::string id)
{
    std::vector<Song> songs;
    std::string name, err;
    if (!fetch_playlist_songs(id, name, songs, err))
        return fail(err);
    if (songs.empty())
        return fail("that playlist is empty");

    auto shared = std::make_shared<std::vector<Song>>(std::move(songs));
    post_main([shared, name, id]() { apply_load(*shared, name, "playlist:" + id); });
}

static void refresh_job(std::string source)
{
    std::vector<Song> songs;
    std::string name, err;
    std::string pl_id = source_playlist_id(source);
    if (pl_id.empty())
    {
        if (!fetch_songs(songs, err))
            return fail(err);
        sort_songs(songs, Order::Folder);
    }
    else if (!fetch_playlist_songs(pl_id, name, songs, err))
        return fail(err);

    auto fresh = std::make_shared<std::vector<Song>>(std::move(songs));

    post_main([fresh, name, source]() {
        Playlist p = find_playlist();
        if (!p.exists())
        {
            // Nothing loaded yet: load what we just fetched.
            if (fresh->empty())
                return fail("nothing to load");
            apply_load(*fresh, name, source);
            return;
        }

        std::unordered_set<std::string> on_server;
        for (auto & s : *fresh)
            on_server.insert(s.id);

        // Drop tracks that are gone (from the end, so indexes stay valid).
        std::unordered_set<std::string> in_playlist;
        int removed = 0;
        for (int i = p.n_entries() - 1; i >= 0; i--)
        {
            std::string id = uri_id(p.entry_filename(i));
            if (id.empty())
                continue;
            if (on_server.count(id))
                in_playlist.insert(id);
            else
            {
                p.remove_entry(i);
                removed++;
            }
        }

        Index<PlaylistAddItem> items;
        for (auto & s : *fresh)
            if (!in_playlist.count(s.id))
                items.append(make_item(s));
        int added = items.len();

        char extra[96] = "";
        if (added || removed)
            snprintf(extra, sizeof extra, " (+%d new, -%d removed)", added, removed);
        set_label_title(p, name, (int)in_playlist.size() + added, extra);
        if (added)
            p.insert_items(-1, std::move(items), false);
        s_busy = false;
        // what this refresh found, and when - not a claim that nothing has changed since
        char status[96];
        if (added || removed)
            snprintf(status, sizeof status, "+%d new, -%d removed (%s)", added, removed, clock_now().c_str());
        else
            snprintf(status, sizeof status, "No changes (%s)", clock_now().c_str());
        set_status_now(status);
    });
}

/* Main thread. Starts a load/refresh unless one is running or the server isn't set. */
static bool start_job(std::function<void()> job, const char * status)
{
    if (base_url().empty())
    {
        set_status_now("Set up your server in Settings > Plugins > Navidrome");
        aud_ui_show_error(_("Navidrome: set your server address, username and password first "
                            "(Settings > Plugins > General > Navidrome > Settings)."));
        return false;
    }
    if (s_busy.exchange(true))
    {
        aud_ui_show_error(_("Navidrome: still loading - give it a moment."));
        return false;
    }
    if (s_worker.joinable())
        s_worker.join();
    set_status_now(status);
    s_worker = std::thread(std::move(job));
    return true;
}

static const char * const order_names[] = {"folder", "artist", "title", "random"};

static Order order_from_name(const char * name)
{
    for (int i = 0; i < 4; i++)
        if (name && !strcmp(name, order_names[i]))
            return (Order)i;
    return Order::Folder;
}

static void play_order(Order order)
{
    aud_set_str(CFG, "order", order_names[(int)order]);
    hook_call("navidrome source", nullptr);
    start_job([order]() { load_job(order); }, "Loading library...");
}

static void play_folder() { play_order(Order::Folder); }
static void play_artist() { play_order(Order::Artist); }
static void play_title() { play_order(Order::Title); }
static void play_random() { play_order(Order::Random); }

static void refresh()
{
    std::string source = cfg("source");
    start_job([source]() { refresh_job(source); }, "Refreshing...");
}

static void play_playlist(const std::string & id)
{
    start_job([id]() { playlist_job(id); }, "Loading playlist...");
}

/* Fetches the list of Navidrome playlists in the background. With force=false
 * it does nothing if the list is already loaded or on its way. */
static void fetch_playlists_async(bool force = true)
{
    if (base_url().empty())
    {
        set_pl_status_now("Set up your server first");
        return;
    }
    if (!force && (s_playlists_loaded || s_pl_busy))
        return;

    int gen = ++s_pl_gen;
    s_pl_busy = true;
    set_pl_status_now("Loading...");
    s_pl_threads++;

    std::thread([gen]() {
        std::vector<NdPlaylist> list;
        std::string err;
        bool ok = fetch_playlists(list, err);
        if (!ok)
            AUDWARN("navidrome: couldn't load playlists: %s\n", err.c_str());
        std::sort(list.begin(), list.end(), [](const NdPlaylist & a, const NdPlaylist & b) {
            return fold(a.name) < fold(b.name);
        });
        auto shared = std::make_shared<std::vector<NdPlaylist>>(std::move(list));
        post_main([shared, ok, err, gen]() {
            if (gen != s_pl_gen)
                return; // a newer request has been made since; its answer wins
            s_pl_busy = false;
            if (ok)
            {
                s_playlists = std::move(*shared);
                s_playlists_loaded = true;
                s_playlists_failed = false;
                int n = (int)s_playlists.size();
                set_pl_status_now(std::to_string(n) + (n == 1 ? " playlist (" : " playlists (") +
                                  clock_now() + ")");
            }
            else
            {
                // keep any list we already had; just say this attempt failed
                s_playlists_failed = !s_playlists_loaded;
                std::string why = err;
                if (why.compare(0, 16, "Navidrome said: ") == 0)
                    why = why.substr(16);
                set_pl_status_now("Error: " + why);
            }
        });
        s_pl_threads--;
    }).detach();
}

/* ------------------------------------------------------------------ */
/* Open a track's album in Navidrome's web interface                     */
/* ------------------------------------------------------------------ */

static void open_web(const std::string & page)
{
    // Navidrome's web interface, e.g. <server>/app/#/album/<id>/show
    std::string url = base_url() + "/app/" + page;
    if (aud_get_mainloop_type() != MainloopType::Qt ||
        !QDesktopServices::openUrl(QUrl(QString::fromStdString(url))))
        aud_ui_show_error(("Navidrome: couldn't open a web browser for " + url).c_str());
}

/* Main thread. Returns false if this isn't a Navidrome track. */
static bool open_in_navidrome(const char * uri)
{
    std::string id = uri_id(uri);
    if (id.empty())
        return false;
    if (base_url().empty())
    {
        aud_ui_show_error(_("Navidrome: set up your server first."));
        return true;
    }

    auto known = s_page_of.find(id);
    if (known != s_page_of.end())
    {
        open_web(known->second);
        return true;
    }

    // Not loaded this session (e.g. the playlist was restored at startup): ask Navidrome.
    std::thread([id]() {
        JVal doc;
        const JVal * resp = nullptr;
        std::string err, page;
        bool ok = api_call("getSong", "id=" + url_encode(id), doc, resp, err);
        if (ok)
        {
            const JVal * song = resp->get("song");
            if (song)
                page = web_page(song->str("albumId"), song->str("album"), song->str("artistId"));
        }
        else
            AUDWARN("navidrome: couldn't look up song %s: %s\n", id.c_str(), err.c_str());
        post_main([id, page, ok]() {
            if (ok)
                s_page_of[id] = page;
            open_web(page); // Navidrome's home page if the track couldn't be found
        });
    }).detach();
    return true;
}

static void open_focused_in_navidrome()
{
    Playlist list = Playlist::active_playlist();
    int focus = list.get_focus();
    String uri = focus >= 0 ? list.entry_filename(focus) : String();
    if (!uri || !open_in_navidrome(uri))
        aud_ui_show_error(_("Navidrome: pick a Navidrome track in the playlist first."));
}

/* ------------------------------------------------------------------ */
/* Plugin, menus and settings page                                      */
/* ------------------------------------------------------------------ */

/* A plain information popup (or a warning) when Audacious is running its Qt
 * interface; Audacious's error popup is the only option otherwise. */
static void tell(const std::string & text, bool ok)
{
    if (aud_get_mainloop_type() == MainloopType::Qt)
        audqt::simple_message(_("Navidrome"), text.c_str(),
                              ok ? QMessageBox::Information : QMessageBox::Warning);
    else
        aud_ui_show_error(("Navidrome: " + text).c_str());
}

static void test_connection()
{
    std::string err;
    JVal doc;
    const JVal * resp = nullptr;
    if (api_call("ping", "", doc, resp, err))
        tell(_("Connected to your Navidrome server."), true);
    else
        tell(err, false);
}

static const PreferencesWidget widgets[] = {
    WidgetLabel(N_("<b>Navidrome server</b>")),
    WidgetEntry(N_("Address:"), WidgetString(CFG, "url")),
    WidgetLabel(N_("for example http://192.168.1.10:4533"), WIDGET_CHILD),
    WidgetEntry(N_("Username:"), WidgetString(CFG, "user")),
    WidgetEntry(N_("Password:"), WidgetString(CFG, "password"), {true}),
    WidgetButton(N_("Test connection"), {test_connection, "network-server"}),
    WidgetCheck(N_("Refresh library when Audacious starts"), WidgetBool(CFG, "refresh_on_start")),
    WidgetLabel(N_("Then use the N button in the Winamp Classic Interface (Navidrome),\n"
                   "or Services > Navidrome in the main or playlist menu.")),
};

// When the settings dialog closes (fields save as you type): fetch the playlist
// list from the server, which may have just changed
static void prefs_closed()
{
    s_playlists.clear();
    s_playlists_loaded = false;
    fetch_playlists_async(true);
    hook_call("navidrome source", nullptr);
}

static const PluginPreferences prefs = {{widgets}, nullptr, nullptr, prefs_closed};

struct MenuItem
{
    const char * name;
    void (*func)();
};

static const MenuItem menu_items[] = {
    {N_("Navidrome: Play Library (folder order)"), play_folder},
    {N_("Navidrome: Play Library (by artist)"), play_artist},
    {N_("Navidrome: Play Library (by title)"), play_title},
    {N_("Navidrome: Play Library (scrambled list)"), play_random},
    {N_("Navidrome: Refresh Library (keeps playing)"), refresh},
};

static const AudMenuID menus[] = {AudMenuID::Main, AudMenuID::Playlist};
static const char * const OPEN_ITEM = N_("Navidrome: Open Track in Navidrome");

class NavidromePlugin : public GeneralPlugin
{
public:
    static constexpr PluginInfo info = {
        N_("Navidrome"), "audacious-navidrome",
        N_("Plays your whole Navidrome library, or one of your Navidrome playlists, "
           "on shuffle.\n\n"
           "Set your server under Settings, then use Services > Navidrome in the main "
           "or playlist menu, or the Navidrome button in the Winamp interface "
           "(Navidrome edition)."),
        &prefs};

    constexpr NavidromePlugin() : GeneralPlugin(info, false) {}

    bool init() override
    {
        aud_config_set_defaults(CFG, cfg_defaults);
        for (AudMenuID m : menus)
            for (auto & item : menu_items)
                aud_plugin_menu_add(m, item.func, _(item.name), "folder-music");
        aud_plugin_menu_add(AudMenuID::Playlist, open_focused_in_navidrome, _(OPEN_ITEM), "internet-web-browser");
        // Silently skipped if no server is set; loads the library if there's no playlist yet.
        if (aud_get_bool(CFG, "refresh_on_start") && !base_url().empty())
            s_startup.queue(1500, refresh);
        // have the playlist list ready by the time the window is opened
        if (!base_url().empty())
            s_pl_startup.queue(500, []() { fetch_playlists_async(false); });
        return true;
    }

    void cleanup() override
    {
        for (AudMenuID m : menus)
            for (auto & item : menu_items)
                aud_plugin_menu_remove(m, item.func);
        aud_plugin_menu_remove(AudMenuID::Playlist, open_focused_in_navidrome);
        s_page_of.clear();
        s_startup.stop();
        s_pl_startup.stop();
        if (s_worker.joinable())
            s_worker.join();
        // playlist requests give up within ~10s (Audacious's HTTP timeout); don't
        // hold up quitting longer than a few seconds for one
        ++s_pl_gen;
        for (int i = 0; i < 60 && s_pl_threads > 0; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        s_drain.stop();
        s_wait_add.stop();
        s_posted.clear();
        s_busy = false;
        s_pl_busy = false;
        s_playlists.clear();
        s_playlists_loaded = false;
        s_playlists_failed = false;
        s_status = String();
        s_pl_status = String();
    }

    /* Messages from the Winamp interface (Navidrome edition). Main thread only.
     *   play            data: "folder"/"artist"/"title"/"random", or null for the saved order
     *   refresh         re-sync whatever is loaded (library or playlist)
     *   play-playlist   data: Navidrome playlist id
     *   fetch-playlists fetch the playlist list if it isn't loaded or on its way
     *   refresh-playlists  fetch the playlist list again now
     *                   ("navidrome playlists" fires whenever the list or its status changes)
     *   get-playlists   data: Index<String> * to fill with id, name, track count triples;
     *                   returns 0 when loaded, 1 while still loading, 2 if loading failed
     *   get-status      data: String * (library status: progress, or the last result and when)
     *   get-playlist-status  data: String * (playlists status)
     *   open-in-navidrome    data: track address; opens its album in Navidrome's web
     *                   interface. Returns -1 if it isn't a Navidrome track.
     *   get-info        data: Index<String> * to fill with: order, source, source name, busy, loaded
     *   set-order       data: order name (used by the next "play" with null data) */
    int take_message(const char * code, const void * data, int) override
    {
        const char * text = (const char *)data;

        if (!strcmp(code, "play"))
        {
            play_order(order_from_name(text ? text : (const char *)aud_get_str(CFG, "order")));
            return 0;
        }
        if (!strcmp(code, "refresh"))
        {
            refresh();
            return 0;
        }
        if (!strcmp(code, "play-playlist") && text)
        {
            play_playlist(text);
            return 0;
        }
        if (!strcmp(code, "fetch-playlists"))
        {
            fetch_playlists_async(false);
            return 0;
        }
        if (!strcmp(code, "refresh-playlists"))
        {
            fetch_playlists_async(true);
            return 0;
        }
        if (!strcmp(code, "open-in-navidrome") && text)
            return open_in_navidrome(text) ? 0 : -1;
        if (!strcmp(code, "get-playlist-status") && data)
        {
            *(String *)data = s_pl_status;
            return 0;
        }
        if (!strcmp(code, "set-order") && text)
        {
            aud_set_str(CFG, "order", order_names[(int)order_from_name(text)]);
            hook_call("navidrome source", nullptr);
            return 0;
        }
        if (!strcmp(code, "get-playlists") && data)
        {
            auto out = (Index<String> *)data;
            out->clear();
            for (auto & p : s_playlists)
            {
                out->append(String(p.id.c_str()));
                out->append(String(p.name.c_str()));
                out->append(String(int_to_str(p.count)));
            }
            return s_playlists_loaded ? 0 : s_playlists_failed ? 2 : 1;
        }
        if (!strcmp(code, "get-status") && data)
        {
            *(String *)data = s_status;
            return 0;
        }
        if (!strcmp(code, "get-info") && data)
        {
            auto out = (Index<String> *)data;
            out->clear();
            out->append(aud_get_str(CFG, "order"));
            out->append(aud_get_str(CFG, "source"));
            out->append(aud_get_str(CFG, "source_name"));
            out->append(String((s_busy || s_pl_busy) ? "busy" : ""));
            out->append(String(find_playlist().exists() ? "loaded" : ""));
            return 0;
        }
        return -1;
    }
};

EXPORT NavidromePlugin aud_plugin_instance;

#endif
