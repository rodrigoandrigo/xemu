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

#include "dzn_nir.h"

#define COBJMACROS
#include <unknwn.h>
#include <directx/d3d12.h>

#include "spirv_to_dxil.h"
#include "nir_to_dxil.h"
#include "nir_builder.h"
#include "nir_builtin_builder.h"
#include "nir_xfb_info.h"
#include "util/u_dynarray.h"
#include "dxil_nir.h"
#include "vk_nir_convert_ycbcr.h"

static nir_def *
dzn_nir_create_bo_desc(nir_builder *b,
                       nir_variable_mode mode,
                       uint32_t desc_set,
                       uint32_t binding,
                       const char *name,
                       unsigned access)
{
   struct glsl_struct_field field = {
      .type = mode == nir_var_mem_ubo ?
              glsl_array_type(glsl_uint_type(), 4096, 4) :
              glsl_uint_type(),
      .name = "dummy_int",
   };
   const struct glsl_type *dummy_type =
      glsl_struct_type(&field, 1, "dummy_type", false);

   nir_variable *var =
      nir_variable_create(b->shader, mode, dummy_type, name);
   var->data.descriptor_set = desc_set;
   var->data.binding = binding;
   var->data.access = access;

   assert(mode == nir_var_mem_ubo || mode == nir_var_mem_ssbo);
   if (mode == nir_var_mem_ubo)
      b->shader->info.num_ubos++;
   else
      b->shader->info.num_ssbos++;

   nir_descriptor_type desc_type =
      var->data.mode == nir_var_mem_ubo ?
      nir_descriptor_type_uniform_buffer :
      nir_descriptor_type_storage_buffer;
   nir_address_format addr_format = nir_address_format_32bit_index_offset;
   nir_def *index =
      nir_vulkan_resource_index(b,
                                nir_address_format_num_components(addr_format),
                                nir_address_format_bit_size(addr_format),
                                nir_imm_int(b, 0),
                                .desc_set = desc_set,
                                .binding = binding,
                                .desc_type = desc_type);

   nir_def *desc =
      nir_load_vulkan_descriptor(b,
                                 nir_address_format_num_components(addr_format),
                                 nir_address_format_bit_size(addr_format),
                                 index,
                                 .desc_type = desc_type);

   return nir_channel(b, desc, 0);
}

nir_shader *
dzn_nir_indirect_draw_shader(struct dzn_indirect_draw_type type)
{
   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_COMPUTE,
                                     dxil_get_base_nir_compiler_options(),
                                     "dzn_meta_indirect_%sdraw%s%s%s()",
                                     type.indexed ? "indexed_" : "",
                                     type.indirect_count ? "_count" : "",
                                     type.triangle_fan ? "_triangle_fan" : "",
                                     type.triangle_fan_primitive_restart ? "_primitive_restart" : "");

   nir_def *params_desc =
      dzn_nir_create_bo_desc(&b, nir_var_mem_ubo, 0, 0, "params", 0);
   nir_def *draw_buf_desc =
      dzn_nir_create_bo_desc(&b, nir_var_mem_ssbo, 0, 1, "draw_buf", ACCESS_NON_WRITEABLE);
   nir_def *exec_buf_desc =
      dzn_nir_create_bo_desc(&b, nir_var_mem_ssbo, 0, 2, "exec_buf", ACCESS_NON_READABLE);

   unsigned params_size = 0;
   if (type.triangle_fan)
      params_size = sizeof(struct dzn_indirect_draw_triangle_fan_rewrite_params);
   else
      params_size = sizeof(struct dzn_indirect_draw_rewrite_params);

   nir_def *params =
      nir_load_ubo(&b, params_size / 4, 32,
                   params_desc, nir_imm_int(&b, 0),
                   .align_mul = 4, .align_offset = 0, .range_base = 0, .range = ~0);

   uint32_t exec_stride_imm = 0;
   uint32_t draw_args_offset = 0;
   if (type.triangle_fan)
      exec_stride_imm += sizeof(D3D12_INDEX_BUFFER_VIEW);
   if (type.draw_params)
      exec_stride_imm += sizeof(uint32_t) * 2;
   if (type.draw_id)
      exec_stride_imm += sizeof(uint32_t);
   draw_args_offset = exec_stride_imm;
   exec_stride_imm += (type.indexed || type.triangle_fan) ?
      sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) : sizeof(D3D12_DRAW_ARGUMENTS);

   nir_def *draw_stride = nir_channel(&b, params, 0);
   nir_def *exec_stride = nir_imm_int(&b, exec_stride_imm);
   nir_def *index =
      nir_channel(&b, nir_load_global_invocation_id(&b, 32), 0);

   if (type.indirect_count) {
      nir_def *count_buf_desc =
         dzn_nir_create_bo_desc(&b, nir_var_mem_ssbo, 0, 3, "count_buf", ACCESS_NON_WRITEABLE);

      nir_def *draw_count =
         nir_load_ssbo(&b, 1, 32, count_buf_desc, nir_imm_int(&b, 0), .align_mul = 4);

      nir_push_if(&b, nir_ieq_imm(&b, index, 0));
      nir_store_ssbo(&b, draw_count, exec_buf_desc, nir_imm_int(&b, 0),
                    .write_mask = 0x1, .access = ACCESS_NON_READABLE,
                    .align_mul = 16);
      nir_pop_if(&b, NULL);

      nir_push_if(&b, nir_ult(&b, index, draw_count));
   }

   nir_def *draw_offset = nir_imul(&b, draw_stride, index);

   /* The first entry contains the indirect count */
   nir_def *exec_offset =
      type.indirect_count ?
      nir_imul(&b, exec_stride, nir_iadd_imm(&b, index, 1)) : 
      nir_imul(&b, exec_stride, index);

   nir_def *draw_info1 =
      nir_load_ssbo(&b, 4, 32, draw_buf_desc, draw_offset, .align_mul = 4);
   nir_def *draw_info2 =
      type.indexed ?
      nir_load_ssbo(&b, 1, 32, draw_buf_desc,
                    nir_iadd_imm(&b, draw_offset, 16), .align_mul = 4) :
      nir_imm_int(&b, 0);

   nir_def *first_vertex = nir_channel(&b, draw_info1, type.indexed ? 3 : 2);
   nir_def *base_instance =
      type.indexed ? draw_info2 : nir_channel(&b, draw_info1, 3);

   uint32_t exec_val_idx = 0;
   nir_def *exec_vals[8] = { NULL };
   if (type.draw_params) {
      exec_vals[exec_val_idx++] = first_vertex;
      exec_vals[exec_val_idx++] = base_instance;
   }
   if (type.draw_id)
      exec_vals[exec_val_idx++] = index;

   if (type.triangle_fan) {
      /* Patch {vertex,index}_count and first_index */
      nir_def *triangle_count =
         nir_usub_sat(&b, nir_channel(&b, draw_info1, 0), nir_imm_int(&b, 2));
      exec_vals[exec_val_idx++] = nir_imul_imm(&b, triangle_count, 3);
      exec_vals[exec_val_idx++] = nir_channel(&b, draw_info1, 1);
      exec_vals[exec_val_idx++] = nir_imm_int(&b, 0);
      exec_vals[exec_val_idx++] = first_vertex;
      exec_vals[exec_val_idx++] = base_instance;

      nir_def *triangle_fan_exec_buf_desc =
         dzn_nir_create_bo_desc(&b, nir_var_mem_ssbo, 0, 4,
                                "triangle_fan_exec_buf",
                                ACCESS_NON_READABLE);
      nir_def *triangle_fan_index_buf_stride = nir_channel(&b, params, 1);
      nir_def *triangle_fan_index_buf_addr_lo =
         nir_iadd(&b, nir_channel(&b, params, 2),
                  nir_imul(&b, triangle_fan_index_buf_stride, index));

      nir_def *triangle_fan_exec_vals[9] = { 0 };
      uint32_t triangle_fan_exec_param_count = 0;
      nir_def *addr_lo_overflow =
         nir_ult(&b, triangle_fan_index_buf_addr_lo, nir_channel(&b, params, 2));
      nir_def *triangle_fan_index_buf_addr_hi =
         nir_iadd(&b, nir_channel(&b, params, 3),
                  nir_bcsel(&b, addr_lo_overflow, nir_imm_int(&b, 1), nir_imm_int(&b, 0)));

      triangle_fan_exec_vals[triangle_fan_exec_param_count++] = triangle_fan_index_buf_addr_lo;
      triangle_fan_exec_vals[triangle_fan_exec_param_count++] = triangle_fan_index_buf_addr_hi;

      if (type.triangle_fan_primitive_restart) {
         triangle_fan_exec_vals[triangle_fan_exec_param_count++] = nir_channel(&b, draw_info1, 2);
         triangle_fan_exec_vals[triangle_fan_exec_param_count++] =
            nir_iadd(&b, nir_channel(&b, draw_info1, 2), nir_channel(&b, draw_info1, 0));
         uint32_t index_count_offset = draw_args_offset +
            offsetof(D3D12_DRAW_INDEXED_ARGUMENTS, IndexCountPerInstance);
         nir_def *exec_buf_start =
            nir_load_ubo(&b, 2, 32,
                         params_desc, nir_imm_int(&b, 16),
                         .align_mul = 4, .align_offset = 0, .range_base = 0, .range = ~0);
         nir_def *exec_buf_start_lo =
            nir_iadd(&b, nir_imm_int(&b, index_count_offset),
                     nir_iadd(&b, nir_channel(&b, exec_buf_start, 0),
                              nir_imul(&b, exec_stride, index)));
         addr_lo_overflow = nir_ult(&b, exec_buf_start_lo, nir_channel(&b, exec_buf_start, 0));
         nir_def *exec_buf_start_hi =
            nir_iadd(&b, nir_channel(&b, exec_buf_start, 1),
                     nir_bcsel(&b, addr_lo_overflow, nir_imm_int(&b, 1), nir_imm_int(&b, 0)));
         triangle_fan_exec_vals[triangle_fan_exec_param_count++] = exec_buf_start_lo;
         triangle_fan_exec_vals[triangle_fan_exec_param_count++] = exec_buf_start_hi;
         triangle_fan_exec_vals[triangle_fan_exec_param_count++] = nir_imm_int(&b, 1);
      } else {
         triangle_fan_exec_vals[triangle_fan_exec_param_count++] =
            type.indexed ? nir_channel(&b, draw_info1, 2) : nir_imm_int(&b, 0);
         triangle_fan_exec_vals[triangle_fan_exec_param_count++] =
            triangle_count;
      }
      triangle_fan_exec_vals[triangle_fan_exec_param_count++] = nir_imm_int(&b, 1);
      triangle_fan_exec_vals[triangle_fan_exec_param_count++] = nir_imm_int(&b, 1);

      unsigned rewrite_index_exec_params =
         type.triangle_fan_primitive_restart ?
         sizeof(struct dzn_indirect_triangle_fan_prim_restart_rewrite_index_exec_params) :
         sizeof(struct dzn_indirect_triangle_fan_rewrite_index_exec_params);
      nir_def *triangle_fan_exec_stride =
         nir_imm_int(&b, rewrite_index_exec_params);
      nir_def *triangle_fan_exec_offset =
         nir_imul(&b, triangle_fan_exec_stride, index);

      for (uint32_t i = 0; i < triangle_fan_exec_param_count; i += 4) {
         unsigned comps = MIN2(triangle_fan_exec_param_count - i, 4);
         uint32_t mask = (1 << comps) - 1;

         nir_store_ssbo(&b, nir_vec(&b, &triangle_fan_exec_vals[i], comps),
                        triangle_fan_exec_buf_desc,
                        nir_iadd_imm(&b, triangle_fan_exec_offset, i * 4),
                        .write_mask = mask, .access = ACCESS_NON_READABLE, .align_mul = 4);
      }

      nir_def *ibview_vals[] = {
         triangle_fan_index_buf_addr_lo,
         triangle_fan_index_buf_addr_hi,
         triangle_fan_index_buf_stride,
         nir_imm_int(&b, DXGI_FORMAT_R32_UINT),
      };

      nir_store_ssbo(&b, nir_vec(&b, ibview_vals, ARRAY_SIZE(ibview_vals)),
                     exec_buf_desc, exec_offset,
                     .write_mask = 0xf, .access = ACCESS_NON_READABLE, .align_mul = 16);
      exec_offset = nir_iadd_imm(&b, exec_offset, ARRAY_SIZE(ibview_vals) * 4);
   } else {
      exec_vals[exec_val_idx++] = nir_channel(&b, draw_info1, 0);
      exec_vals[exec_val_idx++] = nir_channel(&b, draw_info1, 1);
      exec_vals[exec_val_idx++] = nir_channel(&b, draw_info1, 2);
      exec_vals[exec_val_idx++] = nir_channel(&b, draw_info1, 3);
      if (type.indexed)
         exec_vals[exec_val_idx++] = draw_info2;
   }

   nir_store_ssbo(&b, nir_vec(&b, exec_vals, MIN2(exec_val_idx, 4)),
                  exec_buf_desc, exec_offset,
                  .write_mask = ((1 << exec_val_idx) - 1) & 0xf, .access = ACCESS_NON_READABLE, .align_mul = 16);
   if (exec_val_idx > 4) {
      nir_store_ssbo(&b, nir_vec(&b, &exec_vals[4], exec_val_idx - 4),
                     exec_buf_desc, nir_iadd_imm(&b, exec_offset, 16),
                     .write_mask = ((1 << (exec_val_idx - 4)) - 1) & 0xf, .access = ACCESS_NON_READABLE, .align_mul = 16);
   }

   if (type.indirect_count)
      nir_pop_if(&b, NULL);

   return b.shader;
}

nir_shader *
dzn_nir_large_point_draw_args_shader(void)
{
   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_COMPUTE,
                                     dxil_get_base_nir_compiler_options(),
                                     "dzn_large_point_draw_args");

   /* b0 contains the byte stride of one captured point.  t1 is the native
    * 64-bit SO filled-size counter and u2 receives D3D12_DRAW_ARGUMENTS.
    * The replay VS emits two triangles (six vertices) per captured point.
    */
   nir_def *params =
      dzn_nir_create_bo_desc(&b, nir_var_mem_ubo, 0, 0, "params", 0);
   nir_def *counter =
      dzn_nir_create_bo_desc(&b, nir_var_mem_ssbo, 0, 1, "so_counter",
                             ACCESS_NON_WRITEABLE);
   nir_def *args =
      dzn_nir_create_bo_desc(&b, nir_var_mem_ssbo, 0, 2, "draw_args",
                             ACCESS_NON_READABLE);
   nir_def *stride =
      nir_load_ubo(&b, 1, 32, params, nir_imm_int(&b, 0),
                   .align_mul = 4, .range_base = 0, .range = 4);
   nir_def *filled =
      nir_load_ssbo(&b, 2, 32, counter, nir_imm_int(&b, 0),
                    .align_mul = 8, .access = ACCESS_NON_WRITEABLE);

   /* SO buffers are bounded to 32-bit D3D12 view sizes.  Still reject a
    * malformed zero stride and clamp the quotient before producing indirect
    * arguments, rather than allowing a divide-by-zero or silent wrap. */
   nir_def *valid_stride = nir_ine_imm(&b, stride, 0);
   nir_def *safe_stride =
      nir_bcsel(&b, valid_stride, stride, nir_imm_int(&b, 1));
   nir_def *aligned = nir_ieq_imm(
      &b, nir_umod(&b, nir_channel(&b, filled, 0), safe_stride), 0);
   nir_def *valid = nir_iand(&b, valid_stride, aligned);
   nir_def *points =
      nir_bcsel(&b, valid,
                nir_udiv(&b, nir_channel(&b, filled, 0), safe_stride),
                nir_imm_int(&b, 0));
   nir_def *draw[4] = {
      nir_imm_int(&b, 6), points, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
   };
   nir_store_ssbo(&b, nir_vec(&b, draw, ARRAY_SIZE(draw)), args,
                  nir_imm_int(&b, 0), .write_mask = 0xf,
                  .access = ACCESS_NON_READABLE, .align_mul = 16);

   return b.shader;
}

nir_shader *
dzn_nir_list_restart_rewrite_index_shader(uint8_t old_index_size, unsigned width)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE,
      dxil_get_base_nir_compiler_options(), "dzn_list_restart_%u_%u", old_index_size, width);
   nir_def *params_desc = dzn_nir_create_bo_desc(&b, nir_var_mem_ubo, 0, 0, "params", 0);
   nir_def *dst = dzn_nir_create_bo_desc(&b, nir_var_mem_ssbo, 0, 1, "indices", ACCESS_NON_READABLE);
   nir_def *src = dzn_nir_create_bo_desc(&b, nir_var_mem_ssbo, 0, 2, "source", ACCESS_NON_WRITEABLE);
   nir_def *count_dst = dzn_nir_create_bo_desc(&b, nir_var_mem_ssbo, 0, 3, "count", ACCESS_NON_READABLE);
   nir_def *params = nir_load_ubo(&b, 2, 32, params_desc, nir_imm_int(&b, 0),
                                 .align_mul = 4, .range = ~0);
   nir_variable *cursor = nir_local_variable_create(b.impl, glsl_uint_type(), "cursor");
   nir_variable *pending = nir_local_variable_create(b.impl, glsl_uint_type(), "pending");
   nir_variable *written = nir_local_variable_create(b.impl, glsl_uint_type(), "written");
   nir_variable *primitive = nir_local_variable_create(b.impl,
      glsl_array_type(glsl_uint_type(), width, 0), "primitive");
   nir_store_var(&b, cursor, nir_channel(&b, params, 0), 1);
   nir_store_var(&b, pending, nir_imm_int(&b, 0), 1);
   nir_store_var(&b, written, nir_imm_int(&b, 0), 1);
   nir_push_loop(&b);
   nir_def *i = nir_load_var(&b, cursor);
   nir_break_if(&b, nir_uge(&b, i, nir_channel(&b, params, 1)));
   nir_def *offset = nir_imul_imm(&b, i, old_index_size);
   nir_def *value = nir_load_ssbo(&b, 1, 32, src, nir_iand_imm(&b, offset, ~3u), .align_mul = 4);
   if (old_index_size == 2)
      value = nir_iand_imm(&b, nir_ushr(&b, value,
         nir_imul_imm(&b, nir_iand_imm(&b, offset, 2), 8)), 0xffff);
   nir_store_var(&b, cursor, nir_iadd_imm(&b, i, 1), 1);
   nir_push_if(&b, nir_ieq_imm(&b, value, old_index_size == 2 ? 0xffff : 0xffffffff));
   /* Restart discards an incomplete list primitive, not just the marker. */
   nir_store_var(&b, pending, nir_imm_int(&b, 0), 1);
   nir_push_else(&b, NULL);
   nir_def *n = nir_load_var(&b, pending);
   nir_store_deref(&b, nir_build_deref_array(&b, nir_build_deref_var(&b, primitive), n), value, 1);
   n = nir_iadd_imm(&b, n, 1);
   nir_store_var(&b, pending, n, 1);
   nir_push_if(&b, nir_ieq_imm(&b, n, width));
   nir_def *base = nir_load_var(&b, written);
   for (unsigned v = 0; v < width; v++) {
      nir_def *index = nir_load_deref(&b, nir_build_deref_array_imm(&b,
         nir_build_deref_var(&b, primitive), v));
      nir_store_ssbo(&b, index, dst, nir_imul_imm(&b, nir_iadd_imm(&b, base, v), 4),
         .write_mask = 1, .access = ACCESS_NON_READABLE, .align_mul = 4);
   }
   nir_store_var(&b, written, nir_iadd_imm(&b, base, width), 1);
   nir_store_var(&b, pending, nir_imm_int(&b, 0), 1);
   nir_pop_if(&b, NULL);
   nir_pop_if(&b, NULL);
   nir_pop_loop(&b, NULL);
   nir_store_ssbo(&b, nir_load_var(&b, written), count_dst, nir_imm_int(&b, 0),
      .write_mask = 1, .access = ACCESS_NON_READABLE, .align_mul = 4);
   return b.shader;
}

static bool
dzn_lower_patch_vertices(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_patch_vertices_in)
      return false;
   b->cursor = nir_before_instr(&intr->instr);
   nir_def_replace(&intr->def, nir_imm_int(b, *(unsigned *)data));
   return true;
}

bool
dzn_nir_lower_patch_vertices(nir_shader *nir, unsigned count)
{
   if (nir->info.stage == MESA_SHADER_TESS_CTRL) {
      nir_foreach_shader_in_variable(var, nir) {
         if (!var->data.patch && glsl_type_is_array(var->type))
            var->type = glsl_array_type(glsl_get_array_element(var->type), count, 0);
      }
      nir_fixup_deref_types(nir);
   }
   return nir_shader_intrinsics_pass(nir, dzn_lower_patch_vertices,
      nir_metadata_control_flow, &count);
}

nir_shader *
dzn_nir_triangle_fan_prim_restart_rewrite_index_shader(uint8_t old_index_size, bool strip)
{
   assert(old_index_size == 2 || old_index_size == 4);

   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_COMPUTE,
                                     dxil_get_base_nir_compiler_options(),
                                     "dzn_meta_triangle_prim_rewrite_index(old_index_size=%d)",
                                     old_index_size);

   nir_def *params_desc =
      dzn_nir_create_bo_desc(&b, nir_var_mem_ubo, 0, 0, "params", 0);
   nir_def *new_index_buf_desc =
      dzn_nir_create_bo_desc(&b, nir_var_mem_ssbo, 0, 1,
                             "new_index_buf", ACCESS_NON_READABLE);
   nir_def *old_index_buf_desc =
      dzn_nir_create_bo_desc(&b, nir_var_mem_ssbo, 0, 2,
                             "old_index_buf", ACCESS_NON_WRITEABLE);
   nir_def *new_index_count_ptr_desc =
      dzn_nir_create_bo_desc(&b, nir_var_mem_ssbo, 0, 3,
                             "new_index_count_ptr", ACCESS_NON_READABLE);

   nir_def *params =
      nir_load_ubo(&b, sizeof(struct dzn_triangle_fan_prim_restart_rewrite_index_params) / 4, 32,
                   params_desc, nir_imm_int(&b, 0),
                   .align_mul = 4, .align_offset = 0, .range_base = 0, .range = ~0);

   nir_def *prim_restart_val =
      nir_imm_int(&b, old_index_size == 2 ? 0xffff : 0xffffffff);
   nir_variable *old_index_ptr_var =
      nir_local_variable_create(b.impl, glsl_uint_type(), "old_index_ptr_var");
   nir_def *old_index_ptr = nir_channel(&b, params, 0);
   nir_store_var(&b, old_index_ptr_var, old_index_ptr, 1);
   nir_variable *new_index_ptr_var =
      nir_local_variable_create(b.impl, glsl_uint_type(), "new_index_ptr_var");
   nir_store_var(&b, new_index_ptr_var, nir_imm_int(&b, 0), 1);
   nir_def *old_index_count = nir_channel(&b, params, 1);
   nir_variable *index0_var =
      nir_local_variable_create(b.impl, glsl_uint_type(), "index0_var");
   nir_store_var(&b, index0_var, prim_restart_val, 1);
   nir_variable *parity = nir_local_variable_create(b.impl, glsl_uint_type(), "strip_parity");
   nir_store_var(&b, parity, nir_imm_int(&b, 0), 1);

   /*
    * Filter out all primitive-restart magic values, and generate a triangle list
    * from the triangle fan definition.
    *
    * Basically:
    *
    * new_index_ptr = 0;
    * index0 = restart_prim_value; // 0xffff or 0xffffffff
    * for (old_index_ptr = firstIndex; old_index_ptr < indexCount;) {
    *    // If we have no starting-point we need at least 3 vertices,
    *    // otherwise we can do with two. If there's not enough vertices
    *    // to form a primitive, we just bail out.
    *    min_indices = index0 == restart_prim_value ? 3 : 2;
    *    if (old_index_ptr + min_indices > firstIndex + indexCount)
    *       break;
    *
    *    if (index0 == restart_prim_value) {
    *       // No starting point, skip all entries until we have a
    *       // non-primitive-restart value
    *       index0 = old_index_buf[old_index_ptr++];
    *       continue;
    *    }
    *
    *    // If at least one index contains the primitive-restart pattern,
         // ignore this triangle, and skip the unused entries
    *    if (old_index_buf[old_index_ptr + 1] == restart_prim_value) {
    *       old_index_ptr += 2;
    *       continue;
    *    }
    *    if (old_index_buf[old_index_ptr] == restart_prim_value) {
    *       old_index_ptr++;
    *       continue;
    *    }
    *
    *    // We have a valid primitive, queue it to the new index buffer
    *    new_index_buf[new_index_ptr++] = old_index_buf[old_index_ptr];
    *    new_index_buf[new_index_ptr++] = old_index_buf[old_index_ptr + 1];
    *    new_index_buf[new_index_ptr++] = index0;
    * }
    *
    * expressed in NIR, which admitedly is not super easy to grasp with.
    * TODO: Might be a good thing to use use the CL compiler we have and turn
    * those shaders into CL kernels.
    */
   nir_loop *loop = nir_push_loop(&b);
   nir_loop_add_continue_construct(loop);

   old_index_ptr = nir_load_var(&b, old_index_ptr_var);
   nir_def *index0 = nir_load_var(&b, index0_var);

   nir_def *read_index_count =
      nir_bcsel(&b, nir_ieq(&b, index0, prim_restart_val),
                nir_imm_int(&b, 3), nir_imm_int(&b, 2));
   nir_break_if(&b, nir_ult(&b, old_index_count, nir_iadd(&b, old_index_ptr, read_index_count)));

   nir_def *old_index_offset =
      nir_imul_imm(&b, old_index_ptr, old_index_size);

   nir_push_if(&b, nir_ieq(&b, index0, prim_restart_val));
   nir_def *index_val =
      nir_load_ssbo(&b, 1, 32, old_index_buf_desc,
                    old_index_size == 2 ? nir_iand_imm(&b, old_index_offset, ~3ULL) : old_index_offset,
                    .align_mul = 4);
   if (old_index_size == 2) {
     index_val = nir_bcsel(&b, nir_test_mask(&b, old_index_offset, 0x2),
                           nir_ushr_imm(&b, index_val, 16),
                           nir_iand_imm(&b, index_val, 0xffff));
   }

   nir_store_var(&b, index0_var, index_val, 1);
   if (strip)
      nir_store_var(&b, parity, nir_imm_int(&b, 0), 1);
   nir_store_var(&b, old_index_ptr_var, nir_iadd_imm(&b, old_index_ptr, 1), 1);
   nir_jump(&b, nir_jump_continue);
   nir_pop_if(&b, NULL);

   nir_def *index12 =
      nir_load_ssbo(&b, 2, 32, old_index_buf_desc,
                    old_index_size == 2 ? nir_iand_imm(&b, old_index_offset, ~3ULL) : old_index_offset,
                    .align_mul = 4);
   if (old_index_size == 2) {
      nir_def *indices[] = {
         nir_iand_imm(&b, nir_channel(&b, index12, 0), 0xffff),
         nir_ushr_imm(&b, nir_channel(&b, index12, 0), 16),
         nir_iand_imm(&b, nir_channel(&b, index12, 1), 0xffff),
      };

      index12 = nir_bcsel(&b, nir_test_mask(&b, old_index_offset, 0x2),
                          nir_vec2(&b, indices[1], indices[2]),
                          nir_vec2(&b, indices[0], indices[1]));
   }

   nir_push_if(&b, nir_ieq(&b, nir_channel(&b, index12, 1), prim_restart_val));
   nir_store_var(&b, old_index_ptr_var, nir_iadd_imm(&b, old_index_ptr, 2), 1);
   nir_store_var(&b, index0_var, prim_restart_val, 1);
   nir_jump(&b, nir_jump_continue);
   nir_push_else(&b, NULL);
   nir_store_var(&b, old_index_ptr_var, nir_iadd_imm(&b, old_index_ptr, 1), 1);
   nir_push_if(&b, nir_ieq(&b, nir_channel(&b, index12, 0), prim_restart_val));
   nir_store_var(&b, index0_var, prim_restart_val, 1);
   nir_jump(&b, nir_jump_continue);
   nir_push_else(&b, NULL);
   nir_def *new_indices =
      nir_vec3(&b, nir_channel(&b, index12, 0), nir_channel(&b, index12, 1), index0);
   if (strip) {
      nir_def *odd = nir_ine_imm(&b, nir_load_var(&b, parity), 0);
      nir_def *middle = nir_channel(&b, index12, 0);
      new_indices = nir_vec3(&b, nir_bcsel(&b, odd, middle, index0),
         nir_bcsel(&b, odd, index0, middle), nir_channel(&b, index12, 1));
      nir_store_var(&b, index0_var, middle, 1);
      nir_store_var(&b, parity, nir_ixor(&b, nir_load_var(&b, parity), nir_imm_int(&b, 1)), 1);
   }
   nir_def *new_index_ptr = nir_load_var(&b, new_index_ptr_var);
   nir_def *new_index_offset = nir_imul_imm(&b, new_index_ptr, sizeof(uint32_t));
   nir_store_ssbo(&b, new_indices, new_index_buf_desc,
                  new_index_offset,
                  .write_mask = 7, .access = ACCESS_NON_READABLE, .align_mul = 4);
   nir_store_var(&b, new_index_ptr_var, nir_iadd_imm(&b, new_index_ptr, 3), 1);
   nir_pop_if(&b, NULL);
   nir_pop_if(&b, NULL);
   nir_pop_loop(&b, NULL);

   nir_store_ssbo(&b, nir_load_var(&b, new_index_ptr_var),
                  new_index_count_ptr_desc, nir_imm_int(&b, 0),
                  .write_mask = 1, .access = ACCESS_NON_READABLE, .align_mul = 4);

   nir_lower_continue_constructs(b.shader);

   return b.shader;
}

nir_shader *
dzn_nir_triangle_fan_rewrite_index_shader(uint8_t old_index_size)
{
   assert(old_index_size == 0 || old_index_size == 1 || old_index_size == 2 || old_index_size == 4);

   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_COMPUTE,
                                     dxil_get_base_nir_compiler_options(),
                                     "dzn_meta_triangle_rewrite_index(old_index_size=%d)",
                                     old_index_size);

   nir_def *params_desc =
      dzn_nir_create_bo_desc(&b, nir_var_mem_ubo, 0, 0, "params", 0);
   nir_def *new_index_buf_desc =
      dzn_nir_create_bo_desc(&b, nir_var_mem_ssbo, 0, 1,
                             "new_index_buf", ACCESS_NON_READABLE);

   nir_def *old_index_buf_desc = NULL;
   if (old_index_size > 0) {
      old_index_buf_desc =
         dzn_nir_create_bo_desc(&b, nir_var_mem_ssbo, 0, 2,
                                "old_index_buf", ACCESS_NON_WRITEABLE);
   }

   nir_def *params =
      nir_load_ubo(&b, old_index_size == 1 ? 3 : sizeof(struct dzn_triangle_fan_rewrite_index_params) / 4, 32,
                   params_desc, nir_imm_int(&b, 0),
                   .align_mul = 4, .align_offset = 0, .range_base = 0, .range = ~0);

   nir_def *triangle = nir_channel(&b, nir_load_global_invocation_id(&b, 32), 0);
   nir_def *new_indices;

   if (old_index_size == 1) {
      nir_def *gid = nir_load_global_invocation_id(&b, 32);
      nir_def *index = nir_iadd(&b, nir_channel(&b, gid, 0),
                               nir_imul_imm(&b, nir_channel(&b, gid, 1), 65535));
      nir_push_if(&b, nir_ult(&b, index, nir_channel(&b, params, 1)));
      nir_def *offset = nir_iadd(&b, index, nir_channel(&b, params, 0));
      nir_def *word = nir_load_ssbo(&b, 1, 32, old_index_buf_desc,
                                    nir_iand_imm(&b, offset, ~3u), .align_mul = 4);
      nir_def *value = nir_iand_imm(&b, nir_ushr(&b, word,
         nir_imul_imm(&b, nir_iand_imm(&b, offset, 3), 8)), 0xff);
      nir_def *restart = nir_iand(&b, nir_ine_imm(&b, nir_channel(&b, params, 2), 0),
                                  nir_ieq_imm(&b, value, 0xff));
      value = nir_bcsel(&b, restart, nir_imm_int(&b, ~0u), value);
      nir_store_ssbo(&b, value, new_index_buf_desc, nir_imul_imm(&b, index, 4),
                     .write_mask = 1, .access = ACCESS_NON_READABLE, .align_mul = 4);
      nir_pop_if(&b, NULL);
      return b.shader;
   }

   if (old_index_size > 0) {
      nir_def *old_first_index = nir_channel(&b, params, 0);
      nir_def *old_index0_offset =
         nir_imul_imm(&b, old_first_index, old_index_size);
      nir_def *old_index1_offset =
         nir_imul_imm(&b, nir_iadd(&b, nir_iadd_imm(&b, triangle, 1), old_first_index),
                      old_index_size);

      nir_def *old_index0 =
         nir_load_ssbo(&b, 1, 32, old_index_buf_desc,
                       old_index_size == 2 ? nir_iand_imm(&b, old_index0_offset, ~3ULL) : old_index0_offset,
                       .align_mul = 4);

      if (old_index_size == 2) {
        old_index0 = nir_bcsel(&b, nir_test_mask(&b, old_index0_offset, 0x2),
                               nir_ushr_imm(&b, old_index0, 16),
                               nir_iand_imm(&b, old_index0, 0xffff));
      }

      nir_def *old_index12 =
         nir_load_ssbo(&b, 2, 32, old_index_buf_desc,
                       old_index_size == 2 ? nir_iand_imm(&b, old_index1_offset, ~3ULL) : old_index1_offset,
                       .align_mul = 4);
      if (old_index_size == 2) {
         nir_def *indices[] = {
            nir_iand_imm(&b, nir_channel(&b, old_index12, 0), 0xffff),
            nir_ushr_imm(&b, nir_channel(&b, old_index12, 0), 16),
            nir_iand_imm(&b, nir_channel(&b, old_index12, 1), 0xffff),
         };

         old_index12 = nir_bcsel(&b, nir_test_mask(&b, old_index1_offset, 0x2),
                                 nir_vec2(&b, indices[1], indices[2]),
                                 nir_vec2(&b, indices[0], indices[1]));
      }

      /* TODO: VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT */
      new_indices =
         nir_vec3(&b, nir_channel(&b, old_index12, 0),
                  nir_channel(&b, old_index12, 1), old_index0);
   } else {
      new_indices =
         nir_vec3(&b,
                  nir_iadd_imm(&b, triangle, 1),
                  nir_iadd_imm(&b, triangle, 2),
                  nir_imm_int(&b, 0));
   }

   nir_def *new_index_offset =
      nir_imul_imm(&b, triangle, 4 * 3);

   nir_store_ssbo(&b, new_indices, new_index_buf_desc,
                  new_index_offset,
                  .write_mask = 7, .access = ACCESS_NON_READABLE, .align_mul = 4);

   return b.shader;
}

nir_shader *
dzn_nir_blit_vs(void)
{
   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_VERTEX,
                                     dxil_get_base_nir_compiler_options(),
                                     "dzn_meta_blit_vs()");

   nir_def *params_desc =
      dzn_nir_create_bo_desc(&b, nir_var_mem_ubo, 0, 0, "params", 0);

   nir_variable *out_pos =
      nir_variable_create(b.shader, nir_var_shader_out, glsl_vec4_type(),
                          "gl_Position");
   out_pos->data.location = VARYING_SLOT_POS;
   out_pos->data.driver_location = 0;

   nir_variable *out_coords =
      nir_variable_create(b.shader, nir_var_shader_out, glsl_vec_type(3),
                          "coords");
   out_coords->data.location = VARYING_SLOT_TEX0;
   out_coords->data.driver_location = 1;

   nir_def *vertex = nir_load_vertex_id(&b);
   nir_def *coords_arr[4] = {
      nir_load_ubo(&b, 4, 32, params_desc, nir_imm_int(&b, 0),
                   .align_mul = 16, .align_offset = 0, .range_base = 0, .range = ~0),
      nir_load_ubo(&b, 4, 32, params_desc, nir_imm_int(&b, 16),
                   .align_mul = 16, .align_offset = 0, .range_base = 0, .range = ~0),
      nir_load_ubo(&b, 4, 32, params_desc, nir_imm_int(&b, 32),
                   .align_mul = 16, .align_offset = 0, .range_base = 0, .range = ~0),
      nir_load_ubo(&b, 4, 32, params_desc, nir_imm_int(&b, 48),
                   .align_mul = 16, .align_offset = 0, .range_base = 0, .range = ~0),
   };
   nir_def *coords =
      nir_bcsel(&b, nir_ieq_imm(&b, vertex, 0), coords_arr[0],
                nir_bcsel(&b, nir_ieq_imm(&b, vertex, 1), coords_arr[1],
                          nir_bcsel(&b, nir_ieq_imm(&b, vertex, 2), coords_arr[2], coords_arr[3])));
   nir_def *pos =
      nir_vec4(&b, nir_channel(&b, coords, 0), nir_channel(&b, coords, 1),
               nir_imm_float(&b, 0.0), nir_imm_float(&b, 1.0));
   nir_def *z_coord =
      nir_load_ubo(&b, 1, 32, params_desc, nir_imm_int(&b, 4 * 4 * sizeof(float)),
                   .align_mul = 64, .align_offset = 0, .range_base = 0, .range = ~0);
   coords = nir_vec3(&b, nir_channel(&b, coords, 2), nir_channel(&b, coords, 3), z_coord);

   nir_store_var(&b, out_pos, pos, 0xf);
   nir_store_var(&b, out_coords, coords, 0x7);
   return b.shader;
}

nir_shader *
dzn_nir_blit_fs(const struct dzn_nir_blit_info *info)
{
   if (info->bit_copy == DZN_BLIT_CLEAR_INTEGER) {
      nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
         dxil_get_base_nir_compiler_options(), "dzn_meta_clear_integer_fs()");
      nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
         glsl_vector_type(info->out_type, 4), "clear");
      out->data.location = FRAG_RESULT_DATA0;
      nir_def *desc = dzn_nir_create_bo_desc(&b, nir_var_mem_ubo, 0, 0, "clear", 0);
      nir_def *value = nir_load_ubo(&b, 4, 32, desc, nir_imm_int(&b, 0),
         .align_mul = 16, .range_base = 0, .range = 16);
      nir_store_var(&b, out, value, 0xf);
      return b.shader;
   }
   bool ms = info->src_samples > 1;
   enum glsl_base_type in_type = info->out_type;
   if (info->bit_copy)
      in_type = info->bit_copy == DZN_BLIT_COPY_D32_FLOAT_TO_COLOR ||
                info->bit_copy == DZN_BLIT_COPY_D16_TO_COLOR ||
                info->bit_copy == DZN_BLIT_COPY_D24_TO_COLOR ? GLSL_TYPE_FLOAT : GLSL_TYPE_UINT;
   nir_alu_type nir_out_type =
      nir_get_nir_type_for_glsl_base_type(in_type);
   uint32_t coord_comps =
      glsl_get_sampler_dim_coordinate_components(info->sampler_dim) +
      info->src_is_array;

   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
                                     dxil_get_base_nir_compiler_options(),
                                     "dzn_meta_blit_fs()");

   const struct glsl_type *tex_type =
      glsl_texture_type(info->sampler_dim, info->src_is_array, in_type);
   nir_variable *tex_var =
      nir_variable_create(b.shader, nir_var_uniform, tex_type, "texture");
   nir_deref_instr *tex_deref = nir_build_deref_var(&b, tex_var);

   nir_variable *pos_var =
      nir_variable_create(b.shader, nir_var_shader_in,
                          glsl_vector_type(GLSL_TYPE_FLOAT, 4),
                          "gl_FragCoord");
   pos_var->data.location = VARYING_SLOT_POS;
   pos_var->data.driver_location = 0;

   nir_variable *coord_var =
      nir_variable_create(b.shader, nir_var_shader_in,
                          glsl_vector_type(GLSL_TYPE_FLOAT, 3),
                          "coord");
   coord_var->data.location = VARYING_SLOT_TEX0;
   coord_var->data.driver_location = 1;
   nir_def *coord =
      nir_trim_vector(&b, nir_load_var(&b, coord_var), coord_comps);

   uint32_t out_comps =
      (info->loc == FRAG_RESULT_DEPTH || info->loc == FRAG_RESULT_STENCIL) ? 1 : 4;
   nir_variable *out = NULL;
   if (!info->stencil_fallback) {
      out = nir_variable_create(b.shader, nir_var_shader_out,
                                glsl_vector_type(info->out_type, out_comps),
                                "out");
      out->data.location = info->loc;
   }

   nir_def *res = NULL;

   if (info->resolve_mode != dzn_blit_resolve_none) {
      enum dzn_blit_resolve_mode resolve_mode = info->resolve_mode;

      nir_op resolve_op = nir_op_mov;
      switch (resolve_mode) {
      case dzn_blit_resolve_average:
         /* When resolving a float type, we need to calculate the average of all
          * samples. For integer resolve, Vulkan says that one sample should be
          * chosen without telling which. Let's just pick the first one in that
          * case.
          */
         if (info->out_type == GLSL_TYPE_FLOAT)
            resolve_op = nir_op_fadd;
         else
            resolve_mode = dzn_blit_resolve_sample_zero;
         break;
      case dzn_blit_resolve_min:
         switch (info->out_type) {
         case GLSL_TYPE_FLOAT: resolve_op = nir_op_fmin; break;
         case GLSL_TYPE_INT: resolve_op = nir_op_imin; break;
         case GLSL_TYPE_UINT: resolve_op = nir_op_umin; break;
         }
         break;
      case dzn_blit_resolve_max:
         switch (info->out_type) {
         case GLSL_TYPE_FLOAT: resolve_op = nir_op_fmax; break;
         case GLSL_TYPE_INT: resolve_op = nir_op_imax; break;
         case GLSL_TYPE_UINT: resolve_op = nir_op_umax; break;
         }
         break;
      case dzn_blit_resolve_none:
      case dzn_blit_resolve_sample_zero:
         break;
      }

      unsigned nsamples = resolve_mode == dzn_blit_resolve_sample_zero ?
                          1 : info->src_samples;
      for (unsigned s = 0; s < nsamples; s++) {
         nir_tex_instr *tex = nir_tex_instr_create(b.shader, 4);

         tex->op = nir_texop_txf_ms;
         tex->dest_type = nir_out_type;
         tex->texture_index = 0;
         tex->is_array = info->src_is_array;
         tex->sampler_dim = info->sampler_dim;

         tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord,
                                           nir_f2i32(&b, coord));
         tex->coord_components = coord_comps;

         tex->src[1] = nir_tex_src_for_ssa(nir_tex_src_ms_index,
                                           nir_imm_int(&b, s));

         tex->src[2] = nir_tex_src_for_ssa(nir_tex_src_lod,
                                           nir_imm_int(&b, 0));

         tex->src[3] = nir_tex_src_for_ssa(nir_tex_src_texture_deref,
                                           &tex_deref->def);

         nir_def_init(&tex->instr, &tex->def, 4, 32);

         nir_builder_instr_insert(&b, &tex->instr);
         res = res ? nir_build_alu2(&b, resolve_op, res, &tex->def) : &tex->def;
      }

      if (resolve_mode == dzn_blit_resolve_average)
         res = nir_fmul_imm(&b, res, 1.0f / nsamples);
   } else {
      nir_tex_instr *tex =
         nir_tex_instr_create(b.shader, ms ? 4 : 3);

      tex->dest_type = nir_out_type;
      tex->is_array = info->src_is_array;
      tex->sampler_dim = info->sampler_dim;

      if (ms) {
         tex->op = nir_texop_txf_ms;

         tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord,
                                           nir_f2i32(&b, coord));
         tex->coord_components = coord_comps;

         tex->src[1] = nir_tex_src_for_ssa(nir_tex_src_ms_index,
                                           nir_load_sample_id(&b));

         tex->src[2] = nir_tex_src_for_ssa(nir_tex_src_lod,
                                           nir_imm_int(&b, 0));

         tex->src[3] = nir_tex_src_for_ssa(nir_tex_src_texture_deref,
                                           &tex_deref->def);
      } else {
         nir_variable *sampler_var =
            nir_variable_create(b.shader, nir_var_uniform, glsl_bare_sampler_type(), "sampler");
         nir_deref_instr *sampler_deref = nir_build_deref_var(&b, sampler_var);

         tex->op = nir_texop_tex;
         tex->sampler_index = 0;

         tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord, coord);
         tex->coord_components = coord_comps;

         tex->src[1] = nir_tex_src_for_ssa(nir_tex_src_texture_deref,
                                           &tex_deref->def);

         tex->src[2] = nir_tex_src_for_ssa(nir_tex_src_sampler_deref,
                                           &sampler_deref->def);
      }

      nir_def_init(&tex->instr, &tex->def, 4, 32);
      nir_builder_instr_insert(&b, &tex->instr);
      res = &tex->def;
   }

   /* Image copies preserve bits, unlike numerical image blits.  Normalized
    * depth planes are unpacked by the SRV and repacked to their exact integer
    * representation; color planes use unsigned views regardless of VkFormat.
    * D32 has no arithmetic conversion: NIR/DXIL reinterprets the stored bits. */
   switch (info->bit_copy) {
   case DZN_BLIT_COPY_D16_TO_COLOR:
   case DZN_BLIT_COPY_D24_TO_COLOR:
      res = nir_f2u32(&b, nir_fround_even(&b, nir_fmul_imm(&b, res,
         info->bit_copy == DZN_BLIT_COPY_D16_TO_COLOR ? 65535.0f : 16777215.0f)));
      break;
   case DZN_BLIT_COPY_COLOR_TO_D16:
   case DZN_BLIT_COPY_COLOR_TO_D24: {
      uint32_t mask = info->bit_copy == DZN_BLIT_COPY_COLOR_TO_D16 ? 65535 : 16777215;
      res = nir_fdiv(&b, nir_u2f32(&b, nir_iand_imm(&b, res, mask)), nir_imm_float(&b, mask));
      break;
   }
   default:
      break;
   }
   if (info->stencil_fallback) {
      nir_def *mask_desc =
         dzn_nir_create_bo_desc(&b, nir_var_mem_ubo, 0, 0, "mask", 0);
      nir_def *mask = nir_load_ubo(&b, 1, 32, mask_desc, nir_imm_int(&b, 0),
         .align_mul = 16, .align_offset = 0, .range_base = 0, .range = ~0);
      nir_def *fail = nir_ieq_imm(&b, nir_iand(&b, nir_channel(&b, res, 0), mask), 0);
      nir_discard_if(&b, fail);
   } else {
      nir_store_var(&b, out, nir_trim_vector(&b, res, out_comps), 0xf);
   }

   return b.shader;
}

static nir_def *
cull_face(nir_builder *b, nir_variable *vertices, bool ccw)
{
   nir_def *v0 =
      nir_load_deref(b, nir_build_deref_array(b, nir_build_deref_var(b, vertices), nir_imm_int(b, 0)));
   nir_def *v1 =
      nir_load_deref(b, nir_build_deref_array(b, nir_build_deref_var(b, vertices), nir_imm_int(b, 1)));
   nir_def *v2 =
      nir_load_deref(b, nir_build_deref_array(b, nir_build_deref_var(b, vertices), nir_imm_int(b, 2)));

   nir_def *dir = nir_fdot(b, nir_cross4(b, nir_fsub(b, v1, v0),
                                                nir_fsub(b, v2, v0)),
                               nir_imm_vec4(b, 0.0, 0.0, -1.0, 0.0));
   if (ccw)
      return nir_fle_imm(b, dir, 0.0f);
   else
      return nir_fgt_imm(b, dir, 0.0f);
}

static void
copy_vars(nir_builder *b, nir_deref_instr *dst, nir_deref_instr *src)
{
   assert(glsl_get_bare_type(dst->type) == glsl_get_bare_type(src->type));
   if (glsl_type_is_struct(dst->type)) {
      for (unsigned i = 0; i < glsl_get_length(dst->type); ++i) {
         copy_vars(b, nir_build_deref_struct(b, dst, i), nir_build_deref_struct(b, src, i));
      }
   } else if (glsl_type_is_array_or_matrix(dst->type)) {
      copy_vars(b, nir_build_deref_array_wildcard(b, dst), nir_build_deref_array_wildcard(b, src));
   } else {
      nir_copy_deref(b, dst, src);
   }
}

static nir_def *
load_point_runtime_data(nir_builder *b, struct dzn_nir_point_gs_info *info, unsigned offset)
{
   nir_address_format ubo_format = nir_address_format_32bit_index_offset;
   bool declared = false;
   nir_foreach_variable_with_modes(var, b->shader, nir_var_mem_ubo) {
      declared |= var->data.descriptor_set == info->runtime_data_cbv.register_space &&
                  var->data.binding == info->runtime_data_cbv.base_shader_register;
   }
   if (!declared) {
      const struct glsl_struct_field field = {
         .type = glsl_array_type(glsl_uint_type(),
            sizeof(struct dxil_spirv_vertex_runtime_data) / sizeof(uint32_t), 4),
         .name = "arr",
      };
      nir_variable *var = nir_variable_create(b->shader, nir_var_mem_ubo,
         glsl_struct_type(&field, 1, "runtime_data", false), "runtime_data");
      var->data.descriptor_set = info->runtime_data_cbv.register_space;
      var->data.binding = info->runtime_data_cbv.base_shader_register;
      var->data.how_declared = nir_var_hidden;
   }

   nir_def *index = nir_vulkan_resource_index(
      b, nir_address_format_num_components(ubo_format),
      nir_address_format_bit_size(ubo_format),
      nir_imm_int(b, 0),
      .desc_set = info->runtime_data_cbv.register_space,
      .binding = info->runtime_data_cbv.base_shader_register,
      .desc_type = nir_descriptor_type_uniform_buffer);

   nir_def *load_desc = nir_load_vulkan_descriptor(
      b, nir_address_format_num_components(ubo_format),
      nir_address_format_bit_size(ubo_format),
      index, .desc_type = nir_descriptor_type_uniform_buffer);

   return nir_load_ubo(
      b, 1, 32,
      nir_channel(b, load_desc, 0),
      nir_imm_int(b, offset),
      .align_mul = 256,
      .align_offset = offset);
}

bool
dzn_nir_preserve_xfb_position(nir_shader *nir)
{
   bool capture_position = false;
   if (!nir->xfb_info)
      return true;
   for (unsigned i = 0; i < nir->xfb_info->output_count; i++)
      capture_position |= nir->xfb_info->outputs[i].location == VARYING_SLOT_POS;
   if (!capture_position)
      return true;

   nir_variable *position = NULL;
   uint64_t occupied = BITFIELD64_BIT(VARYING_SLOT_VAR12);
   nir_foreach_shader_out_variable(var, nir) {
      if (var->data.location < 64)
         occupied |= BITFIELD64_RANGE(var->data.location,
                                     glsl_count_attribute_slots(var->type, false));
      if (var->data.location == VARYING_SLOT_POS)
         position = var;
   }
   unsigned location = VARYING_SLOT_VAR0;
   while (location <= VARYING_SLOT_VAR31 && (occupied & BITFIELD64_BIT(location)))
      location++;
   if (!position || location > VARYING_SLOT_VAR31)
      return false;

   /* SO observes shader outputs before clip-volume/viewport conversions.
    * Mirror every masked store, rather than loading position at function exit:
    * a GS can emit several distinct vertices from a single invocation.
    */
   nir_variable *original = nir_variable_clone(position, nir);
   nir_variable_set_name(nir, original, "dzn_xfb_original_position");
   original->data.location = location;
   original->data.always_active_io = true;
   original->data.is_xfb = true;
   original->data.is_xfb_only = true;
   nir_shader_add_variable(nir, original);
   position->data.explicit_xfb_buffer = false;
   position->data.explicit_xfb_stride = false;
   position->data.explicit_offset = false;
   position->data.is_xfb = false;
   position->data.is_xfb_only = false;
   for (unsigned i = 0; i < nir->xfb_info->output_count; i++)
      if (nir->xfb_info->outputs[i].location == VARYING_SLOT_POS)
         nir->xfb_info->outputs[i].location = location;

   nir_foreach_function_impl(impl, nir) {
      nir_builder b = nir_builder_create(impl);
      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_store_deref ||
                nir_intrinsic_get_var(intr, 0) != position)
               continue;
            b.cursor = nir_before_instr(instr);
            nir_store_var(&b, original, intr->src[1].ssa,
                          nir_intrinsic_write_mask(intr));
         }
      }
      nir_progress(true, impl, nir_metadata_control_flow);
   }
   return true;
}

nir_shader *
dzn_nir_provoking_vertex_gs(const nir_shader *previous, enum mesa_prim primitive,
                            unsigned register_space, bool primitive_id)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_GEOMETRY,
      previous->options, "last_provoking_passthrough");
   nir_shader *nir = b.shader;
   unsigned vertices = primitive == MESA_PRIM_POINTS ? 1 :
                       primitive == MESA_PRIM_LINES ? 2 : 3;
   nir->info.gs.input_primitive = primitive;
   nir->info.gs.output_primitive = primitive == MESA_PRIM_POINTS ? MESA_PRIM_POINTS :
      primitive == MESA_PRIM_LINES ? MESA_PRIM_LINE_STRIP : MESA_PRIM_TRIANGLE_STRIP;
   nir->info.gs.vertices_in = nir->info.gs.vertices_out = vertices;
   nir->info.gs.invocations = 1;
   nir->info.gs.active_stream_mask = 1;
   nir_variable *inputs[VARYING_SLOT_MAX * 4], *outputs[VARYING_SLOT_MAX * 4];
   unsigned count = 0;
   nir_foreach_shader_out_variable(var, previous) {
      inputs[count] = nir_variable_clone(var, nir);
      inputs[count]->type = glsl_array_type(var->type, vertices, 0);
      inputs[count]->data.mode = nir_var_shader_in;
      nir_shader_add_variable(nir, inputs[count]);
      outputs[count] = nir_variable_clone(var, nir);
      nir_shader_add_variable(nir, outputs[count]);
      count++;
   }
   struct dzn_nir_point_gs_info runtime = {
      .runtime_data_cbv = { register_space, 0 },
   };
   nir_def *last = load_point_runtime_data(&b, &runtime,
      offsetof(struct dxil_spirv_vertex_runtime_data, provoking_vertex_index));
   if (primitive == MESA_PRIM_TRIANGLES) {
      nir_def *strip = load_point_runtime_data(&b, &runtime,
         offsetof(struct dxil_spirv_vertex_runtime_data, provoking_strip));
      last = nir_bcsel(&b, nir_iand(&b, nir_ine_imm(&b, strip, 0),
         nir_ine_imm(&b, nir_iand_imm(&b, nir_load_primitive_id(&b), 1), 0)),
         nir_imm_int(&b, 1), last);
   }
   nir_variable *id = primitive_id ? nir_create_variable_with_location(nir,
      nir_var_shader_out, VARYING_SLOT_PRIMITIVE_ID, glsl_int_type()) : NULL;
   for (unsigned vertex = 0; vertex < vertices; vertex++) {
      for (unsigned i = 0; i < count; i++) {
         nir_def *index = outputs[i]->data.interpolation == INTERP_MODE_FLAT &&
                         !outputs[i]->data.is_xfb_only ? last : nir_imm_int(&b, vertex);
         copy_vars(&b, nir_build_deref_var(&b, outputs[i]),
            nir_build_deref_array(&b, nir_build_deref_var(&b, inputs[i]), index));
      }
      if (id)
         nir_store_var(&b, id, nir_load_primitive_id(&b), 1);
      nir_emit_vertex(&b, 0);
   }
   nir_end_primitive(&b, 0);
   NIR_PASS(_, nir, nir_lower_var_copies);
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
   return nir;
}

/* Delay geometry emissions by one (line) or two (triangle) vertices. D3D's
 * leading vertex then carries the Flat payload of Vulkan's last vertex.
 * Geometry, strip winding, vertex count and shader side effects are unchanged:
 * unlike expanding strips into lists this cannot exceed the GS output limit.
 */
bool
dzn_nir_lower_last_provoking_vertex(nir_shader *nir)
{
   if (nir->info.stage != MESA_SHADER_GEOMETRY ||
       nir->info.gs.output_primitive == MESA_PRIM_POINTS)
      return false;
   unsigned delay = nir->info.gs.output_primitive == MESA_PRIM_LINE_STRIP ? 1 : 2;
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_builder b = nir_builder_create(impl);
   nir_variable *out[VARYING_SLOT_MAX * 4], *history[VARYING_SLOT_MAX * 4];
   unsigned variables = 0;
   bool flat = false;
   nir_foreach_shader_out_variable(var, nir) {
      out[variables] = var;
      history[variables] = nir_local_variable_create(impl,
         glsl_array_type(var->type, 3, 0), "provoking_history");
      flat |= var->data.interpolation == INTERP_MODE_FLAT && !var->data.is_xfb_only;
      variables++;
   }
   if (!flat)
      return false;
   struct util_dynarray operations;
   util_dynarray_init(&operations, NULL);
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         if (intr->intrinsic == nir_intrinsic_emit_vertex ||
             intr->intrinsic == nir_intrinsic_emit_vertex_with_counter ||
             intr->intrinsic == nir_intrinsic_end_primitive ||
             intr->intrinsic == nir_intrinsic_end_primitive_with_counter)
            util_dynarray_append_typed(&operations, nir_intrinsic_instr *, intr);
      }
   }
   nir_variable *count = nir_local_variable_create(impl, glsl_uint_type(), "provoking_count");
   b.cursor = nir_before_impl(impl);
   nir_store_var(&b, count, nir_imm_int(&b, 0), 1);

   /* A NULL operation flushes the implicitly terminated final strip. */
   util_dynarray_append_typed(&operations, nir_intrinsic_instr *, NULL);
   util_dynarray_foreach(&operations, nir_intrinsic_instr *, operation) {
      nir_intrinsic_instr *intr = *operation;
      b.cursor = intr ? nir_before_instr(&intr->instr) : nir_after_impl(impl);
      nir_def *n = nir_load_var(&b, count);
      bool emit = intr && (intr->intrinsic == nir_intrinsic_emit_vertex ||
                          intr->intrinsic == nir_intrinsic_emit_vertex_with_counter);
      if (emit) {
         nir_def *slot = nir_umod_imm(&b, n, 3);
         for (unsigned i = 0; i < variables; i++)
            copy_vars(&b, nir_build_deref_array(&b, nir_build_deref_var(&b, history[i]), slot),
                      nir_build_deref_var(&b, out[i]));
         nir_if *ready = nir_push_if(&b, nir_uge_imm(&b, n, delay));
         nir_def *old = nir_umod_imm(&b, nir_iadd_imm(&b, n, -(int)delay), 3);
         for (unsigned i = 0; i < variables; i++) {
            nir_def *source = out[i]->data.interpolation == INTERP_MODE_FLAT &&
                             !out[i]->data.is_xfb_only ? slot : old;
            copy_vars(&b, nir_build_deref_var(&b, out[i]),
               nir_build_deref_array(&b, nir_build_deref_var(&b, history[i]), source));
         }
         nir_emit_vertex(&b, 0);
         nir_pop_if(&b, ready);
         /* Restore live values for subsequent shader loads/partial stores. */
         for (unsigned i = 0; i < variables; i++)
            copy_vars(&b, nir_build_deref_var(&b, out[i]),
               nir_build_deref_array(&b, nir_build_deref_var(&b, history[i]), slot));
         nir_store_var(&b, count, nir_iadd_imm(&b, n, 1), 1);
      } else {
         for (unsigned tail = delay; tail > 0; tail--) {
            nir_if *pending = nir_push_if(&b, nir_uge_imm(&b, n, tail));
            nir_def *slot = nir_umod_imm(&b, nir_iadd_imm(&b, n, -(int)tail), 3);
            for (unsigned i = 0; i < variables; i++)
               copy_vars(&b, nir_build_deref_var(&b, out[i]),
                  nir_build_deref_array(&b, nir_build_deref_var(&b, history[i]), slot));
            nir_emit_vertex(&b, 0);
            nir_pop_if(&b, pending);
         }
         nir_end_primitive(&b, 0);
         nir_if *has_live = nir_push_if(&b, nir_ine_imm(&b, n, 0));
         nir_def *live = nir_umod_imm(&b, nir_iadd_imm(&b, n, -1), 3);
         for (unsigned i = 0; i < variables; i++)
            copy_vars(&b, nir_build_deref_var(&b, out[i]),
               nir_build_deref_array(&b, nir_build_deref_var(&b, history[i]), live));
         nir_pop_if(&b, has_live);
         nir_store_var(&b, count, nir_imm_int(&b, 0), 1);
      }
      if (intr)
         nir_instr_remove(&intr->instr);
   }
   util_dynarray_fini(&operations);
   nir_progress(true, impl, nir_metadata_none);
   NIR_PASS(_, nir, nir_lower_io_vars_to_temporaries, impl, nir_var_shader_out);
   NIR_PASS(_, nir, nir_lower_global_vars_to_local);
   NIR_PASS(_, nir, nir_lower_var_copies);
   return true;
}

nir_shader *
dzn_nir_polygon_point_mode_gs(const nir_shader *previous_shader, struct dzn_nir_point_gs_info *info)
{
   nir_builder builder;
   nir_builder *b = &builder;
   nir_variable *pos_var = NULL;

   unsigned num_vars = 0;
   nir_variable *in[VARYING_SLOT_MAX];
   nir_variable *out[VARYING_SLOT_MAX];


   builder = nir_builder_init_simple_shader(MESA_SHADER_GEOMETRY,
                                            dxil_get_base_nir_compiler_options(),
                                            "implicit_gs");

   nir_shader *nir = b->shader;
   nir->info.inputs_read = nir->info.outputs_written = previous_shader->info.outputs_written;
   nir->info.outputs_written |= (1ull << VARYING_SLOT_VAR12);
   nir->info.gs.input_primitive = MESA_PRIM_TRIANGLES;
   nir->info.gs.output_primitive = MESA_PRIM_POINTS;
   nir->info.gs.vertices_in = 3;
   nir->info.gs.vertices_out = 3;
   nir->info.gs.invocations = 1;
   nir->info.gs.active_stream_mask = 1;

   nir_foreach_shader_out_variable(var, previous_shader) {
      char tmp[100];
      snprintf(tmp, ARRAY_SIZE(tmp), "in_%d", num_vars);
      in[num_vars] = nir_variable_create(nir,
                                         nir_var_shader_in,
                                         glsl_array_type(var->type, 3, 0),
                                         tmp);
      in[num_vars]->data = var->data;
      in[num_vars]->data.mode = nir_var_shader_in;

      if (var->data.location == VARYING_SLOT_POS)
         pos_var = in[num_vars];

      snprintf(tmp, ARRAY_SIZE(tmp), "out_%d", num_vars);
      out[num_vars] = nir_variable_create(nir, nir_var_shader_out, var->type, tmp);
      out[num_vars]->data = var->data;

      num_vars++;
   }

   nir_variable *front_facing_var = nir_variable_create(nir,
                                                        nir_var_shader_out,
                                                        glsl_uint_type(),
                                                        "gl_FrontFacing");
   front_facing_var->data.location = VARYING_SLOT_VAR12;
   front_facing_var->data.driver_location = num_vars;
   front_facing_var->data.interpolation = INTERP_MODE_FLAT;

   nir_def *depth_bias_scale = NULL;
   nir_def *polygon_bias = NULL;
   nir_def *depth_range = NULL;
   nir_def *provoking_index = load_point_runtime_data(b, info,
      offsetof(struct dxil_spirv_vertex_runtime_data, provoking_vertex_index));
   nir_def *provoking_strip = load_point_runtime_data(b, info,
      offsetof(struct dxil_spirv_vertex_runtime_data, provoking_strip));
   provoking_index = nir_bcsel(b, nir_iand(b, nir_ine_imm(b, provoking_strip, 0),
      nir_ine_imm(b, nir_iand_imm(b, nir_load_primitive_id(b), 1), 0)),
      nir_imm_int(b, 1), provoking_index);
   if (info->depth_bias) {
      nir_def *depth_min = load_point_runtime_data(b, info,
         offsetof(struct dxil_spirv_vertex_runtime_data, viewport_min_depth));
      depth_range = nir_fsub(b, load_point_runtime_data(b, info,
         offsetof(struct dxil_spirv_vertex_runtime_data, viewport_max_depth)), depth_min);
      nir_def *ndc[3], *window_z[3];
      for (unsigned v = 0; v < 3; v++) {
         nir_def *p = nir_load_deref(b, nir_build_deref_array_imm(b,
            nir_build_deref_var(b, pos_var), v));
         ndc[v] = nir_fdiv(b, nir_trim_vector(b, p, 3), nir_channel(b, p, 3));
         nir_def *z = nir_channel(b, ndc[v], 2);
         if (info->negative_one_to_one)
            z = nir_fmul_imm(b, nir_fadd_imm(b, z, 1.0f), 0.5f);
         window_z[v] = nir_fadd(b, nir_fmul(b, z, depth_range), depth_min);
      }
      switch (info->ds_fmt) {
      case DXGI_FORMAT_D16_UNORM:
         depth_bias_scale = nir_imm_float(b, 1.0f / (1 << 16));
         break;
      case DXGI_FORMAT_D24_UNORM_S8_UINT:
         depth_bias_scale = nir_imm_float(b, 1.0f / (1 << 24));
         break;
      case DXGI_FORMAT_D32_FLOAT:
      case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: {
         nir_def *max_z = NULL;
         for (uint32_t i = 0; i < 3; ++i) {
            nir_def *z = nir_iand_imm(b, window_z[i], 0x7fffffff);
            max_z = i == 0 ? z : nir_imax(b, z, max_z);
         }
         nir_def *exponent = nir_ishr_imm(b, nir_iand_imm(b, max_z, 0x7f800000), 23);
         depth_bias_scale = nir_fexp2(b, nir_i2f32(b, nir_iadd_imm(b, exponent, -23)));
         break;
      }
      default:
         depth_bias_scale = nir_imm_float(b, 0.0f);
      }
      nir_def *e1 = nir_fsub(b, ndc[1], ndc[0]);
      nir_def *e2 = nir_fsub(b, ndc[2], ndc[0]);
      nir_def *x1 = nir_channel(b, e1, 0), *y1 = nir_channel(b, e1, 1);
      nir_def *x2 = nir_channel(b, e2, 0), *y2 = nir_channel(b, e2, 1);
      nir_def *z1 = nir_fsub(b, window_z[1], window_z[0]);
      nir_def *z2 = nir_fsub(b, window_z[2], window_z[0]);
      nir_def *det = nir_fsub(b, nir_fmul(b, x1, y2), nir_fmul(b, x2, y1));
      nir_def *dx = nir_fdiv(b, nir_fsub(b, nir_fmul(b, z1, y2), nir_fmul(b, z2, y1)), det);
      nir_def *dy = nir_fdiv(b, nir_fsub(b, nir_fmul(b, x1, z2), nir_fmul(b, x2, z1)), det);
      dx = nir_fabs(b, nir_fdiv(b, nir_fmul_imm(b, dx, 2), load_point_runtime_data(b, info,
         offsetof(struct dxil_spirv_vertex_runtime_data, viewport_width))));
      dy = nir_fabs(b, nir_fdiv(b, nir_fmul_imm(b, dy, 2), load_point_runtime_data(b, info,
         offsetof(struct dxil_spirv_vertex_runtime_data, viewport_height))));
      nir_def *slope = nir_bcsel(b, nir_fneu_imm(b, det, 0), nir_fmax(b, dx, dy), nir_imm_float(b, 0));
      nir_def *constant = info->depth_bias_dynamic ? load_point_runtime_data(b, info,
         offsetof(struct dxil_spirv_vertex_runtime_data, depth_bias)) : nir_imm_float(b, info->constant_depth_bias);
      nir_def *factor = info->depth_bias_dynamic ? load_point_runtime_data(b, info,
         offsetof(struct dxil_spirv_vertex_runtime_data, depth_bias_slope)) : nir_imm_float(b, info->slope_scaled_depth_bias);
      nir_def *clamp = info->depth_bias_dynamic ? load_point_runtime_data(b, info,
         offsetof(struct dxil_spirv_vertex_runtime_data, depth_bias_clamp)) : nir_imm_float(b, info->depth_bias_clamp);
      polygon_bias = nir_fadd(b, nir_fmul(b, factor, slope), nir_fmul(b, constant, depth_bias_scale));
      polygon_bias = nir_bcsel(b, nir_fgt_imm(b, clamp, 0), nir_fmin(b, polygon_bias, clamp),
         nir_bcsel(b, nir_flt_imm(b, clamp, 0), nir_fmax(b, polygon_bias, clamp), polygon_bias));
      /* Convert window-space bias back to clip space, including reversed depth. */
      polygon_bias = nir_bcsel(b, nir_fneu_imm(b, depth_range, 0),
         nir_fdiv(b, polygon_bias, depth_range), nir_imm_float(b, 0));
   }

   /* Temporary variable "loop_index" to loop over input vertices */
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_variable *loop_index_var =
      nir_local_variable_create(impl, glsl_uint_type(), "loop_index");
   nir_deref_instr *loop_index_deref = nir_build_deref_var(b, loop_index_var);
   nir_store_deref(b, loop_index_deref, nir_imm_int(b, 0), 1);

   nir_def *cull_pass = nir_imm_true(b);
   nir_def *front_facing;
   if (info->cull_dynamic || info->front_face_dynamic) {
      nir_def *ccw = info->front_face_dynamic ?
         nir_ine_imm(b, load_point_runtime_data(b, info,
            offsetof(struct dxil_spirv_vertex_runtime_data, point_front_ccw)), 0) :
         nir_imm_bool(b, info->front_ccw);
      nir_def *front = nir_bcsel(b, ccw, cull_face(b, pos_var, true),
                                cull_face(b, pos_var, false));
      nir_def *mode = info->cull_dynamic ? load_point_runtime_data(b, info,
         offsetof(struct dxil_spirv_vertex_runtime_data, point_cull_mode)) :
         nir_imm_int(b, info->cull_mode);
      nir_def *face_bit = nir_bcsel(b, front, nir_imm_int(b, VK_CULL_MODE_FRONT_BIT),
                                  nir_imm_int(b, VK_CULL_MODE_BACK_BIT));
      cull_pass = nir_ieq_imm(b, nir_iand(b, mode, face_bit), 0);
      front_facing = nir_b2i32(b, front);
   } else if (info->cull_mode == VK_CULL_MODE_FRONT_BIT) {
      cull_pass = cull_face(b, pos_var, info->front_ccw);
      front_facing = nir_b2i32(b, cull_pass);
   } else if (info->cull_mode == VK_CULL_MODE_BACK_BIT) {
      cull_pass = cull_face(b, pos_var, !info->front_ccw);
      front_facing = nir_inot(b, nir_b2i32(b, cull_pass));
   } else
      front_facing = nir_i2i32(b, cull_face(b, pos_var, info->front_ccw));

   /**
    *  if (cull_pass) {
    *     while {
    *        if (loop_index >= 3)
    *           break;
    */
   const bool captures_xfb = previous_shader->xfb_info &&
                             previous_shader->xfb_info->output_count;
   nir_if *cull_check = nir_push_if(b, captures_xfb ? nir_imm_true(b) : cull_pass);
   nir_loop *loop = nir_push_loop(b);

   nir_def *loop_index = nir_load_deref(b, loop_index_deref);
   nir_def *cmp = nir_ige(b, loop_index,
                              nir_imm_int(b, 3));
   nir_break_if(b, cmp);

   /**
    *        [...] // Copy all variables
    *        EmitVertex();
    */
   for (unsigned i = 0; i < num_vars; ++i) {
      nir_def *index = out[i]->data.interpolation == INTERP_MODE_FLAT &&
                       !out[i]->data.is_xfb_only ? provoking_index : loop_index;
      nir_deref_instr *in_value = nir_build_deref_array(b, nir_build_deref_var(b, in[i]), index);
      if (in[i] == pos_var && info->depth_bias) {
         nir_def *bias_val = polygon_bias;
         if (info->depth_bias_enable_dynamic) {
            nir_def *enabled = nir_ine_imm(b, load_point_runtime_data(b, info,
               offsetof(struct dxil_spirv_vertex_runtime_data, point_depth_bias_enable)), 0);
            bias_val = nir_bcsel(b, enabled, bias_val, nir_imm_float(b, 0.0f));
         }
         nir_def *old_val = nir_load_deref(b, in_value);
         bias_val = nir_fmul(b, bias_val, nir_channel(b, old_val, 3));
         nir_def *new_val = nir_vector_insert_imm(b, old_val,
                                                      nir_fadd(b, nir_channel(b, old_val, 2), bias_val),
                                                      2);
         nir_store_var(b, out[i], new_val, 0xf);
      } else {
         copy_vars(b, nir_build_deref_var(b, out[i]), in_value);
      }
      if (in[i] == pos_var && captures_xfb) {
         /* Culling is later than XFB in Vulkan. Emit the original payload for
          * every vertex, but move culled raster positions outside the clip
          * volume. The XFB-only position varying remains untouched.
          */
         nir_def *position = nir_load_var(b, out[i]);
         nir_def *clipped = nir_vec4(b, nir_imm_float(b, 0), nir_imm_float(b, 0),
                                     nir_imm_float(b, 2), nir_imm_float(b, 1));
         nir_store_var(b, out[i], nir_bcsel(b, cull_pass, position, clipped), 0xf);
      }
   }
   nir_store_var(b, front_facing_var, front_facing, 0x1);
   nir_emit_vertex(b, 0);

   /**
    *        loop_index++;
    *     }
    *  }
    */
   nir_store_deref(b, loop_index_deref, nir_iadd_imm(b, loop_index, 1), 1);
   nir_pop_loop(b, loop);
   nir_pop_if(b, cull_check);

   nir_validate_shader(nir, "in dzn_nir_polygon_point_mode_gs");

   NIR_PASS(_, nir, nir_lower_var_copies);
   if (previous_shader->xfb_info) {
      const size_t size = nir_xfb_info_size(previous_shader->xfb_info->output_count);
      nir->xfb_info = ralloc_size(nir, size);
      memcpy(nir->xfb_info, previous_shader->xfb_info, size);
   }
   return b->shader;
}
