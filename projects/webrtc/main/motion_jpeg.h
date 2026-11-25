#pragma once

// Use a private copy of the esp_jpeg decoder (tjpgd/, patched for fast 1/8
// decoding and a reduced inverse DCT at 1/2 and 1/4) on both targets. Motion
// analysis needs only luma: grayscale output skips the chroma blocks' inverse
// DCT and both colour conversions. The device's global ROM decoder choice remains
// available to other components.
#include "sdkconfig.h"
#undef CONFIG_JD_SZBUF
#undef CONFIG_JD_FORMAT
#undef CONFIG_JD_USE_SCALE
#undef CONFIG_JD_TBLCLIP
#undef CONFIG_JD_FASTDECODE
#undef CONFIG_JD_DEFAULT_HUFFMAN
#define CONFIG_JD_SZBUF 512
#define CONFIG_JD_FORMAT 2  // Grayscale (luma only).
#define CONFIG_JD_USE_SCALE 1
#define CONFIG_JD_TBLCLIP 1
#define CONFIG_JD_FASTDECODE 2  // Huffman lookup tables: about 6 KB more workspace.
#define esp_jpeg_jd_prepare motion_jd_prepare
#define esp_jpeg_jd_decomp motion_jd_decomp
#include "tjpgd/tjpgd.h"
