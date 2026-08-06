/**************************************************************************/
/*  goap_planner.cpp                                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "goap_planner.h"

#include "goap_state.h"

#include "core/object/class_db.h"
#include "core/templates/local_vector.h"

namespace {

// An action as seen by the search. `is_valid()`, the cost, the preconditions
// and the effects can all be script calls, so they are resolved once per query
// instead of once per expansion.
struct CachedAction {
	Ref<GoapAction> action;
	GoapState preconditions;
	GoapState effects;
	double cost = 0.0;
};

struct SearchNode {
	GoapState remaining; // Conditions still to be satisfied, in goal space.
	double g = 0.0;
	double f = 0.0;
	int parent = -1;
	int action = -1; // Index into the cached action pool.
	int depth = 0;
	uint32_t hash = 0;
};

void heap_push(LocalVector<int> &r_heap, const LocalVector<SearchNode> &p_nodes, int p_node) {
	r_heap.push_back(p_node);
	uint32_t child = r_heap.size() - 1;
	while (child > 0) {
		const uint32_t parent = (child - 1) / 2;
		if (p_nodes[r_heap[parent]].f <= p_nodes[r_heap[child]].f) {
			break;
		}
		SWAP(r_heap[parent], r_heap[child]);
		child = parent;
	}
}

int heap_pop(LocalVector<int> &r_heap, const LocalVector<SearchNode> &p_nodes) {
	const int top = r_heap[0];
	r_heap[0] = r_heap[r_heap.size() - 1];
	r_heap.remove_at(r_heap.size() - 1);

	uint32_t parent = 0;
	while (!r_heap.is_empty()) {
		const uint32_t left = parent * 2 + 1;
		const uint32_t right = left + 1;
		uint32_t lowest = parent;
		if (left < r_heap.size() && p_nodes[r_heap[left]].f < p_nodes[r_heap[lowest]].f) {
			lowest = left;
		}
		if (right < r_heap.size() && p_nodes[r_heap[right]].f < p_nodes[r_heap[lowest]].f) {
			lowest = right;
		}
		if (lowest == parent) {
			break;
		}
		SWAP(r_heap[lowest], r_heap[parent]);
		parent = lowest;
	}
	return top;
}

struct GoalCandidate {
	double priority = 0.0;
	int index = 0;

	bool operator<(const GoalCandidate &p_other) const {
		// Highest priority first, ties broken by the order goals were listed in.
		if (priority == p_other.priority) {
			return index < p_other.index;
		}
		return priority > p_other.priority;
	}
};

} // namespace

void GoapPlanner::set_max_iterations(int p_iterations) {
	max_iterations = MAX(1, p_iterations);
}

void GoapPlanner::set_max_plan_length(int p_length) {
	max_plan_length = MAX(1, p_length);
}

Ref<GoapPlan> GoapPlanner::plan(const Ref<GoapContext> &p_context, const TypedArray<GoapAction> &p_actions, const TypedArray<GoapGoal> &p_goals) {
	LocalVector<GoalCandidate> candidates;
	for (int i = 0; i < p_goals.size(); i++) {
		Ref<GoapGoal> goal = p_goals[i];
		if (goal.is_null() || !goal->is_valid(p_context)) {
			continue;
		}
		GoalCandidate candidate;
		candidate.priority = goal->get_effective_priority(p_context);
		candidate.index = i;
		candidates.push_back(candidate);
	}
	candidates.sort();

	for (const GoalCandidate &candidate : candidates) {
		Ref<GoapGoal> goal = p_goals[candidate.index];
		Ref<GoapPlan> goal_plan = plan_for_goal(p_context, p_actions, goal);
		// An empty plan means the goal is already satisfied; there is nothing
		// to execute for it, so move on to the next one.
		if (goal_plan.is_valid() && !goal_plan->is_empty()) {
			return goal_plan;
		}
	}
	return Ref<GoapPlan>();
}

Ref<GoapPlan> GoapPlanner::plan_for_goal(const Ref<GoapContext> &p_context, const TypedArray<GoapAction> &p_actions, const Ref<GoapGoal> &p_goal) {
	last_iterations = 0;
	ERR_FAIL_COND_V(p_goal.is_null(), Ref<GoapPlan>());

	// Planning without a context is allowed; actions then see an empty one.
	Ref<GoapContext> context = p_context;
	if (context.is_null()) {
		context.instantiate();
	}

	GoapState current;
	current.from_dictionary(context->get_world_state());

	GoapState desired;
	desired.from_dictionary(p_goal->get_effective_desired_state(context));

	Ref<GoapPlan> result;
	result.instantiate();
	result->set_goal(p_goal);

	if (desired.is_empty() || current.satisfies(desired)) {
		return result;
	}

	LocalVector<CachedAction> pool;
	for (int i = 0; i < p_actions.size(); i++) {
		Ref<GoapAction> action = p_actions[i];
		if (action.is_null() || !action->is_valid(context)) {
			continue;
		}
		CachedAction cached;
		cached.action = action;
		cached.cost = action->get_effective_cost(context);
		cached.preconditions.from_dictionary(action->get_effective_preconditions(context));
		cached.effects.from_dictionary(action->get_effective_effects(context));
		if (cached.effects.is_empty()) {
			// Without effects the action can never close an open condition.
			continue;
		}
		pool.push_back(cached);
	}

	LocalVector<SearchNode> nodes;
	LocalVector<int> open;
	// State hash -> indices of the nodes holding that state. States are only
	// compared in full on a hash hit.
	HashMap<uint32_t, LocalVector<int>> visited;

	SearchNode root;
	root.remaining = desired;
	root.hash = desired.hash();
	root.f = (double)current.count_unsatisfied(desired);
	nodes.push_back(root);
	heap_push(open, nodes, 0);
	visited[root.hash].push_back(0);

	int solution = -1;
	while (!open.is_empty() && last_iterations < max_iterations) {
		last_iterations++;
		const int index = heap_pop(open, nodes);

		// Copied because expanding the node can reallocate `nodes`.
		const GoapState remaining = nodes[index].remaining;
		const double g = nodes[index].g;
		const int depth = nodes[index].depth;

		if (current.satisfies(remaining)) {
			solution = index;
			break;
		}
		if (depth >= max_plan_length) {
			continue;
		}

		for (uint32_t i = 0; i < pool.size(); i++) {
			const CachedAction &cached = pool[i];

			// Keep the action only if it closes an open condition without
			// undoing another one.
			bool relevant = false;
			bool conflicts = false;
			for (const KeyValue<StringName, Variant> &E : cached.effects.facts) {
				const Variant *needed = remaining.facts.getptr(E.key);
				if (needed == nullptr) {
					continue;
				}
				if (GoapState::values_match(E.value, *needed)) {
					relevant = true;
				} else {
					conflicts = true;
					break;
				}
			}
			if (!relevant || conflicts) {
				continue;
			}

			// Regression step: what the action provides is no longer open, what
			// it requires becomes open in its place.
			SearchNode next;
			for (const KeyValue<StringName, Variant> &E : remaining.facts) {
				if (!cached.effects.has(E.key)) {
					next.remaining.set_fact(E.key, E.value);
				}
			}
			if (next.remaining.conflicts_with(cached.preconditions)) {
				// The action's own requirements contradict what is still open.
				continue;
			}
			for (const KeyValue<StringName, Variant> &E : cached.preconditions.facts) {
				next.remaining.set_fact(E.key, E.value);
			}

			next.g = g + cached.cost;
			next.f = next.g + (double)current.count_unsatisfied(next.remaining);
			next.parent = index;
			next.action = (int)i;
			next.depth = depth + 1;
			next.hash = next.remaining.hash();

			bool superseded = false;
			const LocalVector<int> *bucket = visited.getptr(next.hash);
			if (bucket != nullptr) {
				for (const int existing : *bucket) {
					if (nodes[existing].remaining == next.remaining) {
						superseded = nodes[existing].g <= next.g;
						break;
					}
				}
			}
			if (superseded) {
				continue;
			}

			nodes.push_back(next);
			const int next_index = (int)nodes.size() - 1;
			visited[next.hash].push_back(next_index);
			heap_push(open, nodes, next_index);
		}
	}

	if (solution < 0) {
		return Ref<GoapPlan>();
	}

	// The search ran from the goal backwards, so walking from the solution node
	// up to the root already yields the actions in execution order.
	Vector<Ref<GoapAction>> steps;
	steps.resize(nodes[solution].depth);
	int index = solution;
	int step = 0;
	while (nodes[index].parent >= 0) {
		steps.write[step++] = pool[nodes[index].action].action;
		index = nodes[index].parent;
	}

	result->set_actions(steps);
	result->set_total_cost(nodes[solution].g);
	return result;
}

void GoapPlanner::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_max_iterations", "iterations"), &GoapPlanner::set_max_iterations);
	ClassDB::bind_method(D_METHOD("get_max_iterations"), &GoapPlanner::get_max_iterations);
	ClassDB::bind_method(D_METHOD("set_max_plan_length", "length"), &GoapPlanner::set_max_plan_length);
	ClassDB::bind_method(D_METHOD("get_max_plan_length"), &GoapPlanner::get_max_plan_length);
	ClassDB::bind_method(D_METHOD("get_last_iterations"), &GoapPlanner::get_last_iterations);

	ClassDB::bind_method(D_METHOD("plan", "context", "actions", "goals"), &GoapPlanner::plan);
	ClassDB::bind_method(D_METHOD("plan_for_goal", "context", "actions", "goal"), &GoapPlanner::plan_for_goal);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_iterations", PROPERTY_HINT_RANGE, "1,100000,1,or_greater"), "set_max_iterations", "get_max_iterations");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_plan_length", PROPERTY_HINT_RANGE, "1,64,1,or_greater"), "set_max_plan_length", "get_max_plan_length");
}
