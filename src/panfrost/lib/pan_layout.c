/*
 * Copyright (C) 2019-2022 Collabora, Ltd.
 * Copyright (C) 2018-2019 Alyssa Rosenzweig
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
 */

#include "util/macros.h"
#include "util/u_math.h"
#include "pan_texture.h"

/* List of supported modifiers, in descending order of preference. AFBC is
 * faster than u-interleaved tiling which is faster than linear. Within AFBC,
 * enabling the YUV-like transform is typically a win where possible. */

uint64_t pan_best_modifiers[PAN_MODIFIER_COUNT] = {
        DRM_FORMAT_MOD_ARM_AFBC(
                AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 |
                AFBC_FORMAT_MOD_SPARSE |
                AFBC_FORMAT_MOD_YTR),

        DRM_FORMAT_MOD_ARM_AFBC(
                AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 |
                AFBC_FORMAT_MOD_SPARSE),

        DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED,
        DRM_FORMAT_MOD_LINEAR
};

/* Table of AFBC superblock sizes */
static const struct pan_block_size
afbc_superblock_sizes[] = {
        [AFBC_FORMAT_MOD_BLOCK_SIZE_16x16]      = { 16, 16 },
        [AFBC_FORMAT_MOD_BLOCK_SIZE_32x8]       = { 32,  8 },
        [AFBC_FORMAT_MOD_BLOCK_SIZE_64x4]       = { 64,  4 },
};

/*
 * Given an AFBC modifier, return the superblock size.
 *
 * We do not yet have any use cases for multiplanar YCBCr formats with different
 * superblock sizes on the luma and chroma planes. These formats are unsupported
 * for now.
 */
struct pan_block_size
panfrost_afbc_superblock_size(uint64_t modifier)
{
        unsigned index = (modifier & AFBC_FORMAT_MOD_BLOCK_SIZE_MASK);

        assert(drm_is_afbc(modifier));
        assert(index < ARRAY_SIZE(afbc_superblock_sizes));

        return afbc_superblock_sizes[index];
}

/*
 * Given an AFBC modifier, return the width of the superblock.
 */
unsigned
panfrost_afbc_superblock_width(uint64_t modifier)
{
        return panfrost_afbc_superblock_size(modifier).width;
}

/*
 * Given an AFBC modifier, return the height of the superblock.
 */
unsigned
panfrost_afbc_superblock_height(uint64_t modifier)
{
        return panfrost_afbc_superblock_size(modifier).height;
}

/* If not explicitly, line stride is calculated for block-based formats as
 * (ceil(width / block_width) * block_size). As a special case, this is left
 * zero if there is only a single block vertically. So, we have a helper to
 * extract the dimensions of a block-based format and use that to calculate the
 * line stride as such.
 */
static inline unsigned
panfrost_block_dim(uint64_t modifier, bool width)
{
        if (!drm_is_afbc(modifier)) {
                assert(modifier == DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED);
                return 16;
        }

        if (width)
                return panfrost_afbc_superblock_width(modifier);
        else
                return panfrost_afbc_superblock_height(modifier);
}

/* Computes sizes for checksumming, which is 8 bytes per 16x16 tile.
 * Checksumming is believed to be a CRC variant (CRC64 based on the size?).
 * This feature is also known as "transaction elimination". */

#define CHECKSUM_TILE_WIDTH 16
#define CHECKSUM_TILE_HEIGHT 16
#define CHECKSUM_BYTES_PER_TILE 8

unsigned
panfrost_compute_checksum_size(
        struct pan_image_slice_layout *slice,
        unsigned width,
        unsigned height)
{
        unsigned tile_count_x = DIV_ROUND_UP(width, CHECKSUM_TILE_WIDTH);
        unsigned tile_count_y = DIV_ROUND_UP(height, CHECKSUM_TILE_HEIGHT);

        slice->crc.stride = tile_count_x * CHECKSUM_BYTES_PER_TILE;

        return slice->crc.stride * tile_count_y;
}

unsigned
panfrost_get_layer_stride(const struct pan_image_layout *layout,
                          unsigned level)
{
        if (layout->dim != MALI_TEXTURE_DIMENSION_3D)
                return layout->array_stride;
        else if (drm_is_afbc(layout->modifier))
                return layout->slices[level].afbc.surface_stride;
        else
                return layout->slices[level].surface_stride;
}

/* Computes the offset into a texture at a particular level/face. Add to
 * the base address of a texture to get the address to that level/face */

unsigned
panfrost_texture_offset(const struct pan_image_layout *layout,
                        unsigned level, unsigned array_idx,
                        unsigned surface_idx)
{
        return layout->slices[level].offset +
               (array_idx * layout->array_stride) +
               (surface_idx * layout->slices[level].surface_stride);
}

bool
pan_image_layout_init(const struct panfrost_device *dev,
                      struct pan_image_layout *layout,
                      uint64_t modifier,
                      enum pipe_format format,
                      enum mali_texture_dimension dim,
                      unsigned width, unsigned height, unsigned depth,
                      unsigned array_size, unsigned nr_samples,
                      unsigned nr_slices, enum pan_image_crc_mode crc_mode,
                      const struct pan_image_explicit_layout *explicit_layout)
{
        /* Explicit stride only work with non-mipmap, non-array; single-sample
         * 2D image, and in-band CRC can't be used.
         */
        if (explicit_layout &&
	    (depth > 1 || nr_samples > 1 || array_size > 1 ||
             dim != MALI_TEXTURE_DIMENSION_2D || nr_slices > 1 ||
             crc_mode == PAN_IMAGE_CRC_INBAND))
                return false;

        /* Mandate 64 byte alignement */
        if (explicit_layout && (explicit_layout->offset & 63))
                return false;

        layout->crc_mode = crc_mode;
        layout->modifier = modifier;
        layout->format = format;
        layout->dim = dim;
        layout->width = width;
        layout->height = height;
        layout->depth = depth;
        layout->array_size = array_size;
        layout->nr_samples = nr_samples;
        layout->nr_slices = nr_slices;

        unsigned bytes_per_pixel = util_format_get_blocksize(format);

        /* MSAA is implemented as a 3D texture with z corresponding to the
         * sample #, horrifyingly enough */

        assert(depth == 1 || nr_samples == 1);

        bool afbc = drm_is_afbc(layout->modifier);
        bool tiled = layout->modifier == DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED;
        bool linear = layout->modifier == DRM_FORMAT_MOD_LINEAR;
        bool should_align = tiled || afbc;
        bool is_3d = layout->dim == MALI_TEXTURE_DIMENSION_3D;

        unsigned oob_crc_offset = 0;
        unsigned offset = explicit_layout ? explicit_layout->offset : 0;
        unsigned tile_h = 1, tile_w = 1, tile_shift = 0;

        if (tiled || afbc) {
                tile_w = panfrost_block_dim(layout->modifier, true);
                tile_h = panfrost_block_dim(layout->modifier, false);
                if (util_format_is_compressed(format))
                        tile_shift = 2;
        }

        for (unsigned l = 0; l < nr_slices; ++l) {
                struct pan_image_slice_layout *slice = &layout->slices[l];

                unsigned effective_width = width;
                unsigned effective_height = height;
                unsigned effective_depth = depth;

                if (should_align) {
                        effective_width = ALIGN_POT(effective_width, tile_w) >> tile_shift;
                        effective_height = ALIGN_POT(effective_height, tile_h) >> tile_shift;

                        /* We don't need to align depth */
                }

                /* Align levels to cache-line as a performance improvement for
                 * linear/tiled and as a requirement for AFBC */

                offset = ALIGN_POT(offset, 64);

                slice->offset = offset;

                /* Compute the would-be stride */
                unsigned stride = bytes_per_pixel * effective_width;

                if (explicit_layout) {
                        /* Make sure the explicit stride is valid */
                        if (explicit_layout->line_stride < stride)
                                return false;

                        stride = explicit_layout->line_stride;
                } else if (linear) {
                        /* Keep lines alignment on 64 byte for performance */
                        stride = ALIGN_POT(stride, 64);
                }

                slice->line_stride = stride;
                slice->row_stride = stride * (tile_h >> tile_shift);

                unsigned slice_one_size = slice->line_stride * effective_height;

                /* Compute AFBC sizes if necessary */
                if (afbc) {
                        slice->afbc.header_size =
                                panfrost_afbc_header_size(width, height);

                        /* Stride between two rows of AFBC headers */
                        slice->afbc.row_stride =
                                (effective_width / tile_w) *
                                AFBC_HEADER_BYTES_PER_TILE;

                        /* AFBC body size */
                        slice->afbc.body_size = slice_one_size;

                        /* 3D AFBC resources have all headers placed at the
                         * beginning instead of having them split per depth
                         * level
                         */
                        if (is_3d) {
                                slice->afbc.surface_stride =
                                        slice->afbc.header_size;
                                slice->afbc.header_size *= effective_depth;
                                slice->afbc.body_size *= effective_depth;
                                offset += slice->afbc.header_size;
                        } else {
                                slice_one_size += slice->afbc.header_size;
                                slice->afbc.surface_stride = slice_one_size;
                        }
                }

                unsigned slice_full_size =
                        slice_one_size * effective_depth * nr_samples;

                slice->surface_stride = slice_one_size;

                /* Compute AFBC sizes if necessary */

                offset += slice_full_size;
                slice->size = slice_full_size;

                /* Add a checksum region if necessary */
                if (crc_mode != PAN_IMAGE_CRC_NONE) {
                        slice->crc.size =
                                panfrost_compute_checksum_size(slice, width, height);

                        if (crc_mode == PAN_IMAGE_CRC_INBAND) {
                                slice->crc.offset = offset;
                                offset += slice->crc.size;
                                slice->size += slice->crc.size;
                        } else {
                                slice->crc.offset = oob_crc_offset;
                                oob_crc_offset += slice->crc.size;
                        }
                }

                width = u_minify(width, 1);
                height = u_minify(height, 1);
                depth = u_minify(depth, 1);
        }

        /* Arrays and cubemaps have the entire miptree duplicated */
        layout->array_stride = ALIGN_POT(offset, 64);
        if (explicit_layout)
                layout->data_size = offset;
        else
                layout->data_size = ALIGN_POT(layout->array_stride * array_size, 4096);
        layout->crc_size = oob_crc_offset;

        return true;
}

void
pan_iview_get_surface(const struct pan_image_view *iview,
                      unsigned level, unsigned layer, unsigned sample,
                      struct pan_surface *surf)
{
        level += iview->first_level;
        assert(level < iview->image->layout.nr_slices);

       layer += iview->first_layer;

        bool is_3d = iview->image->layout.dim == MALI_TEXTURE_DIMENSION_3D;
        const struct pan_image_slice_layout *slice = &iview->image->layout.slices[level];
        mali_ptr base = iview->image->data.bo->ptr.gpu + iview->image->data.offset;

        if (drm_is_afbc(iview->image->layout.modifier)) {
                assert(!sample);

                if (is_3d) {
                        ASSERTED unsigned depth = u_minify(iview->image->layout.depth, level);
                        assert(layer < depth);
                        surf->afbc.header = base + slice->offset +
                                           (layer * slice->afbc.surface_stride);
                        surf->afbc.body = base + slice->offset +
                                          slice->afbc.header_size +
                                          (slice->surface_stride * layer);
                } else {
                        assert(layer < iview->image->layout.array_size);
                        surf->afbc.header = base +
                                            panfrost_texture_offset(&iview->image->layout,
                                                                    level, layer, 0);
                        surf->afbc.body = surf->afbc.header + slice->afbc.header_size;
                }
        } else {
                unsigned array_idx = is_3d ? 0 : layer;
                unsigned surface_idx = is_3d ? layer : sample;

                surf->data = base +
                             panfrost_texture_offset(&iview->image->layout, level,
                                                     array_idx, surface_idx);
        }
}