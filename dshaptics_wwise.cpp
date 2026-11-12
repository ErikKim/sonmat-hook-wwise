// dshaptics_wwise.dll - log every Wwise PostEvent / SetSwitch the game makes, natively.
//
// The UE4SS Lua mod only sees calls that go through UFunctions. Black Myth: Wukong posts some
// sounds straight from C++ (my staff hitting an enemy, the footstep surface switch), so they
// never reach Lua. This DLL hooks the Wwise SDK inside the game and appends one line per call to
// the same file the daemon tails:
//     wid:<eventID>:<gameObjectID>             AK::SoundEngine::PostEvent(AkUniqueID, ...)
//     wsw:<groupID>:<stateID>:<gameObjectID>   AK::SoundEngine::SetSwitch(group, state, obj)
// IDs are Wwise FNV-1 hashes of the names; the daemon maps them back (dll/event_ids_<game>.json).
//
// Where sounds are (only when sigs.txt has SetPosition=; without it nothing below happens): two more kinds of lines,
// written only next to an event, never per SetPosition call (the game places every object every frame):
//     wpos:<gameObjectID>:<x>,<y>,<z>   the object's place in cm (Wwise axes: x right, y up, z front), just before its
//                                       wid line when it is new or has moved 50 cm since its last wpos; an object that
//                                       posts before it was ever placed gets it at its first SetPosition
//     wlis:<x>,<y>,<z>:<fx>,<fy>,<fz>:<tx>,<ty>,<tz>   the listener: place (cm), front and top (x1000), when it has
//                                       moved 50 cm or turned about 10 degrees, at most 10 a second
// AK::SoundEngine::SetPosition(obj, const AkWorldTransform&, flags): 2022+ SDK layout, read from the function itself
// (Wukong, Phantom Blade Zero 2026-11-12): orientationFront 3 floats @0, orientationTop @0x0C, position 3 doubles @0x18.
// The listener is the object the game hands AddDefaultListener / SetDefaultListeners (patterns of those names); with
// neither found there are no wlis lines. PosScale=<x> multiplies the place into cm (default 1: Unreal's own cm).
//
// Function addresses come from byte patterns in sigs.txt next to the DLL (one "name=pattern" per
// line, "??" = wildcard, or "+<rva>" for one exact build; first line that resolves wins), so a game
// patch only needs new patterns, not a rebuild.
// Loaded by the daemon (CreateRemoteThread + LoadLibraryW). Hooks via MinHook.

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "MinHook.h"
#include "rumble_note.h"     // our event lines into the game's vibration log (sonmat_rumble.dll does the vibration)

using AkUInt32 = uint32_t;
using AkUniqueID = uint32_t;
using AkPlayingID = uint32_t;
using AkGameObjectID = uint64_t;
using AKRESULT = int32_t;

// Wwise 2022/2023 SDK signatures (x64, Microsoft ABI).
using PostEvent_t = AkPlayingID(*)(AkUniqueID, AkGameObjectID, AkUInt32, void*, void*, AkUInt32, void*, AkPlayingID);
using SetSwitch_t = AKRESULT(*)(AkUInt32, AkUInt32, AkGameObjectID);
using SetPosition_t = AKRESULT(*)(AkGameObjectID, const uint8_t*, uint64_t);     // (obj, const AkWorldTransform&, flags)
using AddListener_t = AKRESULT(*)(AkGameObjectID);                               // AddDefaultListener(obj)
using SetListeners_t = AKRESULT(*)(const AkGameObjectID*, AkUInt32);             // SetDefaultListeners(objs, count)

static PostEvent_t g_postEvent = nullptr;
static SetSwitch_t g_setSwitch = nullptr;
static SetPosition_t g_setPosition = nullptr;
static AddListener_t g_addListener = nullptr;
static SetListeners_t g_setListeners = nullptr;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_lock;

static void log_line(const char* s, int n) {
    if (g_log == INVALID_HANDLE_VALUE) return;
    EnterCriticalSection(&g_lock);
    DWORD w; WriteFile(g_log, s, (DWORD)n, &w, nullptr);   // FILE_APPEND_DATA: each line lands whole
    LeaveCriticalSection(&g_lock);
    rumble_tap::note(s, n);
}

// ---------------------------------------------------------------- where sounds are
struct Place { AkGameObjectID obj; float p[3], sent[3]; bool has, sent_ok, pending; };
static Place g_places[4096];                    // by object, one slot per hash (a clash just forgets the older one)
static SRWLOCK g_placeLock = SRWLOCK_INIT;      // guards g_places and the listener below
static AkGameObjectID g_listener = 0;           // 0: not known (0 is no object in Wwise)
static float g_lis[9], g_lisSent[6];            // place, front, top / place and front last written
static bool g_lisHas = false, g_lisSentOk = false;
static ULONGLONG g_lisT = 0;
static float g_scale = 1.0f;

static Place& slot_of(AkGameObjectID obj) { return g_places[(obj * 0x9E3779B97F4A7C15ull) >> 52]; }
static bool real_obj(AkGameObjectID obj) { return obj != 0 && obj < ~0ull - 0x20; }

static bool read_xf(const uint8_t* xf, float out[9]) {          // front, top, place (the doubles made floats)
    __try {
        double d[3];
        memcpy(out, xf, 24); memcpy(d, xf + 0x18, 24);
        for (int i = 0; i < 3; i++) out[6 + i] = (float)d[i];
        for (int i = 0; i < 9; i++) if (!(out[i] == out[i]) || out[i] > 1e9f || out[i] < -1e9f) return false;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static float far2(const float* a, const float* b) {
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2]; return x * x + y * y + z * z;
}

static int place_line(Place& s, char* out, int cap) {           // under g_placeLock
    memcpy(s.sent, s.p, sizeof s.p); s.sent_ok = true;
    return snprintf(out, cap, "wpos:%llu:%d,%d,%d\n", (unsigned long long)s.obj,
                    (int)(s.p[0] * g_scale), (int)(s.p[1] * g_scale), (int)(s.p[2] * g_scale));
}

static int listener_line(char* out, int cap) {                  // under g_placeLock
    if (!g_lisHas) return 0;
    ULONGLONG now = GetTickCount64();
    if (now - g_lisT < 100) return 0;
    if (g_lisSentOk) {
        float d = g_lis[3] * g_lisSent[3] + g_lis[4] * g_lisSent[4] + g_lis[5] * g_lisSent[5];
        if (far2(g_lis, g_lisSent) * g_scale * g_scale < 2500.0f && d > 0.985f) return 0;
    }
    memcpy(g_lisSent, g_lis, sizeof g_lisSent); g_lisSentOk = true; g_lisT = now;
    return snprintf(out, cap, "wlis:%d,%d,%d:%d,%d,%d:%d,%d,%d\n",
                    (int)(g_lis[0] * g_scale), (int)(g_lis[1] * g_scale), (int)(g_lis[2] * g_scale),
                    (int)(g_lis[3] * 1000), (int)(g_lis[4] * 1000), (int)(g_lis[5] * 1000),
                    (int)(g_lis[6] * 1000), (int)(g_lis[7] * 1000), (int)(g_lis[8] * 1000));
}

static void take_listener(const float* xf9) {                   // under g_placeLock: front, top, place -> place, front, top
    memcpy(g_lis, xf9 + 6, 12); memcpy(g_lis + 3, xf9, 24); g_lisHas = true;
}

static AKRESULT hk_SetPosition(AkGameObjectID obj, const uint8_t* xf, uint64_t flags) {
    float v[9];
    if (xf && real_obj(obj) && read_xf(xf, v)) {
        char line[96]; int n = 0;
        AcquireSRWLockExclusive(&g_placeLock);
        if (obj == g_listener) take_listener(v);
        Place& s = slot_of(obj);
        if (s.obj != obj) { s = Place{}; s.obj = obj; }
        memcpy(s.p, v + 6, sizeof s.p); s.has = true;
        if (s.pending) { s.pending = false; n = place_line(s, line, sizeof line); }   // it posted before it had a place
        ReleaseSRWLockExclusive(&g_placeLock);
        if (n > 0) log_line(line, n);
    }
    return g_setPosition(obj, xf, flags);
}

static void listener_is(AkGameObjectID obj) {
    AcquireSRWLockExclusive(&g_placeLock);
    if (obj != g_listener) {                                      // its front and top come with its next SetPosition
        g_listener = obj; g_lisHas = false; g_lisSentOk = false;  // (the camera's object is placed every frame)
    }
    ReleaseSRWLockExclusive(&g_placeLock);
}

static AKRESULT hk_AddListener(AkGameObjectID obj) {
    if (real_obj(obj)) listener_is(obj);
    return g_addListener(obj);
}

static bool first_id(const AkGameObjectID* ids, AkGameObjectID* out) {
    __try { *out = ids[0]; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static AKRESULT hk_SetListeners(const AkGameObjectID* ids, AkUInt32 n) {
    AkGameObjectID obj = 0;
    if (ids && n > 0 && first_id(ids, &obj) && real_obj(obj)) listener_is(obj);
    return g_setListeners(ids, n);
}

// the lines that go before an event's wid line: the listener if it moved, the object's place if new or moved
static void place_lines(AkGameObjectID obj) {
    if (!g_setPosition || !real_obj(obj)) return;
    char pl[96], ll[128]; int pn = 0, ln = 0;
    AcquireSRWLockExclusive(&g_placeLock);
    ln = listener_line(ll, sizeof ll);
    Place& s = slot_of(obj);
    if (s.obj != obj) { s = Place{}; s.obj = obj; }
    if (!s.has) s.pending = true;
    else if (!s.sent_ok || far2(s.p, s.sent) * g_scale * g_scale >= 2500.0f) pn = place_line(s, pl, sizeof pl);
    ReleaseSRWLockExclusive(&g_placeLock);
    if (ln > 0) log_line(ll, ln);
    if (pn > 0) log_line(pl, pn);
}

static AkPlayingID hk_PostEvent(AkUniqueID ev, AkGameObjectID obj, AkUInt32 flags, void* cb, void* cookie,
                                AkUInt32 nExt, void* ext, AkPlayingID pid) {
    place_lines(obj);
    char buf[64];
    int n = snprintf(buf, sizeof buf, "wid:%u:%llu\n", ev, (unsigned long long)obj);
    log_line(buf, n);
    return g_postEvent(ev, obj, flags, cb, cookie, nExt, ext, pid);
}

static AKRESULT hk_SetSwitch(AkUInt32 group, AkUInt32 state, AkGameObjectID obj) {
    char buf[80];
    int n = snprintf(buf, sizeof buf, "wsw:%u:%u:%llu\n", group, state, (unsigned long long)obj);
    log_line(buf, n);
    return g_setSwitch(group, state, obj);
}

// ---------------------------------------------------------------- pattern scan
static std::vector<int> parse_pattern(const std::string& p) {
    std::vector<int> out;
    for (size_t i = 0; i < p.size();) {
        if (p[i] == ' ') { i++; continue; }
        if (p[i] == '?') { out.push_back(-1); i += (i + 1 < p.size() && p[i + 1] == '?') ? 2 : 1; continue; }
        out.push_back(std::stoi(p.substr(i, 2), nullptr, 16)); i += 2;
    }
    return out;
}

// The first place the pattern matches; *hits = how many places match (counted up to 2: a pattern is meant
// to match exactly once, more means it may have caught something else - logged, the first is still used).
static uint8_t* scan(const std::vector<int>& pat, int* hits) {
    auto base = (uint8_t*)GetModuleHandleW(nullptr);
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    uint8_t* first = nullptr;
    *hits = 0;
    for (int s = 0; s < nt->FileHeader.NumberOfSections; s++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* p = base + sec->VirtualAddress;
        size_t n = sec->Misc.VirtualSize;
        for (size_t i = 0; i + pat.size() <= n; i++) {
            size_t k = 0;
            while (k < pat.size() && (pat[k] < 0 || p[i + k] == pat[k])) k++;
            if (k == pat.size()) { if (!first) first = p + i; if (++*hits == 2) return first; }
        }
    }
    return first;
}

// The running exe's build = the link time in its PE header. A fixed address ("+<rva>") is right for one
// build only: after a game update it points into some other code, and hooking there crashes the game.
static unsigned long exe_build() {
    auto base = (uint8_t*)GetModuleHandleW(nullptr);
    return ((IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew))->FileHeader.TimeDateStamp;
}

static std::string dll_dir(HMODULE self) {
    char path[MAX_PATH]; GetModuleFileNameA(self, path, MAX_PATH);
    std::string s(path); return s.substr(0, s.find_last_of("\\/") + 1);
}

static DWORD WINAPI init(LPVOID self) {
    InitializeCriticalSection(&g_lock);
    g_log = CreateFileW(L"C:\\Users\\Public\\dshaptics\\events.log", FILE_APPEND_DATA,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, 0, nullptr);
    std::string dir = dll_dir((HMODULE)self);
    std::vector<std::pair<std::string, std::string>> sigs;      // name, pattern - in file order
    unsigned long build = 0;                                     // "Build=<hex>": the exe the fixed addresses came from
    char line[1024];
    FILE* f = fopen((dir + "sigs.txt").c_str(), "r");
    while (f && fgets(line, sizeof line, f)) {
        std::string l(line);
        auto eq = l.find('=');
        if (l.empty() || l[0] == '#' || eq == std::string::npos) continue;
        std::string pat = l.substr(eq + 1);
        while (!pat.empty() && (pat.back() == '\n' || pat.back() == '\r')) pat.pop_back();
        if (l.substr(0, eq) == "Build") build = strtoul(pat.c_str(), nullptr, 16);
        else if (l.substr(0, eq) == "PosScale") g_scale = (float)atof(pat.c_str());
        else if (!pat.empty()) sigs.push_back({l.substr(0, eq), pat});
    }
    if (f) fclose(f);
    if (MH_Initialize() != MH_OK) {
        FILE* status = fopen((dir + "dshaptics_wwise.log").c_str(), "w");
        if (status) { fputs("MinHook init failed\n", status); fclose(status); }
        return 0;
    }
    // Loaded while the game is still starting (the daemon was already running), the functions are found
    // but cannot be hooked yet: 2026-10-06, both "hook failed" 21 s after launch and nothing ever came.
    // So keep trying every 3 s for 5 minutes; the status file is rewritten on every attempt.
    struct Fn { const char* name; void* hook; void** orig; };
    const Fn fns[] = {
        {"PostEvent", (void*)hk_PostEvent, (void**)&g_postEvent},
        {"SetSwitch", (void*)hk_SetSwitch, (void**)&g_setSwitch},
        {"SetPosition", (void*)hk_SetPosition, (void**)&g_setPosition},
        {"AddDefaultListener", (void*)hk_AddListener, (void**)&g_addListener},
        {"SetDefaultListeners", (void*)hk_SetListeners, (void**)&g_setListeners},
    };
    auto fn_of = [&](const std::string& name) -> const Fn* {
        for (auto& x : fns) if (name == x.name) return &x;
        return nullptr;
    };
    auto all_hooked = [&]() {                                   // every function sigs.txt names
        for (auto& sg : sigs) { const Fn* x = fn_of(sg.first); if (x && !*x->orig) return false; }
        return true;
    };
    for (int attempt = 1; attempt <= 100 && !all_hooked(); attempt++) {
        if (attempt > 1) Sleep(3000);
        FILE* status = fopen((dir + "dshaptics_wwise.log").c_str(), "w");
        if (status) fprintf(status, "attempt %d\n", attempt);
        for (auto& sg : sigs) {
            const std::string& name = sg.first; const std::string& pat = sg.second;
            // "+<hex rva>" = fixed address in one exact build (fallback for when the pattern bytes are
            // already patched, e.g. by a probe hook still loaded in the process) - used only when the exe is the
            // build named on the Build= line: after a game update it would hook the middle of other code
            const Fn* x = fn_of(name);
            void* hook = x ? x->hook : nullptr;
            void** orig = x ? x->orig : nullptr;
            if (orig && *orig) { if (status) fprintf(status, "%s: hooked\n", name.c_str()); continue; }
            if (pat[0] == '+' && build != exe_build()) {
                if (status) fprintf(status, "%s: fixed address skipped (taken from build %08lx, this exe is %08lx)\n",
                                    name.c_str(), build, exe_build());
                continue;
            }
            int hits = 1;
            uint8_t* at = pat[0] == '+' ? (uint8_t*)GetModuleHandleW(nullptr) + strtoull(pat.c_str() + 1, nullptr, 16)
                                        : scan(parse_pattern(pat), &hits);
            if (hits > 1 && status) fprintf(status, "%s: pattern matches more than one place - the first is used\n", name.c_str());
            if (!at || !hook) { if (status) fprintf(status, "%s: not found\n", name.c_str()); continue; }
            void* tramp = nullptr;
            MH_STATUS st = MH_CreateHook(at, hook, &tramp);
            if (st == MH_OK) {
                *orig = tramp;                               // set before enabling: the hook may fire at once
                st = MH_EnableHook(at);
                if (st != MH_OK) { MH_RemoveHook(at); *orig = nullptr; }
            }
            if (status) fprintf(status, "%s: %p %s\n", name.c_str(), at, st == MH_OK ? "hooked" : MH_StatusToString(st));
        }
        if (status) fclose(status);
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        CreateThread(nullptr, 0, init, h, 0, nullptr);   // no heavy work under the loader lock
    }
    return TRUE;
}
