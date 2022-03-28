/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2005 Blender Foundation. All rights reserved. */

#include "node_shader_util.hh"

namespace blender::nodes::node_shader_tex_cubemap_cc {
static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Vector>(N_("Vector")).hide_value();
  b.add_output<decl::Color>(N_("Color")).no_muted_links();
}

static void node_shader_init_tex_cubemap(bNodeTree *UNUSED(ntree), bNode *node)
{
  NodeTexCubemap *tex = MEM_cnew<NodeTexCubemap>("NodeTexCubemap");
  BKE_texture_mapping_default(&tex->base.tex_mapping, TEXMAP_TYPE_POINT);
  BKE_texture_colormapping_default(&tex->base.color_mapping);
  tex->mode = SHD_CUBEMAP_MODE_SINGLE;
  BKE_imageuser_default(&tex->iuser);

  node->storage = tex;
}

static int node_shader_gpu_tex_cubemap(GPUMaterial *mat,
                                           bNode *node,
                                           bNodeExecData *UNUSED(execdata),
                                           GPUNodeStack *in,
                                           GPUNodeStack *out)
{
  Image *ima = (Image *)node->id;
  NodeTexCubemap *tex = (NodeTexCubemap *)node->storage;

  /* We get the image user from the original node, since GPU image keeps
   * a pointer to it and the dependency refreshes the original. */
  bNode *node_original = node->original ? node->original : node;
  NodeTexImage *tex_original = (NodeTexImage *)node_original->storage;
  ImageUser *iuser = &tex_original->iuser;
  eGPUSamplerState sampler = GPU_SAMPLER_REPEAT | GPU_SAMPLER_ANISO | GPU_SAMPLER_FILTER;
  /* TODO(fclem): For now assume mipmap is always enabled. */
  if (true) {
    sampler |= GPU_SAMPLER_MIPMAP;
  }

  if (!in[0].link) {
    GPU_link(mat, "node_tex_cubemap_texco", GPU_builtin(GPU_VIEW_POSITION), &in[0].link);
    node_shader_gpu_bump_tex_coord(mat, node, &in[0].link);
  }

  node_shader_gpu_tex_mapping(mat, node, in, out);
  /* Compute texture coordinate. */
  /*
  if (!ima) {
    return GPU_stack_link(mat, node, "node_tex_cubemap_empty", in, out);
  }
  */

  GPUNodeLink* outalpha;
  if (tex->mode == SHD_CUBEMAP_MODE_SINGLE) {
    if (!ima) {
      return GPU_stack_link(mat, node, "node_tex_cubemap_empty", in, out);
    }

    if (!in[0].link) {
      GPU_link(mat, "node_tex_environment_texco", GPU_builtin(GPU_VIEW_POSITION), &in[0].link);
      node_shader_gpu_bump_tex_coord(mat, node, &in[0].link);
    }

    GPU_link(mat, "node_tex_image_linear", in[0].link, GPU_image(mat, ima, iuser, sampler), &out[0].link, &outalpha);
  }
  else {
    if (!tex->up.value || !tex->down.value || !tex->left.value || !tex->right.value || !tex->front.value || !tex->back.value) {
      return GPU_stack_link(mat, node, "node_tex_cubemap_empty", in, out);
    }
    else {
      GPU_link(mat, "node_tex_cubemap_multi", in[0].link,
        GPU_image(mat, tex->up.value, iuser, sampler),
        GPU_image(mat, tex->down.value, iuser, sampler),
        GPU_image(mat, tex->left.value, iuser, sampler),
        GPU_image(mat, tex->right.value, iuser, sampler),
        GPU_image(mat, tex->front.value, iuser, sampler),
        GPU_image(mat, tex->back.value, iuser, sampler),
        &out[0].link);
    }
  }


#if 0
//#if 0
  switch (tex->interpolation) {
    case SHD_INTERP_LINEAR:
      gpu_fn = names[0];
      break;
    case SHD_INTERP_CLOSEST:
      sampler &= ~(GPU_SAMPLER_FILTER | GPU_SAMPLER_MIPMAP);
      gpu_fn = names[0];
      break;
    default:
      gpu_fn = names[1];
      break;
  }
  GPUNodeLink* outalpha;
  /* Sample texture with correct interpolation. */

  GPU_link(mat, gpu_fn, in[0].link, GPU_image(mat, ima, iuser, sampler), &out[0].link, &outalpha);

  if (out[0].hasoutput) {
    if (ELEM(ima->alpha_mode, IMA_ALPHA_IGNORE, IMA_ALPHA_CHANNEL_PACKED) ||
        IMB_colormanagement_space_name_is_data(ima->colorspace_settings.name)) {
      /* Don't let alpha affect color output in these cases. */
      GPU_link(mat, "color_alpha_clear", out[0].link, &out[0].link);
    }
    else {
      /* Always output with premultiplied alpha. */
      if (ima->alpha_mode == IMA_ALPHA_PREMUL) {
        GPU_link(mat, "color_alpha_clear", out[0].link, &out[0].link);
      }
      else {
        GPU_link(mat, "color_alpha_premultiply", out[0].link, &out[0].link);
      }
    }
  }
#endif

  return true;
}

}  // namespace blender::nodes::node_shader_tex_cubemap_cc

/* node type definition */
void register_node_type_sh_tex_cubemap()
{
  
  namespace file_ns = blender::nodes::node_shader_tex_cubemap_cc;

  static bNodeType ntype;
  sh_node_type_base(&ntype, SH_NODE_TEX_CUBEMAP, "Cubemap Texture", NODE_CLASS_TEXTURE);
  ntype.declare = file_ns::node_declare;
  node_type_init(&ntype, file_ns::node_shader_init_tex_cubemap);
  node_type_storage(
      &ntype, "NodeTexCubemap", node_free_standard_storage, node_copy_standard_storage);
  node_type_gpu(&ntype, file_ns::node_shader_gpu_tex_cubemap);
  ntype.labelfunc = node_image_label;
  node_type_size_preset(&ntype, NODE_SIZE_LARGE);
  nodeRegisterType(&ntype);

  return;
}
