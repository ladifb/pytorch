#include "torch/csrc/jit/passes/graph_fuser.h"
#include "torch/csrc/jit/fusion_compiler.h"
#include "torch/csrc/jit/autodiff.h"
#include "torch/csrc/jit/assertions.h"
#include <unordered_map>

#ifdef USE_CUDA
  #include "cuda.h" // for CUDA_VERSION
#endif

namespace torch { namespace jit {

namespace {

// What is a simple mappable operator?  It is:
//    - Has an output with the same sizes as its input
//    - Single output
//    - Can handle non-contiguous input
//    - Produces contiguous output
// Some of these restrictions may be relaxable, but you should
// carefully read the code first, as we rely on these assumptions.
std::unordered_set<NodeKind> simple_mappable = {
  aten::__and__,
  aten::__lshift__,
  aten::__or__,
  aten::__rshift__,
  aten::__xor__,
  aten::abs,
  aten::acos,
  aten::add,
  aten::asin,
  aten::atan,
  aten::atan2,
  aten::ceil,
  aten::cos,
  aten::cosh,
  aten::div,
  aten::eq,
  aten::exp,
  aten::expm1,
  aten::floor,
  aten::fmod,
  aten::frac,
  aten::ge,
  aten::gt,
  aten::le,
  aten::lgamma,
  aten::log,
  aten::log10,
  aten::log1p,
  aten::log2,
  aten::lt,
  aten::max,
  aten::min,
  aten::mul,
  aten::ne,
  aten::neg,
  aten::pow,
  aten::reciprocal,
  aten::relu,
  aten::remainder,
  aten::round,
  aten::rsqrt,
  aten::sigmoid,
  aten::sin,
  aten::sinh,
  aten::sqrt,
  aten::sub,
  aten::tan,
  aten::tanh,
  aten::trunc,
  aten::type_as,
  aten::_sigmoid_backward,
  aten::_tanh_backward,
  // TODO support those
  //aten::clamp,
  //aten::lerp,
  aten::rand_like,
};

bool isSimpleMap(Node *node) {
  // TODO: use signature matching
  if(simple_mappable.count(node->kind()) == 0)
    return false;
  if((node->kind() == aten::min || node->kind() == aten::max) && node->inputs().size() == 1)
    return false;
  return true;
}


struct GraphFuser {
  Block * block;

  // Used to order nodes so we always consider producer-consumer fusions
  // in reverse topological order.
  // If topological_index[a] > topological_index[b] then a occurs after b.
  // Because nodes can be added to this graph during optimization, this mapping is not bijective.
  // Newly generated nodes will copy the location where they are inserted.
  std::unordered_map<Node*,size_t> topological_index;

  GraphFuser(Block * block)
  : block(block) {}

  at::optional<int> getDevice(Node * node) {
    if(node->kind() == prim::FusionGroup) {
      return node->i(attr::device);
    }
    if(auto tt = node->output()->type()->cast<TensorType>()) {
      return tt->device();
    }
    return at::nullopt;
  }
  // TODO: the fusion compiler has a lot of float-specific codegen
  // so for now we only consider nodes that operate on floating point numbers
  // and half values when running on a GPU with sufficient CUDA arch
  bool hasSupportedType(Value* node) {
    if (auto tt = node->type()->cast<TensorType>()) {
      if (tt->scalarType() == at::kFloat) return true;
      #ifdef USE_CUDA
        // Checks for half tensor on GPU
        if (tt->device() != kCPUDevice
          && CUDA_VERSION >= 9
          && tt->scalarType() == at::ScalarType::Half) {
          return true;
        }
      #endif
    }
    return false;
  }

  bool areTensorsOfSameShape(at::ArrayRef<Value*> values) {
    auto expected_type = values.at(0)->type()->cast<TensorType>();
    if (!expected_type) return false;
    for (Value * val : values) {
      auto val_type = val->type()->cast<TensorType>();
      if (!val_type) return false;
      if (expected_type->device() != val_type->device()) return false;
      if (expected_type->sizes() != val_type->sizes()) return false;
    }
    return true;
  }

  bool hasSupportedType(Node* node) {
    return areTensorsOfSameShape(node->inputs()) &&
           haveSupportedType(node->inputs()) &&
           haveSupportedType(node->outputs());
  }

  bool haveSupportedType(at::ArrayRef<Value*> list) {
    for (Value *v : list) {
      if (!hasSupportedType(v)) return false;
    }
    return true;
  }


  bool isFusable(Node * node) {
    if (node->owningBlock() != block) return false;
    if (node->kind() == prim::FusionGroup) return true;
    if (!isSimpleMap(node)) return false;

    if (node->matches("aten::add(Tensor self, Tensor other, *, Scalar alpha) -> Tensor", /*const=*/attr::alpha)) {
      std::vector<Value*> inputs {node->namedInput(attr::self), node->namedInput(attr::other)};
      return areTensorsOfSameShape(inputs) && haveSupportedType(inputs);
    } else if (node->matches("aten::lt(Tensor self, Tensor other) -> Tensor") ||
               node->matches("aten::le(Tensor self, Tensor other) -> Tensor") ||
               node->matches("aten::gt(Tensor self, Tensor other) -> Tensor") ||
               node->matches("aten::ge(Tensor self, Tensor other) -> Tensor") ||
               node->matches("aten::eq(Tensor self, Tensor other) -> Tensor") ||
               node->matches("aten::ne(Tensor self, Tensor other) -> Tensor")) {
      // comparison operators produce Byte type, and it's ok, check only inputs
      return areTensorsOfSameShape(node->inputs()) && haveSupportedType(node->inputs());
    } else if (node->matches("aten::type_as(Tensor self, Tensor other) -> Tensor")) {
      // type_as can have different input types as long as output is float, check only output
      return haveSupportedType(node->outputs());
    } else {
      return hasSupportedType(node);
    }
  }

  bool isFusableCatNode(Node * node) {
    if (node->kind() != aten::cat)
      return false;
    if (!node->is_constant(attr::dim))
      return false;

    auto tensors_node = node->namedInput(attr::tensors)->node();
    if (tensors_node->kind() != prim::ListConstruct) return false;
    // NB: Note that technically other uses of the list aren't a big problem for us.
    // It would be enough to place the prim::FusedConcat before the prim::ListConstruct, and
    // allUsersAreThisConsumerOrOccurAfterIt would still be satisfied. However, I don't expect this
    // to be necessary any time soon, and so we're simply assuming that we don't have to deal with that.
    if (tensors_node->output()->uses().size() > 1) return false;
    auto tensors = tensors_node->inputs();

    // Our fusion code assumes that all inputs have the same shapes, so we need to check this too.
    auto expected = tensors.at(0)->type()->cast<TensorType>();
    if (!expected) return false;
    return std::all_of(tensors.begin(), tensors.end(), [&expected](Value *v) {
        auto actual = v->type()->cast<TensorType>();
        return actual && actual->sizes() == expected->sizes();
    });
  }

  // Can this node produce an _output_ of a fusion group?
  // all Fusable nodes can do this, but additionally Concat, which normally cannot be fused
  // because it is not a simple map, can be put in a fusion group
  // as long as no items in the group read the output of concat
  bool isFusableAsExitNode(Node * node) {
    return isFusable(node) || isFusableCatNode(node);
  }

  // necessary condition for fusion. If all of the uses of producer are consumer
  // then it is safe to merge producer into consumer, because it doesn't have any other uses
  // If there are other uses, but they occur _after_ consumer, then we can still merge in producer
  // with consumer, by rewriting those later uses to use the version of producer generated by the fused blob
  // In this case, producer becomes an output of the fusion group.
  bool allUsersAreThisConsumerOrOccurAfterIt(Node * consumer, Value * producer) {
    auto defining_node = producer->node();
    for(auto o : defining_node->outputs()) {
      for(auto u : o->uses()) {
        if(u.user != consumer && topological_index.at(consumer) > topological_index.at(u.user))
          return false;
      }
    }
    return true;
  }
  bool allUsersAreThisConsumer(Node * consumer, Value * producer) {
    auto defining_node = producer->node();
    for(auto o : defining_node->outputs()) {
      for(auto u : o->uses()) {
        if(u.user != consumer)
          return false;
      }
    }
    return true;
  }

  bool shouldFuse(Node * consumer, Value * producer) {
    // this handles cases where producer can be moved _into_ the fusion group of consumer.
    // TODO: extend to fusion of consumer into _producer's_ fusion blob
    // if the consumer allInputsAreThisProducer(consumer,producer)
    // we can move the consumer up into the producer.
    // but this requires better handling of merging fusion groups so it is not done now
    at::optional<int> consumer_device = getDevice(consumer);
    Node *real_consumer = consumer->kind() == aten::cat ? consumer->namedInput(attr::tensors)->node() : consumer;
    return isFusable(producer->node()) &&
      allUsersAreThisConsumerOrOccurAfterIt(real_consumer, producer) &&
      consumer_device && consumer_device == getDevice(producer->node()) &&
      (*consumer_device != kCPUDevice || sharedFusionCompiler().canCompileOnCPU());
  }

  // insert a producer node into a consuming fusion group.
  // DOES NOT WORK if n is a consumer of an output of the fusion group
  // returns the node _inside_ the group that represents the node
  Graph & getSubgraph(Node * n) {
    JIT_ASSERT(n->kind() == prim::FusionGroup);
    return *n->g(attr::Subgraph);
  }

  void mergeFusionGroups(Node *consumer_group, Node *producer_group) {
    // Now we have two fusion groups!
    // Revert the fusion - place all inner nodes of producer back in the outer graph.
    std::vector<Node*> temporary_nodes;
    auto producer_subgraph = &getSubgraph(producer_group);

    // Initialize a map of inner graph values to outer graph values
    std::unordered_map<Value*, Value*> inner_to_outer;
    auto inner_inputs = producer_subgraph->inputs();
    auto outer_inputs = producer_group->inputs();
    for (size_t i = 0; i < inner_inputs.size(); ++i) {
      inner_to_outer[inner_inputs[i]] = outer_inputs[i];
    }

    // Clone all nodes
    for (auto inner : producer_subgraph->nodes()) {
      Node * outer = block->owningGraph()->createClone(inner, [&](Value * k) -> Value* {
        return inner_to_outer.at(k);
      });
      outer->insertBefore(producer_group);
      temporary_nodes.emplace_back(outer);
      auto inner_outputs = inner->outputs();
      auto outer_outputs = outer->outputs();
      for (size_t i = 0; i < inner_outputs.size(); ++i)
        inner_to_outer[inner_outputs[i]] = outer_outputs[i];
    }

    // Replace uses of producer_group outputs and destroy the producer
    auto subgraph_outputs = producer_subgraph->outputs();
    for (size_t i = 0; i < subgraph_outputs.size(); ++i) {
      auto outer_output = inner_to_outer.at(subgraph_outputs[i]);
      producer_group->outputs()[i]->replaceAllUsesWith(outer_output);
    }
    producer_group->destroy();
    producer_group = nullptr; // Just to get a clear error in case someone uses it

    // Inline the temporary nodes into the first group
    auto consumer_subgraph = &getSubgraph(consumer_group);
    for (auto it = temporary_nodes.rbegin(); it != temporary_nodes.rend(); ++it) {
      Node *node = *it;
      Node *merged = mergeNodeIntoGroup(consumer_group, node);
      // If any of the outputs are still used then we need to add them
      auto outputs = node->outputs();
      for (size_t i = 0; i < outputs.size(); ++i) {
        auto output = outputs[i];
        if (output->uses().size() == 0) continue;
        consumer_subgraph->registerOutput(merged->outputs()[i]);
        auto new_output = consumer_group->addOutput();
        output->replaceAllUsesWith(new_output);
        new_output->setType(output->type());
      }
      node->destroy();
    }
  }

  Node * mergeNodeIntoGroup(Node* group, Node * n) {
    JIT_ASSERT(n->kind() != prim::FusionGroup);
    auto & subgraph = getSubgraph(group);
    // map from nodes in the surrounding graph to parameters in the fusion
    // group's subgraph that correspond to them
    std::unordered_map<Value*,Value*> inputs_map;
    size_t i = 0;
    JIT_ASSERT(group->inputs().size() == subgraph.inputs().size());
    for(auto input : group->inputs()) {
      inputs_map[input] = subgraph.inputs()[i++];
    }
    // add n's inputs to the fusion group's input list if we don't already have them
    Node * insert_after = nullptr;
    for (auto input : n->inputs()) {
      if (inputs_map.count(input) == 0) {
        if (input->type()->isSubtypeOf(DynamicType::get())) {
          auto in_group = subgraph.addInput();
          in_group->setType(input->type());
          inputs_map[input] = in_group;
          group->addInput(input);
        } else {
          // We don't support passing in scalars as arguments to fused kernels, so we generally
          // don't allow fusing tensor-scalar operations unless the scalar is constant. In those
          // cases we inline the constants directly in the body of the fused group.
          JIT_ASSERT(input->node()->kind() == prim::Constant);
          Node * in_const = subgraph.createClone(input->node(), [](Value*) -> Value* { throw std::runtime_error("unexpected input"); });
          subgraph.prependNode(in_const);
          insert_after = in_const;
          inputs_map[input] = in_const->output();
        }
      }
    }
    // copy n into the graph, remapping its inputs to internal nodes
    Node * in_graph = subgraph.createClone(n,[&](Value * k)-> Value* {
      return inputs_map[k];
    });
    // if n is already an input to the fusion group,
    // we need to remove it because n is now inside the fusion group
    // remapping nodes that used the input to the newly-merged node
    // n is not an input when the fusion group is empty
    auto inputs = group->inputs();
    auto it = std::find(inputs.begin(), inputs.end(), n->output());
    if(it != inputs.end()) {
      size_t p = it - inputs.begin();
      group->removeInput(p);
      subgraph.inputs()[p]->replaceAllUsesWith(in_graph->output());
      subgraph.eraseInput(p);
    }
    return insert_after ? in_graph->insertAfter(insert_after) : subgraph.prependNode(in_graph);
  }

  // turn consumer node n into a fusion group with just n inside
  // to prepare for fusion and replace uses of n with the new group
  Node * createSingletonFusionGroup(Node * n) {
    auto group = block->owningGraph()->createFusionGroup(getDevice(n).value());
    // propogate position information for the new node so we can always
    // have a valid mapping
    topological_index[group] = topological_index[n];
    group->insertBefore(n);
    Node * mergedNode = mergeNodeIntoGroup(group,n);
    getSubgraph(group).registerOutput(mergedNode->output());
    auto sel = group->addOutput();
    sel->copyMetadata(n->output());
    n->replaceAllUsesWith(group);
    n->destroy();
    return group;
  }
  void insertAfter(Node * n, Node * after) {
    n->insertAfter(after);
    topological_index[n] = topological_index[after];
  }

  void insertAt(Node ** insertion_point, Node * n) {
    insertAfter(n, *insertion_point);
    *insertion_point = n;
  }

  Node * fuse(Node * consumer, Value * producer) {
    auto group = consumer;
    if (consumer->kind() == aten::cat) {
      Graph * graph = consumer->owningGraph();
      Node * list_construct = consumer->namedInput(attr::tensors)->node();
      int64_t dim = consumer->get<int64_t>(attr::dim).value();

      Node * fused_cat = graph->create(prim::FusedConcat, list_construct->inputs())->i_(attr::dim, dim);
      fused_cat->insertBefore(list_construct);
      fused_cat->output()->copyMetadata(consumer->output());
      consumer->output()->replaceAllUsesWith(fused_cat->output());
      topological_index[fused_cat] = topological_index[list_construct];

      // NB: this deletes the fused_cat node from the original graph
      group = createSingletonFusionGroup(fused_cat);
      consumer->destroy();
      if (list_construct->output()->uses().empty()) {
        list_construct->destroy();
      }
    } else if (consumer->kind() != prim::FusionGroup) {
      group = createSingletonFusionGroup(consumer);
    }
    if (producer->node()->kind() == prim::FusionGroup) {
      mergeFusionGroups(group, producer->node());
      return group;
    }
    Node * merged = mergeNodeIntoGroup(group, producer->node());
    // remaining uses of this producer can occur because we allow
    // fusion in cases where uses remain after the consumer
    // if these exist, re-route them to the version of producer
    // created in FusionGroup
    if(producer->uses().size() != 0) {
      getSubgraph(group).registerOutput(merged->output());
      Value * new_producer = group->addOutput();
      new_producer->copyMetadata(producer);
      producer->replaceAllUsesWith(new_producer);
    }
    producer->node()->destroy();
    return group;
  }

  // TODO: desugar chunks into splits and then remove this special case
  bool isChunk(Node * node) {
    return node->kind() == aten::split || node->kind() == aten::chunk;
  }

  // in places where op can be fused into a consumer but chunk is in the way
  // distribute chunk to op's operands:
  // replace a,b = chunk(op(x,y,z)) with:
  // x0,x1 = chunk(x) (x0 has a's type, x1 has b's type)
  // y0,y1 = chunk(y) (y0 has a's type, y1 has b's type)
  // z0,z1 = chunk(z) (z0 has a's type, z1 has b's type)
  // a = op(x0,y0,z0) (a,b have their same size but are now contiguous)
  // b = op(x1,y1,x1)
  //
  // NB: Chunk motion only occurs with fusable consumers, which implies
  // that there is always some other operation, e.g., a+b, that happens
  // after the chunk, and will be put into the fusion group. This is
  // important, because distributing the chunk changes the contiguity
  // of a and b, and so the results would be invalid, except that we know
  // that simple_mappable operations will restore contiguity before
  // we exit the fusion group.

  bool tryToMoveChunk(Node * consumer, Value * producer) {
    // is the output from a chunk node?
    auto * chunk = producer->node();
    if (!isChunk(chunk))
      return false;
    // and the thing being chunked is fusable into the consumer
    Value * producer_for_chunk = chunk->namedInput(attr::self);
    if (!isFusable(producer_for_chunk->node()) || !allUsersAreThisConsumer(chunk,producer_for_chunk))
      return false;
    // and all uses of the chunk are in this consumer
    for (auto s : chunk->outputs()) {
      for (auto u : s->uses()) {
        if (u.user != consumer)
          return false;
      }
    }

    // multiple return operators
    Node * producer_for_chunk_node = producer_for_chunk->node();
    JIT_ASSERT(producer_for_chunk_node->outputs().size() == 1);
    // Make sure we lay out the nodes in the correct topological order.
    // TODO: There should be some more enshrined way to do this
    Node * insertion_point = chunk;

    // apply chunk to each of op's operands
    // chunked_inputs[input_nr][chunk_output_idx]
    //  = Node* for chunk_output_idx'th output of the chunk(inputs[input_nr])
    std::vector<std::vector<Value*>> chunked_inputs;
    for (auto input : producer_for_chunk_node->inputs()) {
      auto input_type = input->type()->cast<TensorType>();
      // XXX: we only work with pointwise ops in here, so we know it is valid to push
      // the concat only through tensor arguments (and all other args can be safely ignored).
      if (!input_type)
        continue;
      // NB: I decided not to use cloneFrom here, because if we make cloneFrom
      // copy selects one day, it is definitely not what you want here (selects
      // have different types).
      // TODO: Perhaps we should use cloneFrom now, as it seems unlikely
      // to copy select nodes now that we have refactored to have a Value
      // distinct from Node.
      Node * input_chunk = block->owningGraph()->create(aten::chunk, 0);
      input_chunk->addInput(input);
      input_chunk->addInput(chunk->namedInput(attr::chunks));
      input_chunk->addInput(chunk->namedInput(attr::dim));
      insertAt(&insertion_point, input_chunk);

      chunked_inputs.emplace_back(); // alas, to not be C++17
      for (auto chunk_sel : chunk->outputs()) {
          auto chunk_sel_type = chunk_sel->type()->expect<TensorType>();
          Value * input_chunk_sel = input_chunk->addOutput();
          input_chunk_sel->setType(
            input_type->withSizesStrides(chunk_sel_type->sizes(),
                                         chunk_sel_type->strides()));
          chunked_inputs.back().push_back(input_chunk_sel);
      }
    }

    // apply the op to each chunk of the chunked operands,
    // and then rewrite the graph to use them!
    for (auto chunk_sel : chunk->outputs()) {
      auto original_inputs = producer_for_chunk_node->inputs();
      Node * chunked_op = block->owningGraph()->create(producer_for_chunk_node->kind());
      chunked_op->copyAttributes(*producer_for_chunk_node);
      // Invariant: mappable operators always produce contiguous output
      chunked_op->output()->setType(chunk_sel->type()->cast<TensorType>()->contiguous());
      auto chunked_inputs_it = chunked_inputs.begin();
      for (size_t i = 0; i < original_inputs.size(); ++i) {
        if (original_inputs[i]->type()->isSubtypeOf(DynamicType::get())) {
          JIT_ASSERT(chunked_inputs_it != chunked_inputs.end());
          chunked_op->addInput(chunked_inputs_it->at(chunk_sel->offset()));
          ++chunked_inputs_it;
        } else {
          chunked_op->addInput(original_inputs[i]);
        }
      }
      insertAt(&insertion_point, chunked_op);
      chunk_sel->replaceAllUsesWith(chunked_op->output());
    }
    chunk->destroy();
    producer_for_chunk_node->destroy();
    return true;
  }

  // returns where to continue scanning, and whether any fusion was made
  std::pair<graph_node_list::iterator, bool> scanNode(Node * consumer) {
    auto stage_guard = block->owningGraph()->setStageTemporary(consumer->stage());
    if(isFusableAsExitNode(consumer)) {
      value_list inputs;
      auto consumer_inputs = consumer->kind() == aten::cat ?
        consumer->namedInput(attr::tensors)->node()->inputs() :
        consumer->inputs();
      // handle inputs in reverse topological order as well...
      // otherwise in f(a,a+b) it will appear a is used twice if we consider
      // the f-a fusion before the f-(a+b) fusion first.
      for(auto i : consumer_inputs) {
        if (i->node()->owningBlock() == block) {
          inputs.push_back(i);
          JIT_ASSERT(topological_index.count(i->node()) > 0);
        }
      }
      std::sort(inputs.begin(), inputs.end(), [&](Value * a, Value * b) {
        return topological_index.at(a->node()) > topological_index.at(b->node());
      });
      for(auto producer : inputs) {
        // Don't fuse accross stage boundaries
        if (producer->stage() != consumer->stage()) continue;
        if(tryToMoveChunk(consumer,producer)) {
          // the chunk before this consumer was re-arranged to allow fusion,
          // we scan this consumer again to perform the fusion
          return std::make_pair(consumer->reverseIterator(), true);
        }
        if(shouldFuse(consumer, producer)) {
          auto fusion_group = fuse(consumer,producer);
          // after fusion, consumer moves into a FusionGroup, so inputs is no longer valid
          // so we rescan the new FusionGroup for more fusions...
          return std::make_pair(fusion_group->reverseIterator(), true);
        }
      }
    }
    return std::make_pair(++consumer->reverseIterator(), false);
  }

  void run() {
    for(auto p : block->inputs()) {
      topological_index[p->node()] = 0;
    }
    size_t i = 1;
    for(auto consumer : block->nodes()) {
      topological_index[consumer] = i++;
    }
    topological_index[block->return_node()] = i++;

    // Run the pass until no changes are made.
    // This is neccessary, because the algorithm can miss out on certain fusion
    // opportunities if ran only once. Consider this graph:
    //
    // %1 = f(...)
    // %2 = g(%1)
    // %3 = h(%1)
    // %4 = l(%3)
    // return (%4, %2)
    //
    // where f, g, h, l are simple map ops.
    // The first iteration will fuse %4 and %3, and see that %1 is an input, but
    // can't be fused, because it has a different use before the fusion group
    // in our topological ordering. Then, %2 will be considered, and fused with %1.
    // If we do another iteration, the algorithm will consider the fusion of these
    // two groups and fix the situation.
    bool any_changed = true;
    while (any_changed) {
      any_changed = false;
      for (auto it = block->nodes().rbegin(); it != block->nodes().rend();) {
        bool changed;
        std::tie(it, changed) = scanNode(*it);
        any_changed |= changed;
      }
    }
    for (Node * node : block->nodes()) {
      for (Block * sub_block : node->blocks()) {
        GraphFuser(sub_block).run();
      }
    }
  }
};

} // anonymous namespace

void FuseGraph(std::shared_ptr<Graph>& graph) {
  GraphFuser(graph->block()).run();
}

}}
