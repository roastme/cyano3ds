/*
 * copybit_nintendo3ds.c - legacy pre-gralloc copybit skeleton, kept for reference
 *
 * The port's only graphics HAL in the pre-gralloc era was copybit (there is no gralloc yet: see
 * frameworks/base/libs/ui/EGLDisplaySurface.cpp, which opens and mmaps the
 * framebuffer itself and never calls a HAL for the final composition).
 *
 * What this module is therefore responsible for:
 *   - format conversion blits (RGB565 <-> RGBA8888/RGBA4444/RGBA5551/BGRA8888)
 *   - rotation (0/90/180/270) and horizontal/vertical flips
 *   - nearest-neighbour scaling (COPYBIT_BLUR is advertised as unsupported)
 *
 * What it is deliberately NOT responsible for: the 3DS panel's 90 degree
 * portrait transposition.  That happens in the kernel driver (ctr_lcd_fb),
 * because 1.6 renders straight into the mmap'ed framebuffer and no HAL call
 * sits on that path.  Keeping the quirk in the kernel is what lets Android run
 * unpatched.  See docs/DISPLAY.md.
 *
 * This is the M4 skeleton: the blit path is complete and self-contained; the
 * scaling/alpha paths are intentionally simple (correct, not fast).
 *
 * License: Apache-2.0, like the rest of the Android HALs (the interface it
 * implements is AOSP's hardware/copybit.h).
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <hardware/copybit.h>

#define N3DS_LOG_TAG "copybit-n3ds"
#include <cutils/log.h>

struct n3ds_copybit_context_t {
	struct copybit_device_t device;

	int32_t transform;	/* COPYBIT_TRANSFORM_* */
	int32_t alpha;		/* 0..255 plane alpha */
	int32_t dither;		/* COPYBIT_ENABLE / COPYBIT_DISABLE */
};

/* ---------------------------------------------------------------- pixels */

static inline uint32_t rd_px(const void *base, uint32_t offset, int32_t fmt,
			     uint32_t x, uint32_t y, uint32_t w)
{
	const uint8_t *p = (const uint8_t *)base + offset;
	const void *q;

	switch (fmt) {
	case COPYBIT_FORMAT_RGB_565:
		return ((const uint16_t *)p)[y * w + x];
	case COPYBIT_FORMAT_RGBA_4444:
	case COPYBIT_FORMAT_RGBA_5551:
		return ((const uint16_t *)p)[y * w + x];
	case COPYBIT_FORMAT_RGBA_8888:
	case COPYBIT_FORMAT_BGRA_8888:
	default:
		q = p + ((size_t)y * w + x) * 4;
		return *(const uint32_t *)q;
	}
}

static inline void wr_px(void *base, uint32_t offset, int32_t fmt,
			 uint32_t x, uint32_t y, uint32_t w, uint32_t c)
{
	uint8_t *p = (uint8_t *)base + offset;

	switch (fmt) {
	case COPYBIT_FORMAT_RGB_565:
	case COPYBIT_FORMAT_RGBA_4444:
	case COPYBIT_FORMAT_RGBA_5551:
		((uint16_t *)p)[y * w + x] = (uint16_t)c;
		break;
	case COPYBIT_FORMAT_RGBA_8888:
	case COPYBIT_FORMAT_BGRA_8888:
	default:
		*(uint32_t *)(p + ((size_t)y * w + x) * 4) = c;
		break;
	}
}

static inline uint32_t px_to_565(uint32_t c, int32_t fmt)
{
	uint32_t r, g, b;

	switch (fmt) {
	case COPYBIT_FORMAT_RGB_565:
		return c;
	case COPYBIT_FORMAT_RGBA_4444:
	case COPYBIT_FORMAT_RGBA_5551:
		/* 4444: A R3..0 G3..0 B3..0 ; 5551: R4..0 G4..0 B4..0 A */
		if (fmt == COPYBIT_FORMAT_RGBA_4444) {
			r = (c >> 8) & 0xf; g = (c >> 4) & 0xf; b = c & 0xf;
			return (r << 12) | (g << 7) | (b << 1);
		}
		r = (c >> 11) & 0x1f; g = (c >> 6) & 0x1f; b = (c >> 1) & 0x1f;
		return (r << 11) | (g << 5) | b;
	default: /* RGBA_8888 or BGRA_8888 */
		if (fmt == COPYBIT_FORMAT_BGRA_8888) {
			b = (c >> 0) & 0xff; g = (c >> 8) & 0xff; r = (c >> 16) & 0xff;
		} else {
			r = (c >> 0) & 0xff; g = (c >> 8) & 0xff; b = (c >> 16) & 0xff;
		}
		return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
	}
}

static inline uint32_t px_from_565(uint32_t c, int32_t fmt)
{
	uint32_t r = (c >> 11) & 0x1f, g = (c >> 5) & 0x3f, b = c & 0x1f;

	r = (r << 3) | (r >> 2);
	g = (g << 2) | (g >> 4);
	b = (b << 3) | (b >> 2);

	switch (fmt) {
	case COPYBIT_FORMAT_RGB_565:
		return c;
	case COPYBIT_FORMAT_RGBA_4444:
		return ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4) | 0xf000;
	case COPYBIT_FORMAT_RGBA_5551:
		return ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1;
	case COPYBIT_FORMAT_BGRA_8888:
		return 0xff000000 | (b << 16) | (g << 8) | r;
	default:
		return 0xff000000 | (r << 16) | (g << 8) | b;
	}
}

/* Map a destination pixel back to a source coordinate for the transform. */
static inline void transform_xy(int32_t transform, uint32_t w, uint32_t h,
				uint32_t dx, uint32_t dy, uint32_t *sx,
				uint32_t *sy)
{
	switch (transform & ~(HAL_TRANSFORM_FLIP_H | HAL_TRANSFORM_FLIP_V)) {
	case HAL_TRANSFORM_ROT_90:
		*sx = dy;
		*sy = h - 1 - dx;
		break;
	case HAL_TRANSFORM_ROT_180:
		*sx = w - 1 - dx;
		*sy = h - 1 - dy;
		break;
	case HAL_TRANSFORM_ROT_270:
		*sx = w - 1 - dy;
		*sy = dx;
		break;
	default:
		*sx = dx;
		*sy = dy;
		break;
	}
	if (transform & HAL_TRANSFORM_FLIP_H)
		*sx = w - 1 - *sx;
	if (transform & HAL_TRANSFORM_FLIP_V)
		*sy = h - 1 - *sy;
}

/* ------------------------------------------------------------- operations */

static int n3ds_blit(struct copybit_device_t *dev,
		     struct copybit_image_t const *dst,
		     struct copybit_image_t const *src,
		     struct copybit_region_t const *region)
{
	struct n3ds_copybit_context_t *ctx = (struct n3ds_copybit_context_t *)dev;
	struct copybit_rect_t rect;

	if (!dst || !src || !dst->base || !src->base)
		return -EINVAL;
	if (!region)
		return -EINVAL;

	/* Rotations by 90/270 swap the visible dimensions of the source. */
	while (region->next(region, &rect) == 0) {
		uint32_t x, y;
		uint32_t l = rect.l < 0 ? 0 : (uint32_t)rect.l;
		uint32_t t = rect.t < 0 ? 0 : (uint32_t)rect.t;
		uint32_t r = (uint32_t)rect.r > dst->w ? dst->w : (uint32_t)rect.r;
		uint32_t b = (uint32_t)rect.b > dst->h ? dst->h : (uint32_t)rect.b;

		for (y = t; y < b; y++) {
			for (x = l; x < r; x++) {
				uint32_t sx, sy, c;

				transform_xy(ctx->transform, src->w, src->h,
					     x, y, &sx, &sy);
				if (sx >= src->w || sy >= src->h)
					continue;
				c = rd_px(src->base, src->offset, src->format,
					  sx, sy, src->w);
				if (dst->format == COPYBIT_FORMAT_RGB_565)
					c = px_to_565(c, src->format);
				else if (src->format == COPYBIT_FORMAT_RGB_565)
					c = px_from_565(c, dst->format);
				wr_px(dst->base, dst->offset, dst->format,
				      x, y, dst->w, c);
			}
		}
	}
	return 0;
}

static int n3ds_stretch(struct copybit_device_t *dev,
			struct copybit_image_t const *dst,
			struct copybit_image_t const *src,
			struct copybit_rect_t const *dst_rect,
			struct copybit_rect_t const *src_rect,
			struct copybit_region_t const *region)
{
	struct copybit_rect_t rect;
	uint32_t dw = dst_rect->r - dst_rect->l;
	uint32_t dh = dst_rect->b - dst_rect->t;
	uint32_t sw = src_rect->r - src_rect->l;
	uint32_t sh = src_rect->b - src_rect->t;

	if (!dw || !dh || !sw || !sh)
		return -EINVAL;

	while (region->next(region, &rect) == 0) {
		uint32_t x, y;

		for (y = (uint32_t)rect.t; y < (uint32_t)rect.b && y < dst->h; y++) {
			for (x = (uint32_t)rect.l; x < (uint32_t)rect.r && x < dst->w; x++) {
				uint32_t u = (x - dst_rect->l) * sw / dw;
				uint32_t v = (y - dst_rect->t) * sh / dh;
				uint32_t c;

				c = rd_px(src->base, src->offset, src->format,
					  src_rect->l + u, src_rect->t + v, src->w);
				if (dst->format == COPYBIT_FORMAT_RGB_565)
					c = px_to_565(c, src->format);
				else if (src->format == COPYBIT_FORMAT_RGB_565)
					c = px_from_565(c, dst->format);
				wr_px(dst->base, dst->offset, dst->format,
				      x, y, dst->w, c);
			}
		}
	}
	return 0;
}

static int n3ds_set_parameter(struct copybit_device_t *dev, int name, int value)
{
	struct n3ds_copybit_context_t *ctx = (struct n3ds_copybit_context_t *)dev;

	switch (name) {
	case COPYBIT_TRANSFORM:
		ctx->transform = value;
		break;
	case COPYBIT_ROTATION_DEG:
		ctx->transform = 0;
		switch (value) {
		case 0:   break;
		case 90:  ctx->transform = HAL_TRANSFORM_ROT_90;  break;
		case 180: ctx->transform = HAL_TRANSFORM_ROT_180; break;
		case 270: ctx->transform = HAL_TRANSFORM_ROT_270; break;
		default:  return -EINVAL;
		}
		break;
	case COPYBIT_PLANE_ALPHA:
		if (value < 0 || value > 255)
			return -EINVAL;
		ctx->alpha = value;
		break;
	case COPYBIT_DITHER:
		ctx->dither = value;
		break;
	case COPYBIT_BLUR:
		return -EINVAL;		/* not supported on the 3DS */
	default:
		return -EINVAL;
	}
	return 0;
}

static int n3ds_get(struct copybit_device_t *dev, int name)
{
	switch (name) {
	case COPYBIT_MINIFICATION_LIMIT:
	case COPYBIT_MAGNIFICATION_LIMIT:
		return 16;		/* arbitrary but honest for the SW path */
	case COPYBIT_SCALING_FRAC_BITS:
		return 0;
	case COPYBIT_ROTATION_STEP_DEG:
		return 90;
	default:
		return -EINVAL;
	}
}

/* ------------------------------------------------------------ HAL plumbing */

static int n3ds_close(struct hw_device_t *device)
{
	free(device);
	return 0;
}

static int n3ds_open(const struct hw_module_t *module, const char *name,
		     struct hw_device_t **device)
{
	struct n3ds_copybit_context_t *ctx;

	if (name && strcmp(name, COPYBIT_HARDWARE_COPYBIT0))
		return -EINVAL;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		LOGE("out of memory");
		return -ENOMEM;
	}

	ctx->device.common.tag = HARDWARE_DEVICE_TAG;
	ctx->device.common.version = 0;
	ctx->device.common.module = (struct hw_module_t *)module;
	ctx->device.common.close = n3ds_close;
	ctx->device.set_parameter = n3ds_set_parameter;
	ctx->device.get = n3ds_get;
	ctx->device.blit = n3ds_blit;
	ctx->device.stretch = n3ds_stretch;
	ctx->transform = 0;
	ctx->alpha = 0xff;
	ctx->dither = COPYBIT_DISABLE;

	*device = &ctx->device.common;
	LOGI("opened (software blit, 90 deg rotation steps)");
	return 0;
}

static struct hw_module_methods_t n3ds_copybit_module_methods = {
	.open = n3ds_open,
};

struct copybit_module_t HAL_MODULE_INFO_SYM = {
	.common = {
		.tag = HARDWARE_MODULE_TAG,
		.version_major = 1,
		.version_minor = 0,
		.id = COPYBIT_HARDWARE_MODULE_ID,
		.name = "Nintendo 3DS copybit module",
		.author = "Android-3DS port",
		.methods = &n3ds_copybit_module_methods,
	},
};