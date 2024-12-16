#ifndef IMAGE_H_INLCUDED
#define IMAGE_H_INCLUDED

#ifndef STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_FAILURE_USERMSG
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS (1<<11)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include "stb_image.h"
#pragma GCC diagnostic pop
#endif // #ifndef

#ifndef STB_IMAGE_RESIZE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#include "stb_image_resize2.h"
#pragma GCC diagnostic pop
#endif // #ifndef


#include "result.h"

#include <cinttypes>


union Pixel {
	struct {
		uint8_t r, g, b, a; // Little endian
	} components;
	uint8_t block[4];
	uint32_t c; // 0xAABBGGRR
};
static_assert(sizeof(Pixel) == 4, "Unexpected size for Pixel union.");


struct Image {
	int w;
	int h;
	int channels;
	void* data;
};
static_assert(std::is_trivially_copyable_v<Image> == true, "struct Image is not trivially copyable");


static Result<Image> make_image(int width, int height, int channels, uint32_t color) {
	Image result;

	result.data = malloc(width * height * channels);
	if(result.data == NULL) {
		RETURN_ERROR_RESULT(ERROR_OUT_OF_MEMORY);
	}

	result.channels = channels;
	result.w = width;
	result.h = height;

	// TODO: Do I need to support 3 channel images here? Probably not...
	// TODO: This should be moved to it's own function
	if(channels == 4) {
		Pixel* ptr = (Pixel*)result.data;
		for(int i = 0; i < (width * height); ++i) {
			ptr[i].c = color;
		}
	} else
	if(channels == 1) {
		uint8_t* ptr = (uint8_t*)result.data;
		for(int i = 0; i < (width * height); ++i) {
			ptr[i] = (uint8_t)color;
		}
	}

	return {result};
}


static Result<Image> load_image(const char* file, int channels) {
	Image result;
	result.data = stbi_load(file, &result.w, &result.h, &result.channels, channels);
	if(result.data == NULL) {
		RETURN_ERROR_RESULT(ERROR_LOAD_ART_FAILED, file, stbi_failure_reason());
	}
	return {result};
}


static void image_max_alpha(Image* img) {
	if(img->channels == 4) {
		Pixel* ptr = (Pixel*)img->data;
		for(int i = 0; i < (img->w * img->h); ++i) {
			ptr[i].components.a = 0xFF;
		}
	} else
	if(img->channels == 1) {
		uint8_t* ptr = (uint8_t*)img->data;
		for(int i = 0; i < (img->w * img->h); ++i) {
			ptr[i] = (uint8_t)0xFF;
		}
	}
}


// blit src over dst at position x, y using (SourceColor*SourceAlpha)+(DestColor*(1-SourceAlpha)) as the blend function.
static void blit_RGBA_to_RGBA(const Image* src, const Image* dst, int x, int y) {
	assert(src->channels == 4);
	assert(dst->channels == 4);

	for(int row = 0; row < src->h; ++row) {
		int write_offset = (dst->w * y) + (dst->w * row) + x;
		uint32_t* write_ptr = (uint32_t*) ((Pixel*)dst->data + write_offset);

		int read_offset = src->w * row;
		uint32_t* read_ptr = (uint32_t*) ((Pixel*)src->data + read_offset);
		for(int col = 0; col < src->w; ++col) {
			Pixel src_pixel;
			src_pixel.c = *read_ptr++;
			const float src_r = (float)src_pixel.components.r / 255.0f;
			const float src_g = (float)src_pixel.components.g / 255.0f;
			const float src_b = (float)src_pixel.components.b / 255.0f;
			const float src_a = (float)src_pixel.components.a / 255.0f;

			Pixel dst_pixel;
			dst_pixel.c = *write_ptr;
			const float dst_r = (float)dst_pixel.components.r / 255.0f;
			const float dst_g = (float)dst_pixel.components.g / 255.0f;
			const float dst_b = (float)dst_pixel.components.b / 255.0f;
			const float dst_a = (float)dst_pixel.components.a / 255.0f;

			const float one_minus_source_alpha = (1.0f - src_a);
			Pixel out;
			out.components.r = 255 * ((src_r * src_a) + (dst_r * one_minus_source_alpha));
			out.components.g = 255 * ((src_g * src_a) + (dst_g * one_minus_source_alpha));
			out.components.b = 255 * ((src_b * src_a) + (dst_b * one_minus_source_alpha));
			out.components.a = 255 * ((src_a * src_a) + (dst_a * one_minus_source_alpha));

			*write_ptr++ = out.c;
		}
	}
}


static void blit_RGB_to_RGBA(const Image* src, const Image* dst, int x, int y) {
	assert(src->channels == 3);
	assert(dst->channels == 4);

	for(int row = 0; row < src->h; ++row) {
		int write_offset = (dst->w * y) + (dst->w * row) + x;
		uint32_t* write_ptr = (uint32_t*) ((Pixel*)dst->data + write_offset);

		const uint8_t* read_ptr = (uint8_t*) ((uint8_t*)src->data + (src->w * row * src->channels));
		for(int col = 0; col < src->w; ++col) {
			Pixel out;
			out.components.r = *read_ptr++;
			out.components.g = *read_ptr++;
			out.components.b = *read_ptr++;
			out.components.a = 255;
			*write_ptr++ = out.c;
		}
	}
}

static void blit_A8_to_RGBA(const Image* src, int stride, const Pixel color, const Image* dst, int x, int y) {
	assert(src->channels == 1);
	assert(dst->channels == 4);

	const float src_r = (float)color.components.r / 255.0f;
	const float src_g = (float)color.components.g / 255.0f;
	const float src_b = (float)color.components.b / 255.0f;

	for(int row = 0; row < src->h; ++row) {
		int write_offset = (dst->w * y) + (dst->w * row) + x; // FIXME: Should this be size_t?
		uint32_t* write_ptr = (uint32_t*) ((Pixel*)dst->data + write_offset);

		int read_offset = stride * row;
		uint8_t* read_ptr = (uint8_t*)((uint8_t*)src->data + read_offset);
		for(int col = 0; col < src->w; ++col) {
			const float src_a = (float)*read_ptr / 255.0f;
			read_ptr++;

			Pixel dst_pixel;
			dst_pixel.c = *write_ptr;
			const float dst_r = (float)dst_pixel.components.r / 255.0f;
			const float dst_g = (float)dst_pixel.components.g / 255.0f;
			const float dst_b = (float)dst_pixel.components.b / 255.0f;
			const float dst_a = (float)dst_pixel.components.a / 255.0f;

			const float one_minus_source_alpha = 1.0f - src_a;
			Pixel out;
			out.components.r = 255 * ((src_r * src_a) + (dst_r * one_minus_source_alpha));
			out.components.g = 255 * ((src_g * src_a) + (dst_g * one_minus_source_alpha));
			out.components.b = 255 * ((src_b * src_a) + (dst_b * one_minus_source_alpha));
			out.components.a = 255 * ((src_a * src_a) + (dst_a * one_minus_source_alpha));

			*write_ptr++ = out.c;
		}
	}
}

static void blit_A8_to_RGBA_no_alpha(const Image* src, int stride, const Pixel color, const Image* dst, int x, int y) {
	assert(src->channels == 1);
	assert(dst->channels == 4);

	const float src_r = (float)color.components.r / 255.0f;
	const float src_g = (float)color.components.g / 255.0f;
	const float src_b = (float)color.components.b / 255.0f;

	for(int row = 0; row < src->h; ++row) {
		int write_offset = (dst->w * y) + (dst->w * row) + x;
		uint32_t* write_ptr = (uint32_t*) ((Pixel*)dst->data + write_offset);

		int read_offset = stride * row;
		uint8_t* read_ptr = (uint8_t*)((uint8_t*)src->data + read_offset);
		for(int col = 0; col < src->w; ++col) {
			const float src_a = (float)*read_ptr / 255.0f;
			read_ptr++;

			Pixel out;
			out.components.r = 255 * ((src_r * src_a));
			out.components.g = 255 * ((src_g * src_a));
			out.components.b = 255 * ((src_b * src_a));
			out.components.a = 255 * ((src_a * src_a));

			*write_ptr++ = out.c;
		}
	}
}

static void blit_A8_to_A8(const Image* src, int stride, Image* dst, int x, int y) {
	assert(src->channels == 1);
	assert(dst->channels == 1);

	for(int row = 0; row < src->h; ++row) {
		int write_offset = (dst->w * y) + (dst->w * row) + x;
		uint8_t* write_ptr = (uint8_t*) ((uint8_t*)dst->data + write_offset);

		int read_offset = stride * row;
		uint8_t* read_ptr = (uint8_t*) ((uint8_t*)src->data + read_offset);
		for(int col = 0; col < src->w; ++col) {
			const float src_a = (float)*read_ptr / 255.0f;
			read_ptr++;

			const float dst_a = (float)(*write_ptr) / 255.0f;
			const float one_minus_source_alpha = 1.0f - src_a;
			uint8_t out = 255 * ((src_a * src_a) + (dst_a * one_minus_source_alpha));

			*write_ptr++ = out;
		}
	}
}


#endif // IMAGE_H_INCLUDED
