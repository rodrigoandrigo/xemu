/*
 * Copyright © Microsoft Corporation
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef DZN_NIR_H
#define DZN_NIR_H

#include <directx/dxgiformat.h>

#include "nir.h"

struct dzn_indirect_draw_rewrite_params {
   uint32_t draw_buf_stride;
};

struct dzn_indirect_draw_triangle_fan_rewrite_params {
   uint32_t draw_buf_stride;
   uint32_t triangle_fan_index_buf_stride;
   uint64_t triangle_fan_index_buf_start;
};

struct dzn_indirect_draw_triangle_fan_prim_restart_rewrite_params {
   uint32_t draw_buf_stride;
   uint32_t triangle_fan_index_buf_stride;
   uint64_t triangle_fan_index_buf_start;
   uint64_t exec_buf_start;
};

struct dzn_triangle_fan_rewrite_index_params {
   uint32_t first_index;
};

struct dzn_triangle_fan_prim_restart_rewrite_index_params {
   uint32_t first_index;
   uint32_t index_count;
};

struct dzn_indirect_triangle_fan_rewrite_index_exec_params {
   uint64_t new_index_buf;
   struct dzn_triangle_fan_rewrite_index_params params;
   struct {
      uint32_t x, y, z;
   } group_count;
};

struct dzn_indirect_triangle_fan_prim_restart_rewrite_index_exec_params {
   uint64_t new_index_buf;
   struct dzn_triangle_fan_prim_restart_rewrite_index_params params;
   uint64_t index_count_ptr;
   struct {
      uint32_t x, y, z;
   } group_count;
};

struct dzn_indirect_draw_type {
   union {
      struct {
         uint8_t indexed : 1;
         uint8_t indirect_count : 1;
         uint8_t draw_params : 1;
         uint8_t draw_id : 1;
         uint8_t triangle_fan : 1;
         uint8_t triangle_fan_primitive_restart : 1;
      };
      uint8_t value;
   };
};
#define DZN_NUM_INDIRECT_DRAW_TYPES (1 << 6)

nir_shader *
dzn_nir_indirect_draw_shader(struct dzn_indirect_draw_type type);

/* Convert a D3D12 stream-output filled-size counter (bytes) into the
 * non-indexed indirect draw used by the large-point replay pass. */
nir_shader *
dzn_nir_large_point_draw_args_shader(void);

nir_shader *
dzn_nir_triangle_fan_rewrite_index_shader(uint8_t old_index_size);

nir_shader *
dzn_nir_triangle_fan_prim_restart_rewrite_index_shader(uint8_t old_index_size, bool strip);

nir_shader *
dzn_nir_list_restart_rewrite_index_shader(uint8_t old_index_size, unsigned width);

enum dzn_blit_resolve_mode {
   dzn_blit_resolve_none,
   dzn_blit_resolve_average,
   dzn_blit_resolve_min,
   dzn_blit_resolve_max,
   dzn_blit_resolve_sample_zero,
};
struct dzn_nir_blit_info {
   union {
      struct {
         uint32_t src_samples : 6;
         uint32_t loc : 4;
         uint32_t out_type : 4;
         uint32_t sampler_dim : 4;
         uint32_t src_is_array : 1;
         uint32_t resolve_mode : 3;
         uint32_t stencil_fallback : 1;
         uint32_t bit_copy : 4;
         uint32_t padding : 5;
      };
      const uint32_t hash_key;
   };
};

enum dzn_blit_bit_copy {
   DZN_BLIT_COPY_NONE,
   DZN_BLIT_COPY_D32_TO_COLOR,
   DZN_BLIT_COPY_COLOR_TO_D32,
   DZN_BLIT_COPY_D16_TO_COLOR,
   DZN_BLIT_COPY_COLOR_TO_D16,
   DZN_BLIT_COPY_D24_TO_COLOR,
   DZN_BLIT_COPY_COLOR_TO_D24,
   DZN_BLIT_COPY_UINT,
   DZN_BLIT_CLEAR_INTEGER,
   DZN_BLIT_COPY_D32_FLOAT_TO_COLOR,
};

nir_shader *
dzn_nir_blit_vs(void);

nir_shader *
dzn_nir_blit_fs(const struct dzn_nir_blit_info *info);

struct dzn_nir_point_gs_info {
   unsigned cull_mode;
   bool front_ccw;
   bool depth_bias;
   bool depth_bias_dynamic;
   bool cull_dynamic;
   bool front_face_dynamic;
   bool depth_bias_enable_dynamic;
   bool negative_one_to_one;
   DXGI_FORMAT ds_fmt;
   /* Constant values */
   float constant_depth_bias;
   float slope_scaled_depth_bias;
   float depth_bias_clamp;
   /* Used for loading dynamic values */
   struct {
      uint32_t register_space;
      uint32_t base_shader_register;
   } runtime_data_cbv;
};

bool dzn_nir_lower_patch_vertices(nir_shader *nir, unsigned count);
bool dzn_nir_preserve_xfb_position(nir_shader *nir);
nir_shader *dzn_nir_provoking_vertex_gs(const nir_shader *previous,
                                     enum mesa_prim primitive, unsigned register_space,
                                     bool primitive_id);
bool dzn_nir_lower_last_provoking_vertex(nir_shader *nir);

nir_shader *
dzn_nir_polygon_point_mode_gs(const nir_shader *vs, struct dzn_nir_point_gs_info *info);

#endif
