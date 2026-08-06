/**************************************************************************/
/*  goap_agent.cpp                                                        */
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

#include "goap_agent.h"

#include "goap_debugger.h"
#include "goap_state.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "core/templates/hash_set.h"

namespace {

// A name for a resource that may have none, so warnings stay readable.
String describe(const StringName &p_name, int p_index) {
	if (p_name != StringName()) {
		return String(p_name);
	}
	return vformat("#%d", p_index);
}

} // namespace

GoapAgent::GoapAgent() {
	context.instantiate();
	context->set_agent(this);
	planner.instantiate();
}

void GoapAgent::set_actions(const TypedArray<GoapAction> &p_actions) {
	actions = p_actions;
	abort_plan();
	update_configuration_warnings();
}

void GoapAgent::set_goals(const TypedArray<GoapGoal> &p_goals) {
	goals = p_goals;
	abort_plan();
	update_configuration_warnings();
}

void GoapAgent::set_planner(const Ref<GoapPlanner> &p_planner) {
	if (planner.is_valid()) {
		planner->plan_cancel();
	}
	planner = p_planner;
	if (planner.is_null()) {
		planner.instantiate();
	}
}

void GoapAgent::set_context(const Ref<GoapContext> &p_context) {
	context = p_context;
	if (context.is_null()) {
		context.instantiate();
	}
	context->set_agent(this);
	abort_plan();
}

void GoapAgent::set_process_callback(ProcessCallback p_mode) {
	process_callback = p_mode;
	_update_processing();
}

void GoapAgent::set_replan_delay(double p_delay) {
	replan_delay = MAX(0.0, p_delay);
}

void GoapAgent::set_async_planning(bool p_enabled) {
	if (async_planning == p_enabled) {
		return;
	}
	async_planning = p_enabled;
	// A sliced search in flight belongs to the old mode.
	planner->plan_cancel();
}

void GoapAgent::set_planning_budget(int p_budget) {
	planning_budget = MAX(1, p_budget);
}

void GoapAgent::set_active(bool p_active) {
	if (active == p_active) {
		return;
	}
	active = p_active;
	if (!active) {
		abort_plan();
	}
	_update_processing();
}

void GoapAgent::set_world_state(const Dictionary &p_world_state) {
	context->set_world_state(p_world_state);
	update_configuration_warnings();
}

Dictionary GoapAgent::get_world_state() const {
	return context->get_world_state();
}

void GoapAgent::set_state(const StringName &p_key, const Variant &p_value) {
	context->set_state(p_key, p_value);
}

Variant GoapAgent::get_state(const StringName &p_key, const Variant &p_default) const {
	return context->get_state(p_key, p_default);
}

bool GoapAgent::has_state(const StringName &p_key) const {
	return context->has_state(p_key);
}

void GoapAgent::erase_state(const StringName &p_key) {
	context->erase_state(p_key);
}

Ref<GoapAction> GoapAgent::get_current_action() const {
	if (current_plan.is_null()) {
		return Ref<GoapAction>();
	}
	return current_plan->get_current_action();
}

Ref<GoapGoal> GoapAgent::get_current_goal() const {
	if (current_plan.is_null()) {
		return Ref<GoapGoal>();
	}
	return current_plan->get_goal();
}

bool GoapAgent::is_idle() const {
	if (current_plan.is_valid() || planner->is_planning()) {
		return false;
	}
	// "Nothing worth doing" rather than "could not work out what to do".
	return has_planned &&
			(last_plan_result == GoapPlanner::PLAN_RESULT_IDLE || last_plan_result == GoapPlanner::PLAN_RESULT_NO_GOALS);
}

bool GoapAgent::request_plan() {
	abort_plan(ABORT_REASON_REQUESTED);
	// Always synchronous: the caller is asking for an answer right now.
	return _plan();
}

void GoapAgent::abort_plan(AbortReason p_reason) {
	planner->plan_cancel();
	if (current_plan.is_null()) {
		return;
	}
	if (action_running) {
		_finish_current_action(false, ACTION_RESULT_INTERRUPTED);
	}
	current_plan.unref();
	emit_signal(SNAME("plan_aborted"), p_reason);
}

bool GoapAgent::_plan() {
	planner->plan_begin(context, actions, goals);
	planner->plan_step(0); // Runs the whole search in one go.
	return _collect_plan();
}

bool GoapAgent::_collect_plan() {
	time_since_planning = 0.0;
	has_planned = true;
	last_plan_result = planner->get_last_result();
	current_plan = planner->take_plan();
	action_running = false;

	// A plan with no actions means the goal was already satisfied, which is
	// nothing for the agent to execute.
	if (current_plan.is_valid() && current_plan->is_empty()) {
		current_plan.unref();
	}
	if (current_plan.is_null()) {
		emit_signal(SNAME("planning_failed"), last_plan_result);
		return false;
	}
	emit_signal(SNAME("plan_found"), current_plan);
	return true;
}

void GoapAgent::_finish_current_action(bool p_succeeded, ActionResult p_result) {
	Ref<GoapAction> action = get_current_action();
	action_running = false;
	if (action.is_null()) {
		return;
	}
	action->exit(context, p_succeeded);
	emit_signal(SNAME("action_finished"), action, p_succeeded, p_result);
}

void GoapAgent::_complete_plan() {
	current_plan.unref();
	action_running = false;
	emit_signal(SNAME("plan_completed"));
}

void GoapAgent::update(double p_delta) {
	if (!active) {
		return;
	}
	time_since_planning += p_delta;

	if (current_plan.is_valid() && current_plan->is_finished()) {
		_complete_plan();
	}
	if (current_plan.is_null()) {
		if (planner->is_planning()) {
			// A sliced search is already under way; give it this frame's share.
			if (!planner->plan_step(planning_budget)) {
				return;
			}
			if (!_collect_plan()) {
				return;
			}
		} else {
			// The delay only throttles repeated attempts; the first one is free.
			if (has_planned && replan_delay > 0.0 && time_since_planning < replan_delay) {
				return;
			}
			if (async_planning) {
				planner->plan_begin(context, actions, goals);
				return; // The first slice runs on the next update.
			}
			if (!_plan()) {
				return;
			}
		}
	}

	Ref<GoapAction> action = current_plan->get_current_action();
	if (action.is_null()) {
		abort_plan(ABORT_REASON_PLAN_INVALID);
		return;
	}

	if (!action_running) {
		// The world may have moved on since the plan was built, so the action
		// is re-checked right before it starts rather than trusted blindly.
		if (!action->is_valid(context)) {
			abort_plan(ABORT_REASON_ACTION_INVALID);
			return;
		}
		if (!action->check_preconditions(context->get_world_state(), context)) {
			abort_plan(ABORT_REASON_PRECONDITIONS_STALE);
			return;
		}
		action->enter(context);
		action_running = true;
		emit_signal(SNAME("action_started"), action);
	}

	const GoapAction::Status status = action->tick(context, p_delta);
	if (status == GoapAction::STATUS_RUNNING) {
		return;
	}

	const bool succeeded = status == GoapAction::STATUS_SUCCEEDED;
	_finish_current_action(succeeded, succeeded ? ACTION_RESULT_SUCCEEDED : ACTION_RESULT_FAILED);

	if (!succeeded) {
		abort_plan(ABORT_REASON_ACTION_FAILED);
		return;
	}
	if (action->is_auto_apply_effects()) {
		context->set_world_state(action->apply_effects(context->get_world_state(), context));
	}
	current_plan->advance();
	if (current_plan->is_finished()) {
		_complete_plan();
	}
}

PackedStringArray GoapAgent::validate() const {
	PackedStringArray warnings;

	if (goals.is_empty()) {
		warnings.push_back(RTR("This agent has no goals, so it will never plan anything."));
	}
	if (actions.is_empty()) {
		warnings.push_back(RTR("This agent has no actions, so no goal can ever be reached."));
	}

	// Every fact some action is able to bring about. An action that builds its
	// effects from script could produce anything, so a single one of those
	// switches the reachability check off rather than reporting false alarms.
	HashSet<StringName> produced;
	bool effects_unknown = false;
	for (int i = 0; i < actions.size(); i++) {
		Ref<GoapAction> action = actions[i];
		if (action.is_null()) {
			warnings.push_back(vformat(RTR("Action slot %d is empty."), i));
			continue;
		}
		const String label = describe(action->get_action_name(), i);
		if (action->has_dynamic_effects()) {
			effects_unknown = true;
			continue;
		}

		const Dictionary effects = action->get_effects();
		if (effects.is_empty()) {
			warnings.push_back(vformat(RTR("Action \"%s\" has no effects, so the planner will never select it."), label));
		}
		for (const KeyValue<Variant, Variant> &E : effects) {
			produced.insert(E.key);
			if (GoapEffect::is_malformed(E.value)) {
				warnings.push_back(vformat(RTR("Effect \"%s\" of action \"%s\" starts with an operator but is not followed by a number, so it is stored as a plain string."), E.key, label));
			}
		}
	}

	// Facts that already hold count as reachable even if nothing produces them,
	// since a goal may legitimately depend on a static world fact.
	const Dictionary world_state = context->get_world_state();

	for (int i = 0; i < goals.size(); i++) {
		Ref<GoapGoal> goal = goals[i];
		if (goal.is_null()) {
			warnings.push_back(vformat(RTR("Goal slot %d is empty."), i));
			continue;
		}
		const String label = describe(goal->get_goal_name(), i);
		if (goal->has_dynamic_desired_state()) {
			continue;
		}

		const Dictionary desired = goal->get_desired_state();
		if (desired.is_empty()) {
			warnings.push_back(vformat(RTR("Goal \"%s\" has an empty desired state, so it is always satisfied."), label));
		}
		for (const KeyValue<Variant, Variant> &E : desired) {
			if (GoapCondition::is_malformed(E.value)) {
				warnings.push_back(vformat(RTR("Condition \"%s\" of goal \"%s\" starts with an operator but is not followed by a number, so it is compared as a plain string."), E.key, label));
			}
			if (effects_unknown || produced.has(E.key) || world_state.has(E.key)) {
				continue;
			}
			warnings.push_back(vformat(RTR("Goal \"%s\" wants \"%s\", which no action produces and the world state does not define. Check for a typo; the planner will silently fail to reach this goal."), label, E.key));
		}
	}

	for (int i = 0; i < actions.size(); i++) {
		Ref<GoapAction> action = actions[i];
		if (action.is_null() || action->has_dynamic_preconditions()) {
			continue;
		}
		const String label = describe(action->get_action_name(), i);
		const Dictionary preconditions = action->get_preconditions();
		for (const KeyValue<Variant, Variant> &E : preconditions) {
			if (GoapCondition::is_malformed(E.value)) {
				warnings.push_back(vformat(RTR("Precondition \"%s\" of action \"%s\" starts with an operator but is not followed by a number, so it is compared as a plain string."), E.key, label));
			}
			if (effects_unknown || produced.has(E.key) || world_state.has(E.key)) {
				continue;
			}
			warnings.push_back(vformat(RTR("Action \"%s\" requires \"%s\", which no action produces and the world state does not define. Check for a typo; this action can never run."), label, E.key));
		}
	}

	return warnings;
}

PackedStringArray GoapAgent::get_configuration_warnings() const {
	PackedStringArray warnings = Node::get_configuration_warnings();
	warnings.append_array(validate());
	return warnings;
}

void GoapAgent::_update_processing() {
	if (Engine::get_singleton()->is_editor_hint()) {
		set_process(false);
		set_physics_process(false);
		return;
	}
	const bool running = active && is_inside_tree();
	set_process(running && process_callback == PROCESS_CALLBACK_IDLE);
	set_physics_process(running && process_callback == PROCESS_CALLBACK_PHYSICS);
}

void GoapAgent::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			_update_processing();
			GoapDebugger::register_agent(this);
		} break;

		case NOTIFICATION_EXIT_TREE: {
			GoapDebugger::unregister_agent(this);
			abort_plan();
		} break;

		case NOTIFICATION_PROCESS: {
			update(get_process_delta_time());
		} break;

		case NOTIFICATION_PHYSICS_PROCESS: {
			update(get_physics_process_delta_time());
		} break;
	}
}

void GoapAgent::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_actions", "actions"), &GoapAgent::set_actions);
	ClassDB::bind_method(D_METHOD("get_actions"), &GoapAgent::get_actions);
	ClassDB::bind_method(D_METHOD("set_goals", "goals"), &GoapAgent::set_goals);
	ClassDB::bind_method(D_METHOD("get_goals"), &GoapAgent::get_goals);
	ClassDB::bind_method(D_METHOD("set_planner", "planner"), &GoapAgent::set_planner);
	ClassDB::bind_method(D_METHOD("get_planner"), &GoapAgent::get_planner);
	ClassDB::bind_method(D_METHOD("set_context", "context"), &GoapAgent::set_context);
	ClassDB::bind_method(D_METHOD("get_context"), &GoapAgent::get_context);
	ClassDB::bind_method(D_METHOD("set_process_callback", "mode"), &GoapAgent::set_process_callback);
	ClassDB::bind_method(D_METHOD("get_process_callback"), &GoapAgent::get_process_callback);
	ClassDB::bind_method(D_METHOD("set_replan_delay", "delay"), &GoapAgent::set_replan_delay);
	ClassDB::bind_method(D_METHOD("get_replan_delay"), &GoapAgent::get_replan_delay);
	ClassDB::bind_method(D_METHOD("set_async_planning", "enabled"), &GoapAgent::set_async_planning);
	ClassDB::bind_method(D_METHOD("is_async_planning"), &GoapAgent::is_async_planning);
	ClassDB::bind_method(D_METHOD("set_planning_budget", "budget"), &GoapAgent::set_planning_budget);
	ClassDB::bind_method(D_METHOD("get_planning_budget"), &GoapAgent::get_planning_budget);
	ClassDB::bind_method(D_METHOD("set_active", "active"), &GoapAgent::set_active);
	ClassDB::bind_method(D_METHOD("is_active"), &GoapAgent::is_active);

	ClassDB::bind_method(D_METHOD("set_world_state", "world_state"), &GoapAgent::set_world_state);
	ClassDB::bind_method(D_METHOD("get_world_state"), &GoapAgent::get_world_state);
	ClassDB::bind_method(D_METHOD("set_state", "key", "value"), &GoapAgent::set_state);
	ClassDB::bind_method(D_METHOD("get_state", "key", "default"), &GoapAgent::get_state, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("has_state", "key"), &GoapAgent::has_state);
	ClassDB::bind_method(D_METHOD("erase_state", "key"), &GoapAgent::erase_state);

	ClassDB::bind_method(D_METHOD("request_plan"), &GoapAgent::request_plan);
	ClassDB::bind_method(D_METHOD("abort_plan", "reason"), &GoapAgent::abort_plan, DEFVAL(ABORT_REASON_REQUESTED));
	ClassDB::bind_method(D_METHOD("get_plan"), &GoapAgent::get_plan);
	ClassDB::bind_method(D_METHOD("get_current_action"), &GoapAgent::get_current_action);
	ClassDB::bind_method(D_METHOD("get_current_goal"), &GoapAgent::get_current_goal);
	ClassDB::bind_method(D_METHOD("get_last_plan_result"), &GoapAgent::get_last_plan_result);
	ClassDB::bind_method(D_METHOD("is_idle"), &GoapAgent::is_idle);
	ClassDB::bind_method(D_METHOD("validate"), &GoapAgent::validate);
	ClassDB::bind_method(D_METHOD("update", "delta"), &GoapAgent::update);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "active"), "set_active", "is_active");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "process_callback", PROPERTY_HINT_ENUM, "Idle,Physics,Manual"), "set_process_callback", "get_process_callback");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "replan_delay", PROPERTY_HINT_RANGE, "0,10,0.01,or_greater,suffix:s"), "set_replan_delay", "get_replan_delay");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "async_planning"), "set_async_planning", "is_async_planning");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "planning_budget", PROPERTY_HINT_RANGE, "1,4096,1,or_greater"), "set_planning_budget", "get_planning_budget");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "actions", PROPERTY_HINT_ARRAY_TYPE, MAKE_RESOURCE_TYPE_HINT("GoapAction")), "set_actions", "get_actions");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "goals", PROPERTY_HINT_ARRAY_TYPE, MAKE_RESOURCE_TYPE_HINT("GoapGoal")), "set_goals", "get_goals");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "world_state"), "set_world_state", "get_world_state");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "planner", PROPERTY_HINT_RESOURCE_TYPE, "GoapPlanner", PROPERTY_USAGE_NONE), "set_planner", "get_planner");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "context", PROPERTY_HINT_RESOURCE_TYPE, "GoapContext", PROPERTY_USAGE_NONE), "set_context", "get_context");

	ADD_SIGNAL(MethodInfo("plan_found", PropertyInfo(Variant::OBJECT, "plan", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT, "GoapPlan")));
	ADD_SIGNAL(MethodInfo("planning_failed", PropertyInfo(Variant::INT, "result", PROPERTY_HINT_ENUM, "Success,Idle,No Goals,Unreachable,Budget Exhausted,Depth Exceeded")));
	ADD_SIGNAL(MethodInfo("plan_completed"));
	ADD_SIGNAL(MethodInfo("plan_aborted", PropertyInfo(Variant::INT, "reason", PROPERTY_HINT_ENUM, "Requested,Action Failed,Action Invalid,Preconditions Stale,Plan Invalid")));
	ADD_SIGNAL(MethodInfo("action_started", PropertyInfo(Variant::OBJECT, "action", PROPERTY_HINT_RESOURCE_TYPE, "GoapAction")));
	ADD_SIGNAL(MethodInfo("action_finished", PropertyInfo(Variant::OBJECT, "action", PROPERTY_HINT_RESOURCE_TYPE, "GoapAction"), PropertyInfo(Variant::BOOL, "succeeded"), PropertyInfo(Variant::INT, "result", PROPERTY_HINT_ENUM, "Succeeded,Failed,Interrupted")));

	BIND_ENUM_CONSTANT(PROCESS_CALLBACK_IDLE);
	BIND_ENUM_CONSTANT(PROCESS_CALLBACK_PHYSICS);
	BIND_ENUM_CONSTANT(PROCESS_CALLBACK_MANUAL);

	BIND_ENUM_CONSTANT(ABORT_REASON_REQUESTED);
	BIND_ENUM_CONSTANT(ABORT_REASON_ACTION_FAILED);
	BIND_ENUM_CONSTANT(ABORT_REASON_ACTION_INVALID);
	BIND_ENUM_CONSTANT(ABORT_REASON_PRECONDITIONS_STALE);
	BIND_ENUM_CONSTANT(ABORT_REASON_PLAN_INVALID);

	BIND_ENUM_CONSTANT(ACTION_RESULT_SUCCEEDED);
	BIND_ENUM_CONSTANT(ACTION_RESULT_FAILED);
	BIND_ENUM_CONSTANT(ACTION_RESULT_INTERRUPTED);
}
