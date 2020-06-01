/*
 * Copyright (C) 2018 Rob Clark <robclark@freedesktop.org>
 * Copyright © 2018-2019 Google, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#include <stdio.h>

#include "freedreno_layout.h"

/* indexed by cpp, including msaa 2x and 4x:
 * TODO:
 * cpp=1 UBWC needs testing at larger texture sizes
 * missing UBWC blockwidth/blockheight for npot+64 cpp
 * missing 96/128 CPP for 8x MSAA with 32_32_32/32_32_32_32
 */
static const struct tile_alignment {
	unsigned basealign;
	unsigned pitchalign;
	unsigned heightalign;
	/* UBWC block width/height.  Used in size alignment, and calculating a
	 * descriptor's FLAG_BUFFER_LOG2W/H for mipmapping.
	 */
	uint8_t ubwc_blockwidth;
	uint8_t ubwc_blockheight;
} tile_alignment[] = {
	[1]  = {  64, 128, 32, 16, 4 },
	[2]  = { 128, 128, 16, 16, 4 },
	[3]  = { 256,  64, 32 },
	[4]  = { 256,  64, 16, 16, 4 },
	[6]  = { 256,  64, 16 },
	[8]  = { 256,  64, 16, 8, 4, },
	[12] = { 256,  64, 16 },
	[16] = { 256,  64, 16, 4, 4, },
	[24] = { 256,  64, 16 },
	[32] = { 256,  64, 16, 4, 2 },
	[48] = { 256,  64, 16 },
	[64] = { 256,  64, 16 },

	/* special cases for r8g8: */
	[0]  = { 256, 64, 32, 16, 8 },
};

#define RGB_TILE_WIDTH_ALIGNMENT 64
#define RGB_TILE_HEIGHT_ALIGNMENT 16
#define UBWC_PLANE_SIZE_ALIGNMENT 4096

static const struct tile_alignment *
fdl6_tile_alignment(struct fdl_layout *layout)
{
	debug_assert(layout->cpp < ARRAY_SIZE(tile_alignment));

	if ((layout->cpp == 2) && (util_format_get_nr_components(layout->format) == 2))
		return &tile_alignment[0];
	else
		return &tile_alignment[layout->cpp];
}

static int
fdl6_pitchalign(struct fdl_layout *layout, int level)
{
	uint32_t pitchalign = 64;
	if (fdl_tile_mode(layout, level))
		pitchalign = fdl6_tile_alignment(layout)->pitchalign;

	return pitchalign;
}

/* NOTE: good way to test this is:  (for example)
 *  piglit/bin/texelFetch fs sampler3D 100x100x8
 */
bool
fdl6_layout(struct fdl_layout *layout,
		enum pipe_format format, uint32_t nr_samples,
		uint32_t width0, uint32_t height0, uint32_t depth0,
		uint32_t mip_levels, uint32_t array_size, bool is_3d,
		struct fdl_slice *plane_layout)
{
	uint32_t offset;
	uint32_t pitch0;

	assert(nr_samples > 0);
	layout->width0 = width0;
	layout->height0 = height0;
	layout->depth0 = depth0;

	layout->cpp = util_format_get_blocksize(format);
	layout->cpp *= nr_samples;
	layout->cpp_shift = ffs(layout->cpp) - 1;

	layout->format = format;
	layout->nr_samples = nr_samples;
	layout->layer_first = !is_3d;

	if (depth0 > 1)
		layout->ubwc = false;
	if (tile_alignment[layout->cpp].ubwc_blockwidth == 0)
		layout->ubwc = false;

	const struct tile_alignment *ta = fdl6_tile_alignment(layout);

	/* in layer_first layout, the level (slice) contains just one
	 * layer (since in fact the layer contains the slices)
	 */
	uint32_t layers_in_level = layout->layer_first ? 1 : array_size;

	debug_assert(ta->pitchalign);

	if (layout->tile_mode) {
		layout->base_align = ta->basealign;
	} else {
		layout->base_align = 64;
	}

	if (plane_layout) {
		offset = plane_layout->offset;
		pitch0 = plane_layout->pitch;
		if (align(pitch0, fdl6_pitchalign(layout, 0) * layout->cpp) != pitch0)
			return false;
		pitch0 /= layout->cpp; /* explicit pitch is in bytes */
		if (pitch0 < width0 && height0 > 1)
			return false;
	} else {
		offset = 0;
		pitch0 = util_align_npot(width0, fdl6_pitchalign(layout, 0));
	}

	uint32_t ubwc_width0 = width0;
	uint32_t ubwc_height0 = height0;
	uint32_t ubwc_tile_height_alignment = RGB_TILE_HEIGHT_ALIGNMENT;
	if (mip_levels > 1) {
		/* With mipmapping enabled, UBWC layout is power-of-two sized,
		 * specified in log2 width/height in the descriptors.  The height
		 * alignment is 64 for mipmapping, but for buffer sharing (always
		 * single level) other participants expect 16.
		 */
		ubwc_width0 = util_next_power_of_two(width0);
		ubwc_height0 = util_next_power_of_two(height0);
		ubwc_tile_height_alignment = 64;
	}
	ubwc_width0 = align(DIV_ROUND_UP(ubwc_width0, ta->ubwc_blockwidth),
			RGB_TILE_WIDTH_ALIGNMENT);
	ubwc_height0 = align(DIV_ROUND_UP(ubwc_height0,
					ta->ubwc_blockheight),
			ubwc_tile_height_alignment);

	for (uint32_t level = 0; level < mip_levels; level++) {
		uint32_t depth = u_minify(depth0, level);
		struct fdl_slice *slice = &layout->slices[level];
		struct fdl_slice *ubwc_slice = &layout->ubwc_slices[level];
		uint32_t tile_mode = fdl_tile_mode(layout, level);
		uint32_t height;

		/* tiled levels of 3D textures are rounded up to PoT dimensions: */
		if (is_3d && tile_mode) {
			height = u_minify(util_next_power_of_two(height0), level);
		} else {
			height = u_minify(height0, level);
		}

		uint32_t nblocksy = util_format_get_nblocksy(format, height);
		if (tile_mode)
			nblocksy = align(nblocksy, ta->heightalign);

		/* The blits used for mem<->gmem work at a granularity of
		 * 16x4, which can cause faults due to over-fetch on the
		 * last level.  The simple solution is to over-allocate a
		 * bit the last level to ensure any over-fetch is harmless.
		 * The pitch is already sufficiently aligned, but height
		 * may not be. note this only matters if last level is linear
		 */
		if (level == mip_levels - 1)
			height = align(nblocksy, 4);

		uint32_t nblocksx =
			util_align_npot(util_format_get_nblocksx(format, u_minify(pitch0, level)),
					fdl6_pitchalign(layout, level));

		slice->offset = offset + layout->size;
		uint32_t blocks = nblocksx * nblocksy;

		slice->pitch = nblocksx * layout->cpp;

		/* 1d array and 2d array textures must all have the same layer size
		 * for each miplevel on a6xx. 3d textures can have different layer
		 * sizes for high levels, but the hw auto-sizer is buggy (or at least
		 * different than what this code does), so as soon as the layer size
		 * range gets into range, we stop reducing it.
		 */
		if (is_3d) {
			if (level < 1 || layout->slices[level - 1].size0 > 0xf000) {
				slice->size0 = align(blocks * layout->cpp, 4096);
			} else {
				slice->size0 = layout->slices[level - 1].size0;
			}
		} else {
			slice->size0 = blocks * layout->cpp;
		}

		layout->size += slice->size0 * depth * layers_in_level;

		if (layout->ubwc) {
			/* with UBWC every level is aligned to 4K */
			layout->size = align(layout->size, 4096);

			uint32_t meta_pitch = align(u_minify(ubwc_width0, level),
					RGB_TILE_WIDTH_ALIGNMENT);
			uint32_t meta_height = align(u_minify(ubwc_height0, level),
					ubwc_tile_height_alignment);

			ubwc_slice->size0 = align(meta_pitch * meta_height, UBWC_PLANE_SIZE_ALIGNMENT);
			ubwc_slice->pitch = meta_pitch;
			ubwc_slice->offset = offset + layout->ubwc_layer_size;
			layout->ubwc_layer_size += ubwc_slice->size0;
		}
	}

	if (layout->layer_first) {
		layout->layer_size = align(layout->size, 4096);
		layout->size = layout->layer_size * array_size;
	}

	/* Place the UBWC slices before the uncompressed slices, because the
	 * kernel expects UBWC to be at the start of the buffer.  In the HW, we
	 * get to program the UBWC and non-UBWC offset/strides
	 * independently.
	 */
	if (layout->ubwc) {
		for (uint32_t level = 0; level < mip_levels; level++)
			layout->slices[level].offset += layout->ubwc_layer_size * array_size;
		layout->size += layout->ubwc_layer_size * array_size;
	}

	/* include explicit offset in size */
	layout->size += offset;

	return true;
}

void
fdl6_get_ubwc_blockwidth(struct fdl_layout *layout,
		uint32_t *blockwidth, uint32_t *blockheight)
{
	const struct tile_alignment *ta = fdl6_tile_alignment(layout);
	*blockwidth = ta->ubwc_blockwidth;
	*blockheight = ta->ubwc_blockheight;
}
