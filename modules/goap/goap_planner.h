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

#include "core/object/ref_counted.h"
#include "core/variant/typed_array.h"

// Builds a plan with a backward (regressive) A* search: it starts from the
// goal's desired state and works towards the current world state, only ever
// considering actions that are relevant to a condition that is still open.
// That keeps the branching factor tied to the goal instead of to the size of
// the action set.
class GoapPlanner : public RefCounted {
	GDCLASS(GoapPlanner, RefCounted);

	int max_iterations = 1000;
	int max_plan_length = 16;
	int last_iterations = 0;

protected:
	static void _bind_methods();

public:
	void set_max_iterations(int p_iterations);
	int get_max_iterations() const { return max_iterations; }

	void set_max_plan_length(int p_length);
	int get_max_plan_length() const { return max_plan_length; }

	// Number of nodes expanded by the last search. Compare it against
	// `max_iterations` to tell an exhausted budget from an unreachable goal.
	int get_last_iterations() const { return last_iterations; }

	// Returns the plan for the highest priority goal that is both valid and
	// reachable, or `null` when no goal can be planned for.
	Ref<GoapPlan> plan(const Ref<GoapContext> &p_context, const TypedArray<GoapAction> &p_actions, const TypedArray<GoapGoal> &p_goals);
	// Returns `null` when the goal is unreachable, or a plan with no actions
	// when it is already satisfied.
	Ref<GoapPlan> plan_for_goal(const Ref<GoapContext> &p_context, const TypedArray<GoapAction> &p_actions, const Ref<GoapGoal> &p_goal);
};
