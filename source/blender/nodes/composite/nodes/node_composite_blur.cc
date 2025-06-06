/* SPDX-FileCopyrightText: 2006 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup cmpnodes
 */

#include "BLI_assert.h"
#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"

#include "RNA_access.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "GPU_shader.hh"

#include "COM_algorithm_parallel_reduction.hh"
#include "COM_algorithm_recursive_gaussian_blur.hh"
#include "COM_algorithm_symmetric_separable_blur.hh"
#include "COM_node_operation.hh"
#include "COM_symmetric_blur_weights.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"

/* **************** BLUR ******************** */

namespace blender::nodes::node_composite_blur_cc {

NODE_STORAGE_FUNCS(NodeBlurData)

static void cmp_node_blur_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>("Image").default_value({1.0f, 1.0f, 1.0f, 1.0f});
  b.add_input<decl::Vector>("Size").dimensions(2).default_value({0.0f, 0.0f}).min(0.0f);
  b.add_input<decl::Bool>("Extend Bounds").default_value(false).compositor_expects_single_value();
  b.add_input<decl::Bool>("Separable")
      .default_value(true)
      .compositor_expects_single_value()
      .description(
          "Use faster approximation by blurring along the horizontal and vertical directions "
          "independently");

  b.add_output<decl::Color>("Image");
}

static void node_composit_init_blur(bNodeTree * /*ntree*/, bNode *node)
{
  NodeBlurData *data = MEM_callocN<NodeBlurData>(__func__);
  data->filtertype = R_FILTER_GAUSS;
  node->storage = data;
}

static void node_composit_buts_blur(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout->prop(ptr, "filter_type", UI_ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);
}

using namespace blender::compositor;

class BlurOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &input = this->get_input("Image");
    Result &output = this->get_result("Image");
    if (this->is_identity()) {
      output.share_data(input);
      return;
    }

    if (!this->get_input("Size").is_single_value()) {
      this->execute_variable_size(input, output);
    }
    else if (node_storage(bnode()).filtertype == R_FILTER_FAST_GAUSS) {
      recursive_gaussian_blur(context(), input, output, this->get_blur_size());
    }
    else if (use_separable_filter()) {
      symmetric_separable_blur(context(),
                               input,
                               output,
                               this->get_blur_size(),
                               node_storage(bnode()).filtertype,
                               get_extend_bounds());
    }
    else {
      this->execute_constant_size(input, output);
    }
  }

  void execute_constant_size(const Result &input, Result &output)
  {
    if (this->context().use_gpu()) {
      this->execute_constant_size_gpu(input, output);
    }
    else {
      this->execute_constant_size_cpu(input, output);
    }
  }

  void execute_constant_size_gpu(const Result &input, Result &output)
  {
    GPUShader *shader = context().get_shader("compositor_symmetric_blur");
    GPU_shader_bind(shader);

    GPU_shader_uniform_1b(shader, "extend_bounds", get_extend_bounds());

    input.bind_as_texture(shader, "input_tx");

    const float2 blur_radius = this->get_blur_size();

    const Result &weights = context().cache_manager().symmetric_blur_weights.get(
        context(), node_storage(bnode()).filtertype, blur_radius);
    weights.bind_as_texture(shader, "weights_tx");

    Domain domain = compute_domain();
    if (get_extend_bounds()) {
      /* Add a radius amount of pixels in both sides of the image, hence the multiply by 2. */
      domain.size += int2(math::ceil(blur_radius)) * 2;
    }

    output.allocate_texture(domain);
    output.bind_as_image(shader, "output_img");

    compute_dispatch_threads_at_least(shader, domain.size);

    GPU_shader_unbind();
    output.unbind_as_image();
    input.unbind_as_texture();
    weights.unbind_as_texture();
  }

  void execute_constant_size_cpu(const Result &input, Result &output)
  {
    const float2 blur_radius = this->get_blur_size();
    const Result &weights = this->context().cache_manager().symmetric_blur_weights.get(
        this->context(), node_storage(this->bnode()).filtertype, blur_radius);

    Domain domain = this->compute_domain();
    const bool extend_bounds = this->get_extend_bounds();
    if (extend_bounds) {
      /* Add a radius amount of pixels in both sides of the image, hence the multiply by 2. */
      domain.size += int2(math::ceil(blur_radius)) * 2;
    }

    output.allocate_texture(domain);

    auto load_input = [&](const int2 texel) {
      return this->load_input(input, weights, texel, extend_bounds);
    };

    parallel_for(domain.size, [&](const int2 texel) {
      float4 accumulated_color = float4(0.0f);

      /* First, compute the contribution of the center pixel. */
      float4 center_color = load_input(texel);
      accumulated_color += center_color * weights.load_pixel<float>(int2(0));

      int2 weights_size = weights.domain().size;

      /* Then, compute the contributions of the pixels along the x axis of the filter, noting that
       * the weights texture only stores the weights for the positive half, but since the filter is
       * symmetric, the same weight is used for the negative half and we add both of their
       * contributions. */
      for (int x = 1; x < weights_size.x; x++) {
        float weight = weights.load_pixel<float>(int2(x, 0));
        accumulated_color += load_input(texel + int2(x, 0)) * weight;
        accumulated_color += load_input(texel + int2(-x, 0)) * weight;
      }

      /* Then, compute the contributions of the pixels along the y axis of the filter, noting that
       * the weights texture only stores the weights for the positive half, but since the filter is
       * symmetric, the same weight is used for the negative half and we add both of their
       * contributions. */
      for (int y = 1; y < weights_size.y; y++) {
        float weight = weights.load_pixel<float>(int2(0, y));
        accumulated_color += load_input(texel + int2(0, y)) * weight;
        accumulated_color += load_input(texel + int2(0, -y)) * weight;
      }

      /* Finally, compute the contributions of the pixels in the four quadrants of the filter,
       * noting that the weights texture only stores the weights for the upper right quadrant, but
       * since the filter is symmetric, the same weight is used for the rest of the quadrants and
       * we add all four of their contributions. */
      for (int y = 1; y < weights_size.y; y++) {
        for (int x = 1; x < weights_size.x; x++) {
          float weight = weights.load_pixel<float>(int2(x, y));
          accumulated_color += load_input(texel + int2(x, y)) * weight;
          accumulated_color += load_input(texel + int2(-x, y)) * weight;
          accumulated_color += load_input(texel + int2(x, -y)) * weight;
          accumulated_color += load_input(texel + int2(-x, -y)) * weight;
        }
      }

      output.store_pixel(texel, accumulated_color);
    });
  }

  void execute_variable_size(const Result &input, Result &output)
  {
    if (this->context().use_gpu()) {
      this->execute_variable_size_gpu(input, output);
    }
    else {
      this->execute_variable_size_cpu(input, output);
    }
  }

  void execute_variable_size_gpu(const Result &input, Result &output)
  {
    const float2 blur_radius = this->compute_maximum_blur_size();
    const Result &weights = context().cache_manager().symmetric_blur_weights.get(
        context(), node_storage(bnode()).filtertype, blur_radius);

    GPUShader *shader = context().get_shader("compositor_symmetric_blur_variable_size");
    GPU_shader_bind(shader);

    GPU_shader_uniform_1b(shader, "extend_bounds", get_extend_bounds());

    input.bind_as_texture(shader, "input_tx");

    weights.bind_as_texture(shader, "weights_tx");

    const Result &input_size = get_input("Size");
    input_size.bind_as_texture(shader, "size_tx");

    Domain domain = compute_domain();
    if (get_extend_bounds()) {
      /* Add a radius amount of pixels in both sides of the image, hence the multiply by 2. */
      domain.size += int2(math::ceil(blur_radius)) * 2;
    }

    output.allocate_texture(domain);
    output.bind_as_image(shader, "output_img");

    compute_dispatch_threads_at_least(shader, domain.size);

    GPU_shader_unbind();
    output.unbind_as_image();
    input.unbind_as_texture();
    weights.unbind_as_texture();
    input_size.unbind_as_texture();
  }

  void execute_variable_size_cpu(const Result &input, Result &output)
  {
    const float2 blur_radius = this->compute_maximum_blur_size();
    const Result &weights = this->context().cache_manager().symmetric_blur_weights.get(
        this->context(), node_storage(this->bnode()).filtertype, blur_radius);

    Domain domain = this->compute_domain();
    const bool extend_bounds = this->get_extend_bounds();
    if (extend_bounds) {
      /* Add a radius amount of pixels in both sides of the image, hence the multiply by 2. */
      domain.size += int2(math::ceil(blur_radius)) * 2;
    }

    output.allocate_texture(domain);

    auto load_input = [&](const int2 texel) {
      return this->load_input(input, weights, texel, extend_bounds);
    };

    const Result &size = get_input("Size");
    /* Similar to load_input but loads the size instead and clamps to borders instead of returning
     * zero for out of bound access. See load_input for more information. */
    auto load_size = [&](const int2 texel) {
      int2 blur_radius = weights.domain().size - 1;
      int2 offset = extend_bounds ? blur_radius : int2(0);
      return math::max(float2(0.0f), size.load_pixel_extended<float3>(texel - offset).xy());
    };

    parallel_for(domain.size, [&](const int2 texel) {
      float4 accumulated_color = float4(0.0f);
      float4 accumulated_weight = float4(0.0f);

      const float2 size = load_size(texel);
      int2 radius = int2(math::ceil(size));
      float2 coordinates_scale = float2(1.0f) / (size + float2(1.0f));

      /* First, compute the contribution of the center pixel. */
      float4 center_color = load_input(texel);
      float center_weight = weights.load_pixel<float>(int2(0));
      accumulated_color += center_color * center_weight;
      accumulated_weight += center_weight;

      /* Then, compute the contributions of the pixels along the x axis of the filter, noting that
       * the weights texture only stores the weights for the positive half, but since the filter is
       * symmetric, the same weight is used for the negative half and we add both of their
       * contributions. */
      for (int x = 1; x <= radius.x; x++) {
        float weight_coordinates = (x + 0.5f) * coordinates_scale.x;
        float weight = weights.sample_bilinear_extended(float2(weight_coordinates, 0.0f)).x;
        accumulated_color += load_input(texel + int2(x, 0)) * weight;
        accumulated_color += load_input(texel + int2(-x, 0)) * weight;
        accumulated_weight += weight * 2.0f;
      }

      /* Then, compute the contributions of the pixels along the y axis of the filter, noting that
       * the weights texture only stores the weights for the positive half, but since the filter is
       * symmetric, the same weight is used for the negative half and we add both of their
       * contributions. */
      for (int y = 1; y <= radius.y; y++) {
        float weight_coordinates = (y + 0.5f) * coordinates_scale.y;
        float weight = weights.sample_bilinear_extended(float2(0.0f, weight_coordinates)).x;
        accumulated_color += load_input(texel + int2(0, y)) * weight;
        accumulated_color += load_input(texel + int2(0, -y)) * weight;
        accumulated_weight += weight * 2.0f;
      }

      /* Finally, compute the contributions of the pixels in the four quadrants of the filter,
       * noting that the weights texture only stores the weights for the upper right quadrant, but
       * since the filter is symmetric, the same weight is used for the rest of the quadrants and
       * we add all four of their contributions. */
      for (int y = 1; y <= radius.y; y++) {
        for (int x = 1; x <= radius.x; x++) {
          float2 weight_coordinates = (float2(x, y) + float2(0.5f)) * coordinates_scale;
          float weight = weights.sample_bilinear_extended(weight_coordinates).x;
          accumulated_color += load_input(texel + int2(x, y)) * weight;
          accumulated_color += load_input(texel + int2(-x, y)) * weight;
          accumulated_color += load_input(texel + int2(x, -y)) * weight;
          accumulated_color += load_input(texel + int2(-x, -y)) * weight;
          accumulated_weight += weight * 4.0f;
        }
      }

      accumulated_color = math::safe_divide(accumulated_color, accumulated_weight);

      output.store_pixel(texel, accumulated_color);
    });
  }

  float2 compute_maximum_blur_size()
  {
    return maximum_float3(this->context(), this->get_input("Size")).xy();
  }

  /* Loads the input color of the pixel at the given texel. If bounds are extended, then the input
   * is treated as padded by a blur size amount of pixels of zero color, and the given texel is
   * assumed to be in the space of the image after padding. So we offset the texel by the blur
   * radius amount and fall back to a zero color if it is out of bounds. For instance, if the input
   * is padded by 5 pixels to the left of the image, the first 5 pixels should be out of bounds and
   * thus zero, hence the introduced offset. */
  float4 load_input(const Result &input,
                    const Result &weights,
                    const int2 texel,
                    const bool extend_bounds)
  {
    float4 color;
    if (extend_bounds) {
      /* Notice that we subtract 1 because the weights result have an extra center weight, see the
       * SymmetricBlurWeights class for more information. */
      int2 blur_radius = weights.domain().size - 1;
      color = input.load_pixel_zero<float4>(texel - blur_radius);
    }
    else {
      color = input.load_pixel_extended<float4>(texel);
    }

    return color;
  }

  bool is_identity()
  {
    const Result &input = this->get_input("Image");
    if (input.is_single_value()) {
      return true;
    }

    const Result &size = this->get_input("Size");
    if (!size.is_single_value()) {
      return false;
    }

    if (this->get_blur_size() == float2(0.0)) {
      return true;
    }

    return false;
  }

  /* The blur node can operate with different filter types, evaluated on the normalized distance to
   * the center of the filter. Some of those filters are separable and can be computed as such. If
   * the Separable input is true, then the filter is always computed as separable even if it is not
   * in fact separable, in which case, the used filter is a cheaper approximation to the actual
   * filter. Otherwise, the filter is computed as separable if it is in fact separable and as a
   * normal 2D filter otherwise. */
  bool use_separable_filter()
  {
    if (this->get_separable()) {
      return true;
    }

    /* Only Gaussian filters are separable. The rest is not. */
    switch (node_storage(bnode()).filtertype) {
      case R_FILTER_GAUSS:
      case R_FILTER_FAST_GAUSS:
        return true;
      default:
        return false;
    }
  }

  float2 get_blur_size()
  {
    BLI_assert(this->get_input("Size").is_single_value());
    return math::max(float2(0.0f), this->get_input("Size").get_single_value<float3>().xy());
  }

  bool get_separable()
  {
    return this->get_input("Separable").get_single_value_default(true);
  }

  bool get_extend_bounds()
  {
    return this->get_input("Extend Bounds").get_single_value_default(false);
  }
};

static NodeOperation *get_compositor_operation(Context &context, DNode node)
{
  return new BlurOperation(context, node);
}

}  // namespace blender::nodes::node_composite_blur_cc

static void register_node_type_cmp_blur()
{
  namespace file_ns = blender::nodes::node_composite_blur_cc;

  static blender::bke::bNodeType ntype;

  cmp_node_type_base(&ntype, "CompositorNodeBlur", CMP_NODE_BLUR);
  ntype.ui_name = "Blur";
  ntype.ui_description = "Blur an image, using several blur modes";
  ntype.enum_name_legacy = "BLUR";
  ntype.nclass = NODE_CLASS_OP_FILTER;
  ntype.declare = file_ns::cmp_node_blur_declare;
  ntype.draw_buttons = file_ns::node_composit_buts_blur;
  ntype.flag |= NODE_PREVIEW;
  ntype.initfunc = file_ns::node_composit_init_blur;
  blender::bke::node_type_storage(
      ntype, "NodeBlurData", node_free_standard_storage, node_copy_standard_storage);
  ntype.get_compositor_operation = file_ns::get_compositor_operation;

  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(register_node_type_cmp_blur)
