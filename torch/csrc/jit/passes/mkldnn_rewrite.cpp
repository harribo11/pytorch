#include <ATen/Config.h>
#include <ATen/code_template.h>
#include <torch/csrc/jit/ir/ir.h>
#include <torch/csrc/jit/jit_log.h>
#include <torch/csrc/jit/passes/constant_propagation.h>
#include <torch/csrc/jit/passes/dead_code_elimination.h>
#include <torch/csrc/jit/passes/graph_rewrite_helper.h>
#include <torch/csrc/jit/passes/mkldnn_rewrite.h>
#include <torch/csrc/jit/passes/mkldnn_rewrite_helper.h>
#include <torch/csrc/jit/tensorexpr/kernel.h>

namespace torch {
namespace jit {

#if AT_MKLDNN_ENABLED()

c10::VaryingShape<int64_t> getSizesOf(Node* n, size_t idx) {
  auto tt = n->input(idx)->type()->cast<TensorType>();
  return tt->sizes();
}

void insertPrePackedConvOpForNode(Node* n) {
  constexpr int POS_INPUT = 0;
  constexpr int POS_WEIGHT = 1;
  if (!tensorexpr::isContiguous(
          n->input(POS_INPUT), at::MemoryFormat::ChannelsLast)) {
    GRAPH_DEBUG(
        "insertPrePackedConvOpForNode: input is not ChannelsLast contiguous");
    return;
  }

  if (!tensorexpr::isContiguous(
          n->input(POS_WEIGHT), at::MemoryFormat::ChannelsLast)) {
    GRAPH_DEBUG(
        "insertPrePackedConvOpForNode: weight is not ChannelsLast contiguous");
    return;
  }

  // Leave depthwise conv2d to NNC
  if (tensorexpr::conv2dIsSupportedJit(n)) {
    GRAPH_DEBUG("insertPrePackedConvOpForNode: leave depthwise conv2d to NNC");
    return;
  }

  WithInsertPoint guard(n);
  auto graph = n->owningGraph();

  auto input_sizes = getSizesOf(n, POS_INPUT);
  IValue input_size_value(*input_sizes.concrete_sizes());
  auto input_size = graph->insertConstant(input_size_value);

  auto prepack_node = graph->create(
      Symbol::fromQualString("mkldnn_prepacked::conv2d_prepack"), 1);

  // skip input value
  for (auto i = 1; i < n->inputs().size(); i++) {
    Value* v = n->input(i);
    prepack_node->addInput(v);
  }
  prepack_node->addInput(input_size);
  auto attr = graph->insertConstant(IValue("none"));
  prepack_node->addInput(attr);
  std::vector<c10::optional<at::Scalar>> empty_scalars;
  auto scalars = graph->insertConstant(IValue(empty_scalars));
  prepack_node->addInput(scalars);

  c10::optional<std::string> empty_algorithm;
  auto algorithm = graph->insertConstant(IValue(empty_algorithm));
  prepack_node->addInput(algorithm);

  prepack_node->output()->setType(
      getCustomClass("__torch__.torch.classes.mkldnn.ConvOpContext"));
  graph->insertNode(prepack_node);

  auto prepack_conv = graph->insertNode(
      graph->create(Symbol::fromQualString("mkldnn_prepacked::conv2d_run"), 1));
  prepack_conv->addInput(n->input(0));
  prepack_conv->addInput(prepack_node->output());
  prepack_conv->output()->setType(n->output()->type()->cast<TensorType>());

  n->output()->replaceAllUsesWith(prepack_conv->output());
}

bool isTensorTypeCPU(Node* node) {
  for (const auto& input : node->inputs()) {
    auto type = input->type()->cast<TensorType>();
    if (!type) {
      continue;
    }
    auto device = type->device();
    if (!device) {
      return false;
    }
    if (!device->is_cpu()) {
      return false;
    }
  }
  return true;
}

void insertPrePackedConvOp(Block* b) {
  for (Node* n : b->nodes()) {
    for (Block* b : n->blocks()) {
      insertPrePackedConvOp(b);
    }

    if (n->kind() == aten::conv2d) {
      if (isTensorTypeCPU(n)) {
        insertPrePackedConvOpForNode(n);
      }
    }
  }
  EliminateDeadCode(b);
}

void insertMkldnnPrePackedConv2dOp(std::shared_ptr<Graph>& graph) {
  // Replace _convolution with conv2d
  graph_rewrite_helper::replaceConvolutionWithAtenConv(graph);

  insertPrePackedConvOp(graph->block());
}

void insertMkldnnPrePackedOps(std::shared_ptr<Graph>& graph) {
  insertMkldnnPrePackedConv2dOp(graph);
}

void insertMkldnnPrePackedOps(script::Module& module) {
  for (auto& method : module.get_methods()) {
    auto graph = method.graph();
    insertMkldnnPrePackedOps(graph);
  }
  for (script::Module m : module.children()) {
    insertMkldnnPrePackedOps(m);
  }
}

void FuseReluWithPackedOps(std::shared_ptr<Graph>& graph) {
  auto conv_op_rstring = at::jit::CodeTemplate(R"(
    graph(%input, %weight, %bias, %stride:int[], %padding:int[],
          %dilation:int[], %groups:int, %input_size:int[], %dummy_attr:str, %scalars_placeholder: Scalar?[], %algorithm_placeholder: str?):
        %packed_weight_bias = mkldnn_prepacked::conv2d_prepack(
            %weight, %bias, %stride, %padding, %dilation, %groups,
            %input_size, %dummy_attr, %scalars_placeholder, %algorithm_placeholder)
        %conv2d_res = mkldnn_prepacked::conv2d_run(%input, %packed_weight_bias)
        %res = aten::${op}(%conv2d_res)
        return (%res))");

  auto conv_op_fused_rstring = at::jit::CodeTemplate(R"(
    graph(%input, %weight, %bias, %stride:int[], %padding:int[],
          %dilation:int[], %groups:int, %input_size:int[], %dummy_attr:str, %scalars_placeholder: Scalar?[], %algorithm_placeholder: str?):
        %attr: str = prim::Constant[value="${op_attr}"]()
        %packed_weight_bias : __torch__.torch.classes.mkldnn.ConvOpContext = mkldnn_prepacked::conv2d_prepack(
            %weight, %bias, %stride, %padding, %dilation, %groups,
            %input_size, %attr, %scalars_placeholder, %algorithm_placeholder)
        %res = mkldnn_prepacked::conv2d_run(%input, %packed_weight_bias)
        return (%res))");

  for (auto const& it : mkldnn::fusion_rewrite_map) {
    std::string op = it.first;
    if (op == std::string("none")) {
      continue;
    }

    at::jit::TemplateEnv env;
    env.s("op", op);

    at::jit::TemplateEnv env_fused;
    env_fused.s("op_attr", op);

    SubgraphRewriter rewriter;
    rewriter.RegisterRewritePattern(
        conv_op_rstring.format(env), conv_op_fused_rstring.format(env_fused));

    auto filters = it.second;
    rewriter.runOnGraph(graph, filters);
  }
}

void FuseAddReluWithPackedOps(std::shared_ptr<Graph>& graph) {
  SubgraphRewriter rewriter_add_v1, rewriter_add_v2, rewriter_add_relu;
  // conv   Y
  //   \   /
  //    add
  // Y = conv_output + alpha*Y
  auto conv_add_v1 = R"(
    graph(%input, %weight, %bias, %accumu, %alpha, %stride:int[], %padding:int[], %dilation:int[], %groups:int, %input_size:int[],
          %attr_placeholder:str, %scalars_placeholder: Scalar?[], %algorithm_placeholder: str?):
        %packed_weight = mkldnn_prepacked::conv2d_prepack(%weight, %bias, %stride, %padding, %dilation, %groups, %input_size, %attr_placeholder, %scalars_placeholder, %algorithm_placeholder)
        %x = mkldnn_prepacked::conv2d_run(%input, %packed_weight)
        %res = aten::add(%x, %accumu, %alpha)
        return (%res))";

  //  Y     conv
  //   \   /
  //    add
  // Y = Y + alpha*conv_output, alpha should be one.
  auto conv_add_v2 = R"(
    graph(%input, %weight, %bias, %accumu, %alpha, %stride:int[], %padding:int[], %dilation:int[], %groups:int, %input_size:int[],
          %attr_placeholder:str, %scalars_placeholder: Scalar?[], %algorithm_placeholder: str?):
        %packed_weight = mkldnn_prepacked::conv2d_prepack(%weight, %bias, %stride, %padding, %dilation, %groups, %input_size, %attr_placeholder, %scalars_placeholder, %algorithm_placeholder)
        %x = mkldnn_prepacked::conv2d_run(%input, %packed_weight)
        %res = aten::add(%accumu, %x, %alpha)
        return (%res))";

  auto conv_add_fused = R"(
    graph(%input, %weight, %bias, %accumu, %alpha, %stride:int[], %padding:int[], %dilation:int[], %groups:int, %input_size:int[],
          %attr_placeholder:str, %scalars_placeholder: Scalar?[], %algorithm_placeholder: str?):
        %attr: str = prim::Constant[value="sum"]()
        %scalars: Scalar?[] = prim::ListConstruct(%alpha)
        %packed_weight = mkldnn_prepacked::conv2d_prepack(%weight, %bias, %stride, %padding, %dilation, %groups, %input_size, %attr, %scalars, %algorithm_placeholder)
        %res = mkldnn_prepacked::conv2d_sum_run(%input, %accumu, %packed_weight)
        return (%res))";

  auto conv_add_relu = R"(
    graph(%input, %weight, %bias, %accumu, %stride:int[], %padding:int[], %dilation:int[], %groups:int, %input_size:int[],
          %attr:str, %scalars: Scalar?[], %algorithm_placeholder: str?):
        %packed_weight = mkldnn_prepacked::conv2d_prepack(%weight, %bias, %stride, %padding, %dilation, %groups, %input_size, %attr, %scalars, %algorithm_placeholder)
        %x = mkldnn_prepacked::conv2d_sum_run(%input, %accumu, %packed_weight)
        %res = aten::relu(%x)
        return (%res))";

  auto conv_add_relu_fused = R"(
    graph(%input, %weight, %bias, %accumu, %stride:int[], %padding:int[], %dilation:int[], %groups:int, %input_size:int[],
          %attr:str, %scalars: Scalar?[], %algorithm_placeholder: str?):
        %attr_new: str = prim::Constant[value="sum_relu"]()
        %packed_weight = mkldnn_prepacked::conv2d_prepack(%weight, %bias, %stride, %padding, %dilation, %groups, %input_size, %attr_new, %scalars, %algorithm_placeholder)
        %res = mkldnn_prepacked::conv2d_sum_run(%input, %accumu, %packed_weight)
        return (%res))";

  // conv+add
  rewriter_add_v1.RegisterRewritePattern(conv_add_v1, conv_add_fused);
  rewriter_add_v2.RegisterRewritePattern(conv_add_v2, conv_add_fused);
  rewriter_add_relu.RegisterRewritePattern(conv_add_relu, conv_add_relu_fused);
  rewriter_add_v1.runOnGraph(graph, add_accumu_on_right);
  rewriter_add_v2.runOnGraph(graph, add_accumu_on_left);
  rewriter_add_relu.runOnGraph(graph);
}

void PrePackingOpsFolder(Block* b) {
  auto is_foldable_op = [](const Node* n) -> bool {
    return (
        n->kind() ==
        Symbol::fromQualString("mkldnn_prepacked::conv2d_prepack"));
  };

  std::unordered_set<Node*> nodes_to_delete;
  for (Node* n : b->nodes()) {
    for (Block* block : n->blocks()) {
      PrePackingOpsFolder(block);
    }
    if (is_foldable_op(n)) {
      auto optional_outputs = torch::jit::runNodeIfInputsAreConstant(n);
      if (optional_outputs) {
        auto outputs = optional_outputs.value();
        TORCH_CHECK(outputs.size() == 1, "Prepack ops have single output");
        Value* prepack_op_value = n->output(0);
        auto graph = n->owningGraph();
        WithInsertPoint ins(prepack_op_value->node());
        auto weak_class_obj =
            outputs[0].toObject()->copy_to_weak_compilation_ref();
        Value* packed_weight = graph->insertConstant(weak_class_obj)
                                   ->setType(n->output(0)->type());
        prepack_op_value->replaceAllUsesWith(packed_weight);
        nodes_to_delete.insert(n);
      }
    }
  }
  for (auto n : nodes_to_delete) {
    n->removeAllInputs();
  }
  for (auto n : nodes_to_delete) {
    n->destroy();
  }
}

void FoldPrePackingOps(std::shared_ptr<Graph>& graph) {
  PrePackingOpsFolder(graph->block());
}

void FuseConvWithEltwise(std::shared_ptr<Graph>& graph) {
  GRAPH_DEBUG(
      "Before insertMkldnnPrePackedOps. Beginning of FuseConvWithEltwise\n",
      *graph);
  insertMkldnnPrePackedOps(graph);
  GRAPH_DEBUG(
      "After insertMkldnnPrePackedOps, before FuseReluWithPackedOps\n", *graph);
  FuseReluWithPackedOps(graph);
  GRAPH_DEBUG(
      "After FuseReluWithPackedOps, before FuseAddReluWithPackedOps\n", *graph);
  FuseAddReluWithPackedOps(graph);
  GRAPH_DEBUG(
      "After FuseAddReluWithPackedOps, before ConstantPropagation\n", *graph);
  ConstantPropagation(graph);
  GRAPH_DEBUG("After ConstantPropagation, before FoldPrePackingOps\n", *graph);
  FoldPrePackingOps(graph);
  GRAPH_DEBUG("After FoldPrePackingOps. End of FuseConvWithEltwise\n", *graph);
}

#else

void FuseConvWithEltwise(std::shared_ptr<Graph>& graph) {
  GRAPH_DEBUG("MKLDNN Not enabled");
}

#endif // AT_MKLDNN_ENABLED()

} // namespace jit
} // namespace torch
