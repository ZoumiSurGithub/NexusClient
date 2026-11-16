// Implementation de stb_image pour le launcher (decodage des logos serveurs).
// Formats supportes : PNG, JPG, BMP, GIF, TGA, PSD, HDR, PIC, PNM. (Pas de WebP : stb_image
// ne gere pas ce format -> utiliser PNG ou JPG pour les logos.)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
