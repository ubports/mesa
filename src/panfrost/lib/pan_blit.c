/*
 * Copyright (C) 2020 Collabora, Ltd.
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
 *   Alyssa Rosenzweig <alyssa.rosenzweig@collabora.com>
 */

#include <math.h>
#include <stdio.h>
#include "pan_encoder.h"
#include "pan_pool.h"
#include "pan_scoreboard.h"
#include "pan_texture.h"
#include "panfrost-quirks.h"
#include "../midgard/midgard_compile.h"
#include "compiler/nir/nir_builder.h"
#include "util/u_math.h"

/* On Midgard, the native blit infrastructure (via MFBD preloads) is broken or
 * missing in many cases. We instead use software paths as fallbacks to
 * implement blits, which are done as TILER jobs. No vertex shader is
 * necessary since we can supply screen-space coordinates directly.
 *
 * This is primarily designed as a fallback for preloads but could be extended
 * for other clears/blits if needed in the future. */

static void
panfrost_build_blit_shader(panfrost_program *program, unsigned gpu_id, gl_frag_result loc, nir_alu_type T, bool ms)
{
        bool is_colour = loc >= FRAG_RESULT_DATA0;

        nir_builder _b;
        nir_builder_init_simple_shader(&_b, NULL, MESA_SHADER_FRAGMENT, &midgard_nir_options);
        nir_builder *b = &_b;
        nir_shader *shader = b->shader;

        nir_variable *c_src = nir_variable_create(shader, nir_var_shader_in, glsl_vector_type(GLSL_TYPE_FLOAT, 2), "coord");
        nir_variable *c_out = nir_variable_create(shader, nir_var_shader_out, glsl_vector_type(
                                GLSL_TYPE_FLOAT, is_colour ? 4 : 1), "out");

        c_src->data.location = VARYING_SLOT_TEX0;
        c_out->data.location = loc;

        nir_ssa_def *coord = nir_load_var(b, c_src);

        nir_tex_instr *tex = nir_tex_instr_create(shader, ms ? 3 : 1);

        tex->dest_type = T;

        if (ms) {
                tex->src[0].src_type = nir_tex_src_coord;
                tex->src[0].src = nir_src_for_ssa(nir_f2i32(b, coord));
                tex->coord_components = 2;
 
                tex->src[1].src_type = nir_tex_src_ms_index;
                tex->src[1].src = nir_src_for_ssa(nir_load_sample_id(b));

                tex->src[2].src_type = nir_tex_src_lod;
                tex->src[2].src = nir_src_for_ssa(nir_imm_int(b, 0));
                tex->sampler_dim = GLSL_SAMPLER_DIM_MS;
                tex->op = nir_texop_txf_ms;
        } else {
                tex->op = nir_texop_tex;

                tex->src[0].src_type = nir_tex_src_coord;
                tex->src[0].src = nir_src_for_ssa(coord);
                tex->coord_components = 2;

                tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
        }

        nir_ssa_dest_init(&tex->instr, &tex->dest, 4, 32, NULL);
        nir_builder_instr_insert(b, &tex->instr);

        if (is_colour)
                nir_store_var(b, c_out, &tex->dest.ssa, 0xFF);
        else
                nir_store_var(b, c_out, nir_channel(b, &tex->dest.ssa, 0), 0xFF);

        midgard_compile_shader_nir(shader, program, false, 0, gpu_id, false, true);
        ralloc_free(shader);
}

/* Compile and upload all possible blit shaders ahead-of-time to reduce draw
 * time overhead. There's only ~30 of them at the moment, so this is fine */

void
panfrost_init_blit_shaders(struct panfrost_device *dev)
{
        static const struct {
                gl_frag_result loc;
                unsigned types;
        } shader_descs[] = {
                { FRAG_RESULT_DEPTH,   1 << PAN_BLIT_FLOAT },
                { FRAG_RESULT_STENCIL, 1 << PAN_BLIT_UINT },
                { FRAG_RESULT_DATA0,  ~0 },
                { FRAG_RESULT_DATA1,  ~0 },
                { FRAG_RESULT_DATA2,  ~0 },
                { FRAG_RESULT_DATA3,  ~0 },
                { FRAG_RESULT_DATA4,  ~0 },
                { FRAG_RESULT_DATA5,  ~0 },
                { FRAG_RESULT_DATA6,  ~0 },
                { FRAG_RESULT_DATA7,  ~0 }
        };

        nir_alu_type nir_types[PAN_BLIT_NUM_TYPES] = {
                nir_type_float,
                nir_type_uint,
                nir_type_int
        };

        /* Total size = # of shaders * bytes per shader. There are
         * shaders for each RT (so up to DATA7 -- overestimate is
         * okay) and up to NUM_TYPES variants of each, * 2 for multisampling
         * variants. These shaders are simple enough that they should be less
         * than 8 quadwords each (again, overestimate is fine). */

        unsigned offset = 0;
        unsigned total_size = (FRAG_RESULT_DATA7 * PAN_BLIT_NUM_TYPES)
                * (8 * 16) * 2;

        dev->blit_shaders.bo = panfrost_bo_create(dev, total_size, PAN_BO_EXECUTE);

        /* Don't bother generating multisampling variants if we don't actually
         * support multisampling */
        bool has_ms = !(dev->quirks & MIDGARD_SFBD);

        for (unsigned ms = 0; ms <= has_ms; ++ms) {
                for (unsigned i = 0; i < ARRAY_SIZE(shader_descs); ++i) {
                        unsigned loc = shader_descs[i].loc;

                        for (enum pan_blit_type T = 0; T < PAN_BLIT_NUM_TYPES; ++T) {
                                if (!(shader_descs[i].types & (1 << T)))
                                        continue;

                                panfrost_program program;
                                panfrost_build_blit_shader(&program, dev->gpu_id, loc,
                                                nir_types[T], ms);

                                assert(offset + program.compiled.size < total_size);
                                memcpy(dev->blit_shaders.bo->cpu + offset, program.compiled.data, program.compiled.size);

                                dev->blit_shaders.loads[loc][T][ms] = (dev->blit_shaders.bo->gpu + offset) | program.first_tag;
                                offset += ALIGN_POT(program.compiled.size, 64);
                                util_dynarray_fini(&program.compiled);
                        }
                }
        }
}

/* Add a shader-based load on Midgard (draw-time for GL). Shaders are
 * precached */

void
panfrost_load_midg(
                struct pan_pool *pool,
                struct pan_scoreboard *scoreboard,
                mali_ptr blend_shader,
                mali_ptr fbd,
                mali_ptr coordinates, unsigned vertex_count,
                struct pan_image *image,
                unsigned loc)
{
        bool srgb = util_format_is_srgb(image->format);
        unsigned width = u_minify(image->width0, image->first_level);
        unsigned height = u_minify(image->height0, image->first_level);

        struct panfrost_transfer viewport = panfrost_pool_alloc(pool, MALI_VIEWPORT_LENGTH);
        struct panfrost_transfer sampler = panfrost_pool_alloc(pool, MALI_MIDGARD_SAMPLER_LENGTH);
        struct panfrost_transfer varying = panfrost_pool_alloc(pool, MALI_ATTRIBUTE_LENGTH);
        struct panfrost_transfer varying_buffer  = panfrost_pool_alloc(pool, MALI_ATTRIBUTE_BUFFER_LENGTH);

        pan_pack(viewport.cpu, VIEWPORT, cfg) {
                cfg.scissor_maximum_x = width - 1; /* Inclusive */
                cfg.scissor_maximum_y = height - 1;
        }

        pan_pack(varying_buffer.cpu, ATTRIBUTE_BUFFER, cfg) {
                cfg.pointer = coordinates;
                cfg.stride = 4 * sizeof(float);
                cfg.size = cfg.stride * vertex_count;
        }

        pan_pack(varying.cpu, ATTRIBUTE, cfg) {
                cfg.buffer_index = 0;
                cfg.format = (MALI_CHANNEL_R << 0) | (MALI_CHANNEL_G << 3) | (MALI_RGBA32F << 12);
        }

        struct mali_blend_equation_packed eq;

        pan_pack(&eq, BLEND_EQUATION, cfg) {
                cfg.rgb_mode = 0x122;
                cfg.alpha_mode = 0x122;

                if (loc < FRAG_RESULT_DATA0)
                        cfg.color_mask = 0x0;
        }

        union midgard_blend replace = {
                .equation = eq
        };

        if (blend_shader)
                replace.shader = blend_shader;

        /* Determine the sampler type needed. Stencil is always sampled as
         * UINT. Pure (U)INT is always (U)INT. Everything else is FLOAT. */

        enum pan_blit_type T =
                (loc == FRAG_RESULT_STENCIL) ? PAN_BLIT_UINT :
                (util_format_is_pure_uint(image->format)) ? PAN_BLIT_UINT :
                (util_format_is_pure_sint(image->format)) ? PAN_BLIT_INT :
                PAN_BLIT_FLOAT;

        bool ms = image->nr_samples > 1;

        struct mali_midgard_properties_packed properties;

        struct panfrost_transfer shader_meta_t = panfrost_pool_alloc_aligned(
                pool, MALI_STATE_LENGTH + 8 * sizeof(struct midgard_blend_rt), 128);

        pan_pack(&properties, MIDGARD_PROPERTIES, cfg) {
                cfg.work_register_count = 4;
                cfg.early_z_enable = (loc >= FRAG_RESULT_DATA0);
                cfg.stencil_from_shader = (loc == FRAG_RESULT_STENCIL);
                cfg.depth_source = (loc == FRAG_RESULT_DEPTH) ?
                        MALI_DEPTH_SOURCE_SHADER :
                        MALI_DEPTH_SOURCE_FIXED_FUNCTION;
        }

        pan_pack(shader_meta_t.cpu, STATE, cfg) {
                cfg.shader.shader = pool->dev->blit_shaders.loads[loc][T][ms];
                cfg.shader.varying_count = 1;
                cfg.shader.texture_count = 1;
                cfg.shader.sampler_count = 1;

                cfg.properties = properties.opaque[0];

                cfg.multisample_misc.sample_mask = 0xFFFF;
                cfg.multisample_misc.multisample_enable = ms;
                cfg.multisample_misc.evaluate_per_sample = ms;
                cfg.multisample_misc.depth_write_mask = (loc == FRAG_RESULT_DEPTH);
                cfg.multisample_misc.depth_function = MALI_FUNC_ALWAYS;

                cfg.stencil_mask_misc.stencil_enable = (loc == FRAG_RESULT_STENCIL);
                cfg.stencil_mask_misc.stencil_mask_front = 0xFF;
                cfg.stencil_mask_misc.stencil_mask_back = 0xFF;
                cfg.stencil_mask_misc.unknown_1 = 0x7;

                cfg.stencil_front.compare_function = MALI_FUNC_ALWAYS;
                cfg.stencil_front.stencil_fail = MALI_STENCIL_OP_REPLACE;
                cfg.stencil_front.depth_fail = MALI_STENCIL_OP_REPLACE;
                cfg.stencil_front.depth_pass = MALI_STENCIL_OP_REPLACE;

                cfg.stencil_back = cfg.stencil_front;

                if (pool->dev->quirks & MIDGARD_SFBD) {
                        cfg.stencil_mask_misc.sfbd_write_enable = true;
                        cfg.stencil_mask_misc.sfbd_dither_disable = true;
                        cfg.stencil_mask_misc.sfbd_srgb = srgb;
                        cfg.multisample_misc.sfbd_blend_shader = blend_shader;
                        memcpy(&cfg.sfbd_blend, &replace, sizeof(replace));
                } else if (!(pool->dev->quirks & IS_BIFROST)) {
                        memcpy(&cfg.sfbd_blend, &blend_shader, sizeof(blend_shader));
                }

                assert(cfg.shader.shader);
        }

        /* Create the texture descriptor. We partially compute the base address
         * ourselves to account for layer, such that the texture descriptor
         * itself is for a 2D texture with array size 1 even for 3D/array
         * textures, removing the need to separately key the blit shaders for
         * 2D and 3D variants */

        struct panfrost_transfer texture_t = panfrost_pool_alloc_aligned(
                        pool, MALI_MIDGARD_TEXTURE_LENGTH + sizeof(mali_ptr) * 2 * MAX2(image->nr_samples, 1), 128);

        panfrost_new_texture(texture_t.cpu,
                        image->width0, image->height0,
                        MAX2(image->nr_samples, 1), 1,
                        image->format, MALI_TEXTURE_DIMENSION_2D,
                        image->modifier,
                        image->first_level, image->last_level,
                        0, 0,
                        image->nr_samples,
                        0,
                        (MALI_CHANNEL_R << 0) | (MALI_CHANNEL_G << 3) | (MALI_CHANNEL_B << 6) | (MALI_CHANNEL_A << 9),
                        image->bo->gpu + image->first_layer *
                                panfrost_get_layer_stride(image->slices,
                                        image->dim == MALI_TEXTURE_DIMENSION_3D,
                                        image->cubemap_stride, image->first_level),
                        image->slices);

        pan_pack(sampler.cpu, MIDGARD_SAMPLER, cfg)
                cfg.normalized_coordinates = false;

        for (unsigned i = 0; i < 8; ++i) {
                void *dest = shader_meta_t.cpu + MALI_STATE_LENGTH + sizeof(struct midgard_blend_rt) * i;

                if (loc == (FRAG_RESULT_DATA0 + i)) {
                        struct midgard_blend_rt blend_rt = {
                                .blend = replace,
                        };

                        unsigned flags = 0;
                        pan_pack(&flags, BLEND_FLAGS, cfg) {
                                cfg.dither_disable = true;
                                cfg.srgb = srgb;
                                cfg.midgard_blend_shader = blend_shader;
                        }
                        blend_rt.flags.opaque[0] = flags;

                        if (blend_shader)
                                blend_rt.blend.shader = blend_shader;

                        memcpy(dest, &blend_rt, sizeof(struct midgard_blend_rt));
                } else {
                        memset(dest, 0x0, sizeof(struct midgard_blend_rt));
                }
        }

        struct midgard_payload_vertex_tiler payload = {};
        struct mali_primitive_packed primitive;
        struct mali_draw_packed draw;
        struct mali_invocation_packed invocation;

        pan_pack(&draw, DRAW, cfg) {
                cfg.unknown_1 = 0x7;
                cfg.position = coordinates;
                cfg.textures = panfrost_pool_upload(pool, &texture_t.gpu, sizeof(texture_t.gpu));
                cfg.samplers = sampler.gpu;
                cfg.state = shader_meta_t.gpu;
                cfg.varying_buffers = varying_buffer.gpu;
                cfg.varyings = varying.gpu;
                cfg.viewport = viewport.gpu;
                cfg.shared = fbd;
        }

        pan_pack(&primitive, PRIMITIVE, cfg) {
                cfg.draw_mode = MALI_DRAW_MODE_TRIANGLES;
                cfg.index_count = vertex_count;
                cfg.unknown_3 = 6;
        }

        panfrost_pack_work_groups_compute(&invocation, 1, vertex_count, 1, 1, 1, 1, true);

        payload.prefix.primitive = primitive;
        memcpy(&payload.postfix, &draw, MALI_DRAW_LENGTH);
        payload.prefix.invocation = invocation;

        panfrost_new_job(pool, scoreboard, MALI_JOB_TYPE_TILER, false, 0, &payload, sizeof(payload), true);
}
