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
// Function addresses come from byte patterns in sigs.txt next to the DLL (one "name=pattern" per
// line, "??" = wildcard, or "+<rva>" for one exact build; first line that resolves wins), so a game
// patch only needs new patterns, not a rebuild.
// Loaded by the daemon (CreateRemoteThread + LoadLibraryW). Hooks via MinHook.

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "MinHook.h"
#include "rumble_tap.h"     // the game's own vibration requests, logged beside our event lines

using AkUInt32 = uint32_t;
using AkUniqueID = uint32_t;
using AkPlayingID = uint32_t;
using AkGameObjectID = uint64_t;
using AKRESULT = int32_t;

// Wwise 2022/2023 SDK signatures (x64, Microsoft ABI).
using PostEvent_t = AkPlayingID(*)(AkUniqueID, AkGameObjectID, AkUInt32, void*, void*, AkUInt32, void*, AkPlayingID);
using SetSwitch_t = AKRESULT(*)(AkUInt32, AkUInt32, AkGameObjectID);

static PostEvent_t g_postEvent = nullptr;
static SetSwitch_t g_setSwitch = nullptr;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_lock;

static void log_line(const char* s, int n) {
    if (g_log == INVALID_HANDLE_VALUE) return;
    EnterCriticalSection(&g_lock);
    DWORD w; WriteFile(g_log, s, (DWORD)n, &w, nullptr);   // FILE_APPEND_DATA: each line lands whole
    LeaveCriticalSection(&g_lock);
    rumble_tap::note(s, n);
}

static AkPlayingID hk_PostEvent(AkUniqueID ev, AkGameObjectID obj, AkUInt32 flags, void* cb, void* cookie,
                                AkUInt32 nExt, void* ext, AkPlayingID pid) {
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
        else if (!pat.empty()) sigs.push_back({l.substr(0, eq), pat});
    }
    if (f) fclose(f);
    if (MH_Initialize() != MH_OK) {
        FILE* status = fopen((dir + "dshaptics_wwise.log").c_str(), "w");
        if (status) { fputs("MinHook init failed\n", status); fclose(status); }
        return 0;
    }
    rumble_tap::start("C:\\Users\\Public\\dshaptics\\");      // same folder as events.log
    // Loaded while the game is still starting (the daemon was already running), the functions are found
    // but cannot be hooked yet: 2026-10-06, both "hook failed" 21 s after launch and nothing ever came.
    // So keep trying every 3 s for 5 minutes; the status file is rewritten on every attempt.
    for (int attempt = 1; attempt <= 100 && !(g_postEvent && g_setSwitch); attempt++) {
        if (attempt > 1) Sleep(3000);
        FILE* status = fopen((dir + "dshaptics_wwise.log").c_str(), "w");
        if (status) fprintf(status, "attempt %d\n", attempt);
        for (auto& sg : sigs) {
            const std::string& name = sg.first; const std::string& pat = sg.second;
            // "+<hex rva>" = fixed address in one exact build (fallback for when the pattern bytes are
            // already patched, e.g. by a probe hook still loaded in the process) - used only when the exe is the
            // build named on the Build= line: after a game update it would hook the middle of other code
            void* hook = name == "PostEvent" ? (void*)hk_PostEvent : name == "SetSwitch" ? (void*)hk_SetSwitch : nullptr;
            void** orig = name == "PostEvent" ? (void**)&g_postEvent : name == "SetSwitch" ? (void**)&g_setSwitch : nullptr;
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
