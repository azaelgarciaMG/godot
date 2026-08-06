/**************************************************************************/
/*  goap_agent.h                                                          */
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
#include "goap_planner.h"

#include "scene/main/node.h"

// Drives the plan/execute loop: it owns the world state, asks GoapPlanner for a
// plan whenever it has none, and ticks the plan's current action until the plan
// completes or an action reports failure.
class GoapAgent : public Node {
	GDCLASS(GoapAgent, Node);

public:
	enum ProcessCallback {
		PROCESS_CALLBACK_IDLE,
		PROCESS_CALLBACK_PHYSICS,
		PROCESS_CALLBACK_MANUAL,
	};

private:
	TypedArray<GoapAction> actions;
	TypedArray<GoapGoal> goals;
	Ref<GoapPlanner> planner;
	Ref<GoapContext> context;
	Ref<GoapPlan> current_plan;

	ProcessCallback process_callback = PROCESS_CALLBACK_IDLE;
	double replan_delay = 0.0;
	bool active = true;

	double time_since_planning = 0.0;
	bool action_running = false;
	bool has_planned = false;

	void _update_processing();
	void _finish_current_action(bool p_succeeded);
	void _complete_plan();
	bool _plan();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_actions(const TypedArray<GoapAction> &p_actions);
	TypedArray<GoapAction> get_actions() const { return actions; }

	void set_goals(const TypedArray<GoapGoal> &p_goals);
	TypedArray<GoapGoal> get_goals() const { return goals; }

	void set_planner(const Ref<GoapPlanner> &p_planner);
	Ref<GoapPlanner> get_planner() const { return planner; }

	void set_context(const Ref<GoapContext> &p_context);
	Ref<GoapContext> get_context() const { return context; }

	void set_process_callback(ProcessCallback p_mode);
	ProcessCallback get_process_callback() const { return process_callback; }

	// Minimum time between two planning attempts, in seconds. Keeps an agent
	// with no reachable goal from re-running the search every frame.
	void set_replan_delay(double p_delay);
	double get_replan_delay() const { return replan_delay; }

	void set_active(bool p_active);
	bool is_active() const { return active; }

	void set_world_state(const Dictionary &p_world_state);
	Dictionary get_world_state() const;
	void set_state(const StringName &p_key, const Variant &p_value);
	Variant get_state(const StringName &p_key, const Variant &p_default = Variant()) const;
	bool has_state(const StringName &p_key) const;
	void erase_state(const StringName &p_key);

	// Discards the running plan and plans again right away, ignoring
	// `replan_delay`. Returns `true` when a plan was found.
	bool request_plan();
	// Stops the running action and drops the plan.
	void abort_plan();

	Ref<GoapPlan> get_plan() const { return current_plan; }
	Ref<GoapAction> get_current_action() const;
	Ref<GoapGoal> get_current_goal() const;

	// Advances the agent by one step. Called automatically unless
	// `process_callback` is `PROCESS_CALLBACK_MANUAL`.
	void update(double p_delta);

	GoapAgent();
};

VARIANT_ENUM_CAST(GoapAgent::ProcessCallback);
