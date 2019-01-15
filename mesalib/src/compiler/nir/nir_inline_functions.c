/*
 * Copyright © 2015 Intel Corporation
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "nir.h"
#include "nir_builder.h"
#include "nir_control_flow.h"
#include "nir_vla.h"

static bool inline_function_impl(nir_function_impl *impl, struct set *inlined);

static bool
inline_functions_block(nir_block *block, nir_builder *b,
                       struct set *inlined)
{
   bool progress = false;
   /* This is tricky.  We're iterating over instructions in a block but, as
    * we go, the block and its instruction list are being split into
    * pieces.  However, this *should* be safe since foreach_safe always
    * stashes the next thing in the iteration.  That next thing will
    * properly get moved to the next block when it gets split, and we
    * continue iterating there.
    */
   nir_foreach_instr_safe(instr, block) {
      if (instr->type != nir_instr_type_call)
         continue;

      progress = true;

      nir_call_instr *call = nir_instr_as_call(instr);
      assert(call->callee->impl);

      inline_function_impl(call->callee->impl, inlined);

      nir_function_impl *callee_copy =
         nir_function_impl_clone(call->callee->impl);
      callee_copy->function = call->callee;

      exec_list_append(&b->impl->locals, &callee_copy->locals);
      exec_list_append(&b->impl->registers, &callee_copy->registers);

      b->cursor = nir_before_instr(&call->instr);

      /* Rewrite all of the uses of the callee's parameters to use the call
       * instructions sources.  In order to ensure that the "load" happens
       * here and not later (for register sources), we make sure to convert it
       * to an SSA value first.
       */
      const unsigned num_params = call->num_params;
      NIR_VLA(nir_ssa_def *, params, num_params);
      for (unsigned i = 0; i < num_params; i++) {
         params[i] = nir_ssa_for_src(b, call->params[i],
                                     call->callee->params[i].num_components);
      }

      nir_foreach_block(block, callee_copy) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;

            nir_intrinsic_instr *load = nir_instr_as_intrinsic(instr);
            if (load->intrinsic != nir_intrinsic_load_param)
               continue;

            unsigned param_idx = nir_intrinsic_param_idx(load);
            assert(param_idx < num_params);
            assert(load->dest.is_ssa);
            nir_ssa_def_rewrite_uses(&load->dest.ssa,
                                     nir_src_for_ssa(params[param_idx]));

            /* Remove any left-over load_param intrinsics because they're soon
             * to be in another function and therefore no longer valid.
             */
            nir_instr_remove(&load->instr);
         }
      }

      /* Pluck the body out of the function and place it here */
      nir_cf_list body;
      nir_cf_list_extract(&body, &callee_copy->body);
      nir_cf_reinsert(&body, b->cursor);

      nir_instr_remove(&call->instr);
   }

   return progress;
}

static bool
inline_function_impl(nir_function_impl *impl, struct set *inlined)
{
   if (_mesa_set_search(inlined, impl))
      return false; /* Already inlined */

   nir_builder b;
   nir_builder_init(&b, impl);

   bool progress = false;
   nir_foreach_block_safe(block, impl) {
      progress |= inline_functions_block(block, &b, inlined);
   }

   if (progress) {
      /* SSA and register indices are completely messed up now */
      nir_index_ssa_defs(impl);
      nir_index_local_regs(impl);

      nir_metadata_preserve(impl, nir_metadata_none);
   } else {
#ifndef NDEBUG
      impl->valid_metadata &= ~nir_metadata_not_properly_reset;
#endif
   }

   _mesa_set_add(inlined, impl);

   return progress;
}

/** A pass to inline all functions in a shader into their callers
 *
 * For most use-cases, function inlining is a multi-step process.  The general
 * pattern employed by SPIR-V consumers and others is as follows:
 *
 *  1. nir_lower_constant_initializers(shader, nir_var_function)
 *
 *     This is needed because local variables from the callee are simply added
 *     to the locals list for the caller and the information about where the
 *     constant initializer logically happens is lost.  If the callee is
 *     called in a loop, this can cause the variable to go from being
 *     initialized once per loop iteration to being initialized once at the
 *     top of the caller and values to persist from one invocation of the
 *     callee to the next.  The simple solution to this problem is to get rid
 *     of constant initializers before function inlining.
 *
 *  2. nir_lower_returns(shader)
 *
 *     nir_inline_functions assumes that all functions end "naturally" by
 *     execution reaching the end of the function without any return
 *     instructions causing instant jumps to the end.  Thanks to NIR being
 *     structured, we can't represent arbitrary jumps to various points in the
 *     program which is what an early return in the callee would have to turn
 *     into when we inline it into the caller.  Instead, we require returns to
 *     be lowered which lets us just copy+paste the callee directly into the
 *     caller.
 *
 *  3. nir_inline_functions(shader)
 *
 *     This does the actual function inlining and the resulting shader will
 *     contain no call instructions.
 *
 *  4. nir_opt_deref(shader)
 *
 *     Most functions contain pointer parameters where the result of a deref
 *     instruction is passed in as a parameter, loaded via a load_param
 *     intrinsic, and then turned back into a deref via a cast.  Function
 *     inlining will get rid of the load_param but we are still left with a
 *     cast.  Running nir_opt_deref gets rid of the intermediate cast and
 *     results in a whole deref chain again.  This is currently required by a
 *     number of optimizations and lowering passes at least for certain
 *     variable modes.
 *
 *  5. Loop over the functions and delete all but the main entrypoint.
 *
 *     In the Intel Vulkan driver this looks like this:
 *
 *        foreach_list_typed_safe(nir_function, func, node, &nir->functions) {
 *           if (func != entry_point)
 *              exec_node_remove(&func->node);
 *        }
 *        assert(exec_list_length(&nir->functions) == 1);
 *
 *    While nir_inline_functions does get rid of all call instructions, it
 *    doesn't get rid of any functions because it doesn't know what the "root
 *    function" is.  Instead, it's up to the individual driver to know how to
 *    decide on a root function and delete the rest.  With SPIR-V,
 *    spirv_to_nir returns the root function and so we can just use == whereas
 *    with GL, you may have to look for a function named "main".
 *
 *  6. nir_lower_constant_initializers(shader, ~nir_var_function)
 *
 *     Lowering constant initializers on inputs, outputs, global variables,
 *     etc. requires that we know the main entrypoint so that we know where to
 *     initialize them.  Otherwise, we would have to assume that anything
 *     could be a main entrypoint and initialize them at the start of every
 *     function but that would clearly be wrong if any of those functions were
 *     ever called within another function.  Simply requiring a single-
 *     entrypoint function shader is the best way to make it well-defined.
 */
bool
nir_inline_functions(nir_shader *shader)
{
   struct set *inlined = _mesa_pointer_set_create(NULL);
   bool progress = false;

   nir_foreach_function(function, shader) {
      if (function->impl)
         progress = inline_function_impl(function->impl, inlined) || progress;
   }

   _mesa_set_destroy(inlined, NULL);

   return progress;
}
