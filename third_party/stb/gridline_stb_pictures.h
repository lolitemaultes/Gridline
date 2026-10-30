/* Gridline (J-184): picture decoding for Pixel Tone (PNG, JPEG, BMP, GIF first
   frame, TGA, PSD composite, HDR). A private stb_image instance (all of its
   functions static), so the profile-picture decoder (gridline_stb_image.c)
   keeps accepting PNG and JPEG only. */
#ifndef GRIDLINE_STB_PICTURES_H
#define GRIDLINE_STB_PICTURES_H
#ifdef __cplusplus
extern "C" {
#endif
/* Width / height of an encoded picture without decoding it; 0 = not a picture. */
int gl_picture_info(const unsigned char* bytes, int len, int* w, int* h);
/* Decoded RGBA (w * h * 4 bytes, top row first), NULL on failure; free with gl_picture_free. */
unsigned char* gl_picture_decode(const unsigned char* bytes, int len, int* w, int* h);
void gl_picture_free(unsigned char* pixels);
/* Why the last decode failed (stb's short reason). */
const char* gl_picture_failure(void);
#ifdef __cplusplus
}
#endif
#endif
