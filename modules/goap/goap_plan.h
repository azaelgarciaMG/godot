/**************************************************************************/
/*  goap_plan.h                                                           */
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
#include "goap_goal.h"

#include "core/object/ref_counted.h"
#include "core/variant/typed_array.h"

// The ordered list of actions GoapPlanner found for a goal, plus a cursor
// tracking how far the agent has executed it.
class GoapPlan : public RefCounted {
	GDCLASS(GoapPlan, RefCounted);

	Ref<GoapGoal> goal;
	Vector<Ref<GoapAction>> actions;
	double total_cost = 0.0;
	int cursor = 0;

protected:
	static void _bind_methods();

public:
	void set_goal(const Ref<GoapGoal> &p_goal) { goal = p_goal; }
	Ref<GoapGoal> get_goal() const { return goal; }

	void set_actions(const Vector<Ref<GoapAction>> &p_actions);
	const Vector<Ref<GoapAction>> &get_action_list() const { return actions; }
	TypedArray<GoapAction> get_actions() const;
	void set_actions_typed(const TypedArray<GoapAction> &p_actions);

	void set_total_cost(double p_cost) { total_cost = p_cost; }
	double get_total_cost() const { return total_cost; }

	int get_action_count() const { return actions.size(); }
	Ref<GoapAction> get_action(int p_index) const;

	// Execution cursor.
	Ref<GoapAction> get_current_action() const;
	int get_cursor() const { return cursor; }
	void advance();
	void reset() { cursor = 0; }
	bool is_finished() const { return cursor >= actions.size(); }
	bool is_empty() const { return actions.is_empty(); }
};
