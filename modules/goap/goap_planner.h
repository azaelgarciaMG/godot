/**************************************************************************/
/*  goap_planner.h                                                        */
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

#pragma once

#include "goap_action.h"
#include "goap_context.h"
#include "goap_goal.h"
#include "goap_plan.h"
#include "goap_state.h"

#include "core/object/ref_counted.h"
#include "core/variant/typed_array.h"

// Builds a plan with a backward (regressive) A* search: it starts from the
// goal's desired state and works towards the current world state, only ever
// considering actions that are relevant to a condition that is still open.
// That keeps the branching factor tied to the goal instead of to the size of
// the action set.
//
// Optimality: with the default `HEURISTIC_ADMISSIBLE` the search returns the
// cheapest plan that fits within `max_plan_length`. The heuristic is not just
// admissible but consistent, which is what makes it safe never to re-open a
// node that has already been reached more cheaply. `HEURISTIC_GREEDY` trades
// that guarantee for speed.
//
// Snapshotting: `_is_valid`, `_get_cost`, `_get_preconditions` and
// `_get_effects` are evaluated once per planning query, not once per search
// node. Context-varying preconditions therefore do not vary *within* a single
// plan; they are re-read the next time the agent plans.
class GoapPlanner : public RefCounted {
	GDCLASS(GoapPlanner, RefCounted);

public:
	// Why the last planning query ended the way it did.
	enum PlanResult {
		PLAN_RESULT_SUCCESS, // A plan with at least one action was found.
		PLAN_RESULT_IDLE, // Every valid goal is already satisfied.
		PLAN_RESULT_NO_GOALS, // Nothing to plan for at all.
		PLAN_RESULT_UNREACHABLE, // The search space was exhausted.
		PLAN_RESULT_BUDGET_EXHAUSTED, // Hit `max_iterations`.
		PLAN_RESULT_DEPTH_EXCEEDED, // Hit `max_plan_length`.
	};

	enum HeuristicMode {
		// Guarantees the cheapest plan. Scales the number of remaining steps by
		// the cheapest action in the pool, and divides by how many conditions a
		// single action can close at once.
		HEURISTIC_ADMISSIBLE,
		// Sums the per-fact step estimates instead. Usually reaches a plan in
		// far fewer expansions, but that plan may not be the cheapest one.
		HEURISTIC_GREEDY,
	};

private:
	// Search state, kept alive between slices of a time-sliced query.
	struct Search;

	int max_iterations = 1000;
	int max_plan_length = 16;
	int last_iterations = 0;
	HeuristicMode heuristic_mode = HEURISTIC_ADMISSIBLE;
	bool verbose = false;
	PlanResult last_result = PLAN_RESULT_NO_GOALS;

	Search *search = nullptr;

	// Common setup for both entry points; leaves the goal queue to the caller.
	void _begin(const Ref<GoapContext> &p_context, const TypedArray<GoapAction> &p_actions, const TypedArray<GoapGoal> &p_goals);
	// Prepares the next candidate goal. False once the queue is empty.
	bool _open_next_goal();
	// Expands at most `p_budget` nodes of the current goal, returning how many
	// it actually expanded.
	int _run_goal(int p_budget);
	// `p_solution` is the index of the node that closed the goal, or -1.
	void _close_goal(PlanResult p_result, int p_solution);
	void _finish_search();
	double _heuristic(const GoapConditions &p_remaining) const;

protected:
	static void _bind_methods();

public:
	void set_max_iterations(int p_iterations);
	int get_max_iterations() const { return max_iterations; }

	void set_max_plan_length(int p_length);
	int get_max_plan_length() const { return max_plan_length; }

	void set_heuristic_mode(HeuristicMode p_mode);
	HeuristicMode get_heuristic_mode() const { return heuristic_mode; }

	// Prints every expansion to the output. Debugging aid only; it is loud.
	void set_verbose(bool p_verbose);
	bool is_verbose() const { return verbose; }

	// Number of nodes expanded by the last search.
	int get_last_iterations() const { return last_iterations; }
	// Why the last query returned what it did. Prefer this over comparing
	// `get_last_iterations()` against `max_iterations` by hand.
	PlanResult get_last_result() const { return last_result; }

	// Returns the plan for the highest priority goal that is both valid and
	// reachable, or `null` when no goal can be planned for. Check
	// `get_last_result()` to tell "nothing to do" from "cannot be done".
	Ref<GoapPlan> plan(const Ref<GoapContext> &p_context, const TypedArray<GoapAction> &p_actions, const TypedArray<GoapGoal> &p_goals);
	// Returns `null` when the goal is unreachable, or a plan with no actions
	// when it is already satisfied.
	Ref<GoapPlan> plan_for_goal(const Ref<GoapContext> &p_context, const TypedArray<GoapAction> &p_actions, const Ref<GoapGoal> &p_goal);

	// Time-sliced planning. `plan_begin` snapshots the actions and goals and
	// returns immediately; `plan_step` then expands at most `max_iterations`
	// nodes per call and returns true once the search is over, at which point
	// `take_plan` yields the result. The whole search runs on the calling
	// thread, so action and goal callbacks keep their usual scene tree access.
	//
	// One planner can only run one sliced search at a time, so agents that use
	// this must not share a GoapPlanner instance.
	void plan_begin(const Ref<GoapContext> &p_context, const TypedArray<GoapAction> &p_actions, const TypedArray<GoapGoal> &p_goals);
	void plan_begin_for_goal(const Ref<GoapContext> &p_context, const TypedArray<GoapAction> &p_actions, const Ref<GoapGoal> &p_goal);
	// `p_max_iterations` of 0 or less means "run to completion".
	bool plan_step(int p_max_iterations);
	bool is_planning() const { return search != nullptr; }
	// Hands over the finished plan and clears the search. Returns `null` when
	// the search failed or is still running.
	Ref<GoapPlan> take_plan();
	void plan_cancel();

	~GoapPlanner();
};

VARIANT_ENUM_CAST(GoapPlanner::PlanResult);
VARIANT_ENUM_CAST(GoapPlanner::HeuristicMode);
