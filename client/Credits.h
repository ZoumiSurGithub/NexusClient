#pragma once
#include "version.h"   // genere par CMake

// Credits affiches dans le menu (Parametres -> A propos) et le launcher.
namespace Credits {
    constexpr const char* kVersion       = NEXUS_VERSION;
    constexpr const char* kCreatedBy     = "Claude (Anthropic)";   // code du client : launcher, DLL, menu, modules
    constexpr const char* kSignaturesBy  = "Zoumi";                 // signatures, offsets et tests en jeu
}
