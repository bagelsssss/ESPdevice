// ESPdevice.cpp - background keypad driver for the 3-key ESP32 pad (v1).
// -----------------------------------------------------------------------
// Always-on background keypad driver for the 3-key ESP32 pad.
//
// Features:
//   1. Keyboard-like input: scan-code SendInput, tap + hold, typematic
//      repeat from the Windows keyboard delay/rate (ported from 3.5).
//      Bindings can be single keys, combos (CTRL+SHIFT+A) or media keys.
//   2. Minimal input delay: ONE dedicated high-priority device thread
//      blocks on the serial port and wakes the instant a byte arrives.
//      The key path touches no lock, no file, no network: byte ->
//      array lookup -> SendInput. Everything else (website, debugger)
//      only reads a lock-free event ring written AFTER the keystroke.
//   3. Profiles: config\profiles.json, up to 99, atomic save.
//   4. Visualizer: events are PUSHED to the page (Server-Sent Events).
//   5. Debugger: raw report rate, event log, measured byte->SendInput
//      latency (last / avg / max).
//
// UI is a local website (http://localhost:9191) styled as a green-on-
// black terminal. All colours/fonts live in one CSS block at the top of
// the page so the look can be replaced later.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>
#include <shellapi.h>
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")

#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <fstream>
#include <sstream>
#include <chrono>
#include <vector>
#include <filesystem>
#include <ctime>
#include <map>
#include <algorithm>
#include <functional>
#include <exception>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cctype>

namespace fs = std::filesystem;

#include "default_logo.h" // built-in logo bytes (DEFAULT_LOGO_PNG); config\default_logo.png overrides it
#if __has_include("version.h")
#include "version.h" // written by the release build from the git tag: #define ESP_VERSION "1.1.0"
#endif
#ifndef ESP_VERSION
#define ESP_VERSION "1.1.0"
#endif

// ---- Fixed config -----------------------------------------------------

constexpr DWORD BAUD_RATE = 921600;
constexpr BYTE ID_QUERY_BYTE = 0xFE;
constexpr const char* DEVICE_ID = "3KEY";
constexpr int NUM_KEYS = 3;
constexpr int MAX_COMBO = 4;
constexpr double KEY_TIMEOUT_SECONDS = 1.5; // force-release if the board goes silent while a key is held
constexpr int MAX_PROFILES = 99;
constexpr int MAX_NAME_LEN = 24;
constexpr int HTTP_PORT = 9191;
constexpr int MAX_SSE_CLIENTS = 8;
constexpr DWORD READ_WAKE_MS = 8; // read timeout: how often the device thread wakes with no data (repeat/timeout checks)
constexpr const char* VERSION_TEXT = "ESPdevice v" ESP_VERSION;
constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT ID_TRAY_OPEN = 1001;
constexpr UINT ID_TRAY_PAUSE = 1002;
constexpr UINT ID_TRAY_TERMINATE = 1003;

static std::atomic<bool> g_running{ true };
static std::atomic<bool> g_paused{ false };

// ---- Paths + logging --------------------------------------------------

static fs::path g_configDir, g_logPath, g_profilesPath, g_activeIndexPath;

static void InitPaths() {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    fs::path exeDir = fs::path(buffer).parent_path();
    g_configDir = exeDir / L"config";
    std::error_code ec;
    fs::create_directories(g_configDir, ec);
    g_logPath = g_configDir / L"espdevice.log";
    g_profilesPath = g_configDir / L"profiles.json";
    g_activeIndexPath = g_configDir / L"active_profile.txt";
}

static std::mutex g_logMutex;

static std::string TimestampNow() {
    std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tmBuf;
    localtime_s(&tmBuf, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmBuf);
    return std::string(buf);
}

static void Log(const std::string& message) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    std::ofstream file(g_logPath, std::ios::app);
    if (file.is_open()) file << "[" << TimestampNow() << "] " << message << "\n";
}

// Last-resort handler for hard crashes (access violation etc.): leaves a
// FATAL line in the log so a "silent vanish" always leaves evidence.
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) {
    char buf[160];
    DWORD code = (info && info->ExceptionRecord) ? info->ExceptionRecord->ExceptionCode : 0;
    void* addr = (info && info->ExceptionRecord) ? info->ExceptionRecord->ExceptionAddress : nullptr;
    snprintf(buf, sizeof(buf), "FATAL: unhandled exception 0x%08lX at %p", (unsigned long)code, addr);
    std::ofstream file(g_logPath, std::ios::app); // no mutex on purpose: the mutex may be what is stuck
    if (file.is_open()) file << "[" << TimestampNow() << "] " << buf << "\n";
    return EXCEPTION_EXECUTE_HANDLER;
}

static void RunGuarded(const char* name, bool restart, const std::function<void()>& fn) {
    do {
        try { fn(); return; }
        catch (const std::exception& e) { Log(std::string("ERROR in ") + name + ": " + e.what()); }
        catch (...) { Log(std::string("ERROR in ") + name + ": unknown exception"); }
        if (restart && g_running) std::this_thread::sleep_for(std::chrono::milliseconds(500));
    } while (restart && g_running);
}

// ---- High-resolution clock --------------------------------------------

static LARGE_INTEGER g_qpcFreq;
static int64_t g_qpcStart = 0;

static void InitClock() {
    QueryPerformanceFrequency(&g_qpcFreq);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    g_qpcStart = t.QuadPart;
}
static int64_t QpcNow() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static double QpcToSec(int64_t t) { return (double)(t - g_qpcStart) / (double)g_qpcFreq.QuadPart; }
static uint64_t QpcToUs(int64_t t) { return (uint64_t)(QpcToSec(t) * 1e6); }

// ---- Key table --------------------------------------------------------
// kind 0 = scan code, 1 = scan code + extended flag, 2 = virtual key
// (media keys have no scan codes). US layout, scan code set 1.

struct KeyDef { WORD code; uint8_t kind; };

static const std::map<std::string, KeyDef> KEYS = {
    // letters + digits
    {"A",{0x1E,0}},{"B",{0x30,0}},{"C",{0x2E,0}},{"D",{0x20,0}},{"E",{0x12,0}},{"F",{0x21,0}},
    {"G",{0x22,0}},{"H",{0x23,0}},{"I",{0x17,0}},{"J",{0x24,0}},{"K",{0x25,0}},{"L",{0x26,0}},
    {"M",{0x32,0}},{"N",{0x31,0}},{"O",{0x18,0}},{"P",{0x19,0}},{"Q",{0x10,0}},{"R",{0x13,0}},
    {"S",{0x1F,0}},{"T",{0x14,0}},{"U",{0x16,0}},{"V",{0x2F,0}},{"W",{0x11,0}},{"X",{0x2D,0}},
    {"Y",{0x15,0}},{"Z",{0x2C,0}},
    {"0",{0x0B,0}},{"1",{0x02,0}},{"2",{0x03,0}},{"3",{0x04,0}},{"4",{0x05,0}},
    {"5",{0x06,0}},{"6",{0x07,0}},{"7",{0x08,0}},{"8",{0x09,0}},{"9",{0x0A,0}},
    // function keys (F13-F24 have no reliable scan codes, so they go out as virtual keys)
    {"F1",{0x3B,0}},{"F2",{0x3C,0}},{"F3",{0x3D,0}},{"F4",{0x3E,0}},{"F5",{0x3F,0}},{"F6",{0x40,0}},
    {"F7",{0x41,0}},{"F8",{0x42,0}},{"F9",{0x43,0}},{"F10",{0x44,0}},{"F11",{0x57,0}},{"F12",{0x58,0}},
    {"F13",{VK_F13,2}},{"F14",{VK_F14,2}},{"F15",{VK_F15,2}},{"F16",{VK_F16,2}},{"F17",{VK_F17,2}},{"F18",{VK_F18,2}},
    {"F19",{VK_F19,2}},{"F20",{VK_F20,2}},{"F21",{VK_F21,2}},{"F22",{VK_F22,2}},{"F23",{VK_F23,2}},{"F24",{VK_F24,2}},
    // main block
    {"SPACE",{0x39,0}},{"ENTER",{0x1C,0}},{"ESC",{0x01,0}},{"TAB",{0x0F,0}},{"BACKSPACE",{0x0E,0}},
    {"CAPSLOCK",{0x3A,0}},
    {"SEMICOLON",{0x27,0}},{"QUOTE",{0x28,0}},{"COMMA",{0x33,0}},{"PERIOD",{0x34,0}},
    {"SLASH",{0x35,0}},{"LBRACKET",{0x1A,0}},{"RBRACKET",{0x1B,0}},{"BACKSLASH",{0x2B,0}},
    {"MINUS",{0x0C,0}},{"EQUALS",{0x0D,0}},{"GRAVE",{0x29,0}},{"NONUSBACKSLASH",{0x56,0}},
    // modifiers (CTRL/SHIFT/ALT/WIN = the left-hand ones)
    {"CTRL",{0x1D,0}},{"SHIFT",{0x2A,0}},{"ALT",{0x38,0}},{"WIN",{0x5B,1}},
    {"LCTRL",{0x1D,0}},{"LSHIFT",{0x2A,0}},{"LALT",{0x38,0}},{"LWIN",{0x5B,1}},
    {"RCTRL",{0x1D,1}},{"RSHIFT",{0x36,0}},{"RALT",{0x38,1}},{"RWIN",{0x5C,1}},
    {"MENU",{0x5D,1}},
    // navigation cluster
    {"LEFT",{0x4B,1}},{"RIGHT",{0x4D,1}},{"UP",{0x48,1}},{"DOWN",{0x50,1}},
    {"INSERT",{0x52,1}},{"DELETE",{0x53,1}},{"HOME",{0x47,1}},{"END",{0x4F,1}},
    {"PAGEUP",{0x49,1}},{"PAGEDOWN",{0x51,1}},
    {"PRINTSCREEN",{0x37,1}},{"SCROLLLOCK",{0x46,0}},{"PAUSE",{VK_PAUSE,2}},
    // numpad
    {"NUMLOCK",{0x45,0}},
    {"NUM0",{0x52,0}},{"NUM1",{0x4F,0}},{"NUM2",{0x50,0}},{"NUM3",{0x51,0}},{"NUM4",{0x4B,0}},
    {"NUM5",{0x4C,0}},{"NUM6",{0x4D,0}},{"NUM7",{0x47,0}},{"NUM8",{0x48,0}},{"NUM9",{0x49,0}},
    {"NUM_ADD",{0x4E,0}},{"NUM_SUB",{0x4A,0}},{"NUM_MUL",{0x37,0}},{"NUM_DIV",{0x35,1}},
    {"NUM_DECIMAL",{0x53,0}},{"NUM_ENTER",{0x1C,1}},
    // media / browser / launch keys (virtual keys, no scan codes)
    {"MEDIA_PLAY",{VK_MEDIA_PLAY_PAUSE,2}},{"MEDIA_NEXT",{VK_MEDIA_NEXT_TRACK,2}},
    {"MEDIA_PREV",{VK_MEDIA_PREV_TRACK,2}},{"MEDIA_STOP",{VK_MEDIA_STOP,2}},
    {"VOL_UP",{VK_VOLUME_UP,2}},{"VOL_DOWN",{VK_VOLUME_DOWN,2}},{"VOL_MUTE",{VK_VOLUME_MUTE,2}},
    {"BROWSER_BACK",{VK_BROWSER_BACK,2}},{"BROWSER_FORWARD",{VK_BROWSER_FORWARD,2}},
    {"BROWSER_REFRESH",{VK_BROWSER_REFRESH,2}},{"BROWSER_HOME",{VK_BROWSER_HOME,2}},
    {"LAUNCH_MAIL",{VK_LAUNCH_MAIL,2}},{"LAUNCH_CALC",{VK_LAUNCH_APP2,2}},
};

// Typed characters and alternate words -> canonical binding text. A key pad
// key is a PHYSICAL key, so a shifted character is stored as SHIFT + the
// physical key that makes it (on a US layout): "?" becomes SHIFT+SLASH.
static const std::map<std::string, std::string> ALIASES = {
    {"`","GRAVE"},{"-","MINUS"},{"=","EQUALS"},{"[","LBRACKET"},{"]","RBRACKET"},{"\\","BACKSLASH"},
    {";","SEMICOLON"},{"'","QUOTE"},{",","COMMA"},{".","PERIOD"},{"/","SLASH"},
    {"~","SHIFT+GRAVE"},{"!","SHIFT+1"},{"@","SHIFT+2"},{"#","SHIFT+3"},{"$","SHIFT+4"},{"%","SHIFT+5"},
    {"^","SHIFT+6"},{"&","SHIFT+7"},{"*","SHIFT+8"},{"(","SHIFT+9"},{")","SHIFT+0"},{"_","SHIFT+MINUS"},
    {"+","SHIFT+EQUALS"},{"PLUS","SHIFT+EQUALS"},{"{","SHIFT+LBRACKET"},{"}","SHIFT+RBRACKET"},
    {"|","SHIFT+BACKSLASH"},{":","SHIFT+SEMICOLON"},{"\"","SHIFT+QUOTE"},{"<","SHIFT+COMMA"},
    {">","SHIFT+PERIOD"},{"?","SHIFT+SLASH"},
    {"RETURN","ENTER"},{"ESCAPE","ESC"},{"DEL","DELETE"},{"INS","INSERT"},{"PGUP","PAGEUP"},
    {"PGDN","PAGEDOWN"},{"SPACEBAR","SPACE"},{"CONTROL","CTRL"},{"WINDOWS","WIN"},{"APPS","MENU"},
    {"PRTSC","PRINTSCREEN"},
};

static std::string NormalizeBinding(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (c == ' ' || c == '\t') continue;
        out += (char)std::toupper((unsigned char)c);
    }
    return out;
}

struct CompiledBinding {
    int count = 0;
    KeyDef keys[MAX_COMBO];
    char text[48] = { 0 }; // canonical text, e.g. "SHIFT+SLASH"
};

// Splits on '+', where a '+' at the START of a key is the plus character
// itself ("CTRL++" = CTRL and the + key).
static bool SplitBinding(const std::string& text, std::vector<std::string>& toks, std::string& err) {
    size_t pos = 0;
    while (true) {
        std::string tok;
        size_t next;
        if (pos < text.size() && text[pos] == '+') { tok = "+"; next = pos + 1; }
        else {
            size_t plus = text.find('+', pos);
            if (plus == std::string::npos) { tok = text.substr(pos); next = text.size(); }
            else { tok = text.substr(pos, plus - pos); next = plus; }
        }
        if (tok.empty()) { err = "empty key name (stray + ?)"; return false; }
        toks.push_back(tok);
        if (next >= text.size()) return true;
        if (text[next] != '+') { err = "missing + between keys"; return false; }
        pos = next + 1;
        if (pos >= text.size()) { err = "binding ends with +"; return false; }
    }
}

// Turns what the user typed ("CTRL+shift+?", "volup"...) into a ready-to-send
// CompiledBinding with CANONICAL text. Done once when a profile changes,
// never on the key path.
static bool CompileBinding(const std::string& raw, CompiledBinding& out, std::string& err) {
    out = CompiledBinding();
    std::string text = NormalizeBinding(raw);
    if (text.empty()) { err = "binding is empty"; return false; }
    if (text.size() > 80) { err = "binding too long"; return false; }

    std::vector<std::string> toks;
    if (!SplitBinding(text, toks, err)) return false;

    std::vector<std::string> names;
    for (const auto& tok : toks) {
        auto al = ALIASES.find(tok);
        std::string expanded = al != ALIASES.end() ? al->second : tok;
        size_t pos = 0;
        while (true) {
            size_t plus = expanded.find('+', pos);
            std::string name = expanded.substr(pos, plus == std::string::npos ? std::string::npos : plus - pos);
            if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
            if (plus == std::string::npos) break;
            pos = plus + 1;
        }
    }

    std::string canonical;
    for (const auto& name : names) {
        auto it = KEYS.find(name);
        if (it == KEYS.end()) { err = "unknown key: '" + name + "'"; return false; }
        if (out.count >= MAX_COMBO) { err = "too many keys in one binding (max 4)"; return false; }
        out.keys[out.count++] = it->second;
        if (!canonical.empty()) canonical += '+';
        canonical += name;
    }
    if (canonical.size() >= sizeof(out.text)) { err = "binding too long"; return false; }
    snprintf(out.text, sizeof(out.text), "%s", canonical.c_str());
    return true;
}

// ---- SendInput --------------------------------------------------------

static void FillInput(INPUT& in, const KeyDef& k, bool up) {
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_KEYBOARD;
    if (k.kind == 2) {
        in.ki.wVk = k.code;
        in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    } else {
        in.ki.wScan = k.code;
        in.ki.dwFlags = KEYEVENTF_SCANCODE
            | (k.kind == 1 ? KEYEVENTF_EXTENDEDKEY : 0)
            | (up ? KEYEVENTF_KEYUP : 0);
    }
}

// Press: all keys down in order. Release: all keys up in reverse order.
// One SendInput call, so a combo goes out atomically.
static void SendCombo(const CompiledBinding& b, bool down) {
    if (b.count <= 0) return;
    INPUT in[MAX_COMBO];
    int n = 0;
    if (down) for (int i = 0; i < b.count; i++) FillInput(in[n++], b.keys[i], false);
    else for (int i = b.count - 1; i >= 0; i--) FillInput(in[n++], b.keys[i], true);
    SendInput((UINT)n, in, sizeof(INPUT));
}

// Typematic repeat re-sends the LAST key's press only (like a real keyboard).
static void SendRepeat(const CompiledBinding& b) {
    if (b.count <= 0) return;
    INPUT in;
    FillInput(in, b.keys[b.count - 1], false);
    SendInput(1, &in, sizeof(INPUT));
}

// ---- Profiles ---------------------------------------------------------

struct Profile {
    std::string name;
    std::string bindings[NUM_KEYS];
};

static std::mutex g_profileMutex;
static std::vector<Profile> g_profiles;
static int g_activeIndex = 0;

static const char* DEFAULT_BINDINGS[NUM_KEYS] = { "A", "S", "D" };

static Profile DefaultProfile(const std::string& name) {
    Profile p;
    p.name = name;
    for (int k = 0; k < NUM_KEYS; k++) p.bindings[k] = DEFAULT_BINDINGS[k];
    return p;
}

// Profile names go straight into JSON and HTML, so keep them to a safe set.
static std::string SanitizeName(const std::string& in) {
    std::string out;
    for (char c : in) {
        unsigned char u = (unsigned char)c;
        if (std::isalnum(u) || c == ' ' || c == '-' || c == '_' || c == '.' || c == '#' || c == '+' || c == '(' || c == ')')
            out += c;
        if ((int)out.size() >= MAX_NAME_LEN) break;
    }
    size_t a = out.find_first_not_of(' ');
    if (a == std::string::npos) return "";
    size_t b = out.find_last_not_of(' ');
    return out.substr(a, b - a + 1);
}

static std::string JsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        unsigned char u = (unsigned char)c;
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (u < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", u); out += b; }
        else out += c;
    }
    return out;
}

static std::string JsonGetString(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos);
    if (pos == std::string::npos) return "";
    pos++;
    std::string result;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) { result += json[pos + 1]; pos += 2; }
        else { result += json[pos]; pos++; }
    }
    return result;
}

static int JsonGetInt(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return -1;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return -1;
    pos++;
    while (pos < json.size() && json[pos] == ' ') pos++;
    size_t end = pos;
    while (end < json.size() && (std::isdigit((unsigned char)json[end]) || json[end] == '-')) end++;
    if (end == pos) return -1;
    try { return std::stoi(json.substr(pos, end - pos)); }
    catch (...) { return -1; }
}

static std::string ProfilesToJson() {
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < g_profiles.size(); i++) {
        const Profile& p = g_profiles[i];
        out << "{\"name\":\"" << JsonEscape(p.name) << "\",\"bindings\":[";
        for (int k = 0; k < NUM_KEYS; k++) {
            out << "\"" << JsonEscape(p.bindings[k]) << "\"";
            if (k < NUM_KEYS - 1) out << ",";
        }
        out << "]}";
        if (i + 1 < g_profiles.size()) out << ",";
    }
    out << "]";
    return out.str();
}

// Writes to a temp file then swaps it in, so a crash mid-save can never
// leave a half-written profiles.json behind.
static void WriteFileAtomic(const fs::path& target, const std::string& content) {
    fs::path tmp = target;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) return;
        f << content;
    }
    MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

// Caller must hold g_profileMutex.
// ---- Click counters ------------------------------------------------------
// The total lives here (not in the page) so presses are counted even when the page is closed.
// CPS is worked out by the page from the live press events. config\counters.json = {"total":N,"since":unixSeconds}.
static std::atomic<uint64_t> g_clicksTotal{ 0 }, g_clicksSaved{ 0 };
static std::atomic<int64_t> g_clicksSince{ 0 };
static std::mutex g_clicksMx;

static void SaveCounters(bool force) {
    uint64_t t = g_clicksTotal.load();
    if (!force && t == g_clicksSaved.load()) return;
    std::lock_guard<std::mutex> lk(g_clicksMx);
    std::ostringstream o;
    o << "{\"total\":" << t << ",\"since\":" << g_clicksSince.load() << "}";
    WriteFileAtomic(g_configDir / L"counters.json", o.str());
    g_clicksSaved = t;
}
static void LoadCounters() {
    std::string txt;
    {
        std::ifstream f(g_configDir / L"counters.json", std::ios::binary);
        if (f.is_open()) { std::ostringstream ss; ss << f.rdbuf(); txt = ss.str(); }
    }
    auto num = [&](const char* key) -> unsigned long long {
        size_t p = txt.find(key);
        if (p == std::string::npos) return 0;
        p = txt.find(':', p);
        return p == std::string::npos ? 0 : strtoull(txt.c_str() + p + 1, nullptr, 10);
    };
    g_clicksTotal = num("\"total\"");
    g_clicksSince = (int64_t)num("\"since\"");
    if (g_clicksSince.load() <= 0) g_clicksSince = (int64_t)time(nullptr);
    g_clicksSaved = g_clicksTotal.load();
}
static void ResetCounters() {
    g_clicksTotal = 0;
    g_clicksSince = (int64_t)time(nullptr);
    SaveCounters(true);
}
static void CounterSaverLoop() { // writes the file a few seconds after the number changed, never from the key path
    while (g_running) {
        for (int i = 0; i < 50 && g_running; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        SaveCounters(false);
    }
}

static void SaveProfilesLocked() {
    WriteFileAtomic(g_profilesPath, ProfilesToJson());
    WriteFileAtomic(g_activeIndexPath, std::to_string(g_activeIndex));
}

// Parses exactly what SaveProfilesLocked writes (names are sanitized so
// they can never contain braces/brackets/quotes). Anything unreadable
// falls back to defaults instead of failing.
static void LoadProfilesLocked() {
    g_profiles.clear();

    std::ifstream file(g_profilesPath);
    if (file.is_open()) {
        std::ostringstream ss;
        ss << file.rdbuf();
        std::string content = ss.str();

        size_t pos = 0;
        while ((int)g_profiles.size() < MAX_PROFILES) {
            size_t objStart = content.find('{', pos);
            if (objStart == std::string::npos) break;
            size_t objEnd = content.find(']', objStart);
            if (objEnd == std::string::npos) break;
            objEnd = content.find('}', objEnd);
            if (objEnd == std::string::npos) break;

            std::string obj = content.substr(objStart, objEnd - objStart + 1);
            pos = objEnd + 1;

            Profile p = DefaultProfile(SanitizeName(JsonGetString(obj, "name")));
            if (p.name.empty()) continue;

            size_t bPos = obj.find("\"bindings\"");
            size_t arrStart = bPos == std::string::npos ? std::string::npos : obj.find('[', bPos);
            size_t arrClose = arrStart == std::string::npos ? std::string::npos : obj.find(']', arrStart);
            if (arrClose != std::string::npos) {
                std::string arr = obj.substr(arrStart, arrClose - arrStart + 1);
                size_t qPos = 0;
                for (int k = 0; k < NUM_KEYS; k++) {
                    size_t q1 = arr.find('"', qPos);
                    if (q1 == std::string::npos) break;
                    size_t q2 = arr.find('"', q1 + 1);
                    if (q2 == std::string::npos) break;
                    std::string val = NormalizeBinding(arr.substr(q1 + 1, q2 - q1 - 1));
                    CompiledBinding cb; std::string err;
                    if (CompileBinding(val, cb, err)) p.bindings[k] = cb.text; // canonical form; invalid entries keep the default
                    qPos = q2 + 1;
                }
            }
            g_profiles.push_back(p);
        }
    }

    if (g_profiles.empty()) g_profiles.push_back(DefaultProfile("Profile 1"));

    g_activeIndex = 0;
    std::ifstream idxFile(g_activeIndexPath);
    if (idxFile.is_open()) {
        int idx;
        if (idxFile >> idx && idx >= 0 && idx < (int)g_profiles.size()) g_activeIndex = idx;
    }
}

// ---- Published bindings (what the key path reads) ---------------------
// The device thread never takes g_profileMutex. Whenever the active
// profile changes, its bindings are compiled into the next slot of a small
// ring and a pointer to it is published atomically. A slot is only ever
// reused 16 publishes later, far longer than the key path holds a pointer.

struct BindingSet { CompiledBinding b[NUM_KEYS]; };
static BindingSet g_sets[16];
static std::atomic<BindingSet*> g_bindings{ &g_sets[0] };
static int g_setCursor = 0;

// Caller must hold g_profileMutex.
static void PublishBindingsLocked() {
    int next = (g_setCursor + 1) % 16;
    BindingSet& s = g_sets[next];
    const Profile& p = g_profiles[g_activeIndex];
    for (int k = 0; k < NUM_KEYS; k++) {
        std::string err;
        if (!CompileBinding(p.bindings[k], s.b[k], err)) s.b[k] = CompiledBinding(); // sends nothing
    }
    g_setCursor = next;
    g_bindings.store(&s, std::memory_order_release);
}

// ---- Event ring (debugger + visualizer feed) --------------------------
// Written by the device thread (and rarely others) AFTER the keystroke has
// been sent; read by the website threads. Lock-free for writers.

enum EvType : char {
    EVT_PRESS = 'P', EVT_RELEASE = 'R', EVT_TIMEOUT = 'X',
    EVT_CONNECT = 'C', EVT_DISCONNECT = 'D', EVT_INFO = 'I'
};

struct EvSlot {
    std::atomic<uint64_t> seq{ 0 }; // written LAST: index+1 means "slot holds event #index"
    uint64_t tUs = 0;
    uint32_t latUs = 0;
    char type = 0;
    int8_t key = -1;
    char msg[40] = { 0 };
};

constexpr int EVENT_RING_SIZE = 4096;
static EvSlot g_ring[EVENT_RING_SIZE];
static std::atomic<uint64_t> g_evHead{ 0 };
static std::mutex g_evMx;
static std::condition_variable g_evCv;

static void PushEvent(char type, int key, uint32_t latUs, const char* msg) {
    uint64_t t = QpcToUs(QpcNow());
    uint64_t n = g_evHead.fetch_add(1);
    EvSlot& s = g_ring[n % EVENT_RING_SIZE];
    s.tUs = t; s.latUs = latUs; s.type = type; s.key = (int8_t)key;
    snprintf(s.msg, sizeof(s.msg), "%s", msg ? msg : "");
    s.seq.store(n + 1, std::memory_order_release);
    { std::lock_guard<std::mutex> lk(g_evMx); } // closes the missed-wakeup window; uncontended = nanoseconds
    g_evCv.notify_all();
}

struct EvCopy { uint64_t seq, tUs; uint32_t latUs; char type; int key; char msg[40]; };

// 0 = ok, 1 = not written yet (retry shortly), 2 = overwritten (lapped)
static int ReadEvent(uint64_t n, EvCopy& out) {
    EvSlot& s = g_ring[n % EVENT_RING_SIZE];
    uint64_t seq = s.seq.load(std::memory_order_acquire);
    if (seq < n + 1) return 1;
    if (seq > n + 1) return 2;
    out.seq = n; out.tUs = s.tUs; out.latUs = s.latUs; out.type = s.type; out.key = s.key;
    memcpy(out.msg, s.msg, sizeof(out.msg));
    out.msg[sizeof(out.msg) - 1] = 0;
    if (s.seq.load(std::memory_order_acquire) != n + 1) return 2; // overwritten while copying
    return 0;
}

static void Info(const std::string& message) {
    Log(message);
    PushEvent(EVT_INFO, -1, 0, message.c_str());
}

// ---- Debugger stats ---------------------------------------------------

static std::atomic<uint32_t> g_latLast{ 0 }, g_latMax{ 0 };
static std::atomic<uint64_t> g_latSum{ 0 }, g_latCount{ 0 };
static std::atomic<uint32_t> g_reportsPerSec{ 0 };
static std::atomic<bool> g_connected{ false };
static std::atomic<bool> g_hasLogo{ false };   // config\logo.png exists
static std::atomic<int> g_logoVer{ 0 };        // bumps whenever the logo changes (cache-busting for the page)
static std::mutex g_portMx;
static std::string g_portNameNarrow;
static std::atomic<bool> g_heldView[NUM_KEYS];

static void RecordLatency(uint32_t us) {
    g_latLast = us;
    g_latSum += us;
    g_latCount++;
    if (us > g_latMax) g_latMax = us; // single writer (device thread)
}

static void ResetLatencyStats() {
    g_latLast = 0; g_latMax = 0; g_latSum = 0; g_latCount = 0;
}

// ---- Typematic timing (ported from the earlier C# app, Backend.cs) -----

static double g_initialDelay = 0.25;
static double g_repeatInterval = 0.033;

static void InitTypematicTiming() {
    int delayIndex = 1, speedIndex = 31;
    SystemParametersInfoA(SPI_GETKEYBOARDDELAY, 0, &delayIndex, 0);
    SystemParametersInfoA(SPI_GETKEYBOARDSPEED, 0, &speedIndex, 0);
    g_initialDelay = 0.25 * (delayIndex + 1);
    g_repeatInterval = 1.0 / (2.5 + (speedIndex / 31.0) * 27.5);
}

// ---- Device thread state (touched ONLY by the device thread) ----------

static bool g_held[NUM_KEYS];            // keypad says this key is down
static bool g_sent[NUM_KEYS];            // we sent the key-down to Windows (false while paused)
static double g_lastSeen[NUM_KEYS];
static double g_nextRepeat[NUM_KEYS];
static CompiledBinding g_cur[NUM_KEYS];  // binding captured at press time, so release always matches
static std::atomic<bool> g_anySent{ false };

static void RefreshSentFlag() {
    bool any = false;
    for (int i = 0; i < NUM_KEYS; i++) any = any || g_sent[i];
    g_anySent = any;
}

static void ReleaseKey(int idx) {
    if (g_sent[idx]) SendCombo(g_cur[idx], false);
    g_sent[idx] = false;
    g_held[idx] = false;
    g_nextRepeat[idx] = -1;
    g_heldView[idx] = false;
    RefreshSentFlag();
}

static void ReleaseAllKeys() {
    for (int i = 0; i < NUM_KEYS; i++) if (g_held[i] || g_sent[i]) ReleaseKey(i);
}

static void HandleByte(BYTE b, double now, int64_t tRecv) {
    if (b < 1 || b > NUM_KEYS * 2) return; // identity replies / anything else
    int idx = (b - 1) / 2;
    bool isPress = (b & 1) != 0;

    g_lastSeen[idx] = now;

    if (isPress) {
        if (g_held[idx]) return; // keepalive while held
        g_held[idx] = true;
        g_heldView[idx] = true;
        g_nextRepeat[idx] = now + g_initialDelay;
        g_cur[idx] = g_bindings.load(std::memory_order_acquire)->b[idx];

        uint32_t lat = 0;
        if (!g_paused) {
            SendCombo(g_cur[idx], true);
            g_sent[idx] = true;
            RefreshSentFlag();
            lat = (uint32_t)((QpcNow() - tRecv) * 1000000.0 / (double)g_qpcFreq.QuadPart);
            RecordLatency(lat);
        }
        PushEvent(EVT_PRESS, idx, lat, g_cur[idx].text);
        g_clicksTotal.fetch_add(1, std::memory_order_relaxed); // real presses only (keepalives return above)
    } else {
        if (!g_held[idx]) return;
        uint32_t lat = 0;
        if (g_sent[idx]) {
            SendCombo(g_cur[idx], false);
            lat = (uint32_t)((QpcNow() - tRecv) * 1000000.0 / (double)g_qpcFreq.QuadPart);
            RecordLatency(lat);
        }
        g_sent[idx] = false;
        g_held[idx] = false;
        g_nextRepeat[idx] = -1;
        g_heldView[idx] = false;
        RefreshSentFlag();
        PushEvent(EVT_RELEASE, idx, lat, g_cur[idx].text);
    }
}

// Runs on every wake of the device thread: stale-key timeout, pause /
// shutdown release, typematic repeat.
static void ServiceTick(double now) {
    bool blocked = g_paused.load() || !g_running.load();
    for (int idx = 0; idx < NUM_KEYS; idx++) {
        if (!g_held[idx]) continue;

        if (now - g_lastSeen[idx] > KEY_TIMEOUT_SECONDS) {
            ReleaseKey(idx);
            PushEvent(EVT_TIMEOUT, idx, 0, "stale key force-released");
            continue;
        }
        if (!g_sent[idx]) continue;

        if (blocked) { // paused (or quitting) mid-hold: let go of the key
            SendCombo(g_cur[idx], false);
            g_sent[idx] = false;
            RefreshSentFlag();
            continue;
        }
        if (g_nextRepeat[idx] >= 0 && now >= g_nextRepeat[idx]) {
            SendRepeat(g_cur[idx]);
            g_nextRepeat[idx] = now + g_repeatInterval;
        }
    }
}

// ---- Serial port ------------------------------------------------------

static HANDLE OpenPortRaw(const std::wstring& portName) {
    std::wstring fullName = L"\\\\.\\" + portName;
    HANDLE h = CreateFileW(fullName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    DCB dcb = {};
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(h, &dcb)) { CloseHandle(h); return INVALID_HANDLE_VALUE; }
    dcb.BaudRate = BAUD_RATE;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    if (!SetCommState(h, &dcb)) { CloseHandle(h); return INVALID_HANDLE_VALUE; }

    // MAXDWORD/MAXDWORD/constant: ReadFile returns immediately if bytes are
    // waiting, otherwise BLOCKS until the next byte arrives (waking the
    // instant it does) or READ_WAKE_MS passes. Zero polling, zero spin.
    COMMTIMEOUTS to = {};
    to.ReadIntervalTimeout = MAXDWORD;
    to.ReadTotalTimeoutMultiplier = MAXDWORD;
    to.ReadTotalTimeoutConstant = READ_WAKE_MS;
    to.WriteTotalTimeoutConstant = 50;
    if (!SetCommTimeouts(h, &to)) { CloseHandle(h); return INVALID_HANDLE_VALUE; }
    return h;
}

static std::vector<std::wstring> ListComPorts() {
    std::vector<std::wstring> result;
    wchar_t target[5000];
    for (int i = 1; i <= 256; i++) {
        std::wstring name = L"COM" + std::to_wstring(i);
        if (QueryDosDeviceW(name.c_str(), target, 5000) != 0) result.push_back(name);
    }
    return result;
}

static bool ProbeHandle(HANDLE h) {
    std::this_thread::sleep_for(std::chrono::milliseconds(120)); // let the board settle after open
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);

    BYTE query = ID_QUERY_BYTE;
    DWORD written = 0;
    if (!WriteFile(h, &query, 1, &written, nullptr) || written != 1) return false;

    std::string reply;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < deadline) {
        char buf[32];
        DWORD n = 0;
        if (!ReadFile(h, buf, sizeof(buf), &n, nullptr)) return false;
        if (n > 0) {
            reply.append(buf, n);
            if (reply.find(DEVICE_ID) != std::string::npos && reply.find('\n') != std::string::npos) return true;
        }
    }
    return false;
}

// Finds the board and returns the OPEN handle (no close/reopen, so the
// board is never reset twice).
static bool FindDevice(HANDLE& outHandle, std::wstring& outName) {
    for (const auto& name : ListComPorts()) {
        if (!g_running) return false;
        HANDLE h = OpenPortRaw(name);
        if (h == INVALID_HANDLE_VALUE) continue;
        if (ProbeHandle(h)) {
            PurgeComm(h, PURGE_RXCLEAR);
            outHandle = h; outName = name;
            return true;
        }
        CloseHandle(h);
    }
    return false;
}

static void SleepWhileRunning(int ms) {
    for (int waited = 0; waited < ms && g_running; waited += 50)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

// The hot loop. ReadFile blocks until a byte arrives, so there is no
// polling delay; latency is just USB + this thread's wake-up.
static void RunSession(HANDLE h) {
    BYTE buf[64];
    uint32_t reportCount = 0;
    double lastRateT = QpcToSec(QpcNow());
    double lastLinkCheck = lastRateT;

    while (g_running) {
        DWORD n = 0;
        BOOL ok = ReadFile(h, buf, sizeof(buf), &n, nullptr);
        int64_t tRecv = QpcNow(); // timestamp the instant the read returns
        double now = QpcToSec(tRecv);

        if (!ok) { Log("Serial read failed (device unplugged?)."); return; }

        if (n > 0) {
            reportCount += n;
            for (DWORD i = 0; i < n; i++) HandleByte(buf[i], now, tRecv);
        }

        ServiceTick(now);

        if (now - lastRateT >= 1.0) {
            g_reportsPerSec = reportCount;
            reportCount = 0;
            lastRateT = now;
        }
        if (now - lastLinkCheck >= 0.5) { // the keypad is silent when idle, so verify the link actively
            lastLinkCheck = now;
            DWORD errors = 0; COMSTAT stat;
            if (!ClearCommError(h, &errors, &stat)) { Log("Serial link lost."); return; }
        }
    }
}

static void DeviceThread() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    while (g_running) {
        HANDLE h = INVALID_HANDLE_VALUE;
        std::wstring name;
        if (!FindDevice(h, name)) { SleepWhileRunning(1000); continue; }

        std::string narrow(name.begin(), name.end());
        { std::lock_guard<std::mutex> lk(g_portMx); g_portNameNarrow = narrow; }
        g_connected = true;
        Log("Connected: 3-Key on " + narrow);
        PushEvent(EVT_CONNECT, -1, 0, narrow.c_str());

        RunSession(h);

        ReleaseAllKeys();
        CloseHandle(h);
        g_connected = false;
        g_reportsPerSec = 0;
        { std::lock_guard<std::mutex> lk(g_portMx); g_portNameNarrow.clear(); }
        Log("Disconnected.");
        PushEvent(EVT_DISCONNECT, -1, 0, "device disconnected");
    }
    ReleaseAllKeys();
}

// ---- State JSON -------------------------------------------------------

// Caller MUST hold g_profileMutex (std::mutex is not recursive).
static std::string BuildStateJsonLocked(const std::string& error) {
    std::ostringstream out;
    out << "{\"version\":\"" << VERSION_TEXT << "\",\"profiles\":" << ProfilesToJson()
        << ",\"activeIndex\":" << g_activeIndex << ",\"keyHeld\":[";
    for (int k = 0; k < NUM_KEYS; k++) {
        out << (g_heldView[k].load() ? "true" : "false");
        if (k < NUM_KEYS - 1) out << ",";
    }
    std::string port;
    { std::lock_guard<std::mutex> lk(g_portMx); port = g_portNameNarrow; }

    uint64_t cnt = g_latCount.load();
    uint64_t avg = cnt ? g_latSum.load() / cnt : 0;

    out << "],\"connected\":" << (g_connected ? "true" : "false")
        << ",\"port\":\"" << JsonEscape(port) << "\""
        << ",\"paused\":" << (g_paused ? "true" : "false")
        << ",\"maxProfiles\":" << MAX_PROFILES
        << ",\"reportsPerSec\":" << g_reportsPerSec.load()
        << ",\"latLastUs\":" << g_latLast.load()
        << ",\"latAvgUs\":" << avg
        << ",\"latMaxUs\":" << g_latMax.load()
        << ",\"latCount\":" << cnt
        << ",\"clicksTotal\":" << g_clicksTotal.load()
        << ",\"clicksSince\":" << g_clicksSince.load()
        << ",\"hasLogo\":" << (g_hasLogo ? "true" : "false")
        << ",\"logoVer\":" << g_logoVer.load()
        << ",\"holdDelayMs\":" << (int)(g_initialDelay * 1000)
        << ",\"repeatMs\":" << (int)(g_repeatInterval * 1000)
        << ",\"error\":\"" << JsonEscape(error) << "\"}";
    return out.str();
}

static std::string BuildStateJson() {
    std::lock_guard<std::mutex> lock(g_profileMutex);
    return BuildStateJsonLocked("");
}

// ---- Website ----------------------------------------------------------

static std::string WebsiteHtml(); // defined at the bottom of the file
static void QuitApp();            // defined after the tray code

// ---- HTTP server (raw Winsock, loopback only) -------------------------

static std::string HttpResponse(const std::string& status, const std::string& contentType, const std::string& body) {
    std::ostringstream out;
    out << "HTTP/1.1 " << status << "\r\n"
        << "Content-Type: " << contentType << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Cache-Control: no-store\r\n"
        << "Connection: close\r\n\r\n"
        << body;
    return out.str();
}

static bool SendAll(SOCKET s, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int n = send(s, data.c_str() + sent, (int)(data.size() - sent), 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

static std::string ReadHttpRequest(SOCKET client) {
    std::string data;
    char buf[4096];
    while (data.find("\r\n\r\n") == std::string::npos) {
        int n = recv(client, buf, sizeof(buf), 0);
        if (n <= 0) return "";
        data.append(buf, (size_t)n);
        if (data.size() > 65536) return "";
    }
    size_t headerEnd = data.find("\r\n\r\n") + 4;
    std::string lower = data.substr(0, headerEnd);
    for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
    size_t clPos = lower.find("content-length:");
    if (clPos != std::string::npos) {
        int contentLength = std::atoi(lower.c_str() + clPos + 15);
        if (contentLength < 0 || contentLength > 1100000) return ""; // ~1 MB: room for a logo PNG; JSON bodies are tiny
        while ((int)(data.size() - headerEnd) < contentLength) {
            int n = recv(client, buf, sizeof(buf), 0);
            if (n <= 0) break;
            data.append(buf, (size_t)n);
        }
    }
    return data;
}

static std::string HeaderValueLower(const std::string& request, const std::string& nameLower) {
    size_t headerEnd = request.find("\r\n\r\n");
    std::string head = request.substr(0, headerEnd == std::string::npos ? request.size() : headerEnd);
    for (auto& c : head) c = (char)std::tolower((unsigned char)c);
    size_t pos = head.find("\r\n" + nameLower + ":");
    if (pos == std::string::npos) return "";
    pos += 2 + nameLower.size() + 1;
    size_t end = head.find("\r\n", pos);
    std::string v = head.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    size_t a = v.find_first_not_of(" \t");
    size_t b = v.find_last_not_of(" \t");
    return a == std::string::npos ? "" : v.substr(a, b - a + 1);
}

static std::string EvToJson(const EvCopy& e) {
    std::ostringstream o;
    o << "{\"s\":" << e.seq << ",\"t\":" << e.tUs << ",\"y\":\"" << e.type << "\",\"k\":" << e.key
      << ",\"l\":" << e.latUs << ",\"m\":\"" << JsonEscape(e.msg) << "\"}";
    return o.str();
}

static std::atomic<int> g_sseClients{ 0 };
static std::mutex g_themeMutex;
static std::mutex g_logoMutex;
static std::mutex g_cfgMutex; // config\layout.json and config\settings.json

// Accepts only a real PNG of sane size: signature + IHDR, <= 1 MB, <= 1024x1024.
static bool IsValidLogoPng(const std::string& d) {
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    if (d.size() < 33 || d.size() > 1048576) return false;
    if (memcmp(d.data(), sig, 8) != 0) return false;
    if (memcmp(d.data() + 12, "IHDR", 4) != 0) return false;
    auto be32 = [&](size_t o) {
        return ((uint32_t)(unsigned char)d[o] << 24) | ((uint32_t)(unsigned char)d[o + 1] << 16)
             | ((uint32_t)(unsigned char)d[o + 2] << 8) | (uint32_t)(unsigned char)d[o + 3];
    };
    uint32_t w = be32(16), h = be32(20);
    return w >= 1 && h >= 1 && w <= 1024 && h <= 1024;
}

// Server-Sent Events: pushes each event to the page the moment it happens.
static void HandleSse(SOCKET client) {
    if (g_sseClients.load() >= MAX_SSE_CLIENTS) {
        SendAll(client, HttpResponse("503 Service Unavailable", "text/plain", "too many viewers"));
        closesocket(client);
        return;
    }
    g_sseClients++;

    BOOL nodelay = TRUE;
    setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
    DWORD sndTimeout = 3000;
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sndTimeout, sizeof(sndTimeout));

    bool alive = SendAll(client,
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\nConnection: keep-alive\r\n\r\n");

    if (alive) {
        std::ostringstream snap;
        snap << "data: {\"y\":\"S\",\"held\":[";
        for (int k = 0; k < NUM_KEYS; k++) { snap << (g_heldView[k].load() ? "true" : "false"); if (k < NUM_KEYS - 1) snap << ","; }
        snap << "]}\n\n";
        alive = SendAll(client, snap.str());
    }

    uint64_t head = g_evHead.load();
    uint64_t next = head > 40 ? head - 40 : 0; // replay a little history into the debugger log

    while (alive && g_running) {
        head = g_evHead.load();
        while (alive && next < head) {
            EvCopy e;
            int r = ReadEvent(next, e);
            if (r == 1) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; } // writer mid-publish
            if (r == 0) alive = SendAll(client, "data: " + EvToJson(e) + "\n\n");
            next++;
        }
        if (!alive) break;

        std::unique_lock<std::mutex> lk(g_evMx);
        bool woke = g_evCv.wait_for(lk, std::chrono::seconds(2), [&] { return g_evHead.load() > next || !g_running; });
        lk.unlock();
        if (!woke) alive = SendAll(client, ": ping\n\n"); // also detects a closed tab
    }

    g_sseClients--;
    closesocket(client);
}

static bool HostAllowed(const std::string& hostLower) {
    return hostLower == "localhost:9191" || hostLower == "127.0.0.1:9191";
}

static void HandleRequest(SOCKET client) {
    DWORD rcvTimeout = 3000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rcvTimeout, sizeof(rcvTimeout));

    std::string request = ReadHttpRequest(client);
    if (request.empty()) { closesocket(client); return; }

    size_t lineEnd = request.find("\r\n");
    std::string requestLine = request.substr(0, lineEnd);
    size_t methodEnd = requestLine.find(' ');
    if (methodEnd == std::string::npos) { closesocket(client); return; }
    std::string method = requestLine.substr(0, methodEnd);
    size_t pathEnd = requestLine.find(' ', methodEnd + 1);
    std::string path = requestLine.substr(methodEnd + 1, pathEnd == std::string::npos ? std::string::npos : pathEnd - methodEnd - 1);

    size_t queryPos = path.find('?');
    if (queryPos != std::string::npos) path.resize(queryPos); // ignore ?v=... cache-busters

    std::string body;
    size_t headerEnd = request.find("\r\n\r\n");
    if (headerEnd != std::string::npos) body = request.substr(headerEnd + 4);

    // This server can type on your keyboard, so refuse anything that isn't
    // our own page talking to us: wrong Host (DNS rebinding) or a POST
    // without our custom header (a foreign website's form/fetch can't add it).
    if (!HostAllowed(HeaderValueLower(request, "host"))) {
        SendAll(client, HttpResponse("403 Forbidden", "text/plain", "forbidden"));
        closesocket(client);
        return;
    }
    if (method == "POST" && HeaderValueLower(request, "x-espdevice") != "1") {
        SendAll(client, HttpResponse("403 Forbidden", "text/plain", "forbidden"));
        closesocket(client);
        return;
    }

    if (method == "GET" && path == "/events") { HandleSse(client); return; }

    std::string response;

    if (method == "GET" && path == "/") {
        response = HttpResponse("200 OK", "text/html; charset=utf-8", WebsiteHtml());
    }
    else if (method == "GET" && path == "/api/theme") {
        std::string theme;
        {
            std::lock_guard<std::mutex> lk(g_themeMutex);
            std::ifstream f(g_configDir / L"theme.json", std::ios::binary);
            if (f.is_open()) { std::ostringstream ss; ss << f.rdbuf(); theme = ss.str(); }
        }
        response = HttpResponse("200 OK", "application/json", theme.empty() ? "{}" : theme);
    }
    else if (method == "POST" && path == "/api/theme") {
        // The page validates every colour before use; the service only stores the JSON (size-capped).
        if (body.empty() || body.size() > 60000 || body[0] != '{') {
            response = HttpResponse("400 Bad Request", "application/json", "{\"ok\":false}");
        } else {
            std::lock_guard<std::mutex> lk(g_themeMutex);
            WriteFileAtomic(g_configDir / L"theme.json", body);
            response = HttpResponse("200 OK", "application/json", "{\"ok\":true}");
        }
    }
    else if (method == "GET" && (path == "/api/layout" || path == "/api/settings")) {
        // Window layout and page settings: stored as JSON exactly as the page sent it (the page validates everything it reads).
        const wchar_t* fn = (path == "/api/layout") ? L"layout.json" : L"settings.json";
        std::string txt;
        {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            std::ifstream f(g_configDir / fn, std::ios::binary);
            if (f.is_open()) { std::ostringstream ss; ss << f.rdbuf(); txt = ss.str(); }
        }
        response = HttpResponse("200 OK", "application/json", txt.empty() ? "{}" : txt);
    }
    else if (method == "POST" && (path == "/api/layout" || path == "/api/settings")) {
        const size_t cap = (path == "/api/layout") ? 100000 : 20000;
        if (body.empty() || body.size() > cap || body[0] != '{') {
            response = HttpResponse("400 Bad Request", "application/json", "{\"ok\":false}");
        } else {
            const wchar_t* fn = (path == "/api/layout") ? L"layout.json" : L"settings.json";
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            WriteFileAtomic(g_configDir / fn, body);
            response = HttpResponse("200 OK", "application/json", "{\"ok\":true}");
        }
    }
    else if (method == "GET" && path.rfind("/api/icon/", 0) == 0) {
        // Minimized-window icons: config\icons\<Name>_icon.png (for example Profile_icon.png). 404 = the page shows its placeholder.
        std::string name = path.substr(10);
        bool okName = !name.empty() && name.size() <= 24;
        for (char c : name) if (!std::isalnum((unsigned char)c) && c != '_') okName = false;
        std::string png;
        if (okName) {
            std::wstring wname(name.begin(), name.end());
            std::ifstream f(g_configDir / L"icons" / (wname + L"_icon.png"), std::ios::binary);
            if (f.is_open()) { std::ostringstream ss; ss << f.rdbuf(); png = ss.str(); }
        }
        if (png.size() < 8 || png.size() > 1048576 || png.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) != 0)
            response = HttpResponse("204 No Content", "text/plain", ""); // no icon file: the page keeps its placeholder (204 = no console noise)
        else
            response = HttpResponse("200 OK", "image/png", png);
    }
    else if (method == "GET" && path == "/api/logo") {
        std::string png;
        {
            std::lock_guard<std::mutex> lk(g_logoMutex);
            std::ifstream f(g_configDir / L"logo.png", std::ios::binary);
            if (f.is_open()) { std::ostringstream ss; ss << f.rdbuf(); png = ss.str(); }
        }
        if (png.empty()) response = HttpResponse("404 Not Found", "text/plain", "no logo");
        else response = HttpResponse("200 OK", "image/png", png);
    }
    else if (method == "GET" && path == "/api/defaultlogo") {
        // Default logo: config\default_logo.png if the user dropped one in, else the logo built into the exe.
        std::string png;
        std::ifstream f(g_configDir / L"default_logo.png", std::ios::binary);
        if (f.is_open()) { std::ostringstream ss; ss << f.rdbuf(); png = ss.str(); }
        if (png.empty()) png.assign((const char*)DEFAULT_LOGO_PNG, DEFAULT_LOGO_PNG_SIZE);
        response = HttpResponse("200 OK", "image/png", png);
    }
    else if (method == "POST" && path == "/api/logo") {
        std::string err;
        if (!IsValidLogoPng(body)) err = "that is not a usable PNG (max 1 MB, 1024x1024 px)";
        else {
            std::lock_guard<std::mutex> lk(g_logoMutex);
            WriteFileAtomic(g_configDir / L"logo.png", body);
            g_hasLogo = true;
            g_logoVer++;
        }
        std::lock_guard<std::mutex> lock(g_profileMutex);
        response = HttpResponse("200 OK", "application/json", BuildStateJsonLocked(err));
    }
    else if (method == "POST" && path == "/api/logo/reset") {
        {
            std::lock_guard<std::mutex> lk(g_logoMutex);
            std::error_code ec;
            fs::remove(g_configDir / L"logo.png", ec);
            g_hasLogo = false;
            g_logoVer++;
        }
        response = HttpResponse("200 OK", "application/json", BuildStateJson());
    }
    else if (method == "GET" && path == "/api/art") {
        // Title art lives in config\art.txt (plain text); the page trims and fits it.
        std::string art;
        std::ifstream f(g_configDir / L"art.txt", std::ios::binary);
        if (f.is_open()) {
            std::ostringstream ss; ss << f.rdbuf();
            art = ss.str();
            if (art.size() > 20000) art.resize(20000);
        }
        response = HttpResponse("200 OK", "text/plain; charset=utf-8", art);
    }
    else if (method == "GET" && path == "/api/state") {
        response = HttpResponse("200 OK", "application/json", BuildStateJson());
    }
    else if (method == "POST" && path == "/api/select") {
        int idx = JsonGetInt(body, "index");
        std::lock_guard<std::mutex> lock(g_profileMutex);
        if (idx >= 0 && idx < (int)g_profiles.size()) {
            g_activeIndex = idx;
            PublishBindingsLocked();
            SaveProfilesLocked();
        }
        response = HttpResponse("200 OK", "application/json", BuildStateJsonLocked(""));
    }
    else if (method == "POST" && path == "/api/rebind") {
        int idx = JsonGetInt(body, "index");
        int key = JsonGetInt(body, "key");
        std::string value = NormalizeBinding(JsonGetString(body, "value"));
        std::string err;
        std::lock_guard<std::mutex> lock(g_profileMutex);
        if (idx < 0 || idx >= (int)g_profiles.size() || key < 0 || key >= NUM_KEYS) {
            err = "bad profile or key index";
        } else {
            CompiledBinding cb;
            if (CompileBinding(value, cb, err)) {
                g_profiles[idx].bindings[key] = cb.text; // canonical, e.g. "?" is stored as SHIFT+SLASH
                if (idx == g_activeIndex) PublishBindingsLocked();
                SaveProfilesLocked();
            }
        }
        response = HttpResponse("200 OK", "application/json", BuildStateJsonLocked(err));
    }
    else if (method == "POST" && path == "/api/add") {
        std::string name = SanitizeName(JsonGetString(body, "name"));
        std::string err;
        std::lock_guard<std::mutex> lock(g_profileMutex);
        if (name.empty()) err = "name needs letters or numbers";
        else if ((int)g_profiles.size() >= MAX_PROFILES) err = "profile limit reached";
        else {
            g_profiles.push_back(DefaultProfile(name));
            SaveProfilesLocked();
        }
        response = HttpResponse("200 OK", "application/json", BuildStateJsonLocked(err));
    }
    else if (method == "POST" && path == "/api/rename") {
        int idx = JsonGetInt(body, "index");
        std::string name = SanitizeName(JsonGetString(body, "name"));
        std::string err;
        std::lock_guard<std::mutex> lock(g_profileMutex);
        if (name.empty()) err = "name needs letters or numbers";
        else if (idx < 0 || idx >= (int)g_profiles.size()) err = "bad profile index";
        else { g_profiles[idx].name = name; SaveProfilesLocked(); }
        response = HttpResponse("200 OK", "application/json", BuildStateJsonLocked(err));
    }
    else if (method == "POST" && path == "/api/delete") {
        int idx = JsonGetInt(body, "index");
        std::string err;
        std::lock_guard<std::mutex> lock(g_profileMutex);
        if (g_profiles.size() <= 1) err = "cannot delete the last profile";
        else if (idx < 0 || idx >= (int)g_profiles.size()) err = "bad profile index";
        else {
            g_profiles.erase(g_profiles.begin() + idx);
            if (g_activeIndex >= (int)g_profiles.size()) g_activeIndex = (int)g_profiles.size() - 1;
            else if (g_activeIndex > idx) g_activeIndex--;
            PublishBindingsLocked();
            SaveProfilesLocked();
        }
        response = HttpResponse("200 OK", "application/json", BuildStateJsonLocked(err));
    }
    else if (method == "POST" && path == "/api/counters/reset") {
        ResetCounters();
        response = HttpResponse("200 OK", "application/json", BuildStateJson());
    }
    else if (method == "POST" && path == "/api/resetstats") {
        ResetLatencyStats();
        response = HttpResponse("200 OK", "application/json", BuildStateJson());
    }
    else if (method == "POST" && path == "/api/service") {
        std::string action = JsonGetString(body, "action");
        if (action == "pause") { g_paused = true; Info("Paused (web UI)."); }
        else if (action == "resume") { g_paused = false; Info("Resumed (web UI)."); }
        else if (action == "terminate") {
            Log("Terminate requested via web UI.");
            SendAll(client, HttpResponse("200 OK", "application/json", "{\"ok\":true}"));
            closesocket(client);
            QuitApp();
            return;
        }
        response = HttpResponse("200 OK", "application/json", BuildStateJson());
    }
    else {
        response = HttpResponse("404 Not Found", "text/plain", "Not found");
    }

    SendAll(client, response);
    closesocket(client);
}

static void AcceptLoop() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) { Log("WSAStartup failed - web UI unavailable."); return; }

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) { Log("Failed to create listening socket."); return; }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // loopback only: never reachable from the network
    addr.sin_port = htons((u_short)HTTP_PORT);

    if (bind(listener, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        Log("Failed to bind port " + std::to_string(HTTP_PORT) + " - already in use?");
        closesocket(listener);
        return;
    }
    listen(listener, SOMAXCONN);
    Log("Web UI listening on http://localhost:" + std::to_string(HTTP_PORT) + "/");

    while (g_running) {
        SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); continue; }
        std::thread([client]() {
            try { HandleRequest(client); }
            catch (const std::exception& e) { Log(std::string("ERROR in http-request: ") + e.what()); closesocket(client); }
            catch (...) { Log("ERROR in http-request: unknown exception"); closesocket(client); }
        }).detach();
    }
    closesocket(listener);
    WSACleanup();
}

// ---- Tray icon --------------------------------------------------------

static HWND g_trayWindow = nullptr;
static NOTIFYICONDATAW g_trayData = {};
static HICON g_trayIcon = nullptr;
static UINT g_taskbarCreatedMsg = 0;

static void OpenSettingsInBrowser() {
    std::wstring url = L"http://localhost:" + std::to_wstring(HTTP_PORT) + L"/";
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static void ShowTrayMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_TRAY_OPEN, L"Open Settings");
    AppendMenuW(menu, MF_STRING, ID_TRAY_PAUSE, g_paused ? L"Resume" : L"Pause");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_TERMINATE, L"Terminate");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd); // required so the menu closes when clicking away
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessage(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

static LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_taskbarCreatedMsg != 0 && msg == g_taskbarCreatedMsg) { // Explorer restarted: re-add the icon
        Shell_NotifyIconW(NIM_ADD, &g_trayData);
        return 0;
    }
    switch (msg) {
    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP) ShowTrayMenu(hwnd);
        else if (lParam == WM_LBUTTONDBLCLK) OpenSettingsInBrowser();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_TRAY_OPEN: OpenSettingsInBrowser(); break;
        case ID_TRAY_PAUSE:
            if (g_paused) { g_paused = false; Info("Resumed (tray)."); }
            else { g_paused = true; Info("Paused (tray)."); }
            break;
        case ID_TRAY_TERMINATE:
            Log("Terminate requested via tray menu.");
            QuitApp();
            break;
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void InitTrayIcon(HINSTANCE hInstance) {
    const wchar_t* className = L"ESPdeviceTrayWindow";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = className;
    RegisterClassW(&wc);

    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    g_trayWindow = CreateWindowW(className, L"ESPdevice", 0, 0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);

    // Tray icon from the exe's own resources (espdevice.rc: 101 ICON "espdevice.ico").
    g_trayIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(101), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!g_trayIcon) g_trayIcon = LoadIconW(nullptr, IDI_APPLICATION);

    g_trayData.cbSize = sizeof(NOTIFYICONDATAW);
    g_trayData.hWnd = g_trayWindow;
    g_trayData.uID = 1;
    g_trayData.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_trayData.uCallbackMessage = WM_TRAYICON;
    g_trayData.hIcon = g_trayIcon;
    wcscpy_s(g_trayData.szTip, L"ESPdevice");
    Shell_NotifyIconW(NIM_ADD, &g_trayData);
}

static void RemoveTrayIcon() {
    Shell_NotifyIconW(NIM_DELETE, &g_trayData);
    if (g_trayIcon) DestroyIcon(g_trayIcon);
    g_trayIcon = nullptr;
}

// Single clean exit path (tray + web UI): the device thread releases any
// held key (it sees g_running go false within one wake), then we exit.
static void QuitApp() {
    g_running = false;
    g_evCv.notify_all();
    SaveCounters(false);
    for (int i = 0; i < 40 && g_anySent.load(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    RemoveTrayIcon();
    ExitProcess(0);
}

// ---- Entry point ------------------------------------------------------

// Windows 11 may coarsen timers / throttle background processes with no
// visible window. Opt out so repeat timing stays accurate. Looked up at
// runtime so it still builds and runs on older Windows.
static void DisablePowerThrottling() {
    struct PowerThrottlingState { ULONG Version; ULONG ControlMask; ULONG StateMask; };
    typedef BOOL(WINAPI* SetProcessInformationFn)(HANDLE, int, LPVOID, DWORD);
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32) return;
    auto fn = (SetProcessInformationFn)GetProcAddress(k32, "SetProcessInformation");
    if (!fn) return;
    PowerThrottlingState st = { 1, 0x1 | 0x4, 0 }; // EXECUTION_SPEED | IGNORE_TIMER_RESOLUTION, both forced OFF
    fn(GetCurrentProcess(), 4 /* ProcessPowerThrottling */, &st, sizeof(st));
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    // Only one copy may run: two would both type every keystroke.
    HANDLE singleton = CreateMutexW(nullptr, TRUE, L"Local\\ESPdeviceSingleInstance");
    if (singleton && GetLastError() == ERROR_ALREADY_EXISTS) {
        OpenSettingsInBrowser();
        return 0;
    }

    InitPaths();
    LoadCounters();
    SetUnhandledExceptionFilter(CrashFilter);
    InitClock();
    for (int i = 0; i < NUM_KEYS; i++) { g_held[i] = g_sent[i] = false; g_nextRepeat[i] = -1; g_lastSeen[i] = 0; g_heldView[i] = false; }

    try {
        std::lock_guard<std::mutex> lock(g_profileMutex);
        LoadProfilesLocked();
        PublishBindingsLocked();
    } catch (...) {
        Log("ERROR: profiles unreadable - using default profile.");
        std::lock_guard<std::mutex> lock(g_profileMutex);
        g_profiles.clear();
        g_profiles.push_back(DefaultProfile("Profile 1"));
        g_activeIndex = 0;
        PublishBindingsLocked();
    }

    {
        std::error_code ec;
        g_hasLogo = fs::exists(g_configDir / L"logo.png", ec);
        if (g_hasLogo) g_logoVer = 1;
    }

    Log(std::string(VERSION_TEXT) + " starting.");

    timeBeginPeriod(1);
    DisablePowerThrottling();
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    InitTypematicTiming();
    InitTrayIcon(hInstance);

    std::thread([]() { RunGuarded("device", true, DeviceThread); }).detach();
    std::thread([]() { RunGuarded("http-accept", true, AcceptLoop); }).detach();
    std::thread([]() { RunGuarded("counters", true, CounterSaverLoop); }).detach();

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    g_running = false;
    SaveCounters(false);
    RemoveTrayIcon();
    timeEndPeriod(1);
    return 0;
}

// ---- Embedded website -------------------------------------------------
// Split into several literals (MSVC caps a single string literal at ~16 KB).
// All colours and fonts are CSS variables at the very top of the first
// block: swap those (or the whole block) when the visual pass happens.

static const char* HTML_HEAD = R"HTMLX(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESPdevice</title>
<style>
/* ===== THEME =====
   Every colour is a variable (--c-*). These are the Light theme's values and only
   act as a fallback: the THEME panel on the page overrides them at runtime and the
   chosen theme is saved in config\theme.json. */
:root {
  --c-bg: #ffffff;
  --c-fg: #000000;
  --c-dim: #666666;
  --c-border: #000000;
  --c-heading: #666666;
  --c-err: #c00000;
  --c-credit: #000000;
  --c-title: #000000;
  --c-cursor: #666666;
  --c-art: #000000;
  --c-status: #666666;
  --c-btn-fg: #000000;
  --c-btn-bg: #ffffff;
  --c-btn-border: #000000;
  --c-btn-hover-fg: #ffffff;
  --c-btn-hover-bg: #000000;
  --c-btn-active-fg: #ffffff;
  --c-btn-active-bg: #777777;
  --c-input-fg: #000000;
  --c-input-bg: #ffffff;
  --c-input-border: #000000;
  --c-key-off-fg: #000000;
  --c-key-off-bg: #ffffff;
  --c-key-off-border: #000000;
  --c-key-on-fg: #ffffff;
  --c-key-on-bg: #000000;
  --c-key-on-border: #000000;
  --c-stat-label: #000000;
  --c-stat-value: #555555;
  --c-log-bg: #ffffff;
  --c-log-border: #000000;
  --c-log-down: #000000;
  --c-log-up: #888888;
  --c-log-timeout: #c00000;
  --c-log-connect: #444444;
  --c-log-info: #888888;
  --font: Consolas, "Lucida Console", "Courier New", monospace;
  --size: 14px;
}
/* logo: click to replace with your own PNG (1:1 recommended). Max display size is set here. */
.logo { display: flex; flex-direction: column; align-items: flex-start; gap: 4px; max-width: 220px; min-width: 100px; }
.logobox { cursor: pointer; }
.logobox:hover { opacity: 0.8; }
.logobox:focus-visible { outline: 1px dashed var(--c-border); outline-offset: 3px; }
.logoimg { display: block; width: auto; height: auto; max-width: 150px; max-height: 110px; object-fit: contain; }
.logohint { font-size: 11px; line-height: 1.3; color: var(--c-dim); width: 0; min-width: 100%; white-space: nowrap; /* one line: reserves little space, text overflows to the right instead of widening the column */
            position: relative; opacity: 0; visibility: hidden; transition: opacity 0.15s; }
/* Invisible bigger hover zone around the hint (no layout effect). It is hit-testable only while the hint is showing
   (visibility:hidden elements ignore the mouse), so it keeps the hint open while you travel to [reset] but never opens it. */
.logohint::before { content: ""; position: absolute; left: -10px; top: -4px; width: 430px; height: calc(100% + 18px); }
.logo:hover .logohint, .logobox:focus-visible ~ .logohint { opacity: 1; visibility: visible; } /* hint only shows while hovering the logo (or keyboard-focusing it; a mouse click must NOT pin it open) */
.logohint a { color: var(--c-dim); cursor: pointer; text-decoration: underline;
              display: inline-block; padding: 7px 10px; margin: -7px -6px -7px 0; position: relative; } /* big click target for [reset], same line height */
.art { margin: 0; line-height: 1.1; font-size: 16px; white-space: pre; color: var(--c-art); }
.brand { margin: 0; font-size: 2.6em; letter-spacing: 1px; line-height: 1; color: var(--c-title); }
.brand .cur { color: var(--c-cursor); }
          display: flex; align-items: center; gap: 10px; cursor: pointer; user-select: none; }
.row { display: flex; flex-wrap: wrap; gap: 8px; align-items: center; }
button, select, input[type=text] {
  font-family: var(--font); font-size: var(--size); min-width: 0; padding: 4px 8px;
}
button { color: var(--c-btn-fg); background: var(--c-btn-bg); border: 1px solid var(--c-btn-border); cursor: pointer; }
button:hover { background: var(--c-btn-hover-bg); color: var(--c-btn-hover-fg); }
button.cap.on { background: var(--c-btn-active-bg); color: var(--c-btn-active-fg); }
select, input[type=text] { color: var(--c-input-fg); background: var(--c-input-bg); border: 1px solid var(--c-input-border); }
select option { background: var(--c-input-bg); color: var(--c-input-fg); }
input[type=text]:disabled { opacity: 0.7; }
input[type=color] { width: 34px; height: 24px; padding: 0; border: 1px solid var(--c-input-border); background: var(--c-input-bg); cursor: pointer; flex: none; }
input[type=color]:disabled { cursor: default; opacity: 0.7; }
input:focus, select:focus { outline: 1px solid var(--c-border); }
.keys { display: flex; flex-wrap: wrap; gap: 12px; }
.key { width: 92px; height: 92px; border: 2px solid var(--c-key-off-border); background: var(--c-key-off-bg); color: var(--c-key-off-fg);
       display: flex; flex-direction: column; align-items: center; justify-content: center; }
.key .n { font-size: 1.6em; }
.key .b { font-size: 0.85em; margin-top: 4px; max-width: 84px; overflow: hidden; white-space: nowrap; text-overflow: ellipsis; }
.key.on { background: var(--c-key-on-bg); color: var(--c-key-on-fg); border-color: var(--c-key-on-border); }
.key.small { width: 84px; height: 46px; }
table { border-collapse: collapse; width: 100%; }
td, th { border: 1px solid var(--c-border); padding: 5px 8px; text-align: left; }
td input[type=text] { width: 100%; }
.err { color: var(--c-err); min-height: 1.3em; margin: 8px 0 0 0; }
.dim { color: var(--c-dim); }
.stats { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 4px 16px; margin-bottom: 10px; color: var(--c-stat-label); }
.stats div span { color: var(--c-stat-value); }
#log { height: 330px; overflow: auto; border: 1px solid var(--c-log-border); background: var(--c-log-bg); padding: 6px; white-space: pre; margin: 8px 0 0 0; }
#log { scrollbar-width: none; -ms-overflow-style: none; } /* scrollbar hidden; wheel/touch scrolling still works */
#log::-webkit-scrollbar { display: none; width: 0; height: 0; }
.l-P { color: var(--c-log-down); } .l-R { color: var(--c-log-up); } .l-X { color: var(--c-log-timeout); }
.l-C { color: var(--c-log-connect); } .l-D { color: var(--c-log-connect); } .l-I { color: var(--c-log-info); }
.tgroup { margin: 12px 0 4px 0; color: var(--c-heading); }
.tgrid { display: grid; grid-template-columns: repeat(auto-fill, minmax(240px, 1fr)); gap: 6px 14px; }
.tfield { display: flex; align-items: center; gap: 6px; min-width: 0; }
.tfield label { flex: 1; min-width: 0; }
.tfield input[type=text] { width: 82px; flex: none; }
.preview { display: flex; flex-wrap: wrap; gap: 12px; margin: 10px 0 0 0; }
)HTMLX";

static const char* HTML_CSS_WIN = R"HTMLX(/* ===== layout: a fixed header, floating windows, and corner widgets ===== */
* { box-sizing: border-box; }
html, body { height: 100%; margin: 0; overflow: hidden; background: var(--c-bg); color: var(--c-fg); font-family: var(--font); font-size: var(--size); }
.hdr { position: fixed; left: 16px; top: 16px; z-index: 1; max-width: 60vw; }
.hero { display: flex; align-items: center; gap: 20px; margin: 0 0 8px 0; }
.status { margin: 0 0 2px 0; color: var(--c-status); }
/* windows live in #desk (pointer events pass through the empty parts of it) */
#desk { position: fixed; left: 0; top: 0; right: 0; bottom: 0; z-index: 2; pointer-events: none; visibility: hidden; }
#desk.ready { visibility: visible; }
#desk > * { pointer-events: auto; }
.win { position: absolute; display: flex; flex-direction: column; background: var(--c-bg); border: 1px solid var(--c-border); min-width: 0; min-height: 0; overflow: hidden; padding-bottom: 16px; }
.wbar { flex: none; display: flex; align-items: center; gap: 8px; padding: 6px 8px 4px 8px; cursor: move; user-select: none; -webkit-user-select: none; touch-action: none; }
.wbar h2 { margin: 0; font-size: 1em; font-weight: normal; color: var(--c-heading); flex: 1; min-width: 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.win.front .wbar h2 { color: var(--c-fg); }
.grip { flex: none; width: 1.5em; height: 1.5em; border: 1px solid var(--c-border); display: flex; align-items: center; justify-content: center; font-size: 0.8em; line-height: 1; color: var(--c-dim); }
.wbtn { flex: none; width: 1.7em; height: 1.7em; padding: 0; display: flex; align-items: center; justify-content: center; line-height: 1; cursor: pointer; }
.wbody { flex: 1 1 auto; min-height: 0; overflow: auto; padding: 4px 12px 6px 12px; scrollbar-width: thin; scrollbar-color: var(--c-border) var(--c-bg); }
.rsz { position: absolute; left: 0; bottom: 0; width: 16px; height: 16px; cursor: nesw-resize; touch-action: none; z-index: 2; }
.rsz::after { content: ""; position: absolute; left: 3px; bottom: 3px; width: 8px; height: 8px; border-left: 2px solid var(--c-dim); border-bottom: 2px solid var(--c-dim); }
.rsz:hover::after { border-color: var(--c-fg); }
.rsz.r { left: auto; right: 0; cursor: nwse-resize; }
.rsz.r::after { left: auto; right: 3px; border-left: 0; border-right: 2px solid var(--c-dim); }
.wicon { flex: none; width: 22px; height: 22px; object-fit: contain; pointer-events: none; }
/* minimized: a small icon window */
.mini { position: absolute; display: none; background: var(--c-bg); border: 1px solid var(--c-border); cursor: move; touch-action: none; user-select: none; -webkit-user-select: none; overflow: hidden; }
.mph, .mimg { position: absolute; left: 8px; top: 8px; width: calc(100% - 16px); height: calc(100% - 16px); pointer-events: none; }
.mph { color: var(--c-fg); } .mph svg { display: block; width: 100%; height: 100%; }
.mimg { object-fit: contain; }
.mcar { position: absolute; left: 3px; top: 3px; width: 18px; height: 18px; min-width: 0; padding: 0; font-size: 12px; line-height: 1; display: flex; align-items: center; justify-content: center; z-index: 3; }
/* corner widgets */
.tbtns { position: fixed; right: 16px; top: 16px; z-index: 3; display: flex; gap: 8px; }
.gear img { display: block; width: 24px; height: 24px; object-fit: contain; pointer-events: none; }
.gear.hasicon { padding: 3px; background: transparent; border-color: transparent; }
.gear.hasicon:hover { background: var(--c-btn-hover-bg); border-color: var(--c-btn-border); }
.divider { position: fixed; left: 0; right: 0; bottom: 64px; height: 1px; background: var(--c-border); opacity: 0.5; z-index: 1; pointer-events: none; }
.socials { position: fixed; left: 16px; bottom: 12px; z-index: 1; display: flex; gap: 14px; }
.socials a { color: var(--c-dim); text-decoration: none; display: flex; align-items: center; }
.sicon { display: block; width: 22px; height: 22px; object-fit: contain; }
.socials a:hover { color: var(--c-fg); text-decoration: underline; }
.br { position: fixed; right: 16px; bottom: 12px; z-index: 1; text-align: right; }
.credit { opacity: 0.5; color: var(--c-credit); }
/* per-window tweaks */
.vis { display: flex; flex-wrap: wrap; gap: 12px 32px; align-items: center; }
.counters { display: grid; gap: 8px; min-width: 150px; font-size: 1.35em; color: var(--c-stat-label); }
.counters div { white-space: nowrap; justify-self: start; padding: 1px 6px; margin: 0 -6px; } .counters span { color: var(--c-stat-value); font-size: 1.2em; }
.counters div.flash { animation: cflash 0.35s ease-out; }
@keyframes cflash { from { background: var(--c-fg); color: var(--c-bg); } to { background: transparent; } }
.counters div.flash span { animation: cflash 0.35s ease-out; }
#w-debugger .wbody { display: flex; flex-direction: column; }
#w-debugger .wbody > * { flex: none; }
#w-debugger #log { flex: 1 1 90px; height: auto; min-height: 70px; }
.opt { display: flex; align-items: center; gap: 8px; margin: 5px 0; cursor: pointer; user-select: none; }
.opt input { width: 14px; height: 14px; margin: 0; accent-color: var(--c-fg); }
.setgrp { margin: 12px 0 4px 0; color: var(--c-heading); } .setgrp:first-child { margin-top: 2px; }
</style>
</head>
)HTMLX";

static const char* HTML_BODY = R"HTMLX(<body>
<div class="divider" id="divider"></div>
<div class="hdr">
<div class="hero">
<div class="logo">
<div class="logobox" id="logoBox" role="button" tabindex="0" title="Click to change the logo (PNG, 1:1 ratio recommended)"
     onclick="pickLogo()" onkeydown="if (event.key === 'Enter' || event.key === ' ') { event.preventDefault(); pickLogo(); }">
<pre class="art" id="art">  /\___/\
 /  o o  \
( =  Y  = )
 )   ^   (
(__|___|__)</pre>
<img class="logoimg" id="logoImg" alt="logo" style="display:none">
</div>
<div class="logohint">click to change (PNG, 1:1 ratio recommended) <a id="logoReset" style="display:none" onclick="resetLogo(event)">[reset]</a></div>
<input type="file" id="logoFile" accept="image/png" style="display:none" onchange="onLogoChosen(event)">
</div>
<h1 class="brand">ESPdevice<span class="cur">_</span></h1>
</div>
</div>
<div class="tbtns" id="tbtns"><button class="gear" id="themeBtn" onclick="wmToggle('theme')" title="Open / close the themes window">[ THEMES ]</button><button class="gear" id="gearBtn" onclick="wmToggle('settings')" title="Open / close the settings window">[ SETTINGS ]</button></div>

<div id="desk">

<section class="win" id="w-profile" data-title="PROFILE" data-abbr="PRO"><div class="wbody">
<div class="row">
  <select id="profileSelect" onchange="selectProfile()"></select>
  <button onclick="addProfile()">+ ADD</button>
  <button onclick="renameProfile()">RENAME</button>
  <button onclick="deleteProfile()">DELETE</button>
</div>
<p class="dim" id="profileCount"></p>
</div></section>

<section class="win" id="w-visualizer" data-title="VISUALIZER" data-abbr="VIS"><div class="wbody">
<div class="vis">
  <div class="keys" id="keys"></div>
  <div class="counters">
    <div>CPS: <span id="cCps">0</span></div>
    <div id="cTotalRow">Total: <span id="cTotal">0</span></div>
  </div>
</div>
</div></section>

<section class="win" id="w-keybinds" data-title="KEYBINDS" data-abbr="KEY"><div class="wbody">
<table>
  <thead><tr><th>KEY</th><th>BINDING (type, then press ENTER to save)</th><th></th></tr></thead>
  <tbody id="bindRows"></tbody>
</table>
<datalist id="keynames"></datalist>
<p class="err" id="err"></p>
<p class="dim">Type a binding and press ENTER to save it (e.g. A, ?, CTRL+SHIFT+A, F13). Or click CAPTURE and press any key (hold CTRL/SHIFT/ALT first for a combo), or type names (SPACE, F13, NUM5, VOL_UP...) or characters (? &lt; &gt; { } | : " ~ ! @ ...). Typed shifted characters are stored as SHIFT+key. Use PLUS for the + key. Combos join with +, up to 4 keys.</p>
</div></section>

<section class="win" id="w-service" data-title="SERVICE" data-abbr="SVC"><div class="wbody">
<div class="row">
  <button id="pauseBtn" onclick="toggleService()">PAUSE</button>
  <button onclick="terminateService()">TERMINATE</button>
</div>
</div></section>

<section class="win" id="w-debugger" data-title="DEBUGGER" data-abbr="DBG"><div class="wbody">
<div class="stats">
  <div>link: <span id="sLink">-</span></div>
  <div>reports/s: <span id="sRate">-</span></div>
  <div>last latency: <span id="sLast">-</span></div>
  <div>avg latency: <span id="sAvg">-</span></div>
  <div>max latency: <span id="sMax">-</span></div>
  <div>samples: <span id="sCount">-</span></div>
  <div>hold delay: <span id="sDelay">-</span></div>
  <div>repeat every: <span id="sRepeat">-</span></div>
</div>
<div class="row">
  <button onclick="clearLog()">CLEAR LOG</button>
  <button id="freezeBtn" onclick="toggleFreeze()">FREEZE LOG</button>
  <button onclick="resetStats()">RESET LATENCY</button>
</div>
<div id="log"></div>
<p class="dim">latency = keypad byte received by the service until SendInput returned (excludes USB transit time).</p>
</div></section>

<section class="win" id="w-theme" data-title="THEME" data-abbr="THM" data-closable="1"><div class="wbody">
<div class="row">
  <select id="themeSelect" onchange="pickTheme()"></select>
  <button onclick="copyTheme()">COPY</button>
  <button onclick="renameTheme()">RENAME</button>
  <button onclick="deleteTheme()">DELETE</button>
</div>
<p class="dim" id="themeNote"></p>
<div class="preview">
  <div class="key small"><div class="n">1</div></div>
  <div class="key small on"><div class="n">1</div></div>
</div>
<div id="themeFields"></div>
</div></section>

<section class="win" id="w-settings" data-title="SETTINGS" data-abbr="SET" data-closable="1"><div class="wbody">
<div class="setgrp">WINDOWS</div>
<div class="row"><button onclick="resetLayout()">RESET WINDOW LAYOUT</button></div>
<label class="opt"><input type="checkbox" id="optGearTop" onchange="wmSettingChanged()"> keep the settings and themes buttons on top of the windows</label>
<div class="setgrp">COUNTERS</div>
<div class="row"><button onclick="resetCounters()">RESET CPS / TOTAL</button></div>
<div class="setgrp">SOCIAL LINKS (bottom left)</div>
<label class="opt"><input type="checkbox" id="optSocial_github" onchange="wmSettingChanged()"> GitHub</label>
<label class="opt"><input type="checkbox" id="optSocial_youtube" onchange="wmSettingChanged()"> YouTube</label>
<label class="opt"><input type="checkbox" id="optSocial_tiktok" onchange="wmSettingChanged()"> TikTok</label>
<label class="opt"><input type="checkbox" id="optSocial_discord" onchange="wmSettingChanged()"> Discord</label>
<p class="dim">More options will show up here in future versions. Saved in config\settings.json.</p>
</div></section>

</div>
<nav class="socials" id="socials"></nav>
<div class="br">
<p class="status" id="status">connecting to service...</p>
<div class="credit">made by bagelings, assisted by claude &middot; v@@VER@@</div>
</div>
)HTMLX";

static const char* HTML_JS1 = R"HTMLX(<script>
const KEY_NAMES = [
  "A","B","C","D","E","F","G","H","I","J","K","L","M","N","O","P","Q","R","S","T","U","V","W","X","Y","Z",
  "0","1","2","3","4","5","6","7","8","9",
  "F1","F2","F3","F4","F5","F6","F7","F8","F9","F10","F11","F12",
  "F13","F14","F15","F16","F17","F18","F19","F20","F21","F22","F23","F24",
  "SPACE","ENTER","ESC","TAB","BACKSPACE","CAPSLOCK",
  "SEMICOLON","QUOTE","COMMA","PERIOD","SLASH","LBRACKET","RBRACKET","BACKSLASH","MINUS","EQUALS","GRAVE","NONUSBACKSLASH","PLUS",
  "~","!","@","#","$","%","^","&","*","(",")","_","{","}","|",":","\"","<",">","?",
  "CTRL","SHIFT","ALT","WIN","LCTRL","LSHIFT","LALT","LWIN","RCTRL","RSHIFT","RALT","RWIN","MENU",
  "LEFT","RIGHT","UP","DOWN","INSERT","DELETE","HOME","END","PAGEUP","PAGEDOWN",
  "PRINTSCREEN","SCROLLLOCK","PAUSE","NUMLOCK",
  "NUM0","NUM1","NUM2","NUM3","NUM4","NUM5","NUM6","NUM7","NUM8","NUM9",
  "NUM_ADD","NUM_SUB","NUM_MUL","NUM_DIV","NUM_DECIMAL","NUM_ENTER",
  "MEDIA_PLAY","MEDIA_NEXT","MEDIA_PREV","MEDIA_STOP","VOL_UP","VOL_DOWN","VOL_MUTE",
  "BROWSER_BACK","BROWSER_FORWARD","BROWSER_REFRESH","BROWSER_HOME","LAUNCH_MAIL","LAUNCH_CALC"
];
const $ = id => document.getElementById(id);
const ART_MAX_W = 300, ART_MAX_H = 200; // px box the title art is fitted into (edit to taste)
let S = null;
let held = [false, false, false];
let lastSig = "";
let frozen = false;

async function api(path, body) {
  const opts = { headers: { "X-ESPdevice": "1", "Content-Type": "application/json" } };
  if (body !== undefined) { opts.method = "POST"; opts.body = JSON.stringify(body); }
  const r = await fetch(path, opts);
  return r.json();
}

function us(v) { return v + " us"; }

function paintKeys() {
  for (let k = 0; k < 3; k++) {
    const el = $("key" + k);
    if (el) el.classList.toggle("on", !!held[k]);
  }
}

function buildKeyBoxes() {
  const box = $("keys");
  box.innerHTML = "";
  for (let k = 0; k < 3; k++) {
    const d = document.createElement("div");
    d.className = "key"; d.id = "key" + k;
    const n = document.createElement("div"); n.className = "n"; n.textContent = String(k + 1);
    const b = document.createElement("div"); b.className = "b"; b.id = "keyb" + k;
    d.appendChild(n); d.appendChild(b);
    box.appendChild(d);
  }
}

function renderProfiles() {
  const sel = $("profileSelect");
  sel.innerHTML = "";
  S.profiles.forEach((p, i) => {
    const o = document.createElement("option");
    o.value = String(i); o.textContent = p.name;
    sel.appendChild(o);
  });
  sel.value = String(S.activeIndex);
  $("profileCount").textContent = S.profiles.length + " / " + S.maxProfiles + " profiles";

  const active = S.profiles[S.activeIndex];
  const rows = $("bindRows");
  rows.innerHTML = "";
  for (let k = 0; k < 3; k++) {
    const tr = document.createElement("tr");
    const td0 = document.createElement("td"); td0.textContent = "KEY " + (k + 1);
    const td1 = document.createElement("td");
    const inp = document.createElement("input"); inp.type = "text";
    inp.id = "in" + k; inp.value = active.bindings[k]; inp.setAttribute("list", "keynames");
    inp.setAttribute("spellcheck", "false"); inp.setAttribute("autocomplete", "off");
    inp.addEventListener("keydown", e => { if (e.key === "Enter") saveKey(k); });
    td1.appendChild(inp);
    const td2 = document.createElement("td");
    const cap = document.createElement("button"); cap.className = "cap"; cap.id = "cap" + k; cap.textContent = "CAPTURE";
    cap.onclick = () => startCapture(k);
    td2.appendChild(cap);
    tr.appendChild(td0); tr.appendChild(td1); tr.appendChild(td2);
    rows.appendChild(tr);
    $("keyb" + k).textContent = active.bindings[k];
  }
}

function applyState(st) {
  S = st;
  updateLogo(st);
  held = st.keyHeld;
  paintKeys();

  let line = st.connected ? ("CONNECTED " + st.port) : "NO KEYPAD";
  line += st.paused ? "  |  PAUSED" : "  |  ACTIVE";
  $("status").textContent = line;
  $("pauseBtn").textContent = st.paused ? "RESUME" : "PAUSE";

  const sig = JSON.stringify([st.profiles, st.activeIndex]);
  if (sig !== lastSig) { lastSig = sig; renderProfiles(); }

  $("sLink").textContent = st.connected ? st.port : "none";
  $("sRate").textContent = st.reportsPerSec;
  $("sLast").textContent = us(st.latLastUs);
  $("sAvg").textContent = us(st.latAvgUs);
  $("sMax").textContent = us(st.latMaxUs);
  $("sCount").textContent = st.latCount;
  $("sDelay").textContent = st.holdDelayMs + " ms";
  $("sRepeat").textContent = st.repeatMs + " ms";
  $("err").textContent = st.error ? ("ERROR: " + st.error) : "";
  if (typeof st.clicksTotal === "number") syncTotal(st.clicksTotal, st.clicksSince);
}
)HTMLX";

static const char* HTML_JS_THEME = R"HTMLX(
// ---- THEMES -------------------------------------------------------------
const THEME_FIELDS = [["bg","Background","PAGE"],["fg","Main text","PAGE"],["dim","Secondary text","PAGE"],["border","Borders","PAGE"],["heading","Section headings","PAGE"],["err","Error text","PAGE"],["credit","Corner credit","PAGE"],["title","Title text","TITLE"],["cursor","Title cursor","TITLE"],["art","Title art","TITLE"],["status","Status line","TITLE"],["btn-fg","Button text","BUTTONS AND INPUTS"],["btn-bg","Button background","BUTTONS AND INPUTS"],["btn-border","Button border","BUTTONS AND INPUTS"],["btn-hover-fg","Button hover text","BUTTONS AND INPUTS"],["btn-hover-bg","Button hover background","BUTTONS AND INPUTS"],["btn-active-fg","Capture active text","BUTTONS AND INPUTS"],["btn-active-bg","Capture active background","BUTTONS AND INPUTS"],["input-fg","Input text","BUTTONS AND INPUTS"],["input-bg","Input background","BUTTONS AND INPUTS"],["input-border","Input border","BUTTONS AND INPUTS"],["key-off-fg","Unpressed text","VISUALIZER"],["key-off-bg","Unpressed background","VISUALIZER"],["key-off-border","Unpressed border","VISUALIZER"],["key-on-fg","Pressed text","VISUALIZER"],["key-on-bg","Pressed background","VISUALIZER"],["key-on-border","Pressed border","VISUALIZER"],["stat-label","Stat labels","DEBUGGER"],["stat-value","Stat values","DEBUGGER"],["log-bg","Log background","DEBUGGER"],["log-border","Log border","DEBUGGER"],["log-down","Key down lines","DEBUGGER"],["log-up","Key up lines","DEBUGGER"],["log-timeout","Timeout lines","DEBUGGER"],["log-connect","Connect / disconnect lines","DEBUGGER"],["log-info","Info lines","DEBUGGER"]];
const BUILTIN_THEMES = {
  "Light": {"bg":"#ffffff","fg":"#000000","dim":"#666666","border":"#000000","heading":"#666666","err":"#c00000","credit":"#000000","title":"#000000","cursor":"#666666","art":"#000000","status":"#666666","btn-fg":"#000000","btn-bg":"#ffffff","btn-border":"#000000","btn-hover-fg":"#ffffff","btn-hover-bg":"#000000","btn-active-fg":"#ffffff","btn-active-bg":"#777777","input-fg":"#000000","input-bg":"#ffffff","input-border":"#000000","key-off-fg":"#000000","key-off-bg":"#ffffff","key-off-border":"#000000","key-on-fg":"#ffffff","key-on-bg":"#000000","key-on-border":"#000000","stat-label":"#000000","stat-value":"#555555","log-bg":"#ffffff","log-border":"#000000","log-down":"#000000","log-up":"#888888","log-timeout":"#c00000","log-connect":"#444444","log-info":"#888888"},
  "Dark": {"bg":"#000000","fg":"#ffffff","dim":"#999999","border":"#ffffff","heading":"#999999","err":"#ff5050","credit":"#ffffff","title":"#ffffff","cursor":"#999999","art":"#ffffff","status":"#999999","btn-fg":"#ffffff","btn-bg":"#000000","btn-border":"#ffffff","btn-hover-fg":"#000000","btn-hover-bg":"#ffffff","btn-active-fg":"#000000","btn-active-bg":"#888888","input-fg":"#ffffff","input-bg":"#000000","input-border":"#ffffff","key-off-fg":"#ffffff","key-off-bg":"#000000","key-off-border":"#ffffff","key-on-fg":"#000000","key-on-bg":"#ffffff","key-on-border":"#ffffff","stat-label":"#ffffff","stat-value":"#bbbbbb","log-bg":"#000000","log-border":"#ffffff","log-down":"#ffffff","log-up":"#888888","log-timeout":"#ff5050","log-connect":"#bbbbbb","log-info":"#888888"},
  "Terminal": {"bg":"#000000","fg":"#33ff33","dim":"#1a8f1a","border":"#1a8f1a","heading":"#1a8f1a","err":"#ff5050","credit":"#33ff33","title":"#33ff33","cursor":"#1a8f1a","art":"#33ff33","status":"#1a8f1a","btn-fg":"#33ff33","btn-bg":"#000000","btn-border":"#1a8f1a","btn-hover-fg":"#000000","btn-hover-bg":"#33ff33","btn-active-fg":"#000000","btn-active-bg":"#ffb000","input-fg":"#33ff33","input-bg":"#000000","input-border":"#1a8f1a","key-off-fg":"#33ff33","key-off-bg":"#000000","key-off-border":"#33ff33","key-on-fg":"#000000","key-on-bg":"#33ff33","key-on-border":"#33ff33","stat-label":"#33ff33","stat-value":"#ffb000","log-bg":"#000000","log-border":"#1a8f1a","log-down":"#33ff33","log-up":"#1a8f1a","log-timeout":"#ff5050","log-connect":"#ffb000","log-info":"#1a8f1a"},
  "Bagel": {"bg":"#f6ecd8","fg":"#4a2e17","dim":"#9a6b3c","border":"#c98a3b","heading":"#9a6b3c","err":"#b3261e","credit":"#4a2e17","title":"#a8631f","cursor":"#c98a3b","art":"#a8631f","status":"#9a6b3c","btn-fg":"#4a2e17","btn-bg":"#fbf4e4","btn-border":"#c98a3b","btn-hover-fg":"#fff8e8","btn-hover-bg":"#c98a3b","btn-active-fg":"#3b2210","btn-active-bg":"#e0a840","input-fg":"#4a2e17","input-bg":"#fffaf0","input-border":"#c98a3b","key-off-fg":"#4a2e17","key-off-bg":"#fbf4e4","key-off-border":"#c98a3b","key-on-fg":"#3b2210","key-on-bg":"#e0a840","key-on-border":"#a8631f","stat-label":"#4a2e17","stat-value":"#a8631f","log-bg":"#fffaf0","log-border":"#c98a3b","log-down":"#4a2e17","log-up":"#a58058","log-timeout":"#b3261e","log-connect":"#a8631f","log-info":"#a58058"}
};
const HEX = /^#[0-9a-fA-F]{6}$/;
const MAX_CUSTOM_THEMES = 30;
let THEME = { active: "Light", custom: [] };
let themeSaveTimer = null;

function isBuiltin(name) { return Object.prototype.hasOwnProperty.call(BUILTIN_THEMES, name); }
function customTheme(name) { return THEME.custom.find(t => t.name === name) || null; }
function themeColors(name) { return isBuiltin(name) ? BUILTIN_THEMES[name] : (customTheme(name) ? customTheme(name).colors : BUILTIN_THEMES.Light); }

// Only #rrggbb values are ever written into CSS; anything else falls back to Light.
function applyColors(c) {
  const root = document.documentElement, base = BUILTIN_THEMES.Light;
  for (const f of THEME_FIELDS) {
    const v = c && HEX.test(c[f[0]]) ? c[f[0]] : base[f[0]];
    root.style.setProperty("--c-" + f[0], v);
  }
  const bg = (c && HEX.test(c.bg)) ? c.bg : base.bg;
  const lum = (parseInt(bg.slice(1, 3), 16) * 299 + parseInt(bg.slice(3, 5), 16) * 587 + parseInt(bg.slice(5, 7), 16) * 114) / 1000;
  root.style.colorScheme = lum < 128 ? "dark" : "light"; // native dropdown lists / colour popups match
  if (typeof tintLogo === "function") tintLogo(); // default logo follows the theme colours
  try { localStorage.setItem("espdevice-theme", JSON.stringify(c)); } catch (e) {} // instant repaint next load; real copy lives in config\theme.json
}
function applyTheme(name) { applyColors(themeColors(name)); }

function saveTheme(immediate) { // colour dragging is debounced; picking/copying/renaming/deleting saves at once
  clearTimeout(themeSaveTimer);
  const send = () => { api("/api/theme", THEME).catch(() => {}); };
  if (immediate) send(); else themeSaveTimer = setTimeout(send, 400);
}

function cleanThemeName(n) { return (n || "").replace(/[^A-Za-z0-9 _.()#+-]/g, "").trim().slice(0, 24); }
function uniqueThemeName(n, skip) {
  let name = n, i = 2;
  while (isBuiltin(name) || THEME.custom.some(t => t !== skip && t.name === name)) { name = n.slice(0, 20) + " " + i; i++; }
  return name;
}

function renderThemeSelect() {
  const sel = $("themeSelect");
  sel.innerHTML = "";
  const add = (name, label) => { const o = document.createElement("option"); o.value = name; o.textContent = label; sel.appendChild(o); };
  Object.keys(BUILTIN_THEMES).forEach(n => add(n, n + " (built-in)"));
  THEME.custom.forEach(t => add(t.name, t.name));
  sel.value = THEME.active;
}

function setThemeColor(key, val) {
  const t = customTheme(THEME.active);
  if (!t) return;
  t.colors[key] = val;
  applyTheme(THEME.active);
  saveTheme();
}

function renderThemeFields() {
  const wrap = $("themeFields");
  wrap.innerHTML = "";
  const colors = themeColors(THEME.active);
  const ro = isBuiltin(THEME.active);
  $("themeNote").textContent = ro
    ? "Built-in theme (read-only). Press COPY to make an editable version."
    : "Custom theme - every change saves automatically.";
  let group = "", grid = null;
  for (const f of THEME_FIELDS) {
    if (f[2] !== group) {
      group = f[2];
      const h = document.createElement("div"); h.className = "tgroup"; h.textContent = "-- " + group + " --";
      wrap.appendChild(h);
      grid = document.createElement("div"); grid.className = "tgrid";
      wrap.appendChild(grid);
    }
    const row = document.createElement("div"); row.className = "tfield";
    const lab = document.createElement("label"); lab.textContent = f[1];
    const pick = document.createElement("input"); pick.type = "color"; pick.value = colors[f[0]]; pick.disabled = ro;
    const hex = document.createElement("input"); hex.type = "text"; hex.value = colors[f[0]]; hex.maxLength = 7; hex.disabled = ro;
    hex.setAttribute("spellcheck", "false"); hex.setAttribute("autocomplete", "off");
    pick.addEventListener("input", () => { hex.value = pick.value; setThemeColor(f[0], pick.value); });
    hex.addEventListener("input", () => {
      let v = hex.value.trim();
      if (v && v[0] !== "#") v = "#" + v;
      if (HEX.test(v)) { v = v.toLowerCase(); pick.value = v; setThemeColor(f[0], v); }
    });
    row.appendChild(lab); row.appendChild(pick); row.appendChild(hex);
    grid.appendChild(row);
  }
}

function pickTheme() {
  THEME.active = $("themeSelect").value;
  applyTheme(THEME.active); renderThemeFields(); saveTheme(true);
}
function copyTheme() {
  if (THEME.custom.length >= MAX_CUSTOM_THEMES) { alert("Theme limit reached (" + MAX_CUSTOM_THEMES + ")."); return; }
  const raw = prompt("Name for the new theme:", THEME.active + " copy");
  if (raw === null) return;
  const n = cleanThemeName(raw);
  if (!n) return;
  const name = uniqueThemeName(n, null);
  THEME.custom.push({ name: name, colors: Object.assign({}, themeColors(THEME.active)) });
  THEME.active = name;
  applyTheme(name); renderThemeSelect(); renderThemeFields(); saveTheme(true);
}
function renameTheme() {
  const t = customTheme(THEME.active);
  if (!t) { alert("Built-in themes can't be renamed. Press COPY first."); return; }
  const raw = prompt("Rename theme:", t.name);
  if (raw === null) return;
  const n = cleanThemeName(raw);
  if (!n) return;
  t.name = uniqueThemeName(n, t);
  THEME.active = t.name;
  renderThemeSelect(); renderThemeFields(); saveTheme(true);
}
function deleteTheme() {
  const t = customTheme(THEME.active);
  if (!t) { alert("Built-in themes can't be deleted."); return; }
  if (!confirm("Delete theme '" + t.name + "'?")) return;
  THEME.custom = THEME.custom.filter(x => x !== t);
  THEME.active = "Light";
  applyTheme("Light"); renderThemeSelect(); renderThemeFields(); saveTheme(true);
}

async function loadTheme() {
  try {
    const txt = await (await fetch("/api/theme")).text();
    const j = JSON.parse(txt || "{}");
    const custom = [];
    if (Array.isArray(j.custom)) {
      for (const t of j.custom.slice(0, MAX_CUSTOM_THEMES)) {
        if (!t || typeof t.name !== "string" || !t.colors || typeof t.colors !== "object") continue;
        const n = cleanThemeName(t.name);
        if (!n || isBuiltin(n) || custom.some(x => x.name === n)) continue;
        const colors = {};
        for (const f of THEME_FIELDS) colors[f[0]] = HEX.test(t.colors[f[0]]) ? t.colors[f[0]].toLowerCase() : BUILTIN_THEMES.Light[f[0]];
        custom.push({ name: n, colors: colors });
      }
    }
    THEME.custom = custom;
    THEME.active = (typeof j.active === "string" && (isBuiltin(j.active) || custom.some(x => x.name === j.active))) ? j.active : "Light";
  } catch (e) {}
  applyTheme(THEME.active); renderThemeSelect(); renderThemeFields();
}

// Repaint instantly from the last-used colours (no flash), then loadTheme() confirms from the service.
(function () { try { const c = JSON.parse(localStorage.getItem("espdevice-theme") || "null"); if (c) applyColors(c); } catch (e) {} })();
)HTMLX";

static const char* HTML_JS_WM1 = R"HTMLX(
// ================= WINDOWS (v1.1) =================
// Every section is a floating window: drag it by the title bar (freeform, nothing snaps), resize from either bottom corner,
// minimize with "v" (it becomes a small icon window; "^" opens it again).
// Positions, sizes, minimized state and stacking order are saved to config\layout.json.
const WIN_IDS = ["profile", "visualizer", "keybinds", "service", "debugger", "theme", "settings"];
const WIN_ICON = { profile: "Profile", visualizer: "Visualizer", keybinds: "Keybinds", service: "Service", debugger: "Debugger", theme: "Theme", settings: "Settings" }; // config\icons\<Name>_icon.png
const WORK_BOTTOM = 64;   // the bottom strip (links, status) under the divider line; default windows stay above it
const HIDDEN = ["theme", "settings"];   // closed until you open them with the buttons at the top right
const MIN_W = 200, MIN_H = 90, MINI_MIN = 36, MINI_W = 88, MINI_H = 64;
const SOCIALS = [
  { id: "github",  label: "github",  icon: "Github",  url: "https://github.com/bagelsssss" },
  { id: "youtube", label: "youtube", icon: "Youtube", url: "https://www.youtube.com/@bagelsssss" },
  { id: "tiktok",  label: "tiktok",  icon: "Tiktok",  url: "https://www.tiktok.com/@bagel.sssss" },
  { id: "discord", label: "discord", icon: "Discord", url: "https://discord.com/users/920728577622835251" }
];
const WM = { L: null, set: null, el: {}, mini: {}, timer: 0, chain: Promise.resolve(), schain: Promise.resolve(), fly: null };

const has = (o, k) => typeof k === "string" && Object.prototype.hasOwnProperty.call(o, k);
const wmNum = (v, d) => (typeof v === "number" && isFinite(v) && Math.abs(v) < 100000) ? Math.round(v) : d;
function wmVW() { return document.documentElement.clientWidth; }
function wmVH() { return document.documentElement.clientHeight; }

// ---- model: { order:[ids bottom..top], win:{ id:{ n:frame, m:frame, min, open } } }, frame = { x, y, w, h, z, pw, ph } ----
// z / pw / ph are kept (always null / 0 for now) so snapping can come back in a later build without changing the file
function wmCleanFrame(f, d, minW, minH) {
  f = (f && typeof f === "object") ? f : {};
  const pw = wmNum(f.pw, 0), ph = wmNum(f.ph, 0);
  return {
    x: wmNum(f.x, d.x), y: wmNum(f.y, d.y), w: Math.max(minW, wmNum(f.w, d.w)), h: Math.max(minH, wmNum(f.h, d.h)),
    z: null,
    pw: (pw > 0 && ph > 0) ? Math.max(minW, pw) : 0, ph: (pw > 0 && ph > 0) ? Math.max(minH, ph) : 0
  };
}

function wmDefaults() {
  const vw = wmVW(), vh = wmVH();
  const hb = document.querySelector(".hdr").getBoundingClientRect();
  // the same arrangement as 1.0: a page of at most 1200 px in the middle, two equal columns under the header
  const page = Math.min(1200, vw), colW = Math.max(MIN_W, Math.floor((page - 48) / 2)), x0 = Math.max(16, Math.floor((vw - page) / 2) + 16);
  const y0 = Math.max(Math.ceil(hb.bottom), 150) + 8;
  const cols = [
    { x: x0, y: y0, ids: ["profile", "visualizer", "keybinds", "service"] },
    { x: Math.max(16, Math.min(x0 + colW + 16, vw - colW - 16)), y: y0, ids: ["debugger"] }
  ];
  const H = { profile: 128, visualizer: 170, keybinds: 290, service: 90, debugger: 520, theme: 500, settings: 400 };
  const L = { v: 1, order: WIN_IDS.slice(), win: {} };
  const mk = (x, y, w, h, open) => ({
    n: { x: x, y: y, w: w, h: h, z: null, pw: 0, ph: 0 },
    m: { x: x, y: y, w: MINI_W, h: MINI_H, z: null, pw: 0, ph: 0 },
    min: false, open: open
  });
  cols.forEach(c => {
    // a column that is taller than the browser window gets its windows shortened (never below MIN_H); their content scrolls
    const gap = 12, n = c.ids.length, avail = vh - c.y - WORK_BOTTOM - 4 - gap * (n - 1);
    const sum = c.ids.reduce((t, id) => t + H[id], 0), floor = n * MIN_H;
    const k = sum <= avail ? 1 : Math.max(0, (avail - floor) / (sum - floor));
    let y = c.y;
    c.ids.forEach(id => {
      const h = Math.max(MIN_H, Math.round(MIN_H + (H[id] - MIN_H) * k));
      L.win[id] = mk(c.x, y, colW, h, true);
      y += h + gap;
    });
  });
  const sw = Math.min(360, Math.max(MIN_W, vw - 32));
  L.win.settings = mk(Math.max(16, vw - sw - 16), 56, sw, Math.max(MIN_H, Math.min(H.settings, vh - 100)), false);
  L.win.theme = mk(Math.max(16, vw - sw - 44), 84, sw, Math.max(MIN_H, Math.min(H.theme, vh - 130)), false);
  return L;
}

function wmClean(raw) {
  const D = wmDefaults(), L = { v: 1, order: [], win: {} };
  const rw = (raw && typeof raw === "object" && raw.win && typeof raw.win === "object") ? raw.win : {};
  WIN_IDS.forEach(id => {
    const r = has(rw, id) && rw[id] && typeof rw[id] === "object" ? rw[id] : null;
    const d = D.win[id];
    if (!r) { L.win[id] = d; return; }
    L.win[id] = {
      n: wmCleanFrame(r.n, d.n, MIN_W, MIN_H), m: wmCleanFrame(r.m, d.m, MINI_MIN, MINI_MIN),
      min: !!r.min, open: HIDDEN.indexOf(id) >= 0 ? !!r.open : true
    };
    L.win[id].m.z = null; L.win[id].m.pw = 0; L.win[id].m.ph = 0;
  });
  const seen = {};
  if (raw && Array.isArray(raw.order)) raw.order.forEach(id => { if (has(L.win, id) && !seen[id]) { seen[id] = 1; L.order.push(id); } });
  WIN_IDS.forEach(id => { if (!seen[id]) L.order.push(id); });
  return L;
}

// ---- geometry: the stored frame -> the rectangle shown now ----
function wmResolve(f, minW, minH) {
  const w = Math.max(minW, f.w), h = Math.max(minH, f.h), vw = wmVW(), vh = wmVH();
  return {
    x: Math.min(Math.max(f.x, 40 - w), vw - 40),   // the title bar always stays reachable
    y: Math.min(Math.max(f.y, 0), vh - 30),
    w: w, h: h
  };
}

// ---- showing windows ----
function wmSetRect(el, r) { el.style.left = r.x + "px"; el.style.top = r.y + "px"; el.style.width = r.w + "px"; el.style.height = r.h + "px"; }
function wmPlace(id) {
  const W = WM.L.win[id], el = WM.el[id], mi = WM.mini[id];
  wmSetRect(el, wmResolve(W.n, MIN_W, MIN_H));
  wmSetRect(mi, wmResolve(W.m, MINI_MIN, MINI_MIN));
  const z = String(10 + WM.L.order.indexOf(id));
  el.style.zIndex = z; mi.style.zIndex = z;
  const wasHidden = el.style.display === "none";
  el.style.display = (W.open && !W.min) ? "flex" : "none";
  mi.style.display = (W.open && W.min) ? "block" : "none";
  if (id === "debugger" && wasHidden && W.open && !W.min) { const lg = $("log"); lg.scrollTop = lg.scrollHeight; } // a hidden log forgets its scroll position
}
function wmMarkFront() {
  let top = null;
  for (let i = WM.L.order.length - 1; i >= 0; i--) { const id = WM.L.order[i], W = WM.L.win[id]; if (W.open && !W.min) { top = id; break; } }
  WIN_IDS.forEach(id => WM.el[id].classList.toggle("front", id === top));
}
function wmPlaceAll() { WIN_IDS.forEach(wmPlace); wmMarkFront(); }
function wmFront(id) {
  const o = WM.L.order;
  if (o[o.length - 1] === id) return;
  o.splice(o.indexOf(id), 1); o.push(id);
  wmPlaceAll(); wmSave();
}

// ---- saving (config\layout.json): after every change, in order; flushed if the tab is closing ----
function wmSave() {
  if (!WM.L) return;
  clearTimeout(WM.timer);
  WM.timer = setTimeout(() => { WM.timer = 0; wmSaveNow(false); }, 150);
}
function wmSaveNow(keepalive) {
  if (!WM.L) return;
  WM.L.vw = wmVW(); WM.L.vh = wmVH();
  const body = JSON.stringify(WM.L);
  const send = () => fetch("/api/layout", { method: "POST", headers: { "X-ESPdevice": "1", "Content-Type": "application/json" }, body: body, keepalive: !!keepalive && body.length < 60000 }).catch(() => {});
  if (keepalive) send(); else WM.chain = WM.chain.then(send);
}
function wmFlush() { if (WM.timer) { clearTimeout(WM.timer); WM.timer = 0; wmSaveNow(true); } }
)HTMLX";

static const char* HTML_JS_WM2 = R"HTMLX(
// ---- dragging (title bar) and resizing (bottom-left corner) ----
function wmFrameOf(id, mini) { const W = WM.L.win[id]; return mini ? W.m : W.n; }

function wmBeginMove(ev, id, isMini) {
  if (ev.button !== 0 || ev.target.closest("button, a, input, select, textarea")) return;
  const F = wmFrameOf(id, isMini), minW = isMini ? MINI_MIN : MIN_W, minH = isMini ? MINI_MIN : MIN_H;
  const base = wmResolve(F, minW, minH);
  wmFront(id);
  const grab = ev.currentTarget, sx = ev.clientX, sy = ev.clientY;
  let moved = false;
  try { grab.setPointerCapture(ev.pointerId); } catch (e) {}
  const move = e => {
    const dx = e.clientX - sx, dy = e.clientY - sy;
    if (!moved && Math.abs(dx) + Math.abs(dy) < 3) return;
    if (!moved) {
      moved = true;
      F.z = null; F.pw = 0; F.ph = 0;
    }
    F.x = Math.min(Math.max(base.x + dx, 40 - base.w), wmVW() - 40);
    F.y = Math.min(Math.max(base.y + dy, 0), wmVH() - 30);
    F.w = base.w; F.h = base.h;
    wmPlace(id);
  };
  const end = () => {
    grab.removeEventListener("pointermove", move); grab.removeEventListener("pointerup", end); grab.removeEventListener("pointercancel", end);
    if (moved) wmSave();
  };
  grab.addEventListener("pointermove", move); grab.addEventListener("pointerup", end); grab.addEventListener("pointercancel", end);
}

function wmBeginResize(ev, id, isMini, side) {
  if (ev.button !== 0) return;
  ev.stopPropagation();
  const F = wmFrameOf(id, isMini), minW = isMini ? MINI_MIN : MIN_W, minH = isMini ? MINI_MIN : MIN_H;
  const base = wmResolve(F, minW, minH), right = base.x + base.w;
  wmFront(id);
  const grab = ev.currentTarget, sx = ev.clientX, sy = ev.clientY;
  let moved = false;
  try { grab.setPointerCapture(ev.pointerId); } catch (e) {}
  const move = e => {
    const dx = e.clientX - sx, dy = e.clientY - sy;
    if (!moved && Math.abs(dx) + Math.abs(dy) < 2) return;
    moved = true;
    const nb = Math.min(Math.max(base.y + base.h + dy, base.y + minH), wmVH());
    if (side === "r") {   // bottom-right corner: the left edge stays put
      const nr = Math.min(Math.max(right + dx, base.x + minW), Math.max(wmVW(), right));
      F.x = base.x; F.w = nr - base.x;
    } else {              // bottom-left corner: the right edge stays put
      const nx = Math.max(Math.min(base.x + dx, right - minW), Math.min(base.x, 0));
      F.x = nx; F.w = right - nx;
    }
    F.y = base.y; F.h = nb - base.y;
    F.z = null; F.pw = 0; F.ph = 0;
    wmPlace(id);
  };
  const end = () => {
    grab.removeEventListener("pointermove", move); grab.removeEventListener("pointerup", end); grab.removeEventListener("pointercancel", end);
    if (moved) wmSave();
  };
  grab.addEventListener("pointermove", move); grab.addEventListener("pointerup", end); grab.addEventListener("pointercancel", end);
}

// ---- minimize / restore: the icon window and the window follow each other ----
function wmMinimize(id) {
  const W = WM.L.win[id], r = wmResolve(W.n, MIN_W, MIN_H);
  W.m.x = r.x; W.m.y = r.y;                       // the icon window appears where the window is
  W.min = true;
  wmPlaceAll(); wmSave();
}
function wmRestore(id) {
  const W = WM.L.win[id], mr = wmResolve(W.m, MINI_MIN, MINI_MIN), nr = wmResolve(W.n, MIN_W, MIN_H);
  if (mr.x !== nr.x || mr.y !== nr.y) {           // the icon window was moved: the window opens where it is (kept on screen)
    W.n.x = Math.max(0, Math.min(mr.x, wmVW() - nr.w));
    W.n.y = Math.max(0, Math.min(mr.y, wmVH() - nr.h));
    W.n.w = nr.w; W.n.h = nr.h; W.n.z = null; W.n.pw = 0; W.n.ph = 0;
  }
  W.min = false;
  const o = WM.L.order; o.splice(o.indexOf(id), 1); o.push(id);
  wmPlaceAll(); wmSave();
}
function wmToggle(id) {   // the buttons at the top right: open or close the settings / themes window
  const W = WM.L.win[id], o = WM.L.order;
  if (W.open) W.open = false;
  else { W.open = true; W.min = false; o.splice(o.indexOf(id), 1); o.push(id); }
  wmPlaceAll(); wmSave();
}
function resetLayout() {
  if (!confirm("Put every window back in its default place and size?")) return;
  const keep = HIDDEN.map(id => WM.L.win[id].open);
  WM.L = wmClean(null);
  HIDDEN.forEach((id, i) => { WM.L.win[id].open = keep[i]; });
  wmPlaceAll(); wmSaveNow(false);
}

// ---- building the windows ----
function wmBuild() {
  const desk = $("desk");
  WIN_IDS.forEach(id => {
    const el = $("w-" + id), title = el.dataset.title, abbr = el.dataset.abbr;
    WM.el[id] = el;
    const bar = document.createElement("div"); bar.className = "wbar";
    const grip = document.createElement("span"); grip.className = "grip"; grip.textContent = "="; grip.setAttribute("aria-hidden", "true");
    const mb = document.createElement("button"); mb.className = "wbtn"; mb.textContent = "v"; mb.title = "Minimize"; mb.setAttribute("aria-label", "Minimize " + title);
    mb.onclick = () => wmMinimize(id);
    const h = document.createElement("h2"); h.textContent = "[ " + title + " ]";
    bar.appendChild(grip); bar.appendChild(mb); bar.appendChild(h);
    const wi = new Image(); wi.className = "wicon"; wi.alt = ""; wi.draggable = false; wi.style.display = "none";
    wi.onload = () => { wi.style.display = ""; };                         // shown only once your icon file exists
    wi.src = "/api/icon/" + WIN_ICON[id] + "?v=" + Date.now();
    bar.appendChild(wi);
    if (el.dataset.closable) {
      const cb = document.createElement("button"); cb.className = "wbtn"; cb.textContent = "x"; cb.title = "Close"; cb.setAttribute("aria-label", "Close " + title);
      cb.onclick = () => wmToggle(id);
      bar.appendChild(cb);
    }
    el.insertBefore(bar, el.firstChild);
    const rz = document.createElement("div"); rz.className = "rsz"; rz.title = "Drag to resize";
    const rzr = document.createElement("div"); rzr.className = "rsz r"; rzr.title = "Drag to resize";
    el.appendChild(rz); el.appendChild(rzr);
    bar.addEventListener("pointerdown", e => wmBeginMove(e, id, false));
    rz.addEventListener("pointerdown", e => wmBeginResize(e, id, false, "l"));
    rzr.addEventListener("pointerdown", e => wmBeginResize(e, id, false, "r"));
    el.addEventListener("pointerdown", () => wmFront(id));

    const m = document.createElement("div"); m.className = "mini"; m.id = "m-" + id; m.title = title + " - click ^ or double-click to open";
    const ph = document.createElement("div"); ph.className = "mph";
    ph.innerHTML = '<svg viewBox="0 0 100 100" preserveAspectRatio="xMidYMid meet"><rect x="6" y="6" width="88" height="88" fill="none" stroke="currentColor" stroke-width="3"/><text x="50" y="62" text-anchor="middle" font-size="30" fill="currentColor" font-family="monospace">' + abbr + '</text></svg>';
    const img = new Image(); img.className = "mimg"; img.alt = ""; img.draggable = false; img.style.display = "none";
    img.onload = () => { img.style.display = ""; ph.style.display = "none"; };   // your own icon replaces the placeholder
    img.src = "/api/icon/" + WIN_ICON[id] + "?v=" + Date.now();
    const car = document.createElement("button"); car.className = "mcar"; car.textContent = "^"; car.title = "Open"; car.setAttribute("aria-label", "Open " + title);
    car.onclick = () => wmRestore(id);
    const rz2 = document.createElement("div"); rz2.className = "rsz"; rz2.title = "Drag to resize";
    const rz2r = document.createElement("div"); rz2r.className = "rsz r"; rz2r.title = "Drag to resize";
    m.appendChild(ph); m.appendChild(img); m.appendChild(car); m.appendChild(rz2); m.appendChild(rz2r);
    m.addEventListener("pointerdown", e => wmBeginMove(e, id, true));
    rz2.addEventListener("pointerdown", e => wmBeginResize(e, id, true, "l"));
    rz2r.addEventListener("pointerdown", e => wmBeginResize(e, id, true, "r"));
    m.addEventListener("dblclick", () => wmRestore(id));
    desk.appendChild(m);
    WM.mini[id] = m;
  });
}

// ---- settings (config\settings.json) ----
function wmCleanSettings(raw) {
  const s = { gearTop: true, social: {} };
  if (raw && typeof raw === "object") {
    if (typeof raw.gearTop === "boolean") s.gearTop = raw.gearTop;
  }
  SOCIALS.forEach(x => { s.social[x.id] = !(raw && raw.social && typeof raw.social === "object" && raw.social[x.id] === false); });
  return s;
}
function wmApplySettings() {
  const s = WM.set;
  $("optGearTop").checked = s.gearTop;
  $("tbtns").style.zIndex = s.gearTop ? "3" : "1";   // 3 = above the windows, 1 = windows can cover it
  SOCIALS.forEach(x => { $("optSocial_" + x.id).checked = s.social[x.id]; });
  const nav = $("socials"); nav.textContent = "";
  SOCIALS.forEach(x => {
    if (!s.social[x.id] || !x.url) return;
    const a = document.createElement("a"); a.href = x.url; a.target = "_blank"; a.rel = "noopener noreferrer"; a.title = x.label;
    const sp = document.createElement("span"); sp.textContent = "[" + x.label + "]";
    const im = new Image(); im.className = "sicon"; im.alt = x.label; im.style.display = "none";
    im.onload = () => { im.style.display = ""; sp.style.display = "none"; };   // your icon (config\icons\Github_icon.png ...) replaces the text
    im.src = "/api/icon/" + x.icon + "?v=" + Date.now();
    a.appendChild(im); a.appendChild(sp);
    nav.appendChild(a);
  });
}
function wmSettingChanged() {
  const s = WM.set;
  s.gearTop = $("optGearTop").checked;
  SOCIALS.forEach(x => { s.social[x.id] = $("optSocial_" + x.id).checked; });
  wmApplySettings();
  const body = JSON.stringify(s);
  WM.schain = WM.schain.then(() => fetch("/api/settings", { method: "POST", headers: { "X-ESPdevice": "1", "Content-Type": "application/json" }, body: body }).catch(() => {}));
}

async function wmInit() {
  wmBuild();
  const get = async u => { try { const r = await fetch(u); if (!r.ok) return null; return JSON.parse(await r.text()); } catch (e) { return null; } };
  const res = await Promise.all([get("/api/layout"), get("/api/settings")]);
  WM.set = wmCleanSettings(res[1]);
  WM.L = wmClean(res[0]);
  wmApplySettings();
  $("divider").style.bottom = WORK_BOTTOM + "px";
  [["gearBtn", "Settings"], ["themeBtn", "Theme"]].forEach(p => {   // Settings_icon.png / Theme_icon.png replace the button text
    const gi = new Image(); gi.alt = ""; gi.draggable = false;
    gi.onload = () => { const g = $(p[0]); g.textContent = ""; g.appendChild(gi); g.classList.add("hasicon"); };
    gi.src = "/api/icon/" + p[1] + "?v=" + Date.now();
  });
  wmPlaceAll();
  $("desk").classList.add("ready");
  window.addEventListener("resize", wmPlaceAll);
  window.addEventListener("pagehide", wmFlush);
  document.addEventListener("visibilitychange", () => { if (document.visibilityState === "hidden") wmFlush(); });
}
)HTMLX";

static const char* HTML_JS2 = R"HTMLX(
async function selectProfile() { applyState(await api("/api/select", { index: parseInt($("profileSelect").value, 10) })); }
async function addProfile() {
  const name = prompt("New profile name:", "Profile " + (S.profiles.length + 1));
  if (name === null) return;
  const r = await api("/api/add", { name: name });
  applyState(r);
  if (!r.error) { const idx = r.profiles.length - 1; applyState(await api("/api/select", { index: idx })); }
}
async function renameProfile() {
  const name = prompt("Rename profile:", S.profiles[S.activeIndex].name);
  if (name === null) return;
  applyState(await api("/api/rename", { index: S.activeIndex, name: name }));
}
async function deleteProfile() {
  if (!confirm("Delete profile '" + S.profiles[S.activeIndex].name + "'?")) return;
  applyState(await api("/api/delete", { index: S.activeIndex }));
}
async function saveKey(k) {
  const r = await api("/api/rebind", { index: S.activeIndex, key: k, value: $("in" + k).value });
  const typed = $("in" + k).value;
  applyState(r);
  if (r.error) $("in" + k).value = typed; // keep what was typed so it can be fixed
}
async function toggleService() { applyState(await api("/api/service", { action: S.paused ? "resume" : "pause" })); }
async function terminateService() {
  if (!confirm("Terminate ESPdevice? The keypad will stop working until it is started again.")) return;
  try { await api("/api/service", { action: "terminate" }); } catch (e) {}
  $("status").textContent = "service terminated";
}
async function resetStats() { applyState(await api("/api/resetstats", {})); }

function clearLog() { $("log").textContent = ""; }
function toggleFreeze() { frozen = !frozen; $("freezeBtn").textContent = frozen ? "RESUME LOG" : "FREEZE LOG"; }

function logLine(cls, text) {
  if (frozen) return;
  const log = $("log");
  const atBottom = log.scrollTop + log.clientHeight >= log.scrollHeight - 6;
  const d = document.createElement("div");
  d.className = "l-" + cls; d.textContent = text;
  log.appendChild(d);
  while (log.childNodes.length > 600) log.removeChild(log.firstChild);
  if (atBottom) log.scrollTop = log.scrollHeight;
}

function onEvent(v) {
  if (v.y === "S") { held = v.held; paintKeys(); return; }
  const t = (v.t / 1e6).toFixed(3).padStart(9, " ");
  const key = "KEY" + (v.k + 1);
  if (v.y === "P") { if (performance.now() > replayUntil) cpsHit(); held[v.k] = true; paintKeys(); logLine("P", "[" + t + "s] " + key + " DOWN  " + v.m.padEnd(14, " ") + (v.l ? v.l + " us" : "(paused)")); }
  else if (v.y === "R") { held[v.k] = false; paintKeys(); logLine("R", "[" + t + "s] " + key + " UP    " + v.m); }
  else if (v.y === "X") { held[v.k] = false; paintKeys(); logLine("X", "[" + t + "s] " + key + " TIMEOUT " + v.m); }
  else if (v.y === "C") { logLine("C", "[" + t + "s] CONNECTED " + v.m); }
  else if (v.y === "D") { logLine("D", "[" + t + "s] " + v.m); }
  else { logLine("I", "[" + t + "s] " + v.m); }
}

function startStream() {
  const es = new EventSource("/events");
  es.onopen = () => { replayUntil = performance.now() + 500; }; // the server replays recent events on connect: don't count those as new presses
  es.onmessage = e => { try { onEvent(JSON.parse(e.data)); } catch (x) {} };
  es.onerror = () => { $("status").textContent = "service offline - retrying..."; };
}

async function poll() {
  try { applyState(await api("/api/state")); } catch (e) { $("status").textContent = "service offline - retrying..."; }
}

// ---- key capture: press a real key to fill in the binding ----
const CODE_NAMES = {
  Space:"SPACE",Enter:"ENTER",Escape:"ESC",Tab:"TAB",Backspace:"BACKSPACE",CapsLock:"CAPSLOCK",
  Minus:"MINUS",Equal:"EQUALS",BracketLeft:"LBRACKET",BracketRight:"RBRACKET",Backslash:"BACKSLASH",
  Semicolon:"SEMICOLON",Quote:"QUOTE",Comma:"COMMA",Period:"PERIOD",Slash:"SLASH",Backquote:"GRAVE",
  IntlBackslash:"NONUSBACKSLASH",ArrowLeft:"LEFT",ArrowRight:"RIGHT",ArrowUp:"UP",ArrowDown:"DOWN",
  Insert:"INSERT",Delete:"DELETE",Home:"HOME",End:"END",PageUp:"PAGEUP",PageDown:"PAGEDOWN",
  PrintScreen:"PRINTSCREEN",ScrollLock:"SCROLLLOCK",Pause:"PAUSE",NumLock:"NUMLOCK",ContextMenu:"MENU",
  NumpadAdd:"NUM_ADD",NumpadSubtract:"NUM_SUB",NumpadMultiply:"NUM_MUL",NumpadDivide:"NUM_DIV",
  NumpadDecimal:"NUM_DECIMAL",NumpadEnter:"NUM_ENTER",
  MediaPlayPause:"MEDIA_PLAY",MediaTrackNext:"MEDIA_NEXT",MediaTrackPrevious:"MEDIA_PREV",MediaStop:"MEDIA_STOP",
  AudioVolumeUp:"VOL_UP",AudioVolumeDown:"VOL_DOWN",AudioVolumeMute:"VOL_MUTE",
  BrowserBack:"BROWSER_BACK",BrowserForward:"BROWSER_FORWARD",BrowserRefresh:"BROWSER_REFRESH",BrowserHome:"BROWSER_HOME"
};
const MOD_CODES = { ControlLeft:"LCTRL",ControlRight:"RCTRL",ShiftLeft:"LSHIFT",ShiftRight:"RSHIFT",
                    AltLeft:"LALT",AltRight:"RALT",MetaLeft:"LWIN",MetaRight:"RWIN" };

function codeToName(c) {
  if (/^Key[A-Z]$/.test(c)) return c.slice(3);
  if (/^Digit[0-9]$/.test(c)) return c.slice(5);
  if (/^F([1-9]|1[0-9]|2[0-4])$/.test(c)) return c;
  const m = c.match(/^Numpad([0-9])$/);
  if (m) return "NUM" + m[1];
  return CODE_NAMES[c] || null;
}

let capK = -1, capMod = null, capUsed = false;

function stopCapture() {
  capK = -1; capMod = null; capUsed = false;
  document.querySelectorAll("button.cap").forEach(b => { b.classList.remove("on"); b.textContent = "CAPTURE"; });
}
function startCapture(k) {
  if (capK === k) { stopCapture(); return; }
  stopCapture();
  capK = k;
  const b = $("cap" + k);
  b.classList.add("on"); b.textContent = "PRESS A KEY...";
}
function finishCapture(text) {
  const k = capK;
  stopCapture();
  $("in" + k).value = text;
  saveKey(k);
}

window.addEventListener("keydown", e => {
  if (capK < 0) return;
  e.preventDefault(); e.stopPropagation();
  if (MOD_CODES[e.code]) { capMod = MOD_CODES[e.code]; return; }
  const n = codeToName(e.code);
  if (!n) return;
  capUsed = true;
  const parts = [];
  if (e.ctrlKey) parts.push("CTRL");
  if (e.shiftKey) parts.push("SHIFT");
  if (e.altKey) parts.push("ALT");
  if (e.metaKey) parts.push("WIN");
  parts.push(n);
  finishCapture(parts.join("+"));
}, true);

window.addEventListener("keyup", e => {
  if (capK < 0) return;
  e.preventDefault();
  if (MOD_CODES[e.code] && capMod === MOD_CODES[e.code] && !capUsed) finishCapture(capMod); // a modifier pressed on its own
}, true);

window.addEventListener("blur", stopCapture);


// ---- custom logo (click the logo, pick a PNG; saved as config\logo.png) ----
var defaultLogoImg = null;   // var (hoisted): applyColors() can run before this line is reached

function cssVar(name) { return getComputedStyle(document.documentElement).getPropertyValue(name).trim(); }
function hexToRgb(h) { return [parseInt(h.slice(1, 3), 16), parseInt(h.slice(3, 5), 16), parseInt(h.slice(5, 7), 16)]; }

// Recolors the default logo: black pixels become the theme's art colour, white pixels the theme background.
// Always opaque. A custom (uploaded) logo is never touched.
function tintLogo() {
  try {
    if (!defaultLogoImg || (S && S.hasLogo)) return;
    const ink = hexToRgb(cssVar("--c-art")), paper = hexToRgb(cssVar("--c-bg"));
    const sc = Math.min(1, 300 / Math.max(defaultLogoImg.naturalWidth, defaultLogoImg.naturalHeight));
    const cv = document.createElement("canvas");
    cv.width = Math.max(1, Math.round(defaultLogoImg.naturalWidth * sc));
    cv.height = Math.max(1, Math.round(defaultLogoImg.naturalHeight * sc));
    const cx = cv.getContext("2d");
    cx.fillStyle = "#ffffff"; cx.fillRect(0, 0, cv.width, cv.height); // flatten transparency onto white
    cx.drawImage(defaultLogoImg, 0, 0, cv.width, cv.height);
    const d = cx.getImageData(0, 0, cv.width, cv.height), p = d.data;
    for (let i = 0; i < p.length; i += 4) {
      const t = 1 - (0.299 * p[i] + 0.587 * p[i + 1] + 0.114 * p[i + 2]) / 255; // 0 = white, 1 = black
      p[i]     = paper[0] + (ink[0] - paper[0]) * t;
      p[i + 1] = paper[1] + (ink[1] - paper[1]) * t;
      p[i + 2] = paper[2] + (ink[2] - paper[2]) * t;
      p[i + 3] = 255;
    }
    cx.putImageData(d, 0, 0);
    const img = $("logoImg");
    img.src = cv.toDataURL("image/png");
    img.style.display = "block"; $("art").style.display = "none";
  } catch (e) {}
}

function showDefaultLogo(ver) {
  const im = new Image();
  im.onload = () => { defaultLogoImg = im; tintLogo(); };
  im.onerror = () => { // no default_logo.png: fall back to the cat
    defaultLogoImg = null;
    $("logoImg").removeAttribute("src"); $("logoImg").style.display = "none"; $("art").style.display = "";
  };
  im.src = "/api/defaultlogo?v=" + ver;
}

let logoVer = -1;
function updateLogo(st) {
  if (st.logoVer === logoVer) return;
  logoVer = st.logoVer;
  $("logoReset").style.display = st.hasLogo ? "" : "none";
  if (st.hasLogo) {
    const img = $("logoImg");
    img.src = "/api/logo?v=" + st.logoVer;
    img.style.display = "block"; $("art").style.display = "none";
  } else {
    showDefaultLogo(st.logoVer);
  }
}
function pickLogo() { $("logoFile").click(); }
async function onLogoChosen(e) {
  const file = e.target.files && e.target.files[0];
  e.target.value = ""; // allow picking the same file again later
  if (!file) return;
  try {
    if (file.type !== "image/png") { alert("Please choose a .png file."); return; }
    const bmp = await createImageBitmap(file);
    const ratio = bmp.width / bmp.height;
    if (ratio < 0.85 || ratio > 1.18) {
      if (!confirm("This image is " + bmp.width + "x" + bmp.height + ", not square. A 1:1 (square) image is recommended; others will show smaller. Use it anyway?")) return;
    }
    const MAX = 256, sc = Math.min(1, MAX / Math.max(bmp.width, bmp.height));
    const cv = document.createElement("canvas");
    cv.width = Math.max(1, Math.round(bmp.width * sc)); cv.height = Math.max(1, Math.round(bmp.height * sc));
    cv.getContext("2d").drawImage(bmp, 0, 0, cv.width, cv.height);
    const blob = await new Promise(r => cv.toBlob(r, "image/png"));
    const resp = await fetch("/api/logo", { method: "POST", headers: { "X-ESPdevice": "1", "Content-Type": "image/png" }, body: blob });
    const st = await resp.json();
    if (st.error) { alert(st.error); return; }
    applyState(st);
  } catch (err) { alert("Could not use that image."); }
}
async function resetLogo(ev) {
  ev.stopPropagation(); // don't also open the file picker
  if (!confirm("Go back to the default logo?")) return;
  applyState(await api("/api/logo/reset", {}));
}

// Title art: config\art.txt is trimmed (blank edges, shared left indent) and scaled to fit the box above.
async function loadArt() {
  try {
    const t = await (await fetch("/api/art")).text();
    let lines = t.replace(/\r/g, "").split("\n").map(l => l.replace(/\s+$/, ""));
    while (lines.length && !lines[0].trim()) lines.shift();
    while (lines.length && !lines[lines.length - 1].trim()) lines.pop();
    if (!lines.length) return;
    const indent = Math.min(...lines.filter(l => l.trim()).map(l => l.match(/^ */)[0].length));
    const el = $("art");
    el.textContent = lines.map(l => l.slice(indent)).join("\n");
    el.style.lineHeight = "1";
    el.style.fontSize = "10px";
    const w = el.scrollWidth, h = el.scrollHeight;
    el.style.fontSize = (10 * Math.min(ART_MAX_W / w, ART_MAX_H / h)).toFixed(2) + "px";
  } catch (e) {}
}

// ---- CPS (presses in the last second) and Total (all presses, counted by the app and saved across restarts) ----
let replayUntil = 0, cpsTimes = [], cTotal = 0, cSrv = -1;
function paintCounters() {
  const now = performance.now();
  while (cpsTimes.length && now - cpsTimes[0] > 1000) cpsTimes.shift();
  const c = $("cCps"), t = $("cTotal"), cs = String(cpsTimes.length), ts = cTotal.toLocaleString("en-US");
  if (c.textContent !== cs) c.textContent = cs;
  if (t.textContent !== ts) t.textContent = ts;
}
function cpsHit() {
  cpsTimes.push(performance.now()); cTotal++; paintCounters();
  document.querySelectorAll(".counters div").forEach(d => { d.classList.remove("flash"); void d.offsetWidth; d.classList.add("flash"); });   // flash on every press
}
function syncTotal(total, since) {
  if (total !== cSrv) { cSrv = total; cTotal = total; paintCounters(); }
  if (since > 0) $("cTotalRow").title = "since " + new Date(since * 1000).toLocaleDateString();
}
async function resetCounters() {
  if (!confirm("Reset the CPS and Total counters to zero?")) return;
  cpsTimes = []; applyState(await api("/api/counters/reset", {}));
}
setInterval(paintCounters, 100);

(function init() {
  wmInit().catch(() => { const d = $("desk"); if (d) d.classList.add("ready"); }); // windows (see the WINDOWS section)
  loadTheme();
  loadArt();
  const dl = $("keynames");
  KEY_NAMES.forEach(n => { const o = document.createElement("option"); o.value = n; dl.appendChild(o); });
  buildKeyBoxes();
  poll();
  startStream();
  setInterval(poll, 1000); // slow resync only; key presses arrive instantly through the event stream
})();
</script>
</body>
</html>
)HTMLX";

static std::string WebsiteHtml() {
    std::string s;
    s.reserve(65536);
    s += HTML_HEAD;
    s += HTML_CSS_WIN;
    s += HTML_BODY;
    s += HTML_JS1;
    s += HTML_JS_THEME;
    s += HTML_JS_WM1;
    s += HTML_JS_WM2;
    s += HTML_JS2;
    const std::string marker = "@@VER@@", ver = ESP_VERSION;
    for (size_t p = s.find(marker); p != std::string::npos; p = s.find(marker, p + ver.size())) s.replace(p, marker.size(), ver);
    return s;
}
