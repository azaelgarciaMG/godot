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

	// Why the agent stopped executing a plan. Reported by `plan_aborted` so
	// that a deliberate replan can be told from a genuine failure.
	enum AbortReason {
		ABORT_REASON_REQUESTED, // `request_plan`, `abort_plan`, or a new action/goal list.
		ABORT_REASON_ACTION_FAILED, // The running action returned STATUS_FAILED.
		ABORT_REASON_ACTION_INVALID, // `_is_valid` turned false before the action started.
		ABORT_REASON_PRECONDITIONS_STALE, // The world moved on since the plan was built.
		ABORT_REASON_PLAN_INVALID, // The plan held a null action.
	};

	// How the action that just stopped ended. Reported by `action_finished`
	// alongside the `succeeded` flag.
	enum ActionResult {
		ACTION_RESULT_SUCCEEDED,
		ACTION_RESULT_FAILED, // The action itself reported failure.
		ACTION_RESULT_INTERRUPTED, // Something dropped the plan out from under it.
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
	bool async_planning = false;
	int planning_budget = 64;

	double time_since_planning = 0.0;
	bool action_running = false;
	bool has_planned = false;
	GoapPlanner::PlanResult last_plan_result = GoapPlanner::PLAN_RESULT_NO_GOALS;

	void _update_processing();
	void _finish_current_action(bool p_succeeded, ActionResult p_result);
	void _complete_plan();
	bool _plan();
	// Turns a finished planner query into `plan_found` / `planning_failed`.
	bool _collect_plan();

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

	// Spreads the search over several frames instead of blocking on it. The
	// search still runs on the main thread, so action and goal callbacks keep
	// their usual scene tree access. Agents that enable this must not share a
	// GoapPlanner instance, since it holds the in-flight search.
	void set_async_planning(bool p_enabled);
	bool is_async_planning() const { return async_planning; }

	// Nodes the sliced search may expand per frame.
	void set_planning_budget(int p_budget);
	int get_planning_budget() const { return planning_budget; }

	void set_active(bool p_active);
	bool is_active() const { return active; }

	void set_world_state(const Dictionary &p_world_state);
	Dictionary get_world_state() const;
	void set_state(const StringName &p_key, const Variant &p_value);
	Variant get_state(const StringName &p_key, const Variant &p_default = Variant()) const;
	bool has_state(const StringName &p_key) const;
	void erase_state(const StringName &p_key);

	// Discards the running plan and plans again right away, ignoring
	// `replan_delay` and `async_planning`. Returns `true` when a plan was found.
	bool request_plan();
	// Stops the running action and drops the plan.
	void abort_plan(AbortReason p_reason = ABORT_REASON_REQUESTED);

	Ref<GoapPlan> get_plan() const { return current_plan; }
	Ref<GoapAction> get_current_action() const;
	Ref<GoapGoal> get_current_goal() const;

	// Why the last planning attempt ended the way it did.
	GoapPlanner::PlanResult get_last_plan_result() const { return last_plan_result; }
	// True when the agent has no plan because there was nothing worth doing,
	// as opposed to having failed to find one. Distinguishes the two cases
	// `planning_failed` used to conflate.
	bool is_idle() const;

	// Reports authoring mistakes: goals asking for facts no action produces,
	// preconditions nothing can establish, and malformed comparison strings.
	// Also drives the scene tree's configuration warnings.
	PackedStringArray validate() const;
	PackedStringArray get_configuration_warnings() const override;

	// Advances the agent by one step. Called automatically unless
	// `process_callback` is `PROCESS_CALLBACK_MANUAL`.
	void update(double p_delta);

	GoapAgent();
};

VARIANT_ENUM_CAST(GoapAgent::ProcessCallback);
VARIANT_ENUM_CAST(GoapAgent::AbortReason);
VARIANT_ENUM_CAST(GoapAgent::ActionResult);
