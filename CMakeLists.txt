cmake_minimum_required(VERSION 3.20)
project(Nexus LANGUAGES C CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Nexus se compile uniquement pour Windows x64 (Visual Studio 2022, generateur x64).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# /EHa est ajoute plus bas pour la DLL : on retire le /EHsc par defaut (evite l'avertissement D9025)
string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # /MT : pas de dependance aux DLL VC++

# --- Version et depot de mises a jour -------------------------------------------
# Resolution de la version, par ordre de priorite :
#  1. Variable d'environnement NEXUS_RELEASE_VERSION (fixee par le CI release) : CMake lit
#     $ENV{} de facon identique quelle que soit sa version (le -D cache est capricieux en 4.x).
#  2. -DNEXUS_VERSION=... si une valeur valide a bien ete recue.
#  3. Sinon (build local) : 1re version du CHANGELOG.md, suffixee "-dev".
set(_nx_ver "")
if(DEFINED ENV{NEXUS_RELEASE_VERSION} AND NOT "$ENV{NEXUS_RELEASE_VERSION}" STREQUAL "")
    set(_nx_ver "$ENV{NEXUS_RELEASE_VERSION}")
elseif(DEFINED NEXUS_VERSION AND NOT "${NEXUS_VERSION}" STREQUAL "")
    set(_nx_ver "${NEXUS_VERSION}")
endif()
if(NOT _nx_ver MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+")
    set(_nx_ver "")
    if(EXISTS "${CMAKE_SOURCE_DIR}/CHANGELOG.md")
        file(STRINGS "${CMAKE_SOURCE_DIR}/CHANGELOG.md" _nx_lines
             REGEX "^## \\[[0-9]+\\.[0-9]+\\.[0-9]+\\]")
        if(_nx_lines)
            list(GET _nx_lines 0 _nx_first)
            string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" _nx_ver "${_nx_first}")
        endif()
    endif()
    if(_nx_ver MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+")
        set(_nx_ver "${_nx_ver}-dev")     # build local : marque "-dev"
    else()
        set(_nx_ver "0.0.0-dev")
    endif()
endif()
set(NEXUS_VERSION "${_nx_ver}")
set(NEXUS_UPDATE_REPO "ZoumiSurGithub/NexusClient" CACHE STRING "Depot GitHub (proprietaire/nom) des mises a jour")
configure_file(${CMAKE_SOURCE_DIR}/cmake/version.h.in ${CMAKE_BINARY_DIR}/generated/version.h @ONLY)

# Integre offsets.ini (s'il existe) dans la DLL, sous forme obfusquee
set(OFFSETS_FILE "${CMAKE_SOURCE_DIR}/offsets.ini")
include(${CMAKE_SOURCE_DIR}/cmake/embed_offsets.cmake)
message(STATUS "Nexus ${NEXUS_VERSION} (mises a jour : ${NEXUS_UPDATE_REPO})")

include(FetchContent)
# Telechargement en .zip : Git n'est pas necessaire
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

# --- MinHook (hooks de fonctions) ---------------------------------------------
# SOURCE_SUBDIR inexistant : on telecharge seulement, et on compile nous-memes les
# 5 fichiers (evite les soucis de version du CMakeLists de MinHook).
FetchContent_Declare(minhook
    URL            https://github.com/TsudaKageyu/minhook/archive/refs/heads/master.zip
    SOURCE_SUBDIR  _no_cmake)
FetchContent_MakeAvailable(minhook)

add_library(minhook STATIC
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c
    ${minhook_SOURCE_DIR}/src/hde/hde32.c
    ${minhook_SOURCE_DIR}/src/hde/hde64.c)
target_include_directories(minhook PUBLIC ${minhook_SOURCE_DIR}/include)

# --- Dear ImGui (rendu du HUD et du menu) -------------------------------------
FetchContent_Declare(imgui
    URL            https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b.zip)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${imgui_SOURCE_DIR}/misc/cpp)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
target_compile_options(imgui PRIVATE /EHsc)

# --- La DLL injectee dans le jeu ------------------------------------------------
add_library(Nexus SHARED
    client/dllmain.cpp
    client/core/Logger.cpp
    client/core/Config.cpp
    client/core/SigScan.cpp
    client/hooks/RenderHook.cpp
    client/hooks/InputHook.cpp
    client/hooks/MouseBlock.cpp
    client/hooks/FocusGrab.cpp
    client/game/Game.cpp
    client/game/ItemRender.cpp
    client/game/ArmorEcs.cpp
    client/game/IconCache.cpp
    client/game/StbImage.cpp
    client/modules/AutoSprint.cpp
    client/modules/HealthBars.cpp
    client/modules/Stats.cpp
    client/modules/Zoom.cpp
    client/modules/OwnNametag.cpp
    client/modules/ThirdPersonNametag.cpp
    client/modules/ChatSender.cpp
    client/modules/ChatOverlay.cpp
    client/modules/Network.cpp
    client/modules/DiscordRPC.cpp
    client/ui/Hud.cpp
    client/ui/Menu.cpp)
target_include_directories(Nexus PRIVATE client third_party ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(Nexus PRIVATE imgui minhook d3d11 d3d12 dxgi ws2_32 shell32 ole32)
target_compile_definitions(Nexus PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(Nexus PRIVATE /W4 /utf-8 /EHa) # /EHa : __try/__except autour des lectures memoire

# --- Le launcher (application graphique ImGui + DirectX 11) ---------------------
add_executable(NexusLauncher WIN32
    launcher/App.cpp
    launcher/Launcher.cpp
    launcher/Updater.cpp
    launcher/StbImageLauncher.cpp
    launcher/launcher.rc)
target_include_directories(NexusLauncher PRIVATE client third_party ${CMAKE_BINARY_DIR}/generated)
target_compile_definitions(NexusLauncher PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(NexusLauncher PRIVATE /utf-8 /EHsc)
target_link_libraries(NexusLauncher PRIVATE imgui
    advapi32 shell32 winhttp bcrypt
    d3d11 dxgi d3dcompiler dwmapi user32 gdi32 imm32)

# En dev : on copie offsets.ini a cote de la DLL s'il existe (edition a chaud, prioritaire sur l'integre).
# En release (compilation GitHub), offsets.ini vient d'un secret et reste integre dans la DLL, pas copie.
if(EXISTS "${OFFSETS_FILE}" AND NEXUS_VERSION MATCHES "-dev")
    add_custom_command(TARGET Nexus POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${OFFSETS_FILE} $<TARGET_FILE_DIR:Nexus>/offsets.ini)
endif()
add_custom_command(TARGET NexusLauncher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/assets/NexusTitle.mcpack $<TARGET_FILE_DIR:NexusLauncher>/NexusTitle.mcpack)
# Notes de version (panneau "Nouveautes" du launcher) + exemple de manifeste clients.
add_custom_command(TARGET NexusLauncher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/CHANGELOG.md $<TARGET_FILE_DIR:NexusLauncher>/CHANGELOG.md
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/launcher/clients.sample.ini $<TARGET_FILE_DIR:NexusLauncher>/clients.sample.ini)
