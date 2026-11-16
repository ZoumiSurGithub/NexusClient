#pragma once
#include <imgui.h>
// Renvoie la texture ImGui de l'objet (id interne, ex. "diamond_helmet" ou
// "diamond helmet"), chargee depuis %APPDATA%\Nexus\icons\<id>.png a la demande.
// 0 si le fichier n'existe pas. A appeler sur le thread de rendu.
ImTextureID IconFor(const char* itemId);
