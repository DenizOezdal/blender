/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "MOD_nodes_evaluator.hh"

#include "BKE_type_conversions.hh"

#include "NOD_geometry_exec.hh"
#include "NOD_socket_declarations.hh"

#include "DEG_depsgraph_query.h"

#include "FN_field.hh"
#include "FN_field_cpp_type.hh"
#include "FN_generic_value_map.hh"
#include "FN_multi_function.hh"

#include "BLT_translation.h"

#include "BLI_enumerable_thread_specific.hh"
#include "BLI_stack.hh"
#include "BLI_task.h"
#include "BLI_task.hh"
#include "BLI_vector_set.hh"

#include <chrono>

namespace blender::modifiers::geometry_nodes {

using fn::CPPType;
using fn::Field;
using fn::GField;
using fn::GValueMap;
using fn::GVArray;
using fn::ValueOrField;
using fn::ValueOrFieldCPPType;
using nodes::GeoNodeExecParams;
using namespace fn::multi_function_types;

enum class ValueUsage : uint8_t {
  /* The value is definitely used. */
  Required,
  /* The value may be used. */
  Maybe,
  /* The value will definitely not be used. */
  Unused,
};

struct SingleInputValue {
  /**
   * Points either to null or to a value of the type of input.
   */
  void *value = nullptr;
};

struct MultiInputValue {
  /**
   * Ordered sockets connected to this multi-input.
   */
  Vector<DSocket> origins;
  /**
   * A value for every origin socket. The order is determined by #origins.
   * Note, the same origin can occur multiple times. However, it is guaranteed that values coming
   * from the same origin have the same value (the pointer is different, but they point to values
   * that would compare equal).
   */
  Vector<void *> values;
  /**
   * Number of non-null values.
   */
  int provided_value_count = 0;

  bool all_values_available() const
  {
    return this->missing_values() == 0;
  }

  int missing_values() const
  {
    return this->values.size() - this->provided_value_count;
  }

  void add_value(const DSocket origin, void *value)
  {
    const int index = this->find_available_index(origin);
    this->values[index] = value;
    this->provided_value_count++;
  }

 private:
  int find_available_index(DSocket origin) const
  {
    for (const int i : origins.index_range()) {
      if (values[i] != nullptr) {
        continue;
      }
      if (origins[i] != origin) {
        continue;
      }
      return i;
    }
    BLI_assert_unreachable();
    return -1;
  }
};

struct InputState {

  /**
   * Type of the socket. If this is null, the socket should just be ignored.
   */
  const CPPType *type = nullptr;

  /**
   * Value of this input socket. By default, the value is empty. When other nodes are done
   * computing their outputs, the computed values will be forwarded to linked input sockets.
   * The value will then live here until it is consumed by the node or it was found that the value
   * is not needed anymore.
   * Whether the `single` or `multi` value is used depends on the socket.
   */
  union {
    SingleInputValue *single;
    MultiInputValue *multi;
  } value;

  /**
   * How the node intends to use this input. By default all inputs may be used. Based on which
   * outputs are used, a node can tell the evaluator that an input will definitely be used or is
   * never used. This allows the evaluator to free values early, avoid copies and other unnecessary
   * computations.
   */
  ValueUsage usage = ValueUsage::Maybe;

  /**
   * True when this input is/was used for an execution. While a node is running, only the inputs
   * that have this set to true are allowed to be used. This makes sure that inputs created while
   * the node is running correctly trigger the node to run again. Furthermore, it gives the node a
   * consistent view of which inputs are available that does not change unexpectedly.
   *
   * While the node is running, this can be checked without a lock, because no one is writing to
   * it. If this is true, the value can be read without a lock as well, because the value is not
   * changed by others anymore.
   */
  bool was_ready_for_execution = false;

  /**
   * True when this input has to be computed for logging/debugging purposes, regardless of whether
   * it is needed for some output.
   */
  bool force_compute = false;
};

struct OutputState {
  /**
   * If this output has been computed and forwarded already. If this is true, the value is not
   * computed/forwarded again.
   */
  bool has_been_computed = false;

  /**
   * Keeps track of how the output value is used. If a connected input becomes required, this
   * output has to become required as well. The output becomes ignored when it has zero potential
   * users that are counted below.
   */
  ValueUsage output_usage = ValueUsage::Maybe;

  /**
   * This is a copy of `output_usage` that is done right before node execution starts. This is
   * done so that the node gets a consistent view of what outputs are used, even when this changes
   * while the node is running (the node might be reevaluated in that case).
   *
   * While the node is running, this can be checked without a lock, because no one is writing to
   * it.
   */
  ValueUsage output_usage_for_execution = ValueUsage::Maybe;

  /**
   * Counts how many times the value from this output might be used. If this number reaches zero,
   * the output is not needed anymore.
   */
  int potential_users = 0;
};

enum class NodeScheduleState {
  /**
   * Default state of every node.
   */
  NotScheduled,
  /**
   * The node has been added to the task group and will be executed by it in the future.
   */
  Scheduled,
  /**
   * The node is currently running.
   */
  Running,
  /**
   * The node is running and has been rescheduled while running. In this case the node will run
   * again. However, we don't add it to the task group immediately, because then the node might run
   * twice at the same time, which is not allowed. Instead, once the node is done running, it will
   * reschedule itself.
   */
  RunningAndRescheduled,
};

struct NodeState {
  /**
   * Needs to be locked when any data in this state is accessed that is not explicitly marked as
   * otherwise.
   */
  std::mutex mutex;

  /**
   * States of the individual input and output sockets. One can index into these arrays without
   * locking. However, to access the data inside a lock is generally necessary.
   *
   * These spans have to be indexed with the socket index. Unavailable sockets have a state as
   * well. Maybe we can handle unavailable sockets differently in Blender in general, so I did not
   * want to add complexity around it here.
   */
  MutableSpan<InputState> inputs;
  MutableSpan<OutputState> outputs;

  /**
   * Most nodes have inputs that are always required. Those have special handling to avoid an extra
   * call to the node execution function.
   */
  bool non_lazy_inputs_handled = false;

  /**
   * Used to check that nodes that don't support laziness do not run more than once.
   */
  bool has_been_executed = false;

  /**
   * Becomes true when the node will never be executed again and its inputs are destructed.
   * Generally, a node has finished once all of its outputs with (potential) users have been
   * computed.
   */
  bool node_has_finished = false;

  /**
   * Counts the number of values that still have to be forwarded to this node until it should run
   * again. It counts values from a multi input socket separately.
   * This is used as an optimization so that nodes are not scheduled unnecessarily in many cases.
   */
  int missing_required_inputs = 0;

  /**
   * A node is always in one specific schedule state. This helps to ensure that the same node does
   * not run twice at the same time accidentally.
   */
  NodeScheduleState schedule_state = NodeScheduleState::NotScheduled;
};

/**
 * Container for a node and its state. Packing them into a single struct allows the use of
 * `VectorSet` instead of a `Map` for `node_states_` which simplifies parallel loops over all
 * states.
 *
 * Equality operators and a hash function for `DNode` are provided so that one can lookup this type
 * in `node_states_` just with a `DNode`.
 */
struct NodeWithState {
  DNode node;
  /* Store a pointer instead of `NodeState` directly to keep it small and movable. */
  NodeState *state = nullptr;

  friend bool operator==(const NodeWithState &a, const NodeWithState &b)
  {
    return a.node == b.node;
  }

  friend bool operator==(const NodeWithState &a, const DNode &b)
  {
    return a.node == b;
  }

  friend bool operator==(const DNode &a, const NodeWithState &b)
  {
    return a == b.node;
  }

  uint64_t hash() const
  {
    return node.hash();
  }

  static uint64_t hash_as(const DNode &node)
  {
    return node.hash();
  }
};

class GeometryNodesEvaluator;

/**
 * Utility class that wraps a node whose state is locked. Having this is a separate class is useful
 * because it allows methods to communicate that they expect the node to be locked.
 */
class LockedNode : NonCopyable, NonMovable {
 public:
  /**
   * This is the node that is currently locked.
   */
  const DNode node;
  NodeState &node_state;

  /**
   * Used to delay notifying (and therefore locking) other nodes until the current node is not
   * locked anymore. This might not be strictly necessary to avoid deadlocks in the current code,
   * but it is a good measure to avoid accidentally adding a deadlock later on. By not locking
   * more than one node per thread at a time, deadlocks are avoided.
   *
   * The notifications will be send right after the node is not locked anymore.
   */
  Vector<DOutputSocket> delayed_required_outputs;
  Vector<DOutputSocket> delayed_unused_outputs;
  Vector<DNode> delayed_scheduled_nodes;

  LockedNode(const DNode node, NodeState &node_state) : node(node), node_state(node_state)
  {
  }
};

static const CPPType *get_socket_cpp_type(const SocketRef &socket)
{
  const bNodeSocketType *typeinfo = socket.typeinfo();
  if (typeinfo->geometry_nodes_cpp_type == nullptr) {
    return nullptr;
  }
  const CPPType *type = typeinfo->geometry_nodes_cpp_type;
  if (type == nullptr) {
    return nullptr;
  }
  /* The evaluator only supports types that have special member functions. */
  if (!type->has_special_member_functions()) {
    return nullptr;
  }
  return type;
}

static const CPPType *get_socket_cpp_type(const DSocket socket)
{
  return get_socket_cpp_type(*socket.socket_ref());
}

/**
 * \note This is not supposed to be a long term solution. Eventually we want that nodes can
 * specify more complex defaults (other than just single values) in their socket declarations.
 */
static bool get_implicit_socket_input(const SocketRef &socket, void *r_value)
{
  const NodeRef &node = socket.node();
  const nodes::NodeDeclaration *node_declaration = node.declaration();
  if (node_declaration == nullptr) {
    return false;
  }
  const nodes::SocketDeclaration &socket_declaration = *node_declaration->inputs()[socket.index()];
  if (socket_declaration.input_field_type() == nodes::InputSocketFieldType::Implicit) {
    const bNode &bnode = *socket.bnode();
    if (socket.typeinfo()->type == SOCK_VECTOR) {
      if (bnode.type == GEO_NODE_SET_CURVE_HANDLES) {
        StringRef side = ((NodeGeometrySetCurveHandlePositions *)bnode.storage)->mode ==
                                 GEO_NODE_CURVE_HANDLE_LEFT ?
                             "handle_left" :
                             "handle_right";
        new (r_value) ValueOrField<float3>(bke::AttributeFieldInput::Create<float3>(side));
        return true;
      }
      if (bnode.type == GEO_NODE_EXTRUDE_MESH) {
        new (r_value)
            ValueOrField<float3>(Field<float3>(std::make_shared<bke::NormalFieldInput>()));
        return true;
      }
      new (r_value) ValueOrField<float3>(bke::AttributeFieldInput::Create<float3>("position"));
      return true;
    }
    if (socket.typeinfo()->type == SOCK_INT) {
      if (ELEM(bnode.type, FN_NODE_RANDOM_VALUE, GEO_NODE_INSTANCE_ON_POINTS)) {
        new (r_value)
            ValueOrField<int>(Field<int>(std::make_shared<bke::IDAttributeFieldInput>()));
        return true;
      }
      new (r_value) ValueOrField<int>(Field<int>(std::make_shared<fn::IndexFieldInput>()));
      return true;
    }
  }
  return false;
}

static void get_socket_value(const SocketRef &socket, void *r_value)
{
  if (get_implicit_socket_input(socket, r_value)) {
    return;
  }

  const bNodeSocketType *typeinfo = socket.typeinfo();
  typeinfo->get_geometry_nodes_cpp_value(*socket.bsocket(), r_value);
}

static bool node_supports_laziness(const DNode node)
{
  return node->typeinfo()->geometry_node_execute_supports_laziness;
}

struct NodeTaskRunState {
  /** The node that should be run on the same thread after the current node finished. */
  DNode next_node_to_run;
};

/** Implements the callbacks that might be called when a node is executed. */
class NodeParamsProvider : public nodes::GeoNodeExecParamsProvider {
 private:
  GeometryNodesEvaluator &evaluator_;
  NodeState &node_state_;
  NodeTaskRunState *run_state_;

 public:
  NodeParamsProvider(GeometryNodesEvaluator &evaluator,
                     DNode dnode,
                     NodeState &node_state,
                     NodeTaskRunState *run_state);

  bool can_get_input(StringRef identifier) const override;
  bool can_set_output(StringRef identifier) const override;
  GMutablePointer extract_input(StringRef identifier) override;
  Vector<GMutablePointer> extract_multi_input(StringRef identifier) override;
  GPointer get_input(StringRef identifier) const override;
  GMutablePointer alloc_output_value(const CPPType &type) override;
  void set_output(StringRef identifier, GMutablePointer value) override;
  void set_input_unused(StringRef identifier) override;
  bool output_is_required(StringRef identifier) const override;

  bool lazy_require_input(StringRef identifier) override;
  bool lazy_output_is_required(StringRef identifier) const override;

  void set_default_remaining_outputs() override;
};

class GeometryNodesEvaluator {
 private:
  /**
   * This allocator lives on after the evaluator has been destructed. Therefore outputs of the
   * entire evaluator should be allocated here.
   */
  LinearAllocator<> &outer_allocator_;
  /**
   * A local linear allocator for each thread. Only use this for values that do not need to live
   * longer than the lifetime of the evaluator itself. Considerations for the future:
   * - We could use an allocator that can free here, some temporary values don't live long.
   * - If we ever run into false sharing bottlenecks, we could use local allocators that allocate
   *   on cache line boundaries. Note, just because a value is allocated in one specific thread,
   *   does not mean that it will only be used by that thread.
   */
  threading::EnumerableThreadSpecific<LinearAllocator<>> local_allocators_;

  /**
   * Every node that is reachable from the output gets its own state. Once all states have been
   * constructed, this map can be used for lookups from multiple threads.
   */
  VectorSet<NodeWithState> node_states_;

  /**
   * Contains all the tasks for the nodes that are currently scheduled.
   */
  TaskPool *task_pool_ = nullptr;

  GeometryNodesEvaluationParams &params_;
  const blender::bke::DataTypeConversions &conversions_;

  friend NodeParamsProvider;

 public:
  GeometryNodesEvaluator(GeometryNodesEvaluationParams &params)
      : outer_allocator_(params.allocator),
        params_(params),
        conversions_(blender::bke::get_implicit_type_conversions())
  {
  }

  void execute()
  {
    task_pool_ = BLI_task_pool_create(this, TASK_PRIORITY_HIGH);

    this->create_states_for_reachable_nodes();
    this->forward_group_inputs();
    this->schedule_initial_nodes();

    /* This runs until all initially requested inputs have been computed. */
    BLI_task_pool_work_and_wait(task_pool_);
    BLI_task_pool_free(task_pool_);

    this->extract_group_outputs();
    this->destruct_node_states();
  }

  void create_states_for_reachable_nodes()
  {
    /* This does a depth first search for all the nodes that are reachable from the group
     * outputs. This finds all nodes that are relevant. */
    Stack<DNode> nodes_to_check;
    /* Start at the output sockets. */
    for (const DInputSocket &socket : params_.output_sockets) {
      nodes_to_check.push(socket.node());
    }
    for (const DSocket &socket : params_.force_compute_sockets) {
      nodes_to_check.push(socket.node());
    }
    /* Use the local allocator because the states do not need to outlive the evaluator. */
    LinearAllocator<> &allocator = local_allocators_.local();
    while (!nodes_to_check.is_empty()) {
      const DNode node = nodes_to_check.pop();
      if (node_states_.contains_as(node)) {
        /* This node has been handled already. */
        continue;
      }
      /* Create a new state for the node. */
      NodeState &node_state = *allocator.construct<NodeState>().release();
      node_states_.add_new({node, &node_state});

      /* Push all linked origins on the stack. */
      for (const InputSocketRef *input_ref : node->inputs()) {
        const DInputSocket input{node.context(), input_ref};
        input.foreach_origin_socket(
            [&](const DSocket origin) { nodes_to_check.push(origin.node()); });
      }
    }

    /* Initialize the more complex parts of the node states in parallel. At this point no new
     * node states are added anymore, so it is safe to lookup states from `node_states_` from
     * multiple threads. */
    threading::parallel_for(
        IndexRange(node_states_.size()), 50, [&, this](const IndexRange range) {
          LinearAllocator<> &allocator = this->local_allocators_.local();
          for (const NodeWithState &item : node_states_.as_span().slice(range)) {
            this->initialize_node_state(item.node, *item.state, allocator);
          }
        });

    /* Mark input sockets that have to be computed. */
    for (const DSocket &socket : params_.force_compute_sockets) {
      NodeState &node_state = *node_states_.lookup_key_as(socket.node()).state;
      if (socket->is_input()) {
        node_state.inputs[socket->index()].force_compute = true;
      }
    }
  }

  void initialize_node_state(const DNode node, NodeState &node_state, LinearAllocator<> &allocator)
  {
    /* Construct arrays of the correct size. */
    node_state.inputs = allocator.construct_array<InputState>(node->inputs().size());
    node_state.outputs = allocator.construct_array<OutputState>(node->outputs().size());

    /* Initialize input states. */
    for (const int i : node->inputs().index_range()) {
      InputState &input_state = node_state.inputs[i];
      const DInputSocket socket = node.input(i);
      if (!socket->is_available()) {
        /* Unavailable sockets should never be used. */
        input_state.type = nullptr;
        input_state.usage = ValueUsage::Unused;
        continue;
      }
      const CPPType *type = get_socket_cpp_type(socket);
      input_state.type = type;
      if (type == nullptr) {
        /* This is not a known data socket, it shouldn't be used. */
        input_state.usage = ValueUsage::Unused;
        continue;
      }
      /* Construct the correct struct that can hold the input(s). */
      if (socket->is_multi_input_socket()) {
        input_state.value.multi = allocator.construct<MultiInputValue>().release();
        MultiInputValue &multi_value = *input_state.value.multi;
        /* Count how many values should be added until the socket is complete. */
        socket.foreach_origin_socket([&](DSocket origin) { multi_value.origins.append(origin); });
        /* If no links are connected, we do read the value from socket itself. */
        if (multi_value.origins.is_empty()) {
          multi_value.origins.append(socket);
        }
        multi_value.values.resize(multi_value.origins.size(), nullptr);
      }
      else {
        input_state.value.single = allocator.construct<SingleInputValue>().release();
      }
    }
    /* Initialize output states. */
    for (const int i : node->outputs().index_range()) {
      OutputState &output_state = node_state.outputs[i];
      const DOutputSocket socket = node.output(i);
      if (!socket->is_available()) {
        /* Unavailable outputs should never be used. */
        output_state.output_usage = ValueUsage::Unused;
        continue;
      }
      const CPPType *type = get_socket_cpp_type(socket);
      if (type == nullptr) {
        /* Non data sockets should never be used. */
        output_state.output_usage = ValueUsage::Unused;
        continue;
      }
      /* Count the number of potential users for this socket. */
      socket.foreach_target_socket(
          [&, this](const DInputSocket target_socket,
                    const DOutputSocket::TargetSocketPathInfo &UNUSED(path_info)) {
            const DNode target_node = target_socket.node();
            if (!this->node_states_.contains_as(target_node)) {
              /* The target node is not computed because it is not computed to the output. */
              return;
            }
            output_state.potential_users += 1;
          });
      if (output_state.potential_users == 0) {
        /* If it does not have any potential users, it is unused. It might become required again in
         * `schedule_initial_nodes`. */
        output_state.output_usage = ValueUsage::Unused;
      }
    }
  }

  void destruct_node_states()
  {
    threading::parallel_for(
        IndexRange(node_states_.size()), 50, [&, this](const IndexRange range) {
          for (const NodeWithState &item : node_states_.as_span().slice(range)) {
            this->destruct_node_state(item.node, *item.state);
          }
        });
  }

  void destruct_node_state(const DNode node, NodeState &node_state)
  {
    /* Need to destruct stuff manually, because it's allocated by a custom allocator. */
    for (const int i : node->inputs().index_range()) {
      InputState &input_state = node_state.inputs[i];
      if (input_state.type == nullptr) {
        continue;
      }
      const InputSocketRef &socket_ref = node->input(i);
      if (socket_ref.is_multi_input_socket()) {
        MultiInputValue &multi_value = *input_state.value.multi;
        for (void *value : multi_value.values) {
          if (value != nullptr) {
            input_state.type->destruct(value);
          }
        }
        multi_value.~MultiInputValue();
      }
      else {
        SingleInputValue &single_value = *input_state.value.single;
        void *value = single_value.value;
        if (value != nullptr) {
          input_state.type->destruct(value);
        }
        single_value.~SingleInputValue();
      }
    }

    destruct_n(node_state.inputs.data(), node_state.inputs.size());
    destruct_n(node_state.outputs.data(), node_state.outputs.size());

    node_state.~NodeState();
  }

  void forward_group_inputs()
  {
    for (auto &&item : params_.input_values.items()) {
      const DOutputSocket socket = item.key;
      GMutablePointer value = item.value;

      const DNode node = socket.node();
      if (!node_states_.contains_as(node)) {
        /* The socket is not connected to any output. */
        this->log_socket_value({socket}, value);
        value.destruct();
        continue;
      }
      this->forward_output(socket, value, nullptr);
    }
  }

  void schedule_initial_nodes()
  {
    for (const DInputSocket &socket : params_.output_sockets) {
      const DNode node = socket.node();
      NodeState &node_state = this->get_node_state(node);
      this->with_locked_node(node, node_state, nullptr, [&](LockedNode &locked_node) {
        /* Setting an input as required will schedule any linked node. */
        this->set_input_required(locked_node, socket);
      });
    }
    for (const DSocket socket : params_.force_compute_sockets) {
      const DNode node = socket.node();
      NodeState &node_state = this->get_node_state(node);
      this->with_locked_node(node, node_state, nullptr, [&](LockedNode &locked_node) {
        if (socket->is_input()) {
          this->set_input_required(locked_node, DInputSocket(socket));
        }
        else {
          OutputState &output_state = node_state.outputs[socket->index()];
          output_state.output_usage = ValueUsage::Required;
          this->schedule_node(locked_node);
        }
      });
    }
  }

  void schedule_node(LockedNode &locked_node)
  {
    switch (locked_node.node_state.schedule_state) {
      case NodeScheduleState::NotScheduled: {
        /* The node will be scheduled once it is not locked anymore. We could schedule the node
         * right here, but that would result in a deadlock if the task pool decides to run the task
         * immediately (this only happens when Blender is started with a single thread). */
        locked_node.node_state.schedule_state = NodeScheduleState::Scheduled;
        locked_node.delayed_scheduled_nodes.append(locked_node.node);
        break;
      }
      case NodeScheduleState::Scheduled: {
        /* Scheduled already, nothing to do. */
        break;
      }
      case NodeScheduleState::Running: {
        /* Reschedule node while it is running.
         * The node will reschedule itself when it is done. */
        locked_node.node_state.schedule_state = NodeScheduleState::RunningAndRescheduled;
        break;
      }
      case NodeScheduleState::RunningAndRescheduled: {
        /* Scheduled already, nothing to do. */
        break;
      }
    }
  }

  static void run_node_from_task_pool(TaskPool *task_pool, void *task_data)
  {
    void *user_data = BLI_task_pool_user_data(task_pool);
    GeometryNodesEvaluator &evaluator = *(GeometryNodesEvaluator *)user_data;
    const NodeWithState *root_node_with_state = (const NodeWithState *)task_data;

    /* First, the node provided by the task pool is executed. During the execution other nodes
     * might be scheduled. One of those nodes is not added to the task pool but is executed in the
     * loop below directly. This has two main benefits:
     * - Fewer round trips through the task pool which add threading overhead.
     * - Helps with cpu cache efficiency, because a thread is more likely to process data that it
     *   has processed shortly before.
     */
    DNode next_node_to_run = root_node_with_state->node;
    while (next_node_to_run) {
      NodeTaskRunState run_state;
      evaluator.node_task_run(next_node_to_run, &run_state);
      next_node_to_run = run_state.next_node_to_run;
    }
  }

  void node_task_run(const DNode node, NodeTaskRunState *run_state)
  {
    /* These nodes are sometimes scheduled. We could also check for them in other places, but
     * it's the easiest to do it here. */
    if (node->is_group_input_node() || node->is_group_output_node()) {
      return;
    }

    NodeState &node_state = *node_states_.lookup_key_as(node).state;

    const bool do_execute_node = this->node_task_preprocessing(node, node_state, run_state);

    /* Only execute the node if all prerequisites are met. There has to be an output that is
     * required and all required inputs have to be provided already. */
    if (do_execute_node) {
      this->execute_node(node, node_state, run_state);
    }

    this->node_task_postprocessing(node, node_state, do_execute_node, run_state);
  }

  bool node_task_preprocessing(const DNode node,
                               NodeState &node_state,
                               NodeTaskRunState *run_state)
  {
    bool do_execute_node = false;
    this->with_locked_node(node, node_state, run_state, [&](LockedNode &locked_node) {
      BLI_assert(node_state.schedule_state == NodeScheduleState::Scheduled);
      node_state.schedule_state = NodeScheduleState::Running;

      /* Early return if the node has finished already. */
      if (locked_node.node_state.node_has_finished) {
        return;
      }
      /* Prepare outputs and check if actually any new outputs have to be computed. */
      if (!this->prepare_node_outputs_for_execution(locked_node)) {
        return;
      }
      /* Initialize inputs that don't support laziness. This is done after at least one output is
       * required and before we check that all required inputs are provided. This reduces the
       * number of "round-trips" through the task pool by one for most nodes. */
      if (!node_state.non_lazy_inputs_handled) {
        this->require_non_lazy_inputs(locked_node);
        node_state.non_lazy_inputs_handled = true;
      }
      /* Prepare inputs and check if all required inputs are provided. */
      if (!this->prepare_node_inputs_for_execution(locked_node)) {
        return;
      }
      do_execute_node = true;
    });
    return do_execute_node;
  }

  /* A node is finished when it has computed all outputs that may be used have been computed and
   * when no input is still forced to be computed. */
  bool finish_node_if_possible(LockedNode &locked_node)
  {
    if (locked_node.node_state.node_has_finished) {
      /* Early return in case this node is known to have finished already. */
      return true;
    }

    /* Check if there is any output that might be used but has not been computed yet. */
    for (OutputState &output_state : locked_node.node_state.outputs) {
      if (output_state.has_been_computed) {
        continue;
      }
      if (output_state.output_usage != ValueUsage::Unused) {
        return false;
      }
    }

    /* Check if there is an input that still has to be computed. */
    for (InputState &input_state : locked_node.node_state.inputs) {
      if (input_state.force_compute) {
        if (!input_state.was_ready_for_execution) {
          return false;
        }
      }
    }

    /* If there are no remaining outputs, all the inputs can be destructed and/or can become
     * unused. This can also trigger a chain reaction where nodes to the left become finished
     * too. */
    for (const int i : locked_node.node->inputs().index_range()) {
      const DInputSocket socket = locked_node.node.input(i);
      InputState &input_state = locked_node.node_state.inputs[i];
      if (input_state.usage == ValueUsage::Maybe) {
        this->set_input_unused(locked_node, socket);
      }
      else if (input_state.usage == ValueUsage::Required) {
        /* The value was required, so it cannot become unused. However, we can destruct the
         * value. */
        this->destruct_input_value_if_exists(locked_node, socket);
      }
    }
    locked_node.node_state.node_has_finished = true;
    return true;
  }

  bool prepare_node_outputs_for_execution(LockedNode &locked_node)
  {
    bool execution_is_necessary = false;
    for (OutputState &output_state : locked_node.node_state.outputs) {
      /* Update the output usage for execution to the latest value. */
      output_state.output_usage_for_execution = output_state.output_usage;
      if (!output_state.has_been_computed) {
        if (output_state.output_usage == ValueUsage::Required) {
          /* Only evaluate when there is an output that is required but has not been computed. */
          execution_is_necessary = true;
        }
      }
    }
    return execution_is_necessary;
  }

  void require_non_lazy_inputs(LockedNode &locked_node)
  {
    this->foreach_non_lazy_input(locked_node, [&](const DInputSocket socket) {
      this->set_input_required(locked_node, socket);
    });
  }

  void foreach_non_lazy_input(LockedNode &locked_node, FunctionRef<void(DInputSocket socket)> fn)
  {
    if (node_supports_laziness(locked_node.node)) {
      /* In the future only some of the inputs may support laziness. */
      return;
    }
    /* Nodes that don't support laziness require all inputs. */
    for (const int i : locked_node.node->inputs().index_range()) {
      InputState &input_state = locked_node.node_state.inputs[i];
      if (input_state.type == nullptr) {
        /* Ignore unavailable/non-data sockets. */
        continue;
      }
      fn(locked_node.node.input(i));
    }
  }

  /**
   * Checks if requested inputs are available and "marks" all the inputs that are available
   * during the node execution. Inputs that are provided after this function ends but before the
   * node is executed, cannot be read by the node in the execution (note that this only affects
   * nodes that support lazy inputs).
   */
  bool prepare_node_inputs_for_execution(LockedNode &locked_node)
  {
    for (const int i : locked_node.node_state.inputs.index_range()) {
      InputState &input_state = locked_node.node_state.inputs[i];
      if (input_state.type == nullptr) {
        /* Ignore unavailable and non-data sockets. */
        continue;
      }
      const DInputSocket socket = locked_node.node.input(i);
      const bool is_required = input_state.usage == ValueUsage::Required;

      /* No need to check this socket again. */
      if (input_state.was_ready_for_execution) {
        continue;
      }

      if (socket->is_multi_input_socket()) {
        MultiInputValue &multi_value = *input_state.value.multi;
        /* Checks if all the linked sockets have been provided already. */
        if (multi_value.all_values_available()) {
          input_state.was_ready_for_execution = true;
        }
        else if (is_required) {
          /* The input is required but is not fully provided yet. Therefore the node cannot be
           * executed yet. */
          return false;
        }
      }
      else {
        SingleInputValue &single_value = *input_state.value.single;
        if (single_value.value != nullptr) {
          input_state.was_ready_for_execution = true;
        }
        else if (is_required) {
          /* The input is required but has not been provided yet. Therefore the node cannot be
           * executed yet. */
          return false;
        }
      }
    }
    /* All required inputs have been provided. */
    return true;
  }

  /**
   * Actually execute the node. All the required inputs are available and at least one output is
   * required.
   */
  void execute_node(const DNode node, NodeState &node_state, NodeTaskRunState *run_state)
  {
    const bNode &bnode = *node->bnode();

    if (node_state.has_been_executed) {
      if (!node_supports_laziness(node)) {
        /* Nodes that don't support laziness must not be executed more than once. */
        BLI_assert_unreachable();
      }
    }
    node_state.has_been_executed = true;

    /* Use the geometry node execute callback if it exists. */
    if (bnode.typeinfo->geometry_node_execute != nullptr) {
      this->execute_geometry_node(node, node_state, run_state);
      return;
    }

    /* Use the multi-function implementation if it exists. */
    const nodes::NodeMultiFunctions::Item &fn_item = params_.mf_by_node->try_get(node);
    if (fn_item.fn != nullptr) {
      this->execute_multi_function_node(node, fn_item, node_state, run_state);
      return;
    }

    this->execute_unknown_node(node, node_state, run_state);
  }

  void execute_geometry_node(const DNode node, NodeState &node_state, NodeTaskRunState *run_state)
  {
    using Clock = std::chrono::steady_clock;
    const bNode &bnode = *node->bnode();

    NodeParamsProvider params_provider{*this, node, node_state, run_state};
    GeoNodeExecParams params{params_provider};
    Clock::time_point begin = Clock::now();
    bnode.typeinfo->geometry_node_execute(params);
    Clock::time_point end = Clock::now();
    const std::chrono::microseconds duration =
        std::chrono::duration_cast<std::chrono::microseconds>(end - begin);
    if (params_.geo_logger != nullptr) {
      params_.geo_logger->local().log_execution_time(node, duration);
    }
  }

  void execute_multi_function_node(const DNode node,
                                   const nodes::NodeMultiFunctions::Item &fn_item,
                                   NodeState &node_state,
                                   NodeTaskRunState *run_state)
  {
    LinearAllocator<> &allocator = local_allocators_.local();

    bool any_input_is_field = false;
    Vector<const void *, 16> input_values;
    Vector<const ValueOrFieldCPPType *, 16> input_types;
    for (const int i : node->inputs().index_range()) {
      const InputSocketRef &socket_ref = node->input(i);
      if (!socket_ref.is_available()) {
        continue;
      }
      BLI_assert(!socket_ref.is_multi_input_socket());
      InputState &input_state = node_state.inputs[i];
      BLI_assert(input_state.was_ready_for_execution);
      SingleInputValue &single_value = *input_state.value.single;
      BLI_assert(single_value.value != nullptr);
      const ValueOrFieldCPPType &field_cpp_type = static_cast<const ValueOrFieldCPPType &>(
          *input_state.type);
      input_values.append(single_value.value);
      input_types.append(&field_cpp_type);
      if (field_cpp_type.is_field(single_value.value)) {
        any_input_is_field = true;
      }
    }

    if (any_input_is_field) {
      this->execute_multi_function_node__field(
          node, fn_item, node_state, allocator, input_values, input_types, run_state);
    }
    else {
      this->execute_multi_function_node__value(
          node, *fn_item.fn, node_state, allocator, input_values, input_types, run_state);
    }
  }

  void execute_multi_function_node__field(const DNode node,
                                          const nodes::NodeMultiFunctions::Item &fn_item,
                                          NodeState &node_state,
                                          LinearAllocator<> &allocator,
                                          Span<const void *> input_values,
                                          Span<const ValueOrFieldCPPType *> input_types,
                                          NodeTaskRunState *run_state)
  {
    Vector<GField> input_fields;
    for (const int i : input_values.index_range()) {
      const void *input_value_or_field = input_values[i];
      const ValueOrFieldCPPType &field_cpp_type = *input_types[i];
      input_fields.append(field_cpp_type.as_field(input_value_or_field));
    }

    std::shared_ptr<fn::FieldOperation> operation;
    if (fn_item.owned_fn) {
      operation = std::make_shared<fn::FieldOperation>(fn_item.owned_fn, std::move(input_fields));
    }
    else {
      operation = std::make_shared<fn::FieldOperation>(*fn_item.fn, std::move(input_fields));
    }

    int output_index = 0;
    for (const int i : node->outputs().index_range()) {
      const OutputSocketRef &socket_ref = node->output(i);
      if (!socket_ref.is_available()) {
        continue;
      }
      OutputState &output_state = node_state.outputs[i];
      const DOutputSocket socket{node.context(), &socket_ref};
      const ValueOrFieldCPPType *cpp_type = static_cast<const ValueOrFieldCPPType *>(
          get_socket_cpp_type(socket_ref));
      GField new_field{operation, output_index};
      void *buffer = allocator.allocate(cpp_type->size(), cpp_type->alignment());
      cpp_type->construct_from_field(buffer, std::move(new_field));
      this->forward_output(socket, {cpp_type, buffer}, run_state);
      output_state.has_been_computed = true;
      output_index++;
    }
  }

  void execute_multi_function_node__value(const DNode node,
                                          const MultiFunction &fn,
                                          NodeState &node_state,
                                          LinearAllocator<> &allocator,
                                          Span<const void *> input_values,
                                          Span<const ValueOrFieldCPPType *> input_types,
                                          NodeTaskRunState *run_state)
  {
    MFParamsBuilder params{fn, 1};
    for (const int i : input_values.index_range()) {
      const void *input_value_or_field = input_values[i];
      const ValueOrFieldCPPType &field_cpp_type = *input_types[i];
      const CPPType &base_type = field_cpp_type.base_type();
      const void *input_value = field_cpp_type.get_value_ptr(input_value_or_field);
      params.add_readonly_single_input(GVArray::ForSingleRef(base_type, 1, input_value));
    }

    Vector<GMutablePointer, 16> output_buffers;
    for (const int i : node->outputs().index_range()) {
      const DOutputSocket socket = node.output(i);
      if (!socket->is_available()) {
        output_buffers.append({});
        continue;
      }
      const ValueOrFieldCPPType *value_or_field_type = static_cast<const ValueOrFieldCPPType *>(
          get_socket_cpp_type(socket));
      const CPPType &base_type = value_or_field_type->base_type();
      void *value_or_field_buffer = allocator.allocate(value_or_field_type->size(),
                                                       value_or_field_type->alignment());
      value_or_field_type->default_construct(value_or_field_buffer);
      void *value_buffer = value_or_field_type->get_value_ptr(value_or_field_buffer);
      base_type.destruct(value_buffer);
      params.add_uninitialized_single_output(GMutableSpan{base_type, value_buffer, 1});
      output_buffers.append({value_or_field_type, value_or_field_buffer});
    }

    MFContextBuilder context;
    fn.call(IndexRange(1), params, context);

    for (const int i : output_buffers.index_range()) {
      GMutablePointer buffer = output_buffers[i];
      if (buffer.get() == nullptr) {
        continue;
      }
      const DOutputSocket socket = node.output(i);
      this->forward_output(socket, buffer, run_state);

      OutputState &output_state = node_state.outputs[i];
      output_state.has_been_computed = true;
    }
  }

  void execute_unknown_node(const DNode node, NodeState &node_state, NodeTaskRunState *run_state)
  {
    LinearAllocator<> &allocator = local_allocators_.local();
    for (const OutputSocketRef *socket : node->outputs()) {
      if (!socket->is_available()) {
        continue;
      }
      const CPPType *type = get_socket_cpp_type(*socket);
      if (type == nullptr) {
        continue;
      }
      /* Just forward the default value of the type as a fallback. That's typically better than
       * crashing or doing nothing. */
      OutputState &output_state = node_state.outputs[socket->index()];
      output_state.has_been_computed = true;
      void *buffer = allocator.allocate(type->size(), type->alignment());
      this->construct_default_value(*type, buffer);
      this->forward_output({node.context(), socket}, {*type, buffer}, run_state);
    }
  }

  void node_task_postprocessing(const DNode node,
                                NodeState &node_state,
                                bool was_executed,
                                NodeTaskRunState *run_state)
  {
    this->with_locked_node(node, node_state, run_state, [&](LockedNode &locked_node) {
      const bool node_has_finished = this->finish_node_if_possible(locked_node);
      const bool reschedule_requested = node_state.schedule_state ==
                                        NodeScheduleState::RunningAndRescheduled;
      node_state.schedule_state = NodeScheduleState::NotScheduled;
      if (reschedule_requested && !node_has_finished) {
        /* Either the node rescheduled itself or another node tried to schedule it while it ran. */
        this->schedule_node(locked_node);
      }
      if (was_executed) {
        this->assert_expected_outputs_have_been_computed(locked_node);
      }
    });
  }

  void assert_expected_outputs_have_been_computed(LockedNode &locked_node)
  {
#ifdef DEBUG
    /* Outputs can only be computed when all required inputs have been provided. */
    if (locked_node.node_state.missing_required_inputs > 0) {
      return;
    }
    /* If the node is still scheduled, it is not necessary that all its expected outputs are
     * computed yet. */
    if (locked_node.node_state.schedule_state == NodeScheduleState::Scheduled) {
      return;
    }

    const bool supports_laziness = node_supports_laziness(locked_node.node);
    /* Iterating over sockets instead of the states directly, because that makes it easier to
     * figure out which socket is missing when one of the asserts is hit. */
    for (const OutputSocketRef *socket_ref : locked_node.node->outputs()) {
      OutputState &output_state = locked_node.node_state.outputs[socket_ref->index()];
      if (supports_laziness) {
        /* Expected that at least all required sockets have been computed. If more outputs become
         * required later, the node will be executed again. */
        if (output_state.output_usage_for_execution == ValueUsage::Required) {
          BLI_assert(output_state.has_been_computed);
        }
      }
      else {
        /* Expect that all outputs that may be used have been computed, because the node cannot
         * be executed again. */
        if (output_state.output_usage_for_execution != ValueUsage::Unused) {
          BLI_assert(output_state.has_been_computed);
        }
      }
    }
#else
    UNUSED_VARS(locked_node);
#endif
  }

  void extract_group_outputs()
  {
    for (const DInputSocket &socket : params_.output_sockets) {
      BLI_assert(socket->is_available());
      BLI_assert(!socket->is_multi_input_socket());

      const DNode node = socket.node();
      NodeState &node_state = this->get_node_state(node);
      InputState &input_state = node_state.inputs[socket->index()];

      SingleInputValue &single_value = *input_state.value.single;
      void *value = single_value.value;

      /* The value should have been computed by now. If this assert is hit, it means that there
       * was some scheduling issue before. */
      BLI_assert(value != nullptr);

      /* Move value into memory owned by the outer allocator. */
      const CPPType &type = *input_state.type;
      void *buffer = outer_allocator_.allocate(type.size(), type.alignment());
      type.move_construct(value, buffer);

      params_.r_output_values.append({type, buffer});
    }
  }

  /**
   * Load the required input from the socket or trigger nodes to the left to compute the value.
   * \return True when the node will be triggered by another node again when the value is computed.
   */
  bool set_input_required(LockedNode &locked_node, const DInputSocket input_socket)
  {
    BLI_assert(locked_node.node == input_socket.node());
    InputState &input_state = locked_node.node_state.inputs[input_socket->index()];

    /* Value set as unused cannot become used again. */
    BLI_assert(input_state.usage != ValueUsage::Unused);

    if (input_state.was_ready_for_execution) {
      return false;
    }

    if (input_state.usage == ValueUsage::Required) {
      /* If the input was not ready for execution but is required, the node will be triggered again
       * once the input has been computed. */
      return true;
    }
    input_state.usage = ValueUsage::Required;

    /* Count how many values still have to be added to this input until it is "complete". */
    int missing_values = 0;
    if (input_socket->is_multi_input_socket()) {
      MultiInputValue &multi_value = *input_state.value.multi;
      missing_values = multi_value.missing_values();
    }
    else {
      SingleInputValue &single_value = *input_state.value.single;
      if (single_value.value == nullptr) {
        missing_values = 1;
      }
    }
    if (missing_values == 0) {
      return false;
    }
    /* Increase the total number of missing required inputs. This ensures that the node will be
     * scheduled correctly when all inputs have been provided. */
    locked_node.node_state.missing_required_inputs += missing_values;

    /* Get all origin sockets, because we have to tag those as required as well. */
    Vector<DSocket> origin_sockets;
    input_socket.foreach_origin_socket(
        [&](const DSocket origin_socket) { origin_sockets.append(origin_socket); });

    if (origin_sockets.is_empty()) {
      /* If there are no origin sockets, just load the value from the socket directly. */
      this->load_unlinked_input_value(locked_node, input_socket, input_state, input_socket);
      locked_node.node_state.missing_required_inputs -= 1;
      return false;
    }
    bool requested_from_other_node = false;
    for (const DSocket &origin_socket : origin_sockets) {
      if (origin_socket->is_input()) {
        /* Load the value directly from the origin socket. In most cases this is an unlinked
         * group input. */
        this->load_unlinked_input_value(locked_node, input_socket, input_state, origin_socket);
        locked_node.node_state.missing_required_inputs -= 1;
      }
      else {
        /* The value has not been computed yet, so when it will be forwarded by another node, this
         * node will be triggered. */
        requested_from_other_node = true;
        locked_node.delayed_required_outputs.append(DOutputSocket(origin_socket));
      }
    }
    /* If this node will be triggered by another node, we don't have to schedule it now. */
    if (requested_from_other_node) {
      return true;
    }
    return false;
  }

  void set_input_unused(LockedNode &locked_node, const DInputSocket socket)
  {
    InputState &input_state = locked_node.node_state.inputs[socket->index()];

    /* A required socket cannot become unused. */
    BLI_assert(input_state.usage != ValueUsage::Required);

    if (input_state.usage == ValueUsage::Unused) {
      /* Nothing to do in this case. */
      return;
    }
    input_state.usage = ValueUsage::Unused;

    /* If the input is unused, its value can be destructed now. */
    this->destruct_input_value_if_exists(locked_node, socket);

    if (input_state.was_ready_for_execution) {
      /* If the value was already computed, we don't need to notify origin nodes. */
      return;
    }

    /* Notify origin nodes that might want to set its inputs as unused as well. */
    socket.foreach_origin_socket([&](const DSocket origin_socket) {
      if (origin_socket->is_input()) {
        /* Values from these sockets are loaded directly from the sockets, so there is no node to
         * notify. */
        return;
      }
      /* Delay notification of the other node until this node is not locked anymore. */
      locked_node.delayed_unused_outputs.append(DOutputSocket(origin_socket));
    });
  }

  void send_output_required_notification(const DOutputSocket socket, NodeTaskRunState *run_state)
  {
    const DNode node = socket.node();
    NodeState &node_state = this->get_node_state(node);
    OutputState &output_state = node_state.outputs[socket->index()];

    this->with_locked_node(node, node_state, run_state, [&](LockedNode &locked_node) {
      if (output_state.output_usage == ValueUsage::Required) {
        /* Output is marked as required already. So the node is scheduled already. */
        return;
      }
      /* The origin node needs to be scheduled so that it provides the requested input
       * eventually. */
      output_state.output_usage = ValueUsage::Required;
      this->schedule_node(locked_node);
    });
  }

  void send_output_unused_notification(const DOutputSocket socket, NodeTaskRunState *run_state)
  {
    const DNode node = socket.node();
    NodeState &node_state = this->get_node_state(node);
    OutputState &output_state = node_state.outputs[socket->index()];

    this->with_locked_node(node, node_state, run_state, [&](LockedNode &locked_node) {
      output_state.potential_users -= 1;
      if (output_state.potential_users == 0) {
        /* The socket might be required even though the output is not used by other sockets. That
         * can happen when the socket is forced to be computed. */
        if (output_state.output_usage != ValueUsage::Required) {
          /* The output socket has no users anymore. */
          output_state.output_usage = ValueUsage::Unused;
          /* Schedule the origin node in case it wants to set its inputs as unused as well. */
          this->schedule_node(locked_node);
        }
      }
    });
  }

  void add_node_to_task_pool(const DNode node)
  {
    /* Push the task to the pool while it is not locked to avoid a deadlock in case when the task
     * is executed immediately. */
    const NodeWithState *node_with_state = node_states_.lookup_key_ptr_as(node);
    BLI_task_pool_push(
        task_pool_, run_node_from_task_pool, (void *)node_with_state, false, nullptr);
  }

  /**
   * Moves a newly computed value from an output socket to all the inputs that might need it.
   * Takes ownership of the value and destructs if it is unused.
   */
  void forward_output(const DOutputSocket from_socket,
                      GMutablePointer value_to_forward,
                      NodeTaskRunState *run_state)
  {
    BLI_assert(value_to_forward.get() != nullptr);

    LinearAllocator<> &allocator = local_allocators_.local();

    Vector<DSocket> log_original_value_sockets;
    Vector<DInputSocket> forward_original_value_sockets;
    log_original_value_sockets.append(from_socket);

    from_socket.foreach_target_socket(
        [&](const DInputSocket to_socket, const DOutputSocket::TargetSocketPathInfo &path_info) {
          if (!this->should_forward_to_socket(to_socket)) {
            return;
          }
          BLI_assert(to_socket == path_info.sockets.last());
          GMutablePointer current_value = value_to_forward;
          for (const DSocket &next_socket : path_info.sockets) {
            const DNode next_node = next_socket.node();
            const bool is_last_socket = to_socket == next_socket;
            const bool do_conversion_if_necessary = is_last_socket ||
                                                    next_node->is_group_output_node() ||
                                                    (next_node->is_group_node() &&
                                                     !next_node->is_muted());
            if (do_conversion_if_necessary) {
              const CPPType &next_type = *get_socket_cpp_type(next_socket);
              if (*current_value.type() != next_type) {
                void *buffer = allocator.allocate(next_type.size(), next_type.alignment());
                this->convert_value(*current_value.type(), next_type, current_value.get(), buffer);
                if (current_value.get() != value_to_forward.get()) {
                  current_value.destruct();
                }
                current_value = {next_type, buffer};
              }
            }
            if (current_value.get() == value_to_forward.get()) {
              /* Log the original value at the current socket. */
              log_original_value_sockets.append(next_socket);
            }
            else {
              /* Multi-input sockets are logged when all values are available. */
              if (!(next_socket->is_input() && next_socket->as_input().is_multi_input_socket())) {
                /* Log the converted value at the socket. */
                this->log_socket_value({next_socket}, current_value);
              }
            }
          }
          if (current_value.get() == value_to_forward.get()) {
            /* The value has not been converted, so forward the original value. */
            forward_original_value_sockets.append(to_socket);
          }
          else {
            /* The value has been converted. */
            this->add_value_to_input_socket(to_socket, from_socket, current_value, run_state);
          }
        });
    this->log_socket_value(log_original_value_sockets, value_to_forward);
    this->forward_to_sockets_with_same_type(
        allocator, forward_original_value_sockets, value_to_forward, from_socket, run_state);
  }

  bool should_forward_to_socket(const DInputSocket socket)
  {
    const DNode to_node = socket.node();
    const NodeWithState *target_node_with_state = node_states_.lookup_key_ptr_as(to_node);
    if (target_node_with_state == nullptr) {
      /* If the socket belongs to a node that has no state, the entire node is not used. */
      return false;
    }
    NodeState &target_node_state = *target_node_with_state->state;
    InputState &target_input_state = target_node_state.inputs[socket->index()];

    std::lock_guard lock{target_node_state.mutex};
    /* Do not forward to an input socket whose value won't be used. */
    return target_input_state.usage != ValueUsage::Unused;
  }

  void forward_to_sockets_with_same_type(LinearAllocator<> &allocator,
                                         Span<DInputSocket> to_sockets,
                                         GMutablePointer value_to_forward,
                                         const DOutputSocket from_socket,
                                         NodeTaskRunState *run_state)
  {
    if (to_sockets.is_empty()) {
      /* Value is not used anymore, so it can be destructed. */
      value_to_forward.destruct();
    }
    else if (to_sockets.size() == 1) {
      /* Value is only used by one input socket, no need to copy it. */
      const DInputSocket to_socket = to_sockets[0];
      this->add_value_to_input_socket(to_socket, from_socket, value_to_forward, run_state);
    }
    else {
      /* Multiple inputs use the value, make a copy for every input except for one. */
      /* First make the copies, so that the next node does not start modifying the value while we
       * are still making copies. */
      const CPPType &type = *value_to_forward.type();
      for (const DInputSocket &to_socket : to_sockets.drop_front(1)) {
        void *buffer = allocator.allocate(type.size(), type.alignment());
        type.copy_construct(value_to_forward.get(), buffer);
        this->add_value_to_input_socket(to_socket, from_socket, {type, buffer}, run_state);
      }
      /* Forward the original value to one of the targets. */
      const DInputSocket to_socket = to_sockets[0];
      this->add_value_to_input_socket(to_socket, from_socket, value_to_forward, run_state);
    }
  }

  void add_value_to_input_socket(const DInputSocket socket,
                                 const DOutputSocket origin,
                                 GMutablePointer value,
                                 NodeTaskRunState *run_state)
  {
    BLI_assert(socket->is_available());

    const DNode node = socket.node();
    NodeState &node_state = this->get_node_state(node);
    InputState &input_state = node_state.inputs[socket->index()];

    this->with_locked_node(node, node_state, run_state, [&](LockedNode &locked_node) {
      if (socket->is_multi_input_socket()) {
        /* Add a new value to the multi-input. */
        MultiInputValue &multi_value = *input_state.value.multi;
        multi_value.add_value(origin, value.get());

        if (multi_value.all_values_available()) {
          this->log_socket_value({socket}, input_state, multi_value.values);
        }
      }
      else {
        /* Assign the value to the input. */
        SingleInputValue &single_value = *input_state.value.single;
        BLI_assert(single_value.value == nullptr);
        single_value.value = value.get();
      }

      if (input_state.usage == ValueUsage::Required) {
        node_state.missing_required_inputs--;
        if (node_state.missing_required_inputs == 0) {
          /* Schedule node if all the required inputs have been provided. */
          this->schedule_node(locked_node);
        }
      }
    });
  }

  /**
   * Loads the value of a socket that is not computed by another node. Note that the socket may
   * still be linked to e.g. a Group Input node, but the socket on the outside is not connected to
   * anything.
   *
   * \param input_socket: The socket of the node that wants to use the value.
   * \param origin_socket: The socket that we want to load the value from.
   */
  void load_unlinked_input_value(LockedNode &locked_node,
                                 const DInputSocket input_socket,
                                 InputState &input_state,
                                 const DSocket origin_socket)
  {
    /* Only takes locked node as parameter, because the node needs to be locked. */
    UNUSED_VARS(locked_node);

    GMutablePointer value = this->get_value_from_socket(origin_socket, *input_state.type);
    if (input_socket->is_multi_input_socket()) {
      MultiInputValue &multi_value = *input_state.value.multi;
      multi_value.add_value(origin_socket, value.get());
      if (multi_value.all_values_available()) {
        this->log_socket_value({input_socket}, input_state, multi_value.values);
      }
    }
    else {
      SingleInputValue &single_value = *input_state.value.single;
      single_value.value = value.get();
      Vector<DSocket> sockets_to_log_to = {input_socket};
      if (origin_socket != input_socket) {
        /* This might log the socket value for the #origin_socket more than once, but this is
         * handled by the logging system gracefully. */
        sockets_to_log_to.append(origin_socket);
      }
      /* TODO: Log to the intermediate sockets between the group input and where the value is
       * actually used as well. */
      this->log_socket_value(sockets_to_log_to, value);
    }
  }

  void destruct_input_value_if_exists(LockedNode &locked_node, const DInputSocket socket)
  {
    InputState &input_state = locked_node.node_state.inputs[socket->index()];
    if (socket->is_multi_input_socket()) {
      MultiInputValue &multi_value = *input_state.value.multi;
      for (void *&value : multi_value.values) {
        if (value != nullptr) {
          input_state.type->destruct(value);
          value = nullptr;
        }
      }
      multi_value.provided_value_count = 0;
    }
    else {
      SingleInputValue &single_value = *input_state.value.single;
      if (single_value.value != nullptr) {
        input_state.type->destruct(single_value.value);
        single_value.value = nullptr;
      }
    }
  }

  GMutablePointer get_value_from_socket(const DSocket socket, const CPPType &required_type)
  {
    LinearAllocator<> &allocator = local_allocators_.local();

    const CPPType &type = *get_socket_cpp_type(socket);
    void *buffer = allocator.allocate(type.size(), type.alignment());
    get_socket_value(*socket.socket_ref(), buffer);

    if (type == required_type) {
      return {type, buffer};
    }
    void *converted_buffer = allocator.allocate(required_type.size(), required_type.alignment());
    this->convert_value(type, required_type, buffer, converted_buffer);
    type.destruct(buffer);
    return {required_type, converted_buffer};
  }

  void convert_value(const CPPType &from_type,
                     const CPPType &to_type,
                     const void *from_value,
                     void *to_value)
  {
    if (from_type == to_type) {
      from_type.copy_construct(from_value, to_value);
      return;
    }
    const ValueOrFieldCPPType *from_field_type = dynamic_cast<const ValueOrFieldCPPType *>(
        &from_type);
    const ValueOrFieldCPPType *to_field_type = dynamic_cast<const ValueOrFieldCPPType *>(&to_type);

    if (from_field_type != nullptr && to_field_type != nullptr) {
      const CPPType &from_base_type = from_field_type->base_type();
      const CPPType &to_base_type = to_field_type->base_type();
      if (conversions_.is_convertible(from_base_type, to_base_type)) {
        if (from_field_type->is_field(from_value)) {
          const GField &from_field = *from_field_type->get_field_ptr(from_value);
          const MultiFunction &fn = *conversions_.get_conversion_multi_function(
              MFDataType::ForSingle(from_base_type), MFDataType::ForSingle(to_base_type));
          auto operation = std::make_shared<fn::FieldOperation>(fn, Vector<GField>{from_field});
          to_field_type->construct_from_field(to_value, GField(std::move(operation), 0));
        }
        else {
          to_field_type->default_construct(to_value);
          const void *from_value_ptr = from_field_type->get_value_ptr(from_value);
          void *to_value_ptr = to_field_type->get_value_ptr(to_value);
          conversions_.get_conversion_functions(from_base_type, to_base_type)
              ->convert_single_to_initialized(from_value_ptr, to_value_ptr);
        }
        return;
      }
    }
    if (conversions_.is_convertible(from_type, to_type)) {
      /* Do the conversion if possible. */
      conversions_.convert_to_uninitialized(from_type, to_type, from_value, to_value);
    }
    else {
      /* Cannot convert, use default value instead. */
      this->construct_default_value(to_type, to_value);
    }
  }

  void construct_default_value(const CPPType &type, void *r_value)
  {
    type.copy_construct(type.default_value(), r_value);
  }

  NodeState &get_node_state(const DNode node)
  {
    return *node_states_.lookup_key_as(node).state;
  }

  void log_socket_value(DSocket socket, InputState &input_state, Span<void *> values)
  {
    if (params_.geo_logger == nullptr) {
      return;
    }

    Vector<GPointer, 16> value_pointers;
    value_pointers.reserve(values.size());
    const CPPType &type = *input_state.type;
    for (const void *value : values) {
      value_pointers.append({type, value});
    }
    params_.geo_logger->local().log_multi_value_socket(socket, value_pointers);
  }

  void log_socket_value(Span<DSocket> sockets, GPointer value)
  {
    if (params_.geo_logger == nullptr) {
      return;
    }
    params_.geo_logger->local().log_value_for_sockets(sockets, value);
  }

  void log_debug_message(DNode node, std::string message)
  {
    if (params_.geo_logger == nullptr) {
      return;
    }
    params_.geo_logger->local().log_debug_message(node, std::move(message));
  }

  /* In most cases when `NodeState` is accessed, the node has to be locked first to avoid race
   * conditions. */
  template<typename Function>
  void with_locked_node(const DNode node,
                        NodeState &node_state,
                        NodeTaskRunState *run_state,
                        const Function &function)
  {
    LockedNode locked_node{node, node_state};

    node_state.mutex.lock();
    /* Isolate this thread because we don't want it to start executing another node. This other
     * node might want to lock the same mutex leading to a deadlock. */
    threading::isolate_task([&] { function(locked_node); });
    node_state.mutex.unlock();

    /* Then send notifications to the other nodes after the node state is unlocked. This avoids
     * locking two nodes at the same time on this thread and helps to prevent deadlocks. */
    for (const DOutputSocket &socket : locked_node.delayed_required_outputs) {
      this->send_output_required_notification(socket, run_state);
    }
    for (const DOutputSocket &socket : locked_node.delayed_unused_outputs) {
      this->send_output_unused_notification(socket, run_state);
    }
    for (const DNode &node_to_schedule : locked_node.delayed_scheduled_nodes) {
      if (run_state != nullptr && !run_state->next_node_to_run) {
        /* Execute the node on the same thread after the current node finished. */
        /* Currently, this assumes that it is always best to run the first node that is scheduled
         * on the same thread. That is usually correct, because the geometry socket which carries
         * the most data usually comes first in nodes. */
        run_state->next_node_to_run = node_to_schedule;
      }
      else {
        /* Push the node to the task pool so that another thread can start working on it. */
        this->add_node_to_task_pool(node_to_schedule);
      }
    }
  }
};

NodeParamsProvider::NodeParamsProvider(GeometryNodesEvaluator &evaluator,
                                       DNode dnode,
                                       NodeState &node_state,
                                       NodeTaskRunState *run_state)
    : evaluator_(evaluator), node_state_(node_state), run_state_(run_state)
{
  this->dnode = dnode;
  this->self_object = evaluator.params_.self_object;
  this->modifier = &evaluator.params_.modifier_->modifier;
  this->depsgraph = evaluator.params_.depsgraph;
  this->logger = evaluator.params_.geo_logger;
}

bool NodeParamsProvider::can_get_input(StringRef identifier) const
{
  const DInputSocket socket = this->dnode.input_by_identifier(identifier);
  BLI_assert(socket);

  InputState &input_state = node_state_.inputs[socket->index()];
  if (!input_state.was_ready_for_execution) {
    return false;
  }

  if (socket->is_multi_input_socket()) {
    MultiInputValue &multi_value = *input_state.value.multi;
    return multi_value.all_values_available();
  }
  SingleInputValue &single_value = *input_state.value.single;
  return single_value.value != nullptr;
}

bool NodeParamsProvider::can_set_output(StringRef identifier) const
{
  const DOutputSocket socket = this->dnode.output_by_identifier(identifier);
  BLI_assert(socket);

  OutputState &output_state = node_state_.outputs[socket->index()];
  return !output_state.has_been_computed;
}

GMutablePointer NodeParamsProvider::extract_input(StringRef identifier)
{
  const DInputSocket socket = this->dnode.input_by_identifier(identifier);
  BLI_assert(socket);
  BLI_assert(!socket->is_multi_input_socket());
  BLI_assert(this->can_get_input(identifier));

  InputState &input_state = node_state_.inputs[socket->index()];
  SingleInputValue &single_value = *input_state.value.single;
  void *value = single_value.value;
  single_value.value = nullptr;
  return {*input_state.type, value};
}

Vector<GMutablePointer> NodeParamsProvider::extract_multi_input(StringRef identifier)
{
  const DInputSocket socket = this->dnode.input_by_identifier(identifier);
  BLI_assert(socket);
  BLI_assert(socket->is_multi_input_socket());
  BLI_assert(this->can_get_input(identifier));

  InputState &input_state = node_state_.inputs[socket->index()];
  MultiInputValue &multi_value = *input_state.value.multi;

  Vector<GMutablePointer> ret_values;
  for (void *&value : multi_value.values) {
    BLI_assert(value != nullptr);
    ret_values.append({*input_state.type, value});
    value = nullptr;
  }
  return ret_values;
}

GPointer NodeParamsProvider::get_input(StringRef identifier) const
{
  const DInputSocket socket = this->dnode.input_by_identifier(identifier);
  BLI_assert(socket);
  BLI_assert(!socket->is_multi_input_socket());
  BLI_assert(this->can_get_input(identifier));

  InputState &input_state = node_state_.inputs[socket->index()];
  SingleInputValue &single_value = *input_state.value.single;
  return {*input_state.type, single_value.value};
}

GMutablePointer NodeParamsProvider::alloc_output_value(const CPPType &type)
{
  LinearAllocator<> &allocator = evaluator_.local_allocators_.local();
  return {type, allocator.allocate(type.size(), type.alignment())};
}

void NodeParamsProvider::set_output(StringRef identifier, GMutablePointer value)
{
  const DOutputSocket socket = this->dnode.output_by_identifier(identifier);
  BLI_assert(socket);

  OutputState &output_state = node_state_.outputs[socket->index()];
  BLI_assert(!output_state.has_been_computed);
  evaluator_.forward_output(socket, value, run_state_);
  output_state.has_been_computed = true;
}

bool NodeParamsProvider::lazy_require_input(StringRef identifier)
{
  BLI_assert(node_supports_laziness(this->dnode));
  const DInputSocket socket = this->dnode.input_by_identifier(identifier);
  BLI_assert(socket);

  InputState &input_state = node_state_.inputs[socket->index()];
  if (input_state.was_ready_for_execution) {
    return false;
  }
  evaluator_.with_locked_node(this->dnode, node_state_, run_state_, [&](LockedNode &locked_node) {
    if (!evaluator_.set_input_required(locked_node, socket)) {
      /* Schedule the currently executed node again because the value is available now but was not
       * ready for the current execution. */
      evaluator_.schedule_node(locked_node);
    }
  });
  return true;
}

void NodeParamsProvider::set_input_unused(StringRef identifier)
{
  const DInputSocket socket = this->dnode.input_by_identifier(identifier);
  BLI_assert(socket);

  evaluator_.with_locked_node(this->dnode, node_state_, run_state_, [&](LockedNode &locked_node) {
    evaluator_.set_input_unused(locked_node, socket);
  });
}

bool NodeParamsProvider::output_is_required(StringRef identifier) const
{
  const DOutputSocket socket = this->dnode.output_by_identifier(identifier);
  BLI_assert(socket);

  OutputState &output_state = node_state_.outputs[socket->index()];
  if (output_state.has_been_computed) {
    return false;
  }
  return output_state.output_usage_for_execution != ValueUsage::Unused;
}

bool NodeParamsProvider::lazy_output_is_required(StringRef identifier) const
{
  BLI_assert(node_supports_laziness(this->dnode));
  const DOutputSocket socket = this->dnode.output_by_identifier(identifier);
  BLI_assert(socket);

  OutputState &output_state = node_state_.outputs[socket->index()];
  if (output_state.has_been_computed) {
    return false;
  }
  return output_state.output_usage_for_execution == ValueUsage::Required;
}

void NodeParamsProvider::set_default_remaining_outputs()
{
  LinearAllocator<> &allocator = evaluator_.local_allocators_.local();

  for (const int i : this->dnode->outputs().index_range()) {
    OutputState &output_state = node_state_.outputs[i];
    if (output_state.has_been_computed) {
      continue;
    }
    if (output_state.output_usage_for_execution == ValueUsage::Unused) {
      continue;
    }

    const DOutputSocket socket = this->dnode.output(i);
    const CPPType *type = get_socket_cpp_type(socket);
    BLI_assert(type != nullptr);
    void *buffer = allocator.allocate(type->size(), type->alignment());
    type->copy_construct(type->default_value(), buffer);
    evaluator_.forward_output(socket, {type, buffer}, run_state_);
    output_state.has_been_computed = true;
  }
}

void evaluate_geometry_nodes(GeometryNodesEvaluationParams &params)
{
  GeometryNodesEvaluator evaluator{params};
  evaluator.execute();
}

}  // namespace blender::modifiers::geometry_nodes
