// Nexus Launcher
// 1. Lance Minecraft Bedrock (si besoin) via le protocole minecraft:
// 2. Attend que le processus soit pret (DirectX charge)
// 3. Injecte Nexus.dll (LoadLibraryW via CreateRemoteThread)

#include <windows.h>
#include <tlhelp32.h>
#include <aclapi.h>
#include <sddl.h>
#include <shellapi.h>
#include <cstdio>
#include <string>
#include <filesystem>
#include <io.h>
#include <fcntl.h>
#include "Updater.h"
#include "version.h"

namespace fs = std::filesystem;

static constexpr wchar_t kProcessName[] = L"Minecraft.Windows.exe";
static constexpr wchar_t kDllName[] = L"Nexus.dll";

static void Info(const wchar_t* fmt, ...) {
    va_list args; va_start(args, fmt);
    wprintf(L"[*] "); vwprintf(fmt, args); wprintf(L"\n");
    va_end(args);
}
static void Error(const wchar_t* fmt, ...) {
    const DWORD code = GetLastError();
    va_list args; va_start(args, fmt);
    wprintf(L"[!] "); vwprintf(fmt, args); wprintf(L" (code %lu)\n", code);
    va_end(args);
}

static DWORD FindProcess(const wchar_t* name) {
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{ sizeof(pe) };
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; }
    }
    CloseHandle(snap);
    return pid;
}

static bool HasModule(DWORD pid, const wchar_t* module) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me{ sizeof(me) };
    bool found = false;
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
        if (_wcsicmp(me.szModule, module) == 0) { found = true; break; }
    }
    CloseHandle(snap);
    return found;
}

// Ancienne version UWP : le jeu est sandboxe et ne peut lire la DLL que si
// "ALL APPLICATION PACKAGES" a les droits dessus. Sans effet sur la version GDK.
static void GrantAppContainerAccess(const fs::path& file) {
    PSID sid = nullptr;
    if (!ConvertStringSidToSidW(L"S-1-15-2-1", &sid)) return;

    PACL oldAcl = nullptr; PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetNamedSecurityInfoW(file.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                              nullptr, nullptr, &oldAcl, nullptr, &sd) == ERROR_SUCCESS) {
        EXPLICIT_ACCESSW ea{};
        ea.grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
        ea.grfAccessMode = GRANT_ACCESS;
        ea.grfInheritance = NO_INHERITANCE;
        ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        ea.Trustee.ptstrName = static_cast<LPWSTR>(sid);

        PACL newAcl = nullptr;
        if (SetEntriesInAclW(1, &ea, oldAcl, &newAcl) == ERROR_SUCCESS) {
            SetNamedSecurityInfoW(const_cast<LPWSTR>(file.c_str()), SE_FILE_OBJECT,
                                  DACL_SECURITY_INFORMATION, nullptr, nullptr, newAcl, nullptr);
            LocalFree(newAcl);
        }
        LocalFree(sd);
    }
    LocalFree(sid);
}

static bool Inject(DWORD pid, const fs::path& dll) {
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                              PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                              FALSE, pid);
    if (!proc) { Error(L"Impossible d'ouvrir le processus (essaie en administrateur)"); return false; }

    const std::wstring path = dll.wstring();
    const SIZE_T size = (path.size() + 1) * sizeof(wchar_t);

    void* remote = VirtualAllocEx(proc, nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { Error(L"VirtualAllocEx a echoue"); CloseHandle(proc); return false; }

    bool ok = false;
    if (WriteProcessMemory(proc, remote, path.c_str(), size, nullptr)) {
        // kernel32 est mappe a la meme adresse dans tous les processus d'une session.
        auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        HANDLE thread = CreateRemoteThread(proc, nullptr, 0, loadLibrary, remote, 0, nullptr);
        if (thread) {
            WaitForSingleObject(thread, 15000);
            DWORD exitCode = 0;
            GetExitCodeThread(thread, &exitCode);
            ok = exitCode != 0; // HMODULE tronque, mais 0 = echec du LoadLibrary
            if (!ok) Info(L"LoadLibraryW a renvoye NULL : la DLL n'a pas pu etre chargee.");
            CloseHandle(thread);
        } else {
            Error(L"CreateRemoteThread a echoue");
        }
    } else {
        Error(L"WriteProcessMemory a echoue");
    }

    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);
    return ok;
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_U16TEXT);   // accents corrects dans la console (notes de version)
    SetConsoleTitleW(L"Nexus Launcher");
    wprintf(L"\n   _   _                     \n  | \\ | | _____  ___   _ ___ \n  |  \\| |/ _ \\ \\/ / | | / __|\n  | |\\  |  __/>  <| |_| \\__ \\\n  |_| \\_|\\___/_/\\_\\\\__,_|___/\n\n  Client Minecraft Bedrock - version %s\n  Cree par Claude (Anthropic) - signatures : Zoumi\n\n", NEXUS_VERSION_W);

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    const fs::path exeDir = fs::path(exePath).parent_path();

    // ---- Mises a jour obligatoires -------------------------------------------------
    Updater::CleanupOldFiles(exeDir);
    switch (Updater::Run(exeDir, exePath)) {
        case Updater::Result::Updated:
            return 0;                       // le nouveau launcher a pris le relais
        case Updater::Result::Failed:
            Info(L"Nexus ne peut pas etre lance sans la derniere mise a jour.");
            system("pause");
            return 1;
        default:
            break;                          // a jour, hors ligne ou version de dev : on continue
    }
    wprintf(L"\n");

    fs::path dll = exeDir / kDllName;
    // Option : chemin de DLL personnalise. On ignore les drapeaux (--updated, etc.).
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] != L'-') { dll = fs::absolute(argv[i]); break; }
    }

    if (!fs::exists(dll)) {
        Info(L"DLL introuvable : %s", dll.c_str());
        system("pause");
        return 1;
    }
    GrantAppContainerAccess(dll);

    DWORD pid = FindProcess(kProcessName);
    if (!pid) {
        Info(L"Lancement de Minecraft Bedrock...");
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"minecraft:", nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
            Error(L"Impossible de lancer Minecraft (est-il installe ?)");
            system("pause");
            return 1;
        }
        for (int i = 0; i < 120 && !pid; ++i) { Sleep(500); pid = FindProcess(kProcessName); }
        if (!pid) { Info(L"Minecraft ne s'est pas lance dans les 60 secondes."); system("pause"); return 1; }
    }
    Info(L"Minecraft trouve (PID %lu)", pid);

    if (HasModule(pid, kDllName)) {
        Info(L"Nexus est deja injecte. Appuie sur FIN en jeu pour le decharger d'abord.");
        system("pause");
        return 0;
    }

    // Attendre que DirectX soit charge : le hook de rendu en a besoin.
    Info(L"Attente du chargement du rendu...");
    for (int i = 0; i < 120; ++i) {
        if (HasModule(pid, L"d3d12.dll") || HasModule(pid, L"d3d11.dll")) break;
        Sleep(500);
    }
    Sleep(3000); // laisse le jeu creer sa swapchain

    Info(L"Injection de %s", dll.filename().c_str());
    if (!Inject(pid, dll)) { system("pause"); return 1; }

    Info(L"Injection reussie ! INSER = menu, FIN = decharger.");
    Sleep(2500);
    return 0;
}
