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

#include "core/math/math_funcs.h"
#include "core/object/class_db.h"
#include "core/string/print_string.h"
#include "core/templates/local_vector.h"

// Helpers for the search. They live in a named namespace rather than an
// anonymous one so that `GoapPlanner::Search` can hold them as members without
// mixing linkage.
namespace goap_internal {

// An action as seen by the search. `is_valid()`, the cost, the preconditions
// and the effects can all be script calls, so they are resolved once per query
// instead of once per expansion.
struct CachedAction {
	Ref<GoapAction> action;
	GoapConditions preconditions;
	GoapEffects effects;
	double cost = 0.0;
};

struct SearchNode {
	GoapConditions remaining; // Conditions still to be satisfied, in goal space.
	double g = 0.0;
	double f = 0.0;
	int parent = -1;
	int action = -1; // Index into the cached action pool.
	int depth = 0;
	uint32_t hash = 0;
};

// Everything the heuristic needs, derived from the action pool once per query.
struct HeuristicData {
	double min_cost = 0.0;
	int max_effects = 1;
	// Largest single-step progress available per fact. Infinity marks a fact
	// that some action assigns outright, closing it in one step.
	HashMap<StringName, double> best_step;
};

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

String condition_to_string(const StringName &p_key, const GoapCondition &p_condition) {
	if (p_condition.kind == GoapCondition::KIND_EXACT) {
		return vformat("%s==%s", p_key, p_condition.exact);
	}
	String text = p_key;
	if (!Math::is_inf(p_condition.min)) {
		text += vformat("%s%s", p_condition.min_exclusive ? ">" : ">=", rtos(p_condition.min));
	}
	if (!Math::is_inf(p_condition.max)) {
		text += vformat("%s%s", p_condition.max_exclusive ? "<" : "<=", rtos(p_condition.max));
	}
	return text;
}

String conditions_to_string(const GoapConditions &p_conditions) {
	Vector<String> parts;
	for (const KeyValue<StringName, GoapCondition> &E : p_conditions.conditions) {
		parts.push_back(condition_to_string(E.key, E.value));
	}
	parts.sort(); // Facts are unordered; sorting keeps the log readable.
	return "{" + String(", ").join(parts) + "}";
}

} // namespace goap_internal

using namespace goap_internal;

// Search state, kept alive across the slices of a time-sliced query.
struct GoapPlanner::Search {
	Ref<GoapContext> context;
	TypedArray<GoapAction> actions;
	TypedArray<GoapGoal> goals;

	// Goal queue, highest priority first.
	LocalVector<GoalCandidate> candidates;
	uint32_t next_candidate = 0;
	bool single_goal = false;

	// State of the goal currently being searched.
	bool goal_open = false;
	Ref<GoapGoal> goal;
	GoapState current;
	LocalVector<CachedAction> pool;
	HeuristicData heuristic;
	LocalVector<SearchNode> nodes;
	LocalVector<int> open;
	// State hash -> index of the cheapest node holding that state. States are
	// only compared in full on a hash hit.
	HashMap<uint32_t, LocalVector<int>> visited;
	int iterations = 0;
	bool depth_pruned = false;

	// Outcome, aggregated over every goal tried so far.
	int total_iterations = 0;
	bool saw_idle = false;
	bool saw_budget = false;
	bool saw_depth = false;
	bool saw_unreachable = false;

	Ref<GoapPlan> plan;
	bool succeeded = false;
	bool done = false;
};

void GoapPlanner::set_max_iterations(int p_iterations) {
	max_iterations = MAX(1, p_iterations);
}

void GoapPlanner::set_max_plan_length(int p_length) {
	max_plan_length = MAX(1, p_length);
}

void GoapPlanner::set_heuristic_mode(HeuristicMode p_mode) {
	heuristic_mode = p_mode;
}

void GoapPlanner::set_verbose(bool p_verbose) {
	verbose = p_verbose;
}

GoapPlanner::~GoapPlanner() {
	memdelete_notnull(search);
}

void GoapPlanner::plan_cancel() {
	if (search != nullptr) {
		memdelete(search);
		search = nullptr;
	}
}

Ref<GoapPlan> GoapPlanner::take_plan() {
	if (search == nullptr || !search->done) {
		return Ref<GoapPlan>();
	}
	Ref<GoapPlan> result = search->plan;
	plan_cancel();
	return result;
}

void GoapPlanner::_begin(const Ref<GoapContext> &p_context, const TypedArray<GoapAction> &p_actions, const TypedArray<GoapGoal> &p_goals) {
	plan_cancel();
	search = memnew(Search);
	// Planning without a context is allowed; actions then see an empty one.
	search->context = p_context;
	if (search->context.is_null()) {
		search->context.instantiate();
	}
	search->actions = p_actions;
	search->goals = p_goals;

	last_iterations = 0;
	last_result = PLAN_RESULT_NO_GOALS;
}

void GoapPlanner::plan_begin(const Ref<GoapContext> &p_context, const TypedArray<GoapAction> &p_actions, const TypedArray<GoapGoal> &p_goals) {
	_begin(p_context, p_actions, p_goals);

	for (int i = 0; i < p_goals.size(); i++) {
		Ref<GoapGoal> goal = p_goals[i];
		if (goal.is_null() || !goal->is_valid(search->context)) {
			continue;
		}
		GoalCandidate candidate;
		candidate.priority = goal->get_effective_priority(search->context);
		candidate.index = i;
		search->candidates.push_back(candidate);
	}
	search->candidates.sort();
}

void GoapPlanner::plan_begin_for_goal(const Ref<GoapContext> &p_context, const TypedArray<GoapAction> &p_actions, const Ref<GoapGoal> &p_goal) {
	ERR_FAIL_COND(p_goal.is_null());

	TypedArray<GoapGoal> goals;
	goals.push_back(p_goal);
	_begin(p_context, p_actions, goals);

	// A directly requested goal is searched whatever `_is_valid` and the
	// priority say, and an already satisfied one yields an empty plan rather
	// than being skipped, so neither is consulted here.
	search->single_goal = true;
	GoalCandidate candidate;
	candidate.index = 0;
	search->candidates.push_back(candidate);
}

Ref<GoapPlan> GoapPlanner::plan(const Ref<GoapContext> &p_context, const TypedArray<GoapAction> &p_actions, const TypedArray<GoapGoal> &p_goals) {
	plan_begin(p_context, p_actions, p_goals);
	while (!plan_step(0)) {
		// `plan_step(0)` runs to completion, so this loop body is never reached.
	}
	return take_plan();
}

Ref<GoapPlan> GoapPlanner::plan_for_goal(const Ref<GoapContext> &p_context, const TypedArray<GoapAction> &p_actions, const Ref<GoapGoal> &p_goal) {
	ERR_FAIL_COND_V(p_goal.is_null(), Ref<GoapPlan>());
	plan_begin_for_goal(p_context, p_actions, p_goal);
	while (!plan_step(0)) {
	}
	return take_plan();
}

// Prepares the search for the next candidate goal. Returns false once the queue
// is empty. `goal_open` stays false for a goal that needs no search at all.
bool GoapPlanner::_open_next_goal() {
	Search &s = *search;
	s.goal_open = false;
	if (s.next_candidate >= s.candidates.size()) {
		return false;
	}

	s.goal = s.goals[s.candidates[s.next_candidate].index];
	s.next_candidate++;

	s.current.from_dictionary(s.context->get_world_state());

	GoapConditions desired;
	desired.from_dictionary(s.goal->get_effective_desired_state(s.context));

	if (desired.is_empty() || desired.satisfied_by(s.current)) {
		// Nothing to execute for this goal.
		s.saw_idle = true;
		if (s.single_goal) {
			Ref<GoapPlan> empty;
			empty.instantiate();
			empty->set_goal(s.goal);
			s.plan = empty;
			s.done = true;
		}
		return true;
	}

	s.pool.clear();
	for (int i = 0; i < s.actions.size(); i++) {
		Ref<GoapAction> action = s.actions[i];
		if (action.is_null() || !action->is_valid(s.context)) {
			continue;
		}
		CachedAction cached;
		cached.action = action;
		cached.cost = action->get_effective_cost(s.context);
		cached.preconditions.from_dictionary(action->get_effective_preconditions(s.context));
		cached.effects.from_dictionary(action->get_effective_effects(s.context));
		if (cached.effects.is_empty()) {
			// Without effects the action can never close an open condition.
			continue;
		}
		s.pool.push_back(cached);
	}

	// Derive the heuristic constants from the pool.
	s.heuristic = HeuristicData();
	s.heuristic.min_cost = Math::INF;
	for (const CachedAction &cached : s.pool) {
		s.heuristic.min_cost = MIN(s.heuristic.min_cost, cached.cost);
		s.heuristic.max_effects = MAX(s.heuristic.max_effects, cached.effects.size());
		for (const KeyValue<StringName, GoapEffect> &E : cached.effects.effects) {
			const double step = E.value.is_delta ? Math::abs(E.value.delta) : Math::INF;
			double *existing = s.heuristic.best_step.getptr(E.key);
			if (existing == nullptr) {
				s.heuristic.best_step[E.key] = step;
			} else {
				*existing = MAX(*existing, step);
			}
		}
	}
	if (s.pool.is_empty()) {
		s.heuristic.min_cost = 0.0;
	}

	s.nodes.clear();
	s.open.clear();
	s.visited.clear();
	s.iterations = 0;
	s.depth_pruned = false;

	SearchNode root;
	root.remaining = desired;
	root.hash = desired.hash();
	root.f = _heuristic(desired);
	s.nodes.push_back(root);
	heap_push(s.open, s.nodes, 0);
	s.visited[root.hash].push_back(0);
	s.goal_open = true;

	if (verbose) {
		print_line(vformat("[GoapPlanner] goal \"%s\": searching for %s with %d action(s), min_cost=%s.",
				s.goal->get_goal_name(), conditions_to_string(desired), s.pool.size(), rtos(s.heuristic.min_cost)));
	}
	return true;
}

double GoapPlanner::_heuristic(const GoapConditions &p_remaining) const {
	const Search &s = *search;
	const HeuristicData &h = s.heuristic;

	int unsatisfied = 0;
	int max_steps = 0;
	int sum_steps = 0;
	for (const KeyValue<StringName, GoapCondition> &E : p_remaining.conditions) {
		const Variant *value = s.current.getptr(E.key);
		if (E.value.accepts(value)) {
			continue;
		}
		unsatisfied++;
		const double *step = h.best_step.getptr(E.key);
		// A fact no action touches cannot be closed at all; one action is still
		// a valid lower bound, and the search prunes the branch on its own.
		const int steps = step == nullptr ? 1 : E.value.steps_from(value, *step);
		max_steps = MAX(max_steps, steps);
		sum_steps += steps;
	}
	if (unsatisfied == 0) {
		return 0.0;
	}

	if (heuristic_mode == HEURISTIC_GREEDY) {
		// Assumes every open fact needs its own actions. Usually far better
		// guidance, but it can overestimate and so return a costlier plan.
		return (double)sum_steps * h.min_cost;
	}

	// A single action closes at most `max_effects` open facts at once, and one
	// fact may need several delta steps of its own. Both are lower bounds on
	// the number of actions still to come, so the tighter one wins. Scaling by
	// the cheapest action in the pool keeps the estimate at or below the true
	// remaining cost, which is what makes the search return the cheapest plan.
	const int by_count = (unsatisfied + h.max_effects - 1) / h.max_effects;
	return (double)MAX(by_count, max_steps) * h.min_cost;
}

void GoapPlanner::_close_goal(PlanResult p_result, int p_solution) {
	Search &s = *search;
	s.goal_open = false;

	switch (p_result) {
		case PLAN_RESULT_BUDGET_EXHAUSTED: {
			s.saw_budget = true;
		} break;
		case PLAN_RESULT_DEPTH_EXCEEDED: {
			s.saw_depth = true;
		} break;
		case PLAN_RESULT_UNREACHABLE: {
			s.saw_unreachable = true;
		} break;
		default: {
		} break;
	}

	if (verbose && p_result != PLAN_RESULT_SUCCESS) {
		print_line(vformat("[GoapPlanner] goal \"%s\": gave up after %d expansion(s) (%d).",
				s.goal->get_goal_name(), s.iterations, (int)p_result));
	}
	if (p_result != PLAN_RESULT_SUCCESS) {
		return;
	}

	// The search ran from the goal backwards, so walking from the solution node
	// up to the root already yields the actions in execution order.
	Vector<Ref<GoapAction>> steps;
	steps.resize(s.nodes[p_solution].depth);
	int index = p_solution;
	int step = 0;
	while (s.nodes[index].parent >= 0) {
		steps.write[step++] = s.pool[s.nodes[index].action].action;
		index = s.nodes[index].parent;
	}

	Ref<GoapPlan> result;
	result.instantiate();
	result->set_goal(s.goal);
	result->set_actions(steps);
	result->set_total_cost(s.nodes[p_solution].g);
	s.plan = result;
	s.succeeded = true;
	s.done = true;

	if (verbose) {
		print_line(vformat("[GoapPlanner] goal \"%s\": %s after %d expansion(s).",
				s.goal->get_goal_name(), result->to_string(), s.iterations));
	}
}

// Expands at most `p_budget` nodes of the current goal's search. Returns how
// many it actually expanded.
int GoapPlanner::_run_goal(int p_budget) {
	Search &s = *search;
	int spent = 0;

	while (spent < p_budget) {
		if (s.open.is_empty()) {
			_close_goal(s.depth_pruned ? PLAN_RESULT_DEPTH_EXCEEDED : PLAN_RESULT_UNREACHABLE, -1);
			return spent;
		}
		if (s.iterations >= max_iterations) {
			_close_goal(PLAN_RESULT_BUDGET_EXHAUSTED, -1);
			return spent;
		}
		s.iterations++;
		s.total_iterations++;
		spent++;

		const int index = heap_pop(s.open, s.nodes);

		// Copied because expanding the node can reallocate `nodes`.
		const GoapConditions remaining = s.nodes[index].remaining;
		const double g = s.nodes[index].g;
		const int depth = s.nodes[index].depth;

		if (verbose) {
			print_line(vformat("[GoapPlanner]   pop g=%s depth=%d %s", rtos(g), depth, conditions_to_string(remaining)));
		}

		if (remaining.satisfied_by(s.current)) {
			_close_goal(PLAN_RESULT_SUCCESS, index);
			return spent;
		}
		if (depth >= max_plan_length) {
			s.depth_pruned = true;
			continue;
		}

		for (uint32_t i = 0; i < s.pool.size(); i++) {
			const CachedAction &cached = s.pool[i];

			// Keep the action only if it moves an open condition towards being
			// met without contradicting another one.
			bool relevant = false;
			bool conflicts = false;
			for (const KeyValue<StringName, GoapEffect> &E : cached.effects.effects) {
				const GoapCondition *needed = remaining.getptr(E.key);
				if (needed == nullptr) {
					continue;
				}
				if (!E.value.is_delta) {
					if (needed->accepts(&E.value.value)) {
						relevant = true;
					} else {
						conflicts = true;
						break;
					}
					continue;
				}

				double target = 0.0;
				if (needed->kind == GoapCondition::KIND_EXACT && !GoapState::value_as_number(needed->exact, target)) {
					// A delta can never produce a non-numeric fact.
					conflicts = true;
					break;
				}
				// A delta is worth taking only when it pushes the fact the way
				// the open condition needs it to go.
				const int direction = needed->direction_from(s.current.getptr(E.key));
				if (direction != 0 && E.value.delta != 0.0 && (E.value.delta > 0.0) == (direction > 0)) {
					relevant = true;
				}
			}
			if (!relevant || conflicts) {
				continue;
			}

			// Regression step: what the action assigns is no longer open, what
			// it adds to is rolled back by the same amount, and what it
			// requires becomes open in its place.
			SearchNode next;
			bool broken = false;
			for (const KeyValue<StringName, GoapCondition> &E : remaining.conditions) {
				const GoapEffect *effect = cached.effects.getptr(E.key);
				if (effect == nullptr) {
					next.remaining.set_condition(E.key, E.value);
					continue;
				}
				if (!effect->is_delta) {
					// Checked above: the assignment satisfies this condition.
					continue;
				}
				GoapCondition shifted = E.value;
				if (!shifted.shift(effect->delta)) {
					broken = true;
					break;
				}
				next.remaining.set_condition(E.key, shifted);
			}
			if (broken) {
				continue;
			}
			if (!next.remaining.intersect(cached.preconditions)) {
				// The action's own requirements contradict what is still open.
				continue;
			}

			next.g = g + cached.cost;
			next.f = next.g + _heuristic(next.remaining);
			next.parent = index;
			next.action = (int)i;
			next.depth = depth + 1;
			next.hash = next.remaining.hash();

			LocalVector<int> &bucket = s.visited[next.hash];
			int slot = -1;
			for (uint32_t b = 0; b < bucket.size(); b++) {
				if (s.nodes[bucket[b]].remaining == next.remaining) {
					slot = (int)b;
					break;
				}
			}
			if (slot >= 0 && s.nodes[bucket[slot]].g <= next.g) {
				continue;
			}

			s.nodes.push_back(next);
			const int next_index = (int)s.nodes.size() - 1;
			if (slot >= 0) {
				// Keep one entry per state so that the check above always
				// compares against the cheapest node reaching it.
				bucket[slot] = next_index;
			} else {
				bucket.push_back(next_index);
			}
			heap_push(s.open, s.nodes, next_index);

			if (verbose) {
				print_line(vformat("[GoapPlanner]     via \"%s\" g=%s f=%s %s",
						cached.action->get_action_name(), rtos(next.g), rtos(next.f), conditions_to_string(next.remaining)));
			}
		}
	}
	return spent;
}

void GoapPlanner::_finish_search() {
	Search &s = *search;
	s.done = true;
	last_iterations = s.total_iterations;

	if (s.succeeded) {
		last_result = PLAN_RESULT_SUCCESS;
	} else if (s.candidates.is_empty()) {
		last_result = PLAN_RESULT_NO_GOALS;
	} else if (s.saw_budget) {
		// Reported ahead of the others because it is the one the user can fix
		// by raising `max_iterations`.
		last_result = PLAN_RESULT_BUDGET_EXHAUSTED;
	} else if (s.saw_depth) {
		last_result = PLAN_RESULT_DEPTH_EXCEEDED;
	} else if (s.saw_idle) {
		last_result = PLAN_RESULT_IDLE;
	} else if (s.saw_unreachable) {
		last_result = PLAN_RESULT_UNREACHABLE;
	} else {
		last_result = PLAN_RESULT_NO_GOALS;
	}
}

bool GoapPlanner::plan_step(int p_max_iterations) {
	ERR_FAIL_NULL_V_MSG(search, true, "No planning query is in progress. Call \"plan_begin\" first.");
	if (search->done) {
		_finish_search();
		return true;
	}

	int budget = p_max_iterations > 0 ? p_max_iterations : INT32_MAX;
	while (true) {
		if (!search->goal_open) {
			if (!_open_next_goal()) {
				_finish_search();
				return true;
			}
			if (search->done) {
				// A single-goal query that was already satisfied.
				_finish_search();
				return true;
			}
			if (!search->goal_open) {
				continue; // Goal needed no search; try the next one.
			}
		}

		budget -= _run_goal(budget);
		if (search->succeeded) {
			_finish_search();
			return true;
		}
		if (budget <= 0) {
			return false;
		}
	}
}

void GoapPlanner::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_max_iterations", "iterations"), &GoapPlanner::set_max_iterations);
	ClassDB::bind_method(D_METHOD("get_max_iterations"), &GoapPlanner::get_max_iterations);
	ClassDB::bind_method(D_METHOD("set_max_plan_length", "length"), &GoapPlanner::set_max_plan_length);
	ClassDB::bind_method(D_METHOD("get_max_plan_length"), &GoapPlanner::get_max_plan_length);
	ClassDB::bind_method(D_METHOD("set_heuristic_mode", "mode"), &GoapPlanner::set_heuristic_mode);
	ClassDB::bind_method(D_METHOD("get_heuristic_mode"), &GoapPlanner::get_heuristic_mode);
	ClassDB::bind_method(D_METHOD("set_verbose", "verbose"), &GoapPlanner::set_verbose);
	ClassDB::bind_method(D_METHOD("is_verbose"), &GoapPlanner::is_verbose);
	ClassDB::bind_method(D_METHOD("get_last_iterations"), &GoapPlanner::get_last_iterations);
	ClassDB::bind_method(D_METHOD("get_last_result"), &GoapPlanner::get_last_result);

	ClassDB::bind_method(D_METHOD("plan", "context", "actions", "goals"), &GoapPlanner::plan);
	ClassDB::bind_method(D_METHOD("plan_for_goal", "context", "actions", "goal"), &GoapPlanner::plan_for_goal);

	ClassDB::bind_method(D_METHOD("plan_begin", "context", "actions", "goals"), &GoapPlanner::plan_begin);
	ClassDB::bind_method(D_METHOD("plan_begin_for_goal", "context", "actions", "goal"), &GoapPlanner::plan_begin_for_goal);
	ClassDB::bind_method(D_METHOD("plan_step", "max_iterations"), &GoapPlanner::plan_step, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("is_planning"), &GoapPlanner::is_planning);
	ClassDB::bind_method(D_METHOD("take_plan"), &GoapPlanner::take_plan);
	ClassDB::bind_method(D_METHOD("plan_cancel"), &GoapPlanner::plan_cancel);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_iterations", PROPERTY_HINT_RANGE, "1,100000,1,or_greater"), "set_max_iterations", "get_max_iterations");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_plan_length", PROPERTY_HINT_RANGE, "1,64,1,or_greater"), "set_max_plan_length", "get_max_plan_length");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "heuristic_mode", PROPERTY_HINT_ENUM, "Admissible,Greedy"), "set_heuristic_mode", "get_heuristic_mode");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "verbose"), "set_verbose", "is_verbose");

	BIND_ENUM_CONSTANT(PLAN_RESULT_SUCCESS);
	BIND_ENUM_CONSTANT(PLAN_RESULT_IDLE);
	BIND_ENUM_CONSTANT(PLAN_RESULT_NO_GOALS);
	BIND_ENUM_CONSTANT(PLAN_RESULT_UNREACHABLE);
	BIND_ENUM_CONSTANT(PLAN_RESULT_BUDGET_EXHAUSTED);
	BIND_ENUM_CONSTANT(PLAN_RESULT_DEPTH_EXCEEDED);

	BIND_ENUM_CONSTANT(HEURISTIC_ADMISSIBLE);
	BIND_ENUM_CONSTANT(HEURISTIC_GREEDY);
}
