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
#include <array>
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
constexpr const char* DEVICE_ID = "3KEY";    // the original firmware
constexpr const char* DEVICE_ID8 = "8KEY";   // the 8-key capable firmware (works with 3 keys too)
constexpr int NUM_KEYS = 8;                  // keys the app can hold (profiles always store all 8 bindings)
static std::atomic<int> g_activeKeys{ 3 };   // keys in use: 3, or 8 with SETTINGS > 8-key support (experimental)
static std::atomic<bool> g_keysChanged{ false };
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

// 3-key mode keeps the original files (profiles.json / active_profile.txt, unchanged format);
// 8-key mode has its own pair so the two lists never mix.
static fs::path ProfilesPath() { return g_activeKeys.load() >= 8 ? g_configDir / L"profiles8.json" : g_profilesPath; }
static fs::path ActiveIdxPath() { return g_activeKeys.load() >= 8 ? g_configDir / L"active_profile8.txt" : g_activeIndexPath; }

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
    bool isTest = false;   // the built-in "test" profile: never saved, no bindings, cannot be renamed or removed
};

static std::vector<Profile> g_profiles;
static int g_activeIndex = 0;

static const char* DEFAULT_BINDINGS[NUM_KEYS] = { "A", "S", "D", "F", "G", "H", "J", "K" };

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

static int RealCount() { int n = 0; for (const auto& p : g_profiles) if (!p.isTest) n++; return n; }
static int g_lastReal = 0;   // last selected non-test profile (what gets remembered on disk)

static std::string ProfilesToJson(bool forDisk = false) {
    std::ostringstream out;
    out << "[";
    bool first = true;
    for (size_t i = 0; i < g_profiles.size(); i++) {
        const Profile& p = g_profiles[i];
        if (p.isTest && forDisk) continue;          // the test profile is never written to disk
        if (!first) out << ",";
        first = false;
        out << "{\"name\":\"" << JsonEscape(p.name) << "\",\"bindings\":[";
        for (int k = 0; k < NUM_KEYS; k++) {
            out << "\"" << JsonEscape(p.bindings[k]) << "\"";
            if (k < NUM_KEYS - 1) out << ",";
        }
        out << "]" << (p.isTest ? ",\"test\":true" : "") << "}";
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
// 8-key mode lives in the page settings (config\settings.json, "eightKeys":true); the app reads the flag here
static std::atomic<bool> g_logStats{ false };   // SETTINGS > log click stats
static void LoadProfilesLocked();
static void PublishBindingsLocked();
static std::mutex g_profileMutex;
static std::atomic<bool> g_profilesLoaded{ false };
static void ApplyKeyMode(const std::string& settingsJson) {
    int n = settingsJson.find("\"eightKeys\":true") != std::string::npos ? 8 : 3;
    g_logStats = settingsJson.find("\"logStats\":true") != std::string::npos;
    if (g_activeKeys.exchange(n) != n) {
        g_keysChanged = true;
        if (g_profilesLoaded) {                       // each mode has its own profile list: switch lists
            try { std::lock_guard<std::mutex> lk(g_profileMutex); LoadProfilesLocked(); PublishBindingsLocked(); }
            catch (...) {}
        }
    }
}
static void LoadKeyMode() {
    std::ifstream f(g_configDir / L"settings.json", std::ios::binary);
    if (!f.is_open()) return;
    std::ostringstream ss; ss << f.rdbuf();
    ApplyKeyMode(ss.str());
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
// ---- Click statistics (new in 1.2) --------------------------------------------
// Per-key counts since the app started (shown under the keys), a per-minute history kept in memory (the SESSION graph),
// and, only while SETTINGS > "log click stats" is on, per-day counts saved to config\clickstats.json
//   {"v":1,"days":{"2026-11-03":[k1,k2,...,k8], ...}}   (a separate versioned file: counters.json is untouched)
static std::atomic<uint64_t> g_keyClicks[NUM_KEYS];
struct MinSlot { std::atomic<int64_t> minute; std::atomic<uint32_t> c[NUM_KEYS]; };
constexpr int MIN_SLOTS = 1440;                       // the last 24 hours, one slot per minute
static MinSlot g_min[MIN_SLOTS];
static std::atomic<int64_t> g_sessStartMin{ 0 };
static std::atomic<uint64_t> g_dayCnt[NUM_KEYS];      // counted since the last fold into g_days (only while logging)
static std::mutex g_statsMx;
static std::map<std::string, std::array<uint64_t, NUM_KEYS>> g_days;
static bool g_statsDirty = false;

static void CountKeyClick(int idx) {                  // device thread, after the keystroke was sent
    g_keyClicks[idx].fetch_add(1, std::memory_order_relaxed);
    int64_t m = (int64_t)(time(nullptr) / 60);
    MinSlot& sl = g_min[m % MIN_SLOTS];
    if (sl.minute.load(std::memory_order_relaxed) != m) {   // this slot still holds an older minute: reuse it
        for (int k = 0; k < NUM_KEYS; k++) sl.c[k].store(0, std::memory_order_relaxed);
        sl.minute.store(m, std::memory_order_relaxed);
    }
    sl.c[idx].fetch_add(1, std::memory_order_relaxed);
    if (g_logStats.load(std::memory_order_relaxed)) g_dayCnt[idx].fetch_add(1, std::memory_order_relaxed);
}
static std::string LocalDateNow() {
    std::time_t t = std::time(nullptr); std::tm tmv; localtime_s(&tmv, &t);
    char b[16]; strftime(b, sizeof(b), "%Y-%m-%d", &tmv); return b;
}
static void FoldDay() {                               // moves the counts since the last fold into today's entry
    std::lock_guard<std::mutex> lk(g_statsMx);
    std::string d = LocalDateNow();
    for (int k = 0; k < NUM_KEYS; k++) {
        uint64_t v = g_dayCnt[k].exchange(0);
        if (v) { g_days[d][k] += v; g_statsDirty = true; }
    }
}
static std::string StatsFileJson() {                  // caller holds g_statsMx
    std::ostringstream o; o << "{\"v\":1,\"days\":{";
    bool first = true;
    for (const auto& kv : g_days) {
        if (!first) o << ","; first = false;
        o << "\"" << kv.first << "\":[";
        for (int k = 0; k < NUM_KEYS; k++) o << (k ? "," : "") << kv.second[k];
        o << "]";
    }
    o << "}}"; return o.str();
}
static void SaveStats(bool force) {
    std::lock_guard<std::mutex> lk(g_statsMx);
    if (!g_statsDirty && !force) return;
    WriteFileAtomic(g_configDir / L"clickstats.json", StatsFileJson());
    g_statsDirty = false;
}
static void LoadStats() {
    g_sessStartMin = (int64_t)(time(nullptr) / 60);
    std::ifstream f(g_configDir / L"clickstats.json", std::ios::binary);
    if (!f.is_open()) return;
    std::ostringstream ss; ss << f.rdbuf(); std::string t = ss.str();
    size_t p = t.find("\"days\"");
    if (p == std::string::npos) return;
    std::lock_guard<std::mutex> lk(g_statsMx);
    p += 6;
    while (p < t.size()) {                             // "YYYY-MM-DD":[n,n,...]
        size_t q1 = t.find('"', p); if (q1 == std::string::npos) break;
        size_t q2 = t.find('"', q1 + 1); if (q2 == std::string::npos) break;
        std::string d = t.substr(q1 + 1, q2 - q1 - 1);
        size_t b1 = t.find('[', q2); size_t b2 = b1 == std::string::npos ? b1 : t.find(']', b1);
        if (b2 == std::string::npos) break;
        p = b2 + 1;
        if (d.size() != 10 || d[4] != '-' || d[7] != '-') continue;
        std::array<uint64_t, NUM_KEYS> a{};
        const char* c = t.c_str() + b1 + 1;
        for (int k = 0; k < NUM_KEYS; k++) { char* e = nullptr; a[k] = strtoull(c, &e, 10); if (e == c) break; c = e; while (*c == ',' || *c == ' ') c++; }
        g_days[d] = a;
    }
}
static void ClearStats() {
    { std::lock_guard<std::mutex> lk(g_statsMx); g_days.clear(); for (int k = 0; k < NUM_KEYS; k++) g_dayCnt[k] = 0; g_statsDirty = true; }
    for (int i = 0; i < MIN_SLOTS; i++) { g_min[i].minute = -1; for (int k = 0; k < NUM_KEYS; k++) g_min[i].c[k] = 0; }
    for (int k = 0; k < NUM_KEYS; k++) g_keyClicks[k] = 0;
    g_sessStartMin = (int64_t)(time(nullptr) / 60);
    SaveStats(true);
}
static std::string BuildStatsJson(bool all) {
    std::ostringstream o;
    int n = g_activeKeys.load();
    if (!all) {                                        // SESSION: clicks per minute since the app started (at most the last 24 h)
        int64_t now = (int64_t)(time(nullptr) / 60), start = std::max<int64_t>(g_sessStartMin.load(), now - (MIN_SLOTS - 1));
        o << "{\"mode\":\"session\",\"keys\":" << n << ",\"startMin\":" << start << ",\"nowMin\":" << now << ",\"data\":[";
        for (int64_t m = start; m <= now; m++) {
            const MinSlot& sl = g_min[m % MIN_SLOTS];
            bool has = sl.minute.load() == m;
            o << (m > start ? "," : "") << "[";
            for (int k = 0; k < NUM_KEYS; k++) o << (k ? "," : "") << (has ? sl.c[k].load() : 0u);
            o << "]";
        }
        o << "]}";
    } else {                                           // ALL TIME: clicks per day (only days that were logged)
        FoldDay();
        std::lock_guard<std::mutex> lk(g_statsMx);
        o << "{\"mode\":\"all\",\"keys\":" << n << ",\"logging\":" << (g_logStats ? "true" : "false") << ",\"days\":{";
        bool first = true;
        for (const auto& kv : g_days) {
            if (!first) o << ","; first = false;
            o << "\"" << kv.first << "\":[";
            for (int k = 0; k < NUM_KEYS; k++) o << (k ? "," : "") << kv.second[k];
            o << "]";
        }
        o << "}}";
    }
    return o.str();
}

static std::string KeyClicksJson() { std::string r; for (int k = 0; k < NUM_KEYS; k++) r += (k ? "," : "") + std::to_string(g_keyClicks[k].load()); return r; }

static void ResetCounters() {
    for (int k = 0; k < NUM_KEYS; k++) g_keyClicks[k] = 0;   // the per-key numbers under the keys reset with the Total
    g_clicksTotal = 0;
    g_clicksSince = (int64_t)time(nullptr);
    SaveCounters(true);
}
static void CounterSaverLoop() { // writes the file a few seconds after the number changed, never from the key path
    while (g_running) {
        for (int i = 0; i < 50 && g_running; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        SaveCounters(false);
        FoldDay(); SaveStats(false);   // per-day click stats (only anything while "log click stats" is on)
    }
}

static void SaveProfilesLocked() {
    WriteFileAtomic(ProfilesPath(), ProfilesToJson(true));
    WriteFileAtomic(ActiveIdxPath(), std::to_string(g_lastReal));
}

// Parses exactly what SaveProfilesLocked writes (names are sanitized so
// they can never contain braces/brackets/quotes). Anything unreadable
// falls back to defaults instead of failing.
static void LoadProfilesLocked() {
    g_profiles.clear();

    std::error_code fec;
    // one-time safety copy of the pre-1.2 profile file (3-key list stays in the same format)
    {
        fs::path bak = g_configDir / L"profiles.pre-1.2.bak";
        if (fs::exists(g_profilesPath, fec) && !fs::exists(bak, fec)) fs::copy_file(g_profilesPath, bak, fec);
    }
    // first time in 8-key mode: start from a copy of the 3-key list (keys 4-8 keep their F G H J K bindings)
    fs::path src = ProfilesPath();
    bool seeded = false;
    if (g_activeKeys.load() >= 8 && !fs::exists(src, fec)) { src = g_profilesPath; seeded = true; }

    std::ifstream file(src);
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
    std::ifstream idxFile(seeded ? g_activeIndexPath : ActiveIdxPath());
    if (idxFile.is_open()) {
        int idx;
        if (idxFile >> idx && idx >= 0 && idx < (int)g_profiles.size()) g_activeIndex = idx;
    }
    g_lastReal = g_activeIndex;

    Profile t;                                   // the built-in test profile: always last, never saved
    t.name = "test"; t.isTest = true;
    g_profiles.push_back(t);                     // bindings stay empty = sends nothing

    if (seeded) { WriteFileAtomic(ProfilesPath(), ProfilesToJson(true)); WriteFileAtomic(ActiveIdxPath(), std::to_string(g_lastReal)); }
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

static bool g_extNoted[NUM_KEYS];   // debugger-log notes about extension keys (device thread only; reset on connect / mode change)

static void HandleByte(BYTE b, double now, int64_t tRecv) {
    if (b < 1 || b > g_activeKeys.load() * 2) {   // identity replies / keys that are not enabled / anything else
        if (b >= 1 && b <= NUM_KEYS * 2 && !g_extNoted[(b - 1) / 2]) {   // say so once, so "my extension does nothing" can be diagnosed in the DEBUGGER
            g_extNoted[(b - 1) / 2] = true;
            PushEvent(EVT_INFO, (b - 1) / 2, 0, "key signal received but ignored: 8-key support is off in SETTINGS");
        }
        return;
    }
    int idx = (b - 1) / 2;
    bool isPress = (b & 1) != 0;

    g_lastSeen[idx] = now;
    if (idx >= 3 && !g_extNoted[idx]) {   // first sign of life from each extension key
        g_extNoted[idx] = true;
        PushEvent(EVT_INFO, idx, 0, "extension key is sending signals");
    }

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
        CountKeyClick(idx);
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
    if (g_keysChanged.exchange(false)) {   // 3-key / 8-key mode was switched: let go of everything
        ReleaseAllKeys();
        for (int i = 0; i < NUM_KEYS; i++) g_extNoted[i] = false;
        PushEvent(EVT_INFO, -1, 0, g_activeKeys.load() >= 8 ? "8-key mode on" : "8-key mode off");
    }
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
            if ((reply.find(DEVICE_ID) != std::string::npos || reply.find(DEVICE_ID8) != std::string::npos) && reply.find('\n') != std::string::npos) return true;
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

    out << "],\"keys\":" << g_activeKeys.load() << ",\"connected\":" << (g_connected ? "true" : "false")
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
        << ",\"logStats\":" << (g_logStats ? "true" : "false") << ",\"keyClicks\":[" << KeyClicksJson() << "]"
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
    std::string query;
    if (queryPos != std::string::npos) { query = path.substr(queryPos + 1); path.resize(queryPos); } // ?v=... cache-busters are ignored

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
    else if (method == "GET" && path == "/api/themeadv") {
        std::string adv;
        {
            std::lock_guard<std::mutex> lk(g_themeMutex);
            std::ifstream f(g_configDir / L"theme_advanced.json", std::ios::binary);
            if (f.is_open()) { std::ostringstream ss; ss << f.rdbuf(); adv = ss.str(); }
        }
        response = HttpResponse("200 OK", "application/json", adv.empty() ? "{}" : adv);
    }
    else if (method == "POST" && path == "/api/themeadv") {   // advanced theming edits (stored as sent; the page validates every value)
        if (body.empty() || body.size() > 300000 || body[0] != '{') {
            response = HttpResponse("400 Bad Request", "application/json", "{\"ok\":false}");
        } else {
            std::lock_guard<std::mutex> lk(g_themeMutex);
            WriteFileAtomic(g_configDir / L"theme_advanced.json", body);
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
            if (path == "/api/settings") ApplyKeyMode(body);
            response = HttpResponse("200 OK", "application/json", "{\"ok\":true}");
        }
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
            if (!g_profiles[idx].isTest) g_lastReal = idx;
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
        } else if (g_profiles[idx].isTest) {
            err = "the test profile has no bindings";
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
        else if (RealCount() >= MAX_PROFILES) err = "profile limit reached";
        else {
            g_profiles.insert(g_profiles.end() - 1, DefaultProfile(name));   // always before the test profile
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
        else if (g_profiles[idx].isTest) err = "the test profile cannot be renamed";
        else { g_profiles[idx].name = name; SaveProfilesLocked(); }
        response = HttpResponse("200 OK", "application/json", BuildStateJsonLocked(err));
    }
    else if (method == "POST" && path == "/api/delete") {
        int idx = JsonGetInt(body, "index");
        std::string err;
        std::lock_guard<std::mutex> lock(g_profileMutex);
        if (idx >= 0 && idx < (int)g_profiles.size() && g_profiles[idx].isTest) err = "the test profile cannot be removed";
        else if (RealCount() <= 1) err = "cannot delete the last profile";
        else if (idx < 0 || idx >= (int)g_profiles.size()) err = "bad profile index";
        else {
            bool wasActive = (g_activeIndex == idx);
            g_profiles.erase(g_profiles.begin() + idx);
            if (g_activeIndex > idx) g_activeIndex--;
            else if (wasActive) g_activeIndex = std::min(idx, RealCount() - 1);
            if (g_lastReal > idx) g_lastReal--;
            if (wasActive) g_lastReal = g_activeIndex;
            if (g_lastReal >= RealCount()) g_lastReal = RealCount() - 1;
            PublishBindingsLocked();
            SaveProfilesLocked();
        }
        response = HttpResponse("200 OK", "application/json", BuildStateJsonLocked(err));
    }
    else if (method == "GET" && path == "/api/stats") {
        response = HttpResponse("200 OK", "application/json", BuildStatsJson(query.find("mode=all") != std::string::npos));
    }
    else if (method == "POST" && path == "/api/stats/clear") {
        ClearStats();
        response = HttpResponse("200 OK", "application/json", BuildStateJson());
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
    FoldDay(); SaveStats(false);
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
    LoadStats();
    LoadKeyMode();
    SetUnhandledExceptionFilter(CrashFilter);
    InitClock();
    for (int i = 0; i < NUM_KEYS; i++) { g_held[i] = g_sent[i] = false; g_nextRepeat[i] = -1; g_lastSeen[i] = 0; g_heldView[i] = false; }

    try {
        std::lock_guard<std::mutex> lock(g_profileMutex);
        LoadProfilesLocked();
        PublishBindingsLocked();
        g_profilesLoaded = true;
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
    FoldDay(); SaveStats(false);
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
.win { position: absolute; display: flex; flex-direction: column; background: var(--c-bg); border: 1px solid var(--c-border); min-width: 0; min-height: 0; overflow: hidden; padding-bottom: 12px; }
.wbar { flex: none; display: flex; align-items: center; gap: 8px; padding: 6px 8px 4px 8px; cursor: move; user-select: none; -webkit-user-select: none; touch-action: none; }
.wbar h2 { margin: 0; font-size: 1em; font-weight: normal; color: var(--c-heading); flex: 1; min-width: 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.win.front .wbar h2 { color: var(--c-fg); }
.grip { flex: none; width: 1.5em; height: 1.5em; border: 1px solid var(--c-border); display: flex; align-items: center; justify-content: center; font-size: 0.8em; line-height: 1; color: var(--c-dim); }
.wbtn { flex: none; width: 1.7em; height: 1.7em; padding: 0; display: flex; align-items: center; justify-content: center; line-height: 1; cursor: pointer; }
.wbody { flex: 1 1 auto; min-height: 0; overflow: auto; padding: 4px 12px 2px 12px; scrollbar-width: thin; scrollbar-color: var(--c-border) var(--c-bg); }
.rsz { position: absolute; left: 0; bottom: 0; width: 12px; height: 12px; cursor: nesw-resize; touch-action: none; z-index: 2; }
.rsz::after { content: ""; position: absolute; left: 2px; bottom: 2px; width: 7px; height: 7px; border-left: 2px solid var(--c-dim); border-bottom: 2px solid var(--c-dim); }
.rsz:hover::after { border-color: var(--c-fg); }
.rsz.r { left: auto; right: 0; cursor: nwse-resize; }
.rsz.r::after { left: auto; right: 2px; border-left: 0; border-right: 2px solid var(--c-dim); }
.wbody > :last-child { margin-bottom: 0; }
.pix { display: block; background-color: currentColor; -webkit-mask: var(--pix) 0 0 / 100% 100% no-repeat; mask: var(--pix) 0 0 / 100% 100% no-repeat; image-rendering: pixelated; pointer-events: none; }   /* the built-in 16x16 icons, drawn in the text colour */
.wicon { flex: none; width: 16px; height: 16px; margin: 0 4px; color: var(--c-fg); }
/* minimized: a small icon window */
.mini { position: absolute; display: none; background: var(--c-bg); border: 1px solid var(--c-border); cursor: move; touch-action: none; user-select: none; -webkit-user-select: none; overflow: hidden; }
.mimg { position: absolute; left: 0; top: 0; width: 32px; height: 32px; color: var(--c-fg); }   /* size and position are set in whole pixels by wmFitMini so the pixels stay sharp and centred */
.mcar { position: absolute; left: 3px; top: 3px; width: 18px; height: 18px; min-width: 0; padding: 0; font-size: 12px; line-height: 1; display: flex; align-items: center; justify-content: center; z-index: 3; }
/* corner widgets */
.tbtns { position: fixed; right: 16px; top: 16px; z-index: 3; display: grid; grid-template-columns: auto auto; gap: 8px; justify-items: end; }
.tbt { grid-column: 1; grid-row: 1; } .tbs { grid-column: 2; grid-row: 1; } .tbd { grid-column: 2; grid-row: 2; }
.tbtns.open { z-index: 100 !important; }   /* an open pull-down is always above the windows */
.tb { position: relative; }
.tb.mo { z-index: 5; }   /* the button whose panel is open (and the panel) sit above the other buttons: the panel covers DEBUGGER instead of hiding behind it */
.gear.on { border-color: var(--c-border); position: relative; z-index: 2; }
/* pull-down menus: hang from their button (the button is the tab at the top-right corner of the panel) */
.menu { position: absolute; right: 0; top: 100%; margin-top: -1px; display: none; background: var(--c-bg); border: 1px solid var(--c-border); padding: 6px 14px 14px 14px; overflow: auto; width: 360px; max-width: calc(100vw - 32px); max-height: calc(100vh - 116px); transform-origin: 100% 0; scrollbar-width: thin; scrollbar-color: var(--c-border) var(--c-bg); }
#menu-theme { width: 480px; }
#menu-debugger { width: 560px; max-height: calc(100vh - 170px); }
#menu-debugger #log { height: min(300px, calc(100vh - 420px)); min-height: 90px; }
button:disabled { opacity: 0.45; cursor: default; }
button:disabled:hover { background: var(--c-btn-bg); color: var(--c-btn-fg); }
.err:empty { display: none; }
.keys.k8 { gap: 8px; }
.keys.k8 .key { width: 56px; height: 56px; }
.keys.k8 .key .n { font-size: 1.2em; } .keys.k8 .key .b { font-size: 0.7em; margin-top: 2px; max-width: 48px; }
.mode8 { position: fixed; left: 50%; bottom: 12px; transform: translateX(-50%); z-index: 1; margin: 0; white-space: nowrap; color: var(--c-status); }
/* opening: a thin bar spreads out from the button along the x axis, then the panel grows downward (30 fps = 12 steps in 0.4 s) */
.menu.opening { animation: mopen 0.4s linear both; }
@keyframes mopen {
  0% { clip-path: inset(0 0 calc(100% - 6px) 100%); animation-timing-function: steps(5, end); }
  40% { clip-path: inset(0 0 calc(100% - 6px) 0); animation-timing-function: steps(7, end); }
  100% { clip-path: inset(0 0 0 0); }
}
/* closing: shrink and fade out */
.menu.closing { animation: mclose 0.4s steps(12, end) both; }
@keyframes mclose { from { transform: none; opacity: 1; } to { transform: scale(0.82); opacity: 0; } }
.menu.opening.oldanim { animation: mzoom 0.4s steps(12, end) both; }   /* SETTINGS > old animations: zoom and fade */
@keyframes mzoom { from { transform: scale(0.82); opacity: 0; } to { transform: none; opacity: 1; } }
@media (prefers-reduced-motion: reduce) { .menu.opening, .menu.closing { animation: none; } }
html.noanim .menu.opening, html.noanim .menu.closing, html.noanim .crow span.pop { animation: none !important; }   /* SETTINGS > disable animations */
.gear img { display: block; width: 24px; height: 24px; object-fit: contain; pointer-events: none; }
.divider { position: fixed; left: 0; right: 0; bottom: 64px; height: 1px; background: var(--c-border); opacity: 0.5; z-index: 1; pointer-events: none; }
.socials { position: fixed; left: 16px; bottom: 12px; z-index: 1; display: flex; gap: 14px; }
.socials a { color: var(--c-dim); text-decoration: none; display: flex; align-items: center; }
.socials a:hover { color: var(--c-fg); text-decoration: underline; }
.br { position: fixed; right: 16px; bottom: 12px; z-index: 1; text-align: right; }
.credit { opacity: 0.5; color: var(--c-credit); }
/* per-window tweaks */
.vis { display: flex; flex-direction: column; align-items: center; gap: 12px; min-width: 300px; padding-bottom: 10px; }   /* room for the CPS pop, so it never makes a scrollbar */
.vis .keys { justify-content: center; }
.crow { align-self: stretch; display: flex; align-items: center; gap: 16px; white-space: nowrap; color: var(--c-stat-label); line-height: 1.5; min-height: 1.8em; }
.crow span { display: inline-block; color: var(--c-stat-value); transform-origin: 50% 65%; }
#cCpsRow { font-size: 1.2em; } #cCps { font-size: 1.2em; }
#cTotalRow { margin-left: auto; font-size: 1.2em; text-align: right; }   /* same size as CPS */
#cTotal { font-size: 1.2em; }
.crow span.pop { animation: cpop 0.3s steps(9, end); }   /* only the CPS number pops: size only, never see-through (30 fps) */
@keyframes cpop { from { transform: scale(1.45); } to { transform: scale(1); } }
.crow span.cps1 { color: #e8890c; } .crow span.cps2 { color: #e02020; } .crow span.cps3 { color: #9b30ff; }
.kcol { display: flex; flex-direction: column; align-items: center; gap: 4px; }
.kt { color: var(--c-stat-value); font-size: 0.9em; white-space: nowrap; line-height: 1.2; min-height: 1.2em; }
.keys.k8 .kt { font-size: 0.75em; }
.statbtn svg { display: block; width: 70%; height: 70%; fill: currentColor; }
.statspanel { position: fixed; left: 50%; top: 96px; transform: translateX(-50%); width: min(700px, calc(100vw - 24px)); max-height: calc(100vh - 150px); overflow: auto; background: var(--c-bg); border: 1px solid var(--c-border); z-index: 90; padding: 6px 14px 12px 14px; scrollbar-width: thin; scrollbar-color: var(--c-border) var(--c-bg); }
.sbar { display: flex; align-items: center; gap: 8px; padding: 2px 0 4px 0; } .sbar h2 { flex: 1; margin: 0; font-size: 1em; font-weight: normal; color: var(--c-fg); }
.stabs, .skeys { display: flex; flex-wrap: wrap; gap: 6px; margin: 6px 0; }
.stab.on, .skeys button.on { background: var(--c-btn-active-bg); color: var(--c-btn-active-fg); }
.schart { position: relative; height: min(260px, calc(100vh - 380px)); min-height: 120px; border: 1px solid var(--c-log-border); background: var(--c-log-bg); }
#statsChart { width: 100%; height: 100%; overflow: hidden; } #statsChart svg { display: block; }
.sbarr { fill: var(--c-stat-value); } .sgrid { stroke: var(--c-border); opacity: 0.4; } .stxt { fill: var(--c-dim); font-size: 11px; font-family: inherit; }
.sempty { padding: 18px; color: var(--c-dim); }
.stip { position: absolute; display: none; pointer-events: none; background: var(--c-bg); border: 1px solid var(--c-border); padding: 2px 6px; white-space: nowrap; z-index: 2; }
.advgrp { margin: 8px 0; border: 1px solid var(--c-border); padding: 2px 8px; }
.advgrp summary { cursor: pointer; color: var(--c-heading); padding: 4px 0; user-select: none; }
.advrow { margin: 6px 0 8px; padding: 6px 0 2px; border-top: 1px dashed var(--c-dim); }
.advrow.flash { outline: 2px solid var(--c-fg); outline-offset: 2px; }
.advlab { display: block; margin-bottom: 4px; }
.advprops { display: flex; flex-wrap: wrap; gap: 6px 14px; }
.advp { display: flex; align-items: center; gap: 4px; }
.advp .lab { color: var(--c-dim); } .advp.ov .lab { color: var(--c-fg); text-decoration: underline; }
.advp input[type=text] { width: 78px; } .advp input[type=number] { width: 56px; font-family: var(--font); font-size: var(--size); color: var(--c-input-fg); background: var(--c-input-bg); border: 1px solid var(--c-input-border); }
.advp .x { padding: 0 6px; }
#pickBox { position: fixed; z-index: 300; pointer-events: none; outline: 2px solid #ff2bd6; background: rgba(255, 43, 214, 0.12); display: none; }
#pickLbl { position: fixed; z-index: 301; pointer-events: none; background: #ff2bd6; color: #fff; padding: 1px 6px; font-size: 12px; display: none; white-space: nowrap; }
#pickBar { position: fixed; left: 50%; top: 10px; transform: translateX(-50%); z-index: 302; background: var(--c-bg); color: var(--c-fg); border: 1px solid var(--c-border); padding: 6px 12px; display: none; }
body.picking, body.picking * { cursor: crosshair !important; }
.opt { display: flex; align-items: center; gap: 8px; margin: 5px 0; cursor: pointer; user-select: none; }
.opt input { width: 14px; height: 14px; margin: 0; accent-color: var(--c-fg); }
/* small / square browser window: the social links hide, and [8-key Mode Active] takes their place at the bottom left */
@media (max-width: 780px) { .credit { display: none; } .socials { display: none; } .mode8 { left: 16px; transform: none; } }
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
<div class="tbtns" id="tbtns">
<div class="tb tbt"><button class="gear" id="themeBtn" onclick="menuToggle('theme')" title="Themes">[ THEMES ]</button>
<div class="menu" id="menu-theme">
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
<div id="advWrap" style="display:none"></div>
</div></div>
<div class="tb tbs"><button class="gear" id="gearBtn" onclick="menuToggle('settings')" title="Settings">[ SETTINGS ]</button>
<div class="menu" id="menu-settings">
<div class="setgrp">WINDOWS</div>
<div class="row"><button onclick="resetLayout()">RESET WINDOW LAYOUT</button></div>
<label class="opt"><input type="checkbox" id="optGearTop" onchange="wmSettingChanged()"> keep the settings and themes buttons on top of the windows</label>
<div class="setgrp">KEYPAD</div>
<label class="opt"><input type="checkbox" id="optEight" onchange="wmSettingChanged()"> 8-key support (experimental)</label>
<p class="dim">Needs the 8-key firmware and 5 extra keys (see the README).</p>
<div class="setgrp">COUNTERS</div>
<div class="row"><button onclick="resetCounters()">RESET TOTAL / CPS</button></div>
<label class="opt"><input type="checkbox" id="optCpsOff" onchange="wmSettingChanged()"> disable CPS counter</label>
<label class="opt"><input type="checkbox" id="optCpsColor" onchange="wmSettingChanged()"> colored cps</label>
<div class="setgrp">STATISTICS</div>
<label class="opt"><input type="checkbox" id="optPerKey" onchange="wmSettingChanged()"> per-key totals</label>
<label class="opt"><input type="checkbox" id="optLogStats" onchange="wmSettingChanged()"> log click stats</label>
<p class="dim">Logging saves clicks per day to config\clickstats.json (only while it is on). It is needed for the ALL TIME graph.</p>
<div class="row"><button onclick="clearStats()">CLEAR CLICK STATS</button></div>
<div class="setgrp">THEMING</div>
<label class="opt"><input type="checkbox" id="optAdvTheme" onchange="wmSettingChanged()"> advanced theming</label>
<p class="dim">Edit any element of the page in the THEMES panel (on a custom theme). Off = the normal theme only.</p>
<div class="setgrp">ANIMATIONS</div>
<label class="opt"><input type="checkbox" id="optAnimOff" onchange="wmSettingChanged()"> disable animations</label>
<label class="opt"><input type="checkbox" id="optOldAnim" onchange="wmSettingChanged()"> old animations</label>
<div class="setgrp">SERVICE</div>
<div class="row"><button id="pauseBtn" onclick="toggleService()">PAUSE</button><button onclick="terminateService()">TERMINATE</button></div>
<div class="setgrp">SOCIAL LINKS (bottom left)</div>
<label class="opt"><input type="checkbox" id="optSocial_github" onchange="wmSettingChanged()"> GitHub</label>
<label class="opt"><input type="checkbox" id="optSocial_youtube" onchange="wmSettingChanged()"> YouTube</label>
<label class="opt"><input type="checkbox" id="optSocial_tiktok" onchange="wmSettingChanged()"> TikTok</label>
<label class="opt"><input type="checkbox" id="optSocial_discord" onchange="wmSettingChanged()"> Discord</label>
<p class="dim">More options will show up here in future versions. Saved in config\settings.json.</p>
</div></div>
<div class="tb tbd"><button class="gear" id="dbgBtn" onclick="menuToggle('debugger')" title="Debugger">[ DEBUGGER ]</button>
<div class="menu" id="menu-debugger">
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
</div></div>
</div>

<div id="desk">

<section class="win" id="w-profile" data-title="PROFILE" data-abbr="PRO"><div class="wbody">
<div class="row">
  <select id="profileSelect" onchange="selectProfile()"></select>
  <button onclick="addProfile()">+ ADD</button>
  <button id="btnRen" onclick="renameProfile()">RENAME</button>
  <button id="btnDel" onclick="deleteProfile()">DELETE</button>
</div>
<p class="dim" id="profileCount"></p>
</div></section>

<section class="win" id="w-visualizer" data-title="VISUALIZER" data-abbr="VIS"><div class="wbody">
<div class="vis">
  <div class="keys" id="keys"></div>
  <div class="crow">
    <div id="cCpsRow">CPS: <span id="cCps">0</span></div>
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



</div>
<div class="statspanel" id="statsPanel" style="display:none">
<div class="sbar"><h2>[ CLICK STATS ]</h2><button class="wbtn" onclick="statsClose()" title="Close" aria-label="Close click stats">x</button></div>
<div class="stabs"><button id="tabSess" class="stab on" onclick="statsTab('session')">SESSION</button><button id="tabAll" class="stab" onclick="statsTab('all')">ALL TIME</button></div>
<div class="skeys" id="statsKeys"></div>
<div class="schart" id="schartW"><div id="statsChart"></div><div class="stip" id="statsTip"></div></div>
<p class="dim" id="statsNote"></p>
</div>
<div id="pickBox"></div><div id="pickLbl"></div>
<div id="pickBar" class="pickui" style="display:none">Click any part of the page to edit it. <button onclick="advPickStop()">CANCEL</button></div>
<nav class="socials" id="socials"></nav>
<p class="mode8" id="mode8" style="display:none">[8-key Mode Active]</p>
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
let held = [false, false, false, false, false, false, false, false];
function keyCount() { return (typeof WM !== "undefined" && WM.set && WM.set.eightKeys) ? 8 : 3; }   // 3 keys, or 8 in 8-key mode
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
  for (let k = 0; k < 8; k++) {
    const el = $("key" + k);
    if (el) el.classList.toggle("on", !!held[k]);
  }
}

function buildKeyBoxes() {
  const box = $("keys");
  box.innerHTML = "";
  box.classList.toggle("k8", keyCount() === 8);
  for (let k = 0; k < keyCount(); k++) {
    const d = document.createElement("div");
    d.className = "key"; d.id = "key" + k;
    const n = document.createElement("div"); n.className = "n"; n.textContent = String(k + 1);
    const b = document.createElement("div"); b.className = "b"; b.id = "keyb" + k;
    d.appendChild(n); d.appendChild(b);
    if (typeof WM !== "undefined" && WM.set && WM.set.perKey) {   // SETTINGS > per-key totals: a counter under each key
      const col = document.createElement("div"); col.className = "kcol"; col.appendChild(d);
      const t = document.createElement("div"); t.className = "kt"; t.id = "kt" + k; col.appendChild(t);
      box.appendChild(col);
    } else box.appendChild(d);
  }
}

function applyKeyCount() {   // 3 <-> 8 keys: rebuild the key boxes and the binding rows
  buildKeyBoxes();
  if (S && S.profiles && S.profiles.length) renderProfiles();
  paintKeys();
}

function renderProfiles() {
  const sel = $("profileSelect");
  sel.innerHTML = "";
  S.profiles.forEach((p, i) => {
    const o = document.createElement("option");
    o.value = String(i); o.textContent = p.test ? "test" : p.name;
    sel.appendChild(o);
  });
  sel.value = String(S.activeIndex);
  const real = S.profiles.filter(p => !p.test).length;
  $("profileCount").textContent = real + " / " + S.maxProfiles + " profiles (+ test)";

  const active = S.profiles[S.activeIndex], isTest = !!active.test;
  $("btnRen").disabled = isTest; $("btnDel").disabled = isTest;
  const rows = $("bindRows");
  rows.innerHTML = "";
  for (let k = 0; k < keyCount(); k++) {
    const tr = document.createElement("tr");
    const td0 = document.createElement("td"); td0.textContent = "KEY " + (k + 1);
    const td1 = document.createElement("td");
    const inp = document.createElement("input"); inp.type = "text";
    inp.id = "in" + k; inp.value = isTest ? "" : active.bindings[k]; inp.setAttribute("list", "keynames");
    if (isTest) { inp.disabled = true; inp.placeholder = "nothing (test profile)"; }
    inp.setAttribute("spellcheck", "false"); inp.setAttribute("autocomplete", "off");
    inp.addEventListener("keydown", e => { if (e.key === "Enter") saveKey(k); });
    td1.appendChild(inp);
    const td2 = document.createElement("td");
    const cap = document.createElement("button"); cap.className = "cap"; cap.id = "cap" + k; cap.textContent = "CAPTURE";
    cap.onclick = () => startCapture(k); cap.disabled = isTest;
    td2.appendChild(cap);
    tr.appendChild(td0); tr.appendChild(td1); tr.appendChild(td2);
    rows.appendChild(tr);
    $("keyb" + k).textContent = isTest ? "-" : active.bindings[k];
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
  if (Array.isArray(st.keyClicks)) syncKeys(st.keyClicks);
  if (typeof wmFitFresh === "function") wmFitFresh();
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
function applyTheme(name) { applyColors(themeColors(name)); if (typeof advApply === "function") advApply(true); }

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
  if (typeof advRender === "function") advRender();   // the advanced section follows the chosen theme
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
  advCopy(THEME.active, name);
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
  const oldName = t.name;
  t.name = uniqueThemeName(n, t);
  advRename(oldName, t.name);
  THEME.active = t.name;
  renderThemeSelect(); renderThemeFields(); saveTheme(true);
}
function deleteTheme() {
  const t = customTheme(THEME.active);
  if (!t) { alert("Built-in themes can't be deleted."); return; }
  if (!confirm("Delete theme '" + t.name + "'?")) return;
  THEME.custom = THEME.custom.filter(x => x !== t);
  advDelete(t.name);
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
const WIN_IDS = ["profile", "visualizer", "keybinds"];   // SERVICE moved into SETTINGS, DEBUGGER is a pull-down now
// Built-in 16x16 one-bit icons (centred, 1px margin); drawn through a mask so they take the theme's text colour.
const WIN_PIX = {
  profile: "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABAAAAAQCAYAAAAf8/9hAAAARElEQVR42mNgoBAw4pH7T4JaDI3/SRAnaDNeOSZKw2DwGvCf2LChSTRSJSH9J1Y9Ix6NpMgRTEAE1f4nw/v/qZIOKAYAvnwRA0TxpasAAAAASUVORK5CYII=",
  visualizer: "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABAAAAAQCAYAAAAf8/9hAAAATklEQVR42qWRsRIAIAhCyev/f5kWl7zqElyceAIC5ozcNPUSgAAQboRQrqoAZmY6DuwIW/MdAKuwA7iKXwA6HRzb/v0vG04oWa6aaUL8WXJxEgwRkwinAAAAAElFTkSuQmCC",
  keybinds: "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABAAAAAQCAYAAAAf8/9hAAAAOUlEQVR42mNgGGjAiEfuPwlqCRrwH5ciJkq9QLEBLCR4Y5ACRjxRRqwYzigjSmxwReN/IqNxkEUtAIktEAdBbQ6UAAAAAElFTkSuQmCC"
};
function wmFitMini(m, img) {   // whole-number scale (16 px per step) and whole-pixel centring of the icon in its icon window
  const w = m.clientWidth, h = m.clientHeight; if (!w || !h) return;
  const k = Math.max(1, Math.floor((Math.min(w, h) - 8) / 16)), z = 16 * k;
  img.style.width = z + "px"; img.style.height = z + "px";
  img.style.left = Math.floor((w - z) / 2) + "px"; img.style.top = Math.floor((h - z) / 2) + "px";
}
const WORK_BOTTOM = 64;   // the bottom strip (links, status) under the divider line; default windows stay above it
const MIN_W = 200, MIN_H = 56, MINI_MIN = 36, MINI_W = 88, MINI_H = 64;
const SOCIALS = [
  { id: "github",  label: "github",   url: "https://github.com/bagelsssss" },
  { id: "youtube", label: "youtube", url: "https://www.youtube.com/@bagelsssss" },
  { id: "tiktok",  label: "tiktok",   url: "https://www.tiktok.com/@bagel.sssss" },
  { id: "discord", label: "discord", url: "https://discord.com/users/920728577622835251" }
];
const WM = { fresh: false, fitted: false, L: null, set: null, el: {}, mini: {}, timer: 0, chain: Promise.resolve(), schain: Promise.resolve(), fly: null };

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

// the height a window needs for its content at a given width (measured off-screen)
function wmNatural(id, w) {
  const el = WM.el[id]; if (!el) return 0;
  const keep = el.style.cssText;
  el.style.cssText = "display:flex;visibility:hidden;left:0;top:0;width:" + w + "px;height:auto";
  const h = Math.ceil(el.getBoundingClientRect().height);
  el.style.cssText = keep;
  return h;
}
function wmNaturalW(id) {   // the width a window needs for its content, with nothing wrapping
  const el = WM.el[id]; if (!el) return 0;
  const keep = el.style.cssText;
  el.style.cssText = "display:flex;visibility:hidden;left:0;top:0;width:max-content;height:auto";
  const w = Math.ceil(el.getBoundingClientRect().width) + 1;
  el.style.cssText = keep;
  return w;
}
// 3 <-> 8 keys changes how tall VISUALIZER and KEYBINDS need to be: refit them and move the windows below them along
function wmRefitKeys() {
  const L = WM.L; if (!L) return;
  if (L.auto) { wmRelayoutAuto(); wmPlaceAll(); wmSave(); return; }
  const vw = wmVW(), vh = wmVH();
  ["visualizer", "keybinds"].forEach(id => {
    const W = L.win[id], r = wmResolve(W.n, MIN_W, MIN_H);
    const w = (id === "visualizer" && keyCount() === 8) ? Math.max(r.w, Math.min(wmNaturalW(id), vw - r.x - 16)) : r.w;
    const nat = wmNatural(id, w);
    const below = W.min ? [] : WIN_IDS.filter(o => {   // windows under this one (same columns): they follow
      if (o === id || L.win[o].min) return false;
      const q = wmResolve(L.win[o].n, MIN_W, MIN_H);
      return q.y >= r.y + r.h - 1 && q.x < r.x + w && q.x + q.w > r.x;
    }).map(o => L.win[o]);
    const lowest = Math.max.apply(null, [r.y + r.h].concat(below.map(V => { const q = wmResolve(V.n, MIN_W, MIN_H); return q.y + q.h; })));
    let nh = Math.max(MIN_H, nat), d = nh - r.h;
    const over = lowest + d - (vh - WORK_BOTTOM - 4);                // would the column reach below the divider line?
    if (id === "keybinds" && over > 0) { nh = Math.max(Math.min(nh, 170), nh - over); d = nh - r.h; }   // KEYBINDS gives up height first (it scrolls)
    W.n.w = w; W.n.h = nh; W.n.z = null; W.n.pw = 0; W.n.ph = 0;
    below.forEach(V => { V.n.y = Math.max(0, wmResolve(V.n, MIN_W, MIN_H).y + d); V.n.z = null; });
  });
  wmPlaceAll(); wmSave();
}
// the browser window changed size (small, square, fullscreen): an untouched layout is rebuilt for the new size,
// an arranged one keeps its saved frames and is only kept inside the window (see wmResolve)
function wmRelayoutAuto() {
  const D = wmDefaults();
  WIN_IDS.forEach(id => { const W = WM.L.win[id], d = D.win[id]; W.n = d.n; if (!W.min) { W.m.x = d.m.x; W.m.y = d.m.y; } });
}
let wmRzT = 0;
function wmOnResize() {
  if (!WM.L) return;
  wmPlaceAll();                                   // immediately: nothing leaves the window
  clearTimeout(wmRzT);
  wmRzT = setTimeout(() => { if (WM.L && WM.L.auto) { wmRelayoutAuto(); wmPlaceAll(); wmSave(); } }, 120);
}
function wmFitFresh() {   // first run (nothing saved yet): once the keybind rows exist, cut every window to its content
  if (WM.fitted || !WM.L || !WM.L.auto) return;    // an untouched layout (new, or saved as "auto") is fitted once to the real content
  WM.fitted = true; wmRelayoutAuto(); wmPlaceAll();
}

function wmDefaults() {
  const vw = wmVW(), vh = wmVH();
  const hb = document.querySelector(".hdr").getBoundingClientRect();
  // the same arrangement as 1.0: a page of at most 1200 px in the middle, two equal columns under the header
  const page = Math.min(1200, vw), colW = Math.max(MIN_W, Math.floor((page - 48) / 2)), x0 = Math.max(16, Math.floor((vw - page) / 2) + 16);
  const y0 = ((vw < 780 || vh < 760) ? Math.ceil(hb.bottom) : Math.max(Math.ceil(hb.bottom), 150)) + 8;   // small browser window: start right under the header
  const narrow = vw < 780;   // square or small browser window: everything in one column
  const colW1 = narrow ? Math.max(MIN_W, Math.min(vw - 32, 620)) : colW;
  const cols = narrow ? [{ x: Math.max(16, Math.floor((vw - colW1) / 2)), y: y0, ids: ["profile", "visualizer", "keybinds"] }] : [
    { x: x0, y: y0, ids: ["profile", "visualizer"] },
    { x: Math.max(16, Math.min(x0 + colW + 16, vw - colW - 16)), y: y0, ids: ["keybinds"] }
  ];
  // new rule: windows are cut to their content, so there is no empty space (the debugger log gets some room)
  // width = what the content needs (up to the column); KEYBINDS and DEBUGGER always get the full column
  const W = {}, H = {};
  WIN_IDS.forEach(id => {
    const nw = (id === "keybinds") ? colW1 : Math.min(colW1, Math.max(MIN_W, wmNaturalW(id)));
    W[id] = nw > 0 ? nw : colW1;
  });
  const lw = Math.max(W.profile, W.visualizer);   // the two left windows share one width
  W.profile = lw; W.visualizer = lw;
  WIN_IDS.forEach(id => { const nat = wmNatural(id, W[id]); H[id] = nat > 0 ? nat : 120; });
  const L = { v: 1, auto: true, order: WIN_IDS.slice(), win: {} };   // auto = untouched default layout: it follows the browser window size
  const mk = (x, y, w, h, open) => ({
    n: { x: x, y: y, w: w, h: h, z: null, pw: 0, ph: 0 },
    m: { x: x, y: y, w: MINI_W, h: MINI_H, z: null, pw: 0, ph: 0 },
    min: false, open: open
  });
  cols.forEach(c => {
    // a column taller than the browser window: the long windows (keybinds with its hint text, the debugger log) give up height first;
    // the others keep their content height. Whatever still does not fit scrolls inside its window.
    const gap = 12, n = c.ids.length, avail = vh - c.y - WORK_BOTTOM - 4 - gap * (n - 1);
    const hs = {}; c.ids.forEach(id => { hs[id] = H[id]; });
    let deficit = c.ids.reduce((t, id) => t + hs[id], 0) - avail;
    ["keybinds"].forEach(id => {   // only KEYBINDS gives up height (it scrolls); PROFILE and VISUALIZER keep their content height
      if (deficit <= 0 || hs[id] === undefined) return;
      const take = Math.min(Math.max(0, hs[id] - (vh < 760 ? 110 : 170)), deficit);
      hs[id] -= take; deficit -= take;
    });
    let y = c.y;
    c.ids.forEach(id => {
      const h = Math.max(MIN_H, Math.round(hs[id]));
      L.win[id] = mk(c.x, y, W[id], h, true);
      y += h + gap;
    });
  });
  return L;
}

function wmClean(raw) {
  const D = wmDefaults(), L = { v: 1, auto: !(raw && typeof raw === "object" && raw.win) || raw.auto === true, order: [], win: {} };   // a saved layout stays "auto" only if it was saved that way
  const rw = (raw && typeof raw === "object" && raw.win && typeof raw.win === "object") ? raw.win : {};
  // entries this version does not use (SERVICE / DEBUGGER windows of 1.1) are carried along untouched, so going back to an older build keeps them
  if (raw && typeof raw === "object" && raw.win) {
    const keep = (raw.keep && typeof raw.keep === "object") ? raw.keep : {};
    Object.keys(rw).forEach(k => { if (WIN_IDS.indexOf(k) < 0 && rw[k] && typeof rw[k] === "object") keep[k] = rw[k]; });
    if (Object.keys(keep).length) L.keep = keep;
  }
  WIN_IDS.forEach(id => {
    const r = has(rw, id) && rw[id] && typeof rw[id] === "object" ? rw[id] : null;
    const d = D.win[id];
    if (!r) { L.win[id] = d; return; }
    L.win[id] = {
      n: wmCleanFrame(r.n, d.n, MIN_W, MIN_H), m: wmCleanFrame(r.m, d.m, MINI_MIN, MINI_MIN),
      min: !!r.min, open: true
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
  const vw = wmVW(), vh = wmVH();
  const w = Math.max(minW, Math.min(f.w, vw - 8)), h = Math.max(minH, Math.min(f.h, vh - 8));
  return {
    x: Math.min(Math.max(f.x, 40 - w), Math.max(0, vw - w)),   // never past the right edge (the left edge may be dragged partly out)
    y: Math.min(Math.max(f.y, 0), Math.max(0, vh - h)),         // and never below the bottom of the browser window
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
  el.style.display = (W.open && !W.min) ? "flex" : "none";
  mi.style.display = (W.open && W.min) ? "block" : "none";
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
  WM.fresh = false;
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
    F.x = Math.min(Math.max(base.x + dx, 40 - base.w), Math.max(0, wmVW() - base.w));
    F.y = Math.min(Math.max(base.y + dy, 0), Math.max(0, wmVH() - base.h));
    F.w = base.w; F.h = base.h;
    wmPlace(id);
  };
  const end = () => {
    grab.removeEventListener("pointermove", move); grab.removeEventListener("pointerup", end); grab.removeEventListener("pointercancel", end);
    if (moved) { WM.L.auto = false; wmSave(); }
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
    if (moved) { WM.L.auto = false; wmSave(); }
  };
  grab.addEventListener("pointermove", move); grab.addEventListener("pointerup", end); grab.addEventListener("pointercancel", end);
}

// ---- Windows 11 style zoom + fade between a window and its icon window, at 30 fps ----
const ANIM_MS = 400, ANIM_EASE = "steps(12, end)";   // 30 fps
function wmAnimOK() { return !(WM.set && WM.set.animOff) && !(window.matchMedia && matchMedia("(prefers-reduced-motion: reduce)").matches) && typeof Element.prototype.animate === "function"; }
function wmGhost(id) {   // a picture of the window that does the flying
  const g = WM.el[id].cloneNode(true);
  g.removeAttribute("id"); g.querySelectorAll("[id]").forEach(n => n.removeAttribute("id"));
  g.querySelectorAll("input, select, textarea, button").forEach(n => { n.tabIndex = -1; });
  g.setAttribute("aria-hidden", "true"); g.classList.add("ghost");
  g.style.pointerEvents = "none"; g.style.display = "flex"; g.style.zIndex = "5000";
  $("desk").appendChild(g);
  return g;
}
function wmFly(g, big, small, shrink, done) {
  const t = "translate(" + Math.round(small.x + small.w / 2 - big.x - big.w / 2) + "px," + Math.round(small.y + small.h / 2 - big.y - big.h / 2) + "px) scale(" + (small.w / big.w) + "," + (small.h / big.h) + ")";
  const k = shrink ? [{ transform: "none", opacity: 1 }, { transform: t, opacity: 0 }] : [{ transform: t, opacity: 0 }, { transform: "none", opacity: 1 }];
  g.style.transformOrigin = "50% 50%";
  const fin = () => { if (g.parentNode) g.remove(); if (done) done(); };
  try { g.animate(k, { duration: ANIM_MS, easing: ANIM_EASE, fill: "both" }).onfinish = fin; } catch (e) { fin(); return; }
  setTimeout(fin, ANIM_MS + 150);
}

// "expand then roll out" (the default): a thin bar spreads out along x, then the window grows downward. 30 fps = 12 steps in 0.4 s.
// Minimizing is the same thing backwards: it rolls up, then narrows away. SETTINGS > old animations brings back the zoom and fade.
function wmRoll(el, closing, done, fill) {
  const bar = "inset(0 50% calc(100% - 6px) 50%)", wide = "inset(0 0 calc(100% - 6px) 0)", full = "inset(0 0 0 0)";
  const k = closing ? [{ clipPath: full, easing: "steps(7, end)" }, { clipPath: wide, offset: 0.6, easing: "steps(5, end)" }, { clipPath: bar }]
                    : [{ clipPath: bar, easing: "steps(5, end)" }, { clipPath: wide, offset: 0.4, easing: "steps(7, end)" }, { clipPath: full }];
  const fin = () => { if (done) { const d = done; done = null; d(); } };
  try { el.animate(k, { duration: ANIM_MS, fill: fill }).onfinish = fin; } catch (e) { fin(); return; }
  setTimeout(fin, ANIM_MS + 150);
}
function wmOld() { return !!(WM.set && WM.set.oldAnim); }
function wmMinimize(id) {
  const W = WM.L.win[id], r = wmResolve(W.n, MIN_W, MIN_H);
  const ghost = wmAnimOK() ? wmGhost(id) : null;
  W.m.x = r.x; W.m.y = r.y;                       // the icon window appears where the window is
  W.min = true;
  wmPlaceAll(); wmSave();
  if (!ghost) return;
  if (wmOld()) { wmFly(ghost, r, wmResolve(W.m, MINI_MIN, MINI_MIN), true); return; }   // old: zooms and fades into its icon
  const mi = WM.mini[id]; mi.style.visibility = "hidden";                                 // new: rolls up, narrows, then the icon appears
  wmRoll(ghost, true, () => { ghost.remove(); mi.style.visibility = ""; }, "both");
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
  if (wmAnimOK() && !wmOld()) { wmRoll(WM.el[id], false, null, "backwards"); return; }   // expands, then rolls out
  if (wmAnimOK()) {                               // old: the window zooms and fades out of its icon
    const el = WM.el[id], ghost = wmGhost(id);
    el.style.visibility = "hidden";
    wmFly(ghost, wmResolve(W.n, MIN_W, MIN_H), mr, false, () => { el.style.visibility = ""; });
  }
}

)HTMLX";

static const char* HTML_JS_WM3 = R"HTMLX(
// ---- pull-down menus (THEMES, SETTINGS) ----
const MENU_BTN = { theme: "themeBtn", settings: "gearBtn", debugger: "dbgBtn" };
let menuCur = null;
function menuSyncZ() { $("tbtns").classList.toggle("open", !!menuCur || Object.keys(MENU_BTN).some(i => $("menu-" + i).style.display === "block")); }
function menuShow(id) {
  const m = $("menu-" + id); clearTimeout(m._t);
  m.parentNode.classList.add("mo");
  m.classList.remove("closing"); m.style.display = "block"; m.classList.toggle("oldanim", wmOld()); void m.offsetWidth; m.classList.add("opening");
  $(MENU_BTN[id]).classList.add("on");
  if (id === "debugger") { const lg = $("log"); lg.scrollTop = lg.scrollHeight; }   // a hidden log forgets its scroll position
}
function menuHide(id) {
  const m = $("menu-" + id); if (m.style.display !== "block") return;
  $(MENU_BTN[id]).classList.remove("on");
  m.classList.remove("opening"); m.classList.add("closing"); clearTimeout(m._t);
  m._t = setTimeout(() => { m.style.display = "none"; m.classList.remove("closing"); m.parentNode.classList.remove("mo"); menuSyncZ(); }, wmAnimOK() ? ANIM_MS : 0);
}
function menuToggle(id) {   // only one is open at a time
  const was = menuCur;
  if (was) menuHide(was);
  menuCur = (was === id) ? null : id;
  if (menuCur) menuShow(id);
  menuSyncZ();
}
function menuClose() { if (menuCur) { menuHide(menuCur); menuCur = null; menuSyncZ(); } }
document.addEventListener("pointerdown", e => { if (menuCur && !e.target.closest(".tb")) menuClose(); });
document.addEventListener("keydown", e => { if (e.key === "Escape") menuClose(); });

function resetLayout() {
  if (!confirm("Put every window back in its default place and size?")) return;
  WM.L = wmClean(null);
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
    if (id === "visualizer") {   // the stats (bar chart) button, top right of the window
      const sb = document.createElement("button"); sb.className = "wbtn statbtn"; sb.title = "Click stats"; sb.setAttribute("aria-label", "Click stats");
      sb.innerHTML = '<svg viewBox="0 0 16 16" aria-hidden="true"><rect x="2" y="9" width="3" height="5"/><rect x="6.5" y="5" width="3" height="9"/><rect x="11" y="2" width="3" height="12"/></svg>';
      sb.onclick = e => { e.stopPropagation(); statsToggle(); };
      bar.appendChild(sb);
    }
    const wi = document.createElement("span"); wi.className = "wicon pix"; wi.style.setProperty("--pix", "url(" + WIN_PIX[id] + ")");
    bar.appendChild(wi);
    el.insertBefore(bar, el.firstChild);
    const rz = document.createElement("div"); rz.className = "rsz"; rz.title = "Drag to resize";
    const rzr = document.createElement("div"); rzr.className = "rsz r"; rzr.title = "Drag to resize";
    el.appendChild(rz); el.appendChild(rzr);
    bar.addEventListener("pointerdown", e => wmBeginMove(e, id, false));
    rz.addEventListener("pointerdown", e => wmBeginResize(e, id, false, "l"));
    rzr.addEventListener("pointerdown", e => wmBeginResize(e, id, false, "r"));
    el.addEventListener("pointerdown", () => wmFront(id));

    const m = document.createElement("div"); m.className = "mini"; m.id = "m-" + id; m.title = title + " - click ^ or double-click to open";
    const img = document.createElement("span"); img.className = "mimg pix"; img.style.setProperty("--pix", "url(" + WIN_PIX[id] + ")");
    if (window.ResizeObserver) new ResizeObserver(() => wmFitMini(m, img)).observe(m);
    const car = document.createElement("button"); car.className = "mcar"; car.textContent = "^"; car.title = "Open"; car.setAttribute("aria-label", "Open " + title);
    car.onclick = () => wmRestore(id);
    const rz2 = document.createElement("div"); rz2.className = "rsz"; rz2.title = "Drag to resize";
    const rz2r = document.createElement("div"); rz2r.className = "rsz r"; rz2r.title = "Drag to resize";
    m.appendChild(img); m.appendChild(car); m.appendChild(rz2); m.appendChild(rz2r);
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
  const s = { gearTop: true, cpsOff: false, cpsColor: true, animOff: false, oldAnim: false, eightKeys: false, perKey: true, logStats: false, advTheme: false, social: {} };
  if (raw && typeof raw === "object") {
    Object.keys(raw).forEach(k => { if (!has(s, k)) s[k] = raw[k]; });   // settings from other versions are kept, never dropped
    if (typeof raw.oldAnim === "boolean") s.oldAnim = raw.oldAnim;
    if (typeof raw.perKey === "boolean") s.perKey = raw.perKey;
    if (typeof raw.logStats === "boolean") s.logStats = raw.logStats;
    if (typeof raw.advTheme === "boolean") s.advTheme = raw.advTheme;
    if (typeof raw.gearTop === "boolean") s.gearTop = raw.gearTop;
    if (typeof raw.cpsOff === "boolean") s.cpsOff = raw.cpsOff;
    if (typeof raw.cpsColor === "boolean") s.cpsColor = raw.cpsColor;
    if (typeof raw.animOff === "boolean") s.animOff = raw.animOff;
    if (typeof raw.eightKeys === "boolean") s.eightKeys = raw.eightKeys;
  }
  SOCIALS.forEach(x => { s.social[x.id] = !(raw && raw.social && typeof raw.social === "object" && raw.social[x.id] === false); });
  return s;
}
function wmApplySettings() {
  const s = WM.set;
  $("optGearTop").checked = s.gearTop; $("optCpsOff").checked = s.cpsOff; $("optCpsColor").checked = s.cpsColor; $("optAnimOff").checked = s.animOff; $("optOldAnim").checked = s.oldAnim; $("optPerKey").checked = s.perKey; $("optLogStats").checked = s.logStats; $("optAdvTheme").checked = s.advTheme;
  if (typeof advApply === "function") { advApply(true); advRender(); }
  document.documentElement.classList.toggle("noanim", s.animOff);
  $("optEight").checked = s.eightKeys; $("mode8").style.display = s.eightKeys ? "" : "none";
  $("divider").style.bottom = WORK_BOTTOM + "px";
  if (WM.keysShown !== keyCount() || WM.perShown !== !!s.perKey) { WM.keysShown = keyCount(); WM.perShown = !!s.perKey; applyKeyCount(); }
  $("cCpsRow").style.display = s.cpsOff ? "none" : "";
  if (typeof paintCounters === "function") paintCounters();
  $("tbtns").style.zIndex = s.gearTop ? "3" : "1";   // 3 = above the windows, 1 = windows can cover it
  SOCIALS.forEach(x => { $("optSocial_" + x.id).checked = s.social[x.id]; });
  const nav = $("socials"); nav.textContent = "";
  SOCIALS.forEach(x => {
    if (!s.social[x.id] || !x.url) return;
    const a = document.createElement("a"); a.href = x.url; a.target = "_blank"; a.rel = "noopener noreferrer"; a.title = x.label;
    const sp = document.createElement("span"); sp.textContent = "[" + x.label + "]";
    a.appendChild(sp);
    nav.appendChild(a);
  });
}
function wmSettingChanged() {
  const s = WM.set;
  s.gearTop = $("optGearTop").checked; s.cpsOff = $("optCpsOff").checked; s.cpsColor = $("optCpsColor").checked; s.animOff = $("optAnimOff").checked; s.oldAnim = $("optOldAnim").checked; s.perKey = $("optPerKey").checked; s.logStats = $("optLogStats").checked; s.advTheme = $("optAdvTheme").checked; s.eightKeys = $("optEight").checked;
  SOCIALS.forEach(x => { s.social[x.id] = $("optSocial_" + x.id).checked; });
  const keysBefore = WM.keysShown, perBefore = WM.perShown;
  wmApplySettings();
  if (keysBefore !== undefined && (keysBefore !== WM.keysShown || perBefore !== WM.perShown)) wmRefitKeys();   // 3 <-> 8 keys or the per-key counters change the height
  if (typeof statsSettingsChanged === "function") statsSettingsChanged();
  const body = JSON.stringify(s), modeChanged = keysBefore !== undefined && keysBefore !== WM.keysShown;
  WM.schain = WM.schain.then(() => fetch("/api/settings", { method: "POST", headers: { "X-ESPdevice": "1", "Content-Type": "application/json" }, body: body }).catch(() => {}))
    .then(() => { if (modeChanged) return poll(); });   // 3-key and 8-key have separate profile lists: load the other one now
}

async function wmInit() {
  wmBuild();
  const get = async u => { try { const r = await fetch(u); if (!r.ok) return null; return JSON.parse(await r.text()); } catch (e) { return null; } };
  const res = await Promise.all([get("/api/layout"), get("/api/settings")]);
  WM.set = wmCleanSettings(res[1]);
  WM.fresh = !(res[0] && typeof res[0] === "object" && res[0].win);
  WM.L = wmClean(res[0]);
  wmApplySettings();
  wmPlaceAll();
  $("desk").classList.add("ready");
  window.addEventListener("resize", wmOnResize);
  window.addEventListener("pagehide", wmFlush);
  document.addEventListener("visibilitychange", () => { if (document.visibilityState === "hidden") wmFlush(); });
}
)HTMLX";

static const char* HTML_JS_STATS = R"HTMLX(
// ================= CLICK STATS (v1.2) =================
// Per-key totals under the keys (SETTINGS > per-key totals) and the stats panel (bar-chart button in the VISUALIZER title bar):
// SESSION = clicks per minute since the app started (kept in memory by the app), ALL TIME = clicks per day (needs SETTINGS > log click stats).
let kc = [0, 0, 0, 0, 0, 0, 0, 0], kcSrv = "";
function syncKeys(a) { const sig = a.join(","); if (sig !== kcSrv) { kcSrv = sig; kc = a.slice(); paintKeyTotals(); } }
function paintKeyTotals() {
  for (let k = 0; k < keyCount(); k++) {
    const el = $("kt" + k); if (!el) continue;
    const f = fmtBig(kc[k]), tt = "Key " + (k + 1) + ": " + kc[k].toLocaleString("en-US") + " clicks since the app started";
    if (el.textContent !== f.t) el.textContent = f.t;
    if (el.title !== tt) el.title = tt;
  }
}
const ST = { open: false, mode: "session", key: "all", data: null, timer: 0 };
const two = n => (n < 10 ? "0" : "") + n;
function fmtMin(m) { const d = new Date(m * 60000), t = two(d.getHours()) + ":" + two(d.getMinutes()), n = new Date(); return d.toDateString() === n.toDateString() ? t : two(d.getMonth() + 1) + "-" + two(d.getDate()) + " " + t; }
function ymd(d) { return d.getFullYear() + "-" + two(d.getMonth() + 1) + "-" + two(d.getDate()); }
function statsOn() { return !!(WM.set && WM.set.logStats); }
function statsToggle() { if (ST.open) statsClose(); else statsOpen(); }
function statsOpen() {
  if (ST.open) return; ST.open = true;
  const p = $("statsPanel"); statsKeysBuild(); statsTabs(); p.style.display = "block";
  if (wmAnimOK()) { if (wmOld()) p.animate([{ opacity: 0, transform: "translateX(-50%) scale(0.82)" }, { opacity: 1, transform: "translateX(-50%)" }], { duration: ANIM_MS, easing: ANIM_EASE }); else wmRoll(p, false, null, "backwards"); }
  statsLoad(); ST.timer = setInterval(statsLoad, 3000);
}
function statsClose() {
  if (!ST.open) return; ST.open = false; clearInterval(ST.timer);
  const p = $("statsPanel"), hide = () => { if (!ST.open) p.style.display = "none"; };
  if (wmAnimOK()) { try { p.animate([{ opacity: 1, transform: "translateX(-50%)" }, { opacity: 0, transform: "translateX(-50%) scale(0.82)" }], { duration: ANIM_MS, easing: ANIM_EASE }).onfinish = hide; } catch (e) { hide(); } setTimeout(hide, ANIM_MS + 150); } else hide();
}
document.addEventListener("keydown", e => { if (e.key === "Escape" && ST.open) statsClose(); });
function statsTab(m) { if (m === "all" && !statsOn()) return; ST.mode = m; ST.data = null; statsTabs(); statsDraw(); statsLoad(); }
function statsTabs() {
  const on = statsOn(); if (!on && ST.mode === "all") ST.mode = "session";
  $("tabSess").classList.toggle("on", ST.mode === "session"); $("tabAll").classList.toggle("on", ST.mode === "all");
  $("tabAll").disabled = !on; $("tabAll").title = on ? "" : "turn on 'log click stats' in SETTINGS first";
}
function statsKeysBuild() {
  const box = $("statsKeys"), n = keyCount(); box.textContent = "";
  if (ST.key !== "all" && ST.key >= n) ST.key = "all";
  const mk = (label, val) => { const b = document.createElement("button"); b.textContent = label; b.className = ST.key === val ? "on" : ""; b.onclick = () => { ST.key = val; statsKeysBuild(); statsDraw(); }; box.appendChild(b); };
  mk("ALL", "all"); for (let k = 0; k < n; k++) mk(String(k + 1), k);
}
function statsSettingsChanged() { if (!ST.open) return; statsTabs(); statsKeysBuild(); statsLoad(); }
async function statsLoad() {
  const mode = ST.mode;
  try { const r = await fetch("/api/stats?mode=" + mode + "&_=" + Date.now()); const d = await r.json(); if (d.mode === ST.mode) { ST.data = d; statsDraw(); } } catch (e) {}
}
function statsSeries() {
  const d = ST.data; if (!d) return null;
  const n = Math.min(d.keys || 3, 8);
  const pick = a => { if (ST.key === "all") { let t = 0; for (let k = 0; k < n; k++) t += a[k] || 0; return t; } return a[ST.key] || 0; };
  const bins = [];
  if (d.mode === "session") {
    const raw = d.data.map(pick), g = Math.max(1, Math.ceil(raw.length / 240));
    for (let i = 0; i < raw.length; i += g) {
      let t = 0; for (let j = i; j < Math.min(raw.length, i + g); j++) t += raw[j];
      bins.push({ v: t, label: fmtMin(d.startMin + i) + (g > 1 ? " - " + fmtMin(d.startMin + Math.min(raw.length, i + g) - 1) : ""), first: fmtMin(d.startMin + i) });
    }
    while (bins.length < 15) { const t0 = d.startMin + bins.length * g; bins.push({ v: 0, label: fmtMin(t0), first: fmtMin(t0) }); }
    return { bins: bins, unit: g === 1 ? "clicks per minute" : "clicks per " + g + " minutes", real: raw.length };
  }
  const keys = Object.keys(d.days).sort(); if (!keys.length) return { bins: [], unit: "clicks per day", real: 0 };
  const p = s => { const a = s.split("-"); return new Date(+a[0], +a[1] - 1, +a[2]); };
  let cur = p(keys[0]); const end = p(keys[keys.length - 1]);
  while (cur <= end) { const k = ymd(cur); bins.push({ v: d.days[k] ? pick(d.days[k]) : 0, label: k, first: k }); cur = new Date(cur.getFullYear(), cur.getMonth(), cur.getDate() + 1); }
  const real = bins.length; while (bins.length > 365) bins.shift();
  while (bins.length < 14) { const k = ymd(cur); bins.push({ v: 0, label: k, first: k }); cur = new Date(cur.getFullYear(), cur.getMonth(), cur.getDate() + 1); }
  return { bins: bins, unit: "clicks per day", real: real };
}
function niceMax(m) { if (m <= 0) return 4; const p = Math.pow(10, Math.floor(Math.log10(m))), f = m / p; return (f <= 1 ? 1 : f <= 2 ? 2 : f <= 4 ? 4 : f <= 6 ? 6 : f <= 8 ? 8 : 10) * p; }
function statsDraw() {
  if (!ST.open) return;
  const box = $("statsChart"), note = $("statsNote"), s = statsSeries();
  if (!s || !s.bins.length) { box.innerHTML = '<div class="sempty">' + (ST.mode === "all" && ST.data ? "No data yet. Click stats are saved from the moment 'log click stats' is turned on." : "Loading...") + "</div>"; note.textContent = ""; ST.bins = null; return; }
  const W = Math.max(200, box.clientWidth), H = Math.max(120, box.clientHeight), L = 48, R = 10, T = 12, B = 24, pw = W - L - R, ph = H - T - B;
  let max = 0, sum = 0; s.bins.forEach(b => { if (b.v > max) max = b.v; sum += b.v; });
  const top = niceMax(max), bw = pw / s.bins.length, ticks = top >= 2 ? [0, top / 2, top] : [0, top];
  let g = "";
  ticks.forEach(v => { const y = T + ph - ph * v / top; g += '<line class="sgrid" x1="' + L + '" x2="' + (W - R) + '" y1="' + y + '" y2="' + y + '"/><text class="stxt" x="' + (L - 6) + '" y="' + (y + 4) + '" text-anchor="end">' + fmtBig(v).t + "</text>"; });
  s.bins.forEach((b, i) => { if (b.v <= 0) return; const h = Math.max(1, ph * b.v / top); g += '<rect class="sbarr" x="' + (L + i * bw + bw * 0.12).toFixed(1) + '" y="' + (T + ph - h).toFixed(1) + '" width="' + Math.max(1, bw * 0.76).toFixed(1) + '" height="' + h.toFixed(1) + '"/>'; });
  g += '<text class="stxt" x="' + L + '" y="' + (H - 6) + '">' + s.bins[0].first + '</text><text class="stxt" x="' + (W - R) + '" y="' + (H - 6) + '" text-anchor="end">' + s.bins[s.bins.length - 1].first + "</text>";
  box.innerHTML = '<svg viewBox="0 0 ' + W + " " + H + '" width="' + W + '" height="' + H + '">' + g + "</svg>";
  ST.bins = { list: s.bins, L: L, bw: bw, W: W };
  note.textContent = (ST.key === "all" ? "all keys" : "key " + (ST.key + 1)) + "  |  " + s.unit + "  |  in view: " + sum.toLocaleString("en-US") + "  |  busiest: " + max.toLocaleString("en-US") +
    (ST.mode === "session" ? "  |  since the app started" : "");
}
(function () {
  const w = () => $("schartW");
  document.addEventListener("mousemove", e => {
    if (!ST.open || !ST.bins) return;
    const r = w().getBoundingClientRect(), x = e.clientX - r.left, tip = $("statsTip");
    const i = Math.floor((x - ST.bins.L) / ST.bins.bw);
    if (e.clientY < r.top || e.clientY > r.bottom || i < 0 || i >= ST.bins.list.length) { tip.style.display = "none"; return; }
    const b = ST.bins.list[i];
    tip.textContent = b.label + ": " + b.v.toLocaleString("en-US") + " clicks"; tip.style.display = "block";
    tip.style.left = Math.min(Math.max(0, x + 12), r.width - tip.offsetWidth - 4) + "px"; tip.style.top = Math.max(0, e.clientY - r.top - 30) + "px";
  });
})();
async function clearStats() {
  if (!confirm("Clear all click stats (the per-key counts, the SESSION graph and the saved ALL TIME history)?")) return;
  applyState(await api("/api/stats/clear", {}));
  if (ST.open) statsLoad();
}
)HTMLX";

static const char* HTML_JS_ADV = R"HTMLX(
// ================= ADVANCED THEMING (v1.2) =================
// SETTINGS > advanced theming (off by default): every element of the page can be restyled one by one (colour, border colour and
// thickness, text size, ...). The edits of a custom theme are turned into a small override stylesheet, so the normal theme stays
// untouched. They are saved in config\theme_advanced.json (a separate file) as { "v":1, "themes": { "<theme name>": { "<element>.<property>": value } } }.
const ADV_SRC = `#PAGE
page|Page background and text|html, body|cb
dim|Secondary text (hints, notes)|.dim, .logohint, .logohint a|cs
err|Error text|.err|cs
divider|Divider line|.divider|b
#HEADER
title|Title text|.brand|cs
cursor|Title cursor|.brand .cur|c
art|Title art|.art|c
status|Status line|.status|cs
credit|Corner credit|.credit|cs
social|Social links|.socials a|cs
mode8|8-key mode text|.mode8|cs
#TOP-RIGHT BUTTONS
topbtn|THEMES / SETTINGS / DEBUGGER buttons|.gear|cbdws
topbtnh|Top buttons: hover|.gear:hover|cbd
topbtno|Top buttons: open|.gear.on|cbd
#WINDOWS
win|Window frame|.win|bdw
wtitle|Window title|.wbar h2|cs
wtitlef|Window title (front window)|.win.front .wbar h2|c
grip|Window grip mark|.grip|cdw
wbtn|Window buttons (minimize, stats)|.wbtn|cbdw
rsz|Resize corner marks|.rsz::after|d
mini|Icon window (minimized)|.mini|bdw
mph|Icons (title bar and icon window)|.wicon, .mimg|c
mcar|Icon window open button|.mcar|cbd
#MENUS
menu|Pull-down panels|.menu|bdw
setgrp|Menu headings|.setgrp, .tgroup|cs
opt|Checkbox labels|.opt|cs
#BUTTONS AND INPUTS
btn|All buttons|button|cbdws
btnh|Buttons: hover|button:hover|cbd
btnon|Active buttons (capture, tabs, chosen key)|button.cap.on, .stab.on, .skeys button.on|cb
btnoff|Buttons: disabled|button:disabled|cbd
inp|Inputs and dropdowns|select, input[type=text]|cbdws
#PROFILE
pcount|Profile counter text|#profileCount|cs
#VISUALIZER
keyoff|Keys (not pressed)|.key|cbdw
keyon|Keys (pressed)|.key.on|cbd
keyn|Key number|.key .n|cs
keyb|Key binding text|.key .b|cs
kt|Per-key totals|.kt|cs
cps|CPS label|#cCpsRow|cs
cpsn|CPS number|#cCps|cs
total|Total label|#cTotalRow|cs
totaln|Total number|#cTotal|cs
#KEYBINDS
th|Table header|th|cbs
td|Table cells|td|cs
tline|Table lines|td, th|dw
#DEBUGGER
stlab|Stat labels|.stats|cs
stval|Stat values|.stats div span|c
log|Log box|#log|cbdws
ld|Log: key down lines|.l-P|c
lu|Log: key up lines|.l-R|c
lx|Log: timeout lines|.l-X|c
lc|Log: connect lines|.l-C, .l-D|c
li|Log: info lines|.l-I|c
#CLICK STATS
sp|Stats panel|.statspanel|bdw
spt|Stats title|.sbar h2|cs
sch|Graph box|.schart|bdw
sbar|Graph bars|.sbarr|f
sgrid|Graph grid lines|.sgrid|k
sx|Graph labels|.stxt|fs
stip|Graph tooltip|.stip|cbd`;
const ADV_P = { c: ["color", "text", "c"], b: ["background-color", "background", "c"], d: ["border-color", "border", "c"], w: ["border-width", "thickness", "n", 0, 8], s: ["font-size", "size", "n", 8, 48], f: ["fill", "fill", "c"], k: ["stroke", "line", "c"] };
const ADV_FONTS = { "Consolas": 'Consolas, "Lucida Console", monospace', "Courier New": '"Courier New", monospace', "Lucida Console": '"Lucida Console", Consolas, monospace', "Arial": "Arial, Helvetica, sans-serif", "Verdana": "Verdana, sans-serif", "Georgia": "Georgia, serif", "Times New Roman": '"Times New Roman", serif', "Trebuchet MS": '"Trebuchet MS", sans-serif', "Segoe UI": '"Segoe UI", sans-serif', "Tahoma": "Tahoma, sans-serif" };
const ADV_GROUPS = [], ADV_ALL = [];
ADV_SRC.split("\n").forEach(l => {
  if (l[0] === "#") { ADV_GROUPS.push({ name: l.slice(1), els: [] }); return; }
  const a = l.split("|"), el = { id: a[0], label: a[1], sel: a[2], props: a[3].split(""), group: ADV_GROUPS.length - 1 };
  ADV_GROUPS[ADV_GROUPS.length - 1].els.push(el); ADV_ALL.push(el);
});
let ADVD = {}, ADVX = {}, ADV_READY = false, advSaveT = 0, advRT = 0;
const advOf = create => { if (isBuiltin(THEME.active)) return null; if (create && !ADVD[THEME.active]) ADVD[THEME.active] = {}; return ADVD[THEME.active] || null; };
const advOn = () => !!(WM.set && WM.set.advTheme);
function advCss() {
  const o = advOf(false); if (!advOn() || !o) return "";
  let css = "", root = "";
  if (ADV_FONTS[o["g.font"]]) root += "--font:" + ADV_FONTS[o["g.font"]] + ";";
  const gs = parseInt(o["g.size"], 10); if (gs >= 10 && gs <= 28) root += "--size:" + gs + "px;";
  if (root) css += "html:root{" + root + "}\n";
  ADV_ALL.forEach(el => {
    let d = "";
    el.props.forEach(p => {
      const v = o[el.id + "." + p], P = ADV_P[p]; if (v === undefined) return;
      if (P[2] === "c") { if (HEX.test(v)) d += P[0] + ":" + v + " !important;"; }
      else { const n = parseInt(v, 10); if (n >= P[3] && n <= P[4]) { d += P[0] + ":" + n + "px !important;"; if (p === "w" && n > 0) d += "border-style:solid !important;"; } }
    });
    if (d) css += el.sel + "{" + d + "}\n";
  });
  return css;
}
function advApply(refit) {
  if (!ADV_READY || !WM.set) return;
  const css = advCss();
  let st = $("advStyle"); if (!st) { st = document.createElement("style"); st.id = "advStyle"; document.head.appendChild(st); }
  if (st.textContent !== css) st.textContent = css;
  try { localStorage.setItem("espdevice-advcss", css); } catch (e) {}
  if (refit && WM.L && WM.L.auto) { clearTimeout(advRT); advRT = setTimeout(() => { wmRelayoutAuto(); wmPlaceAll(); wmSave(); }, 150); }   // bigger or smaller text: an untouched layout re-fits
}
function advSave() { clearTimeout(advSaveT); advSaveT = setTimeout(() => { api("/api/themeadv", Object.assign({}, ADVX, { v: 1, themes: ADVD })).catch(() => {}); }, 400); }
function advSet(key, val) {
  const o = advOf(true); if (!o) return;
  if (val === "" || val == null) delete o[key]; else o[key] = val;
  advApply(true); advSave();
}
async function advLoad() {
  try {
    const j = JSON.parse((await (await fetch("/api/themeadv")).text()) || "{}");
    ADVX = {}; Object.keys(j).forEach(k => { if (k !== "themes") ADVX[k] = j[k]; });   // fields from other versions are kept
    ADVD = {};
    if (j.themes && typeof j.themes === "object") Object.keys(j.themes).forEach(n => {
      const t = j.themes[n]; if (!t || typeof t !== "object" || n.length > 24) return;
      const o = {}; Object.keys(t).forEach(k => { if (/^[a-z0-9.\-]{1,24}$/.test(k) && typeof t[k] === "string" && /^[#\w .,-]{0,40}$/.test(t[k])) o[k] = t[k]; });
      ADVD[n] = o;
    });
  } catch (e) {}
  ADV_READY = true; advApply(); advRender();
}
function advCopy(from, to) { if (ADVD[from]) ADVD[to] = Object.assign({}, ADVD[from]); advSave(); }
function advRename(from, to) { if (ADVD[from]) { ADVD[to] = ADVD[from]; delete ADVD[from]; advSave(); } }
function advDelete(n) { if (ADVD[n]) { delete ADVD[n]; advSave(); } }
function advResetAll() { if (!confirm("Remove every advanced edit of this theme?")) return; delete ADVD[THEME.active]; advApply(true); advSave(); advRender(); }

function rgb2hex(c) { const m = /rgba?\((\d+), *(\d+), *(\d+)/.exec(c || ""); return m ? "#" + [m[1], m[2], m[3]].map(x => (+x).toString(16).padStart(2, "0")).join("") : "#000000"; }
function advDefault(el, p) {   // what the element looks like now (shown until you change it)
  let n = null; try { n = document.querySelector(el.sel); } catch (e) {}
  if (!n) return ADV_P[p][2] === "c" ? "#000000" : "";
  const cs = getComputedStyle(n), P = ADV_P[p][0];
  if (ADV_P[p][2] === "c") { const v = cs.getPropertyValue(P); return rgb2hex(v === "none" ? "" : v); }
  return String(Math.round(parseFloat(cs.getPropertyValue(P)) || 0));
}
function advCtl(el, p, o, ro) {
  const P = ADV_P[p], key = el.id + "." + p, box = document.createElement("span"); box.className = "advp";
  const lab = document.createElement("span"); lab.className = "lab"; lab.textContent = P[1]; box.appendChild(lab);
  const x = document.createElement("button"); x.className = "x"; x.textContent = "x"; x.title = "Back to the normal look";
  const mark = () => { box.classList.toggle("ov", o[key] !== undefined); x.disabled = ro || o[key] === undefined; };
  let inp, hex = null;
  if (P[2] === "c") {
    inp = document.createElement("input"); inp.type = "color"; inp.value = o[key] || advDefault(el, p);
    hex = document.createElement("input"); hex.type = "text"; hex.maxLength = 7; hex.value = o[key] || ""; hex.placeholder = inp.value;
    hex.setAttribute("spellcheck", "false"); hex.setAttribute("autocomplete", "off");
    inp.addEventListener("input", () => { hex.value = inp.value; advSet(key, inp.value); mark(); });
    hex.addEventListener("input", () => { let v = hex.value.trim(); if (v && v[0] !== "#") v = "#" + v; if (HEX.test(v)) { v = v.toLowerCase(); inp.value = v; advSet(key, v); mark(); } });
    x.onclick = () => { advSet(key, null); hex.value = ""; inp.value = advDefault(el, p); mark(); };
  } else {
    inp = document.createElement("input"); inp.type = "number"; inp.min = P[3]; inp.max = P[4]; inp.step = 1; inp.value = o[key] || ""; inp.placeholder = advDefault(el, p);
    inp.addEventListener("input", () => { const v = parseInt(inp.value, 10); if (v >= P[3] && v <= P[4]) advSet(key, String(v)); else if (inp.value === "") advSet(key, null); mark(); });
    x.onclick = () => { advSet(key, null); inp.value = ""; inp.placeholder = advDefault(el, p); mark(); };
  }
  inp.disabled = ro; if (hex) hex.disabled = ro;
  box.appendChild(inp); if (hex) box.appendChild(hex); box.appendChild(x); mark(); return box;
}
function advRender() {
  const wrap = $("advWrap"); if (!wrap) return;
  wrap.textContent = "";
  if (!advOn()) { wrap.style.display = "none"; return; }
  wrap.style.display = "";
  const ro = isBuiltin(THEME.active), o = advOf(!ro) || {};   // the live object of this theme: the controls read and write it
  const h = document.createElement("div"); h.className = "tgroup"; h.textContent = "-- ADVANCED: EVERY ELEMENT --"; wrap.appendChild(h);
  const note = document.createElement("p"); note.className = "dim";
  note.textContent = ro ? "Built-in themes can't be edited. Press COPY first." : "Click PICK, then click any part of the page to edit it. Only what you change is saved (in this theme).";
  wrap.appendChild(note);
  const bar = document.createElement("div"); bar.className = "row";
  const pk = document.createElement("button"); pk.textContent = "PICK AN ELEMENT"; pk.disabled = ro; pk.onclick = advPickStart;
  const ra = document.createElement("button"); ra.textContent = "RESET ALL ADVANCED"; ra.disabled = ro; ra.onclick = advResetAll;
  bar.appendChild(pk); bar.appendChild(ra); wrap.appendChild(bar);
  const g0 = document.createElement("div"); g0.className = "advrow"; g0.id = "adv-g";
  const fl = document.createElement("span"); fl.className = "advlab"; fl.textContent = "Text (whole page)"; g0.appendChild(fl);
  const pr = document.createElement("div"); pr.className = "advprops";
  const fs = document.createElement("span"); fs.className = "advp"; fs.innerHTML = '<span class="lab">font</span>';
  const sel = document.createElement("select"); sel.disabled = ro;
  ["Default"].concat(Object.keys(ADV_FONTS)).forEach(n => { const op = document.createElement("option"); op.value = n; op.textContent = n; sel.appendChild(op); });
  sel.value = o["g.font"] || "Default"; sel.onchange = () => advSet("g.font", sel.value === "Default" ? null : sel.value);
  fs.appendChild(sel); pr.appendChild(fs);
  const sz = document.createElement("span"); sz.className = "advp"; sz.innerHTML = '<span class="lab">base size</span>';
  const sn = document.createElement("input"); sn.type = "number"; sn.min = 10; sn.max = 28; sn.step = 1; sn.value = o["g.size"] || ""; sn.placeholder = "14"; sn.disabled = ro;
  sn.oninput = () => { const v = parseInt(sn.value, 10); if (v >= 10 && v <= 28) advSet("g.size", String(v)); else if (sn.value === "") advSet("g.size", null); };
  sz.appendChild(sn); pr.appendChild(sz); g0.appendChild(pr); wrap.appendChild(g0);
  ADV_GROUPS.forEach((g, gi) => {
    const d = document.createElement("details"); d.className = "advgrp"; d.id = "advg" + gi;
    const sm = document.createElement("summary"); sm.textContent = g.name; d.appendChild(sm);
    g.els.forEach(el => {
      const r = document.createElement("div"); r.className = "advrow"; r.id = "adv-" + el.id;
      const l = document.createElement("span"); l.className = "advlab"; l.textContent = el.label; r.appendChild(l);
      const ps = document.createElement("div"); ps.className = "advprops";
      el.props.forEach(p => ps.appendChild(advCtl(el, p, o, ro)));
      r.appendChild(ps); d.appendChild(r);
    });
    wrap.appendChild(d);
  });
}

)HTMLX";

static const char* HTML_JS_ADV2 = R"HTMLX(
// ---- pick an element: click anything on the page to jump to its settings ----
const PK = { on: false };
function advHit(t) {   // the element entry whose selector is closest to what was clicked
  let best = null, bd = 1e9, bn = null;
  ADV_ALL.forEach(el => {
    if (/:(hover|disabled)/.test(el.sel)) return;
    const sel = el.sel.replace(/::after/g, "");
    let n = t, d = 0;
    while (n && n.nodeType === 1) { let m = false; try { m = n.matches(sel); } catch (e) {} if (m) break; n = n.parentElement; d++; }
    if (n && n.nodeType === 1 && d < bd) { bd = d; best = el; bn = n; }
  });
  return best ? { el: best, node: bn } : null;
}
function advPickStart() {
  if (isBuiltin(THEME.active)) { alert("Built-in themes can't be edited. Press COPY first."); return; }
  menuClose(); PK.on = true; document.body.classList.add("picking"); $("pickBar").style.display = "block";
}
function advPickStop() { PK.on = false; document.body.classList.remove("picking"); ["pickBar", "pickBox", "pickLbl"].forEach(i => { $(i).style.display = "none"; }); }
function advReveal(id) {
  menuClose(); menuToggle("theme");
  const row = $("adv-" + id); if (!row) return;
  const g = row.closest("details"); if (g) g.open = true;
  setTimeout(() => { row.scrollIntoView({ block: "center" }); row.classList.add("flash"); setTimeout(() => row.classList.remove("flash"), 1500); }, 60);
}
document.addEventListener("pointermove", e => {
  if (!PK.on || e.target.closest(".pickui")) return;
  const h = advHit(e.target), b = $("pickBox"), l = $("pickLbl");
  if (!h) { b.style.display = "none"; l.style.display = "none"; return; }
  const r = h.node.getBoundingClientRect();
  b.style.cssText = "display:block;left:" + r.left + "px;top:" + r.top + "px;width:" + r.width + "px;height:" + r.height + "px";
  l.textContent = h.el.label; l.style.cssText = "display:block;left:" + Math.max(0, r.left) + "px;top:" + Math.max(0, r.top - 20) + "px";
}, true);
["pointerdown", "pointerup", "mousedown", "mouseup", "dblclick", "contextmenu"].forEach(t => document.addEventListener(t, e => { if (PK.on && !e.target.closest(".pickui")) { e.preventDefault(); e.stopPropagation(); } }, true));
document.addEventListener("click", e => {
  if (!PK.on || e.target.closest(".pickui")) return;
  e.preventDefault(); e.stopPropagation();
  const h = advHit(e.target); advPickStop(); if (h) advReveal(h.el.id);
}, true);
document.addEventListener("keydown", e => { if (e.key === "Escape" && PK.on) advPickStop(); }, true);
try { const css = localStorage.getItem("espdevice-advcss"); if (css) { const st = document.createElement("style"); st.id = "advStyle"; st.textContent = css; document.head.appendChild(st); } } catch (e) {}   // instant repaint, like the colours
)HTMLX";

static const char* HTML_JS2 = R"HTMLX(
async function selectProfile() { applyState(await api("/api/select", { index: parseInt($("profileSelect").value, 10) })); }
async function addProfile() {
  const name = prompt("New profile name:", "Profile " + (S.profiles.filter(p => !p.test).length + 1));
  if (name === null) return;
  const r = await api("/api/add", { name: name });
  applyState(r);
  if (!r.error) { const idx = r.profiles.length - 2; applyState(await api("/api/select", { index: idx })); }   /* the new profile sits just above "test" */
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
  if (v.y === "P") { if (performance.now() > replayUntil) { cpsHit(); kc[v.k]++; } held[v.k] = true; paintKeys(); logLine("P", "[" + t + "s] " + key + " DOWN  " + v.m.padEnd(14, " ") + (v.l ? v.l + " us" : "(paused)")); }
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
let replayUntil = 0, cpsTimes = [], cTotal = 0, cSrv = -1, cSince = 0;
// big numbers: written out up to 99,999; from 100,000 on short (123K, 1.25M, 12.3M, 123M, 1.23B); the exact value is the hover text
function fmtBig(n) {
  if (n < 1e5) return { t: n.toLocaleString("en-US"), c: "" };
  const u = [[1e12, "T"], [1e9, "B"], [1e6, "M"], [1e3, "K"]];
  for (let i = 0; i < u.length; i++) if (n >= u[i][0]) {
    const v = n / u[i][0], d = v >= 100 ? 1 : v >= 10 ? 10 : 100;
    return { t: (Math.floor(v * d) / d) + u[i][1], c: "" };
  }
}
function paintCounters() {
  paintKeyTotals();
  const now = performance.now(), st = (typeof WM !== "undefined" && WM.set) ? WM.set : { cpsOff: false, cpsColor: true };
  while (cpsTimes.length && now - cpsTimes[0] > 1000) cpsTimes.shift();
  const n = st.cpsOff ? 0 : cpsTimes.length;
  const c = $("cCps"), t = $("cTotal"), cs = String(n), f = fmtBig(cTotal);
  if (c.textContent !== cs) c.textContent = cs;
  if (t.textContent !== f.t) { t.textContent = f.t; t.className = f.c; $("cTotalRow").title = "Total: " + cTotal.toLocaleString("en-US") + (cSince > 0 ? "  (since " + new Date(cSince * 1000).toLocaleDateString() + ")" : ""); }
  // 0-5 normal colour, 5-10 orange, 10-15 red, 15+ purple (can be switched off in SETTINGS)
  const k = (st.cpsColor && n >= 15) ? 3 : (st.cpsColor && n >= 10) ? 2 : (st.cpsColor && n >= 5) ? 1 : 0;
  c.className = (c.className.indexOf("pop") >= 0 ? "pop " : "") + (k ? "cps" + k : "");
}
function cpsHit() {
  const off = typeof WM !== "undefined" && WM.set && WM.set.cpsOff;
  if (!off) cpsTimes.push(performance.now());
  cTotal++; paintCounters();
  if (!off) { const e = $("cCps"); e.classList.remove("pop"); void e.offsetWidth; e.classList.add("pop"); }   // only the CPS number pops
}
function syncTotal(total, since) {
  if (total !== cSrv) { cSrv = total; cTotal = total; paintCounters(); }
  if (since > 0 && since !== cSince) { cSince = since; cSrv = -2; if (total === cTotal) cTotal = total; paintCounters(); }
}
async function resetCounters() {
  if (!confirm("Reset the Total and CPS counters to zero?")) return;
  cpsTimes = []; applyState(await api("/api/counters/reset", {}));
}
setInterval(paintCounters, 100);

(function init() {
  wmInit().catch(() => { const d = $("desk"); if (d) d.classList.add("ready"); }); // windows (see the WINDOWS section)
  loadTheme().then(advLoad);
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
    s += HTML_JS_WM3;
    s += HTML_JS_STATS;
    s += HTML_JS_ADV;
    s += HTML_JS_ADV2;
    s += HTML_JS2;
    const std::string marker = "@@VER@@", ver = ESP_VERSION;
    for (size_t p = s.find(marker); p != std::string::npos; p = s.find(marker, p + ver.size())) s.replace(p, marker.size(), ver);
    return s;
}
