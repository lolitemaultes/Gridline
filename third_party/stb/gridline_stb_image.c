/* Gridline (J-111): the stb_image / stb_image_write implementations, PNG and JPEG only,
   used to sanitise profile pictures (src/collab/Profile.cpp). Built without warnings
   (vendored code); the headers are unmodified. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 16384
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"
