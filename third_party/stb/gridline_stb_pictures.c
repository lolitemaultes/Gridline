/* Gridline (J-184): see gridline_stb_pictures.h. The stb_image header is unmodified. */
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_ONLY_TGA
#define STBI_ONLY_PSD
#define STBI_ONLY_HDR
#define STBI_MAX_DIMENSIONS 32768
#include "stb_image.h"
#include "gridline_stb_pictures.h"

int gl_picture_info(const unsigned char* bytes, int len, int* w, int* h) {
  int comp = 0;
  return stbi_info_from_memory(bytes, len, w, h, &comp);
}

unsigned char* gl_picture_decode(const unsigned char* bytes, int len, int* w, int* h) {
  int comp = 0;
  return stbi_load_from_memory(bytes, len, w, h, &comp, 4);
}

void gl_picture_free(unsigned char* pixels) { stbi_image_free(pixels); }

const char* gl_picture_failure(void) { return stbi_failure_reason(); }
