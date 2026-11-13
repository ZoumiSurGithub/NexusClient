#pragma once
#include <cstdint>
#include <string>

namespace SigScan {
    // Format : octets hexadecimaux separes par des espaces, "?" ou "??" = joker.
    //   "48 89 5C 24 ? 57 48 83 EC 20"
    // Prefixes optionnels :
    //   "call:E8 ? ? ? ? 48 8B D8"      -> la signature pointe sur un CALL rel32, renvoie la cible
    //   "ref:3:7:48 8B 0D ? ? ? ? ..."  -> lit un rel32 a +3, instruction de 7 octets (lea/mov rip)
    // Renvoie 0 si introuvable ou si la signature est vide.
    uintptr_t Find(const std::string& signature, const char* nameForLog);
}
