#ifndef FONT_H_INCLUDED
#define FONT_H_INCLUDED

#ifndef STB_TRUETYPE_IMPLEMENTATION
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#endif

#include "image.h"
#include "utf8.h"

struct Text_Dim {
	int w;
	int h;
};

static Text_Dim get_text_dimensions(stbtt_fontinfo* font, const int size, const u8* str) {
	f32 scale = stbtt_ScaleForPixelHeight(font, size);

	Text_Dim dim;

	int ascent, descent, linegap;
	stbtt_GetFontVMetrics(font, &ascent, &descent, &linegap);

	dim.h = ceil(scale * (ascent - descent));

	f32 xpos = 0.0f;
	u32 ch = 0;
	int index = 0;
	while((str[index] != 0) && ((ch = u8_nextchar(str, &index)) != 0)) {
		f32 x_shift = xpos - (f32) floor(xpos);
		int advance, lsb;
		stbtt_GetCodepointHMetrics(font, ch, &advance, &lsb);
		int x0, y0, x1, y1;
	  	stbtt_GetCodepointBitmapBoxSubpixel(font, ch, scale, scale, x_shift, 0, &x0, &y0, &x1, &y1);
		xpos += advance * scale;
		if(str[index+1] != 0 && isutf(str[index+1])) {
			int tmp = index;
			xpos += scale * stbtt_GetCodepointKernAdvance(font, ch, u8_nextchar(str, &tmp));
		}
	}

	dim.w = ceil(xpos);

	return dim;
}

static void render_text_to_image(stbtt_fontinfo* font, const u8* str, const int size, Image* canvas, int x, int y, const Pixel color) {
	f32 scale = stbtt_ScaleForPixelHeight(font, size);
	int ascent, descent, linegap;
	stbtt_GetFontVMetrics(font, &ascent, &descent, &linegap);
	int baseline = (int) ceil(((float)ascent * scale));

	static const int GLYPH_WIDTH_MAX = 400;
	static const int GLYPH_HEIGHT_MAX = 400;
	u8 bitmap_buffer[GLYPH_WIDTH_MAX * GLYPH_HEIGHT_MAX];

	Image bitmap;
	bitmap.data = (u8*)bitmap_buffer;
	bitmap.channels = 1;

	f32 xpos = (f32)x;
	int ch = 0;
	int index = 0;
	while((str[index]) != 0 && ((ch = u8_nextchar(str, &index)) != 0)) {
		f32 x_shift = xpos - (f32) floor(xpos);
		int advance, lsb;
		stbtt_GetCodepointHMetrics(font, ch, &advance, &lsb);
		int x0, y0, x1, y1;
	  	stbtt_GetCodepointBitmapBoxSubpixel(font, ch, scale, scale, x_shift, 0, &x0, &y0, &x1, &y1);
		bitmap.w = x1-x0;
		bitmap.h = y1-y0;
	  	stbtt_MakeCodepointBitmapSubpixel(font, (u8*)bitmap.data, bitmap.w, bitmap.h, GLYPH_WIDTH_MAX, scale, scale, x_shift, 0, ch);

		if(canvas->channels == 4) {
			blit_A8_to_RGBA(&bitmap, GLYPH_WIDTH_MAX, color, canvas, (int)xpos + x0, y + baseline + y0);
		} else
		if(canvas->channels == 1) {
			blit_A8_to_A8(&bitmap, GLYPH_WIDTH_MAX, canvas, (int)xpos + x0, y + baseline + y0);
		} else {
			log(LOG_LEVEL_ERROR, "Unsupported channel count {} in {}", canvas->channels, __FUNCTION__);
		}

		xpos += advance * scale;
		if(str[index+1] != 0 && isutf(str[index+1])) {
			int index_copy = index;
			xpos += scale * stbtt_GetCodepointKernAdvance(font, ch, u8_nextchar(str, &index_copy));
		}
	}
}



#endif // FONT_H_INCLUDED
