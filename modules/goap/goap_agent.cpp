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

#include "core/config/engine.h"
#include "core/object/class_db.h"

GoapAgent::GoapAgent() {
	context.instantiate();
	context->set_agent(this);
	planner.instantiate();
}

void GoapAgent::set_actions(const TypedArray<GoapAction> &p_actions) {
	actions = p_actions;
	abort_plan();
}

void GoapAgent::set_goals(const TypedArray<GoapGoal> &p_goals) {
	goals = p_goals;
	abort_plan();
}

void GoapAgent::set_planner(const Ref<GoapPlanner> &p_planner) {
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

bool GoapAgent::request_plan() {
	abort_plan();
	return _plan();
}

void GoapAgent::abort_plan() {
	if (current_plan.is_null()) {
		return;
	}
	if (action_running) {
		_finish_current_action(false);
	}
	current_plan.unref();
	emit_signal(SNAME("plan_aborted"));
}

bool GoapAgent::_plan() {
	time_since_planning = 0.0;
	has_planned = true;
	current_plan = planner->plan(context, actions, goals);
	action_running = false;

	if (current_plan.is_null()) {
		emit_signal(SNAME("planning_failed"));
		return false;
	}
	emit_signal(SNAME("plan_found"), current_plan);
	return true;
}

void GoapAgent::_finish_current_action(bool p_succeeded) {
	Ref<GoapAction> action = get_current_action();
	action_running = false;
	if (action.is_null()) {
		return;
	}
	action->exit(context, p_succeeded);
	emit_signal(SNAME("action_finished"), action, p_succeeded);
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
		// The delay only throttles repeated attempts; the first one is free.
		if (has_planned && replan_delay > 0.0 && time_since_planning < replan_delay) {
			return;
		}
		if (!_plan()) {
			return;
		}
	}

	Ref<GoapAction> action = current_plan->get_current_action();
	if (action.is_null()) {
		abort_plan();
		return;
	}

	if (!action_running) {
		// The world may have moved on since the plan was built, so the action
		// is re-checked right before it starts rather than trusted blindly.
		if (!action->is_valid(context) || !action->check_preconditions(context->get_world_state(), context)) {
			abort_plan();
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
	_finish_current_action(succeeded);

	if (!succeeded) {
		abort_plan();
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
		} break;

		case NOTIFICATION_EXIT_TREE: {
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
	ClassDB::bind_method(D_METHOD("set_active", "active"), &GoapAgent::set_active);
	ClassDB::bind_method(D_METHOD("is_active"), &GoapAgent::is_active);

	ClassDB::bind_method(D_METHOD("set_world_state", "world_state"), &GoapAgent::set_world_state);
	ClassDB::bind_method(D_METHOD("get_world_state"), &GoapAgent::get_world_state);
	ClassDB::bind_method(D_METHOD("set_state", "key", "value"), &GoapAgent::set_state);
	ClassDB::bind_method(D_METHOD("get_state", "key", "default"), &GoapAgent::get_state, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("has_state", "key"), &GoapAgent::has_state);
	ClassDB::bind_method(D_METHOD("erase_state", "key"), &GoapAgent::erase_state);

	ClassDB::bind_method(D_METHOD("request_plan"), &GoapAgent::request_plan);
	ClassDB::bind_method(D_METHOD("abort_plan"), &GoapAgent::abort_plan);
	ClassDB::bind_method(D_METHOD("get_plan"), &GoapAgent::get_plan);
	ClassDB::bind_method(D_METHOD("get_current_action"), &GoapAgent::get_current_action);
	ClassDB::bind_method(D_METHOD("get_current_goal"), &GoapAgent::get_current_goal);
	ClassDB::bind_method(D_METHOD("update", "delta"), &GoapAgent::update);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "active"), "set_active", "is_active");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "process_callback", PROPERTY_HINT_ENUM, "Idle,Physics,Manual"), "set_process_callback", "get_process_callback");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "replan_delay", PROPERTY_HINT_RANGE, "0,10,0.01,or_greater,suffix:s"), "set_replan_delay", "get_replan_delay");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "actions", PROPERTY_HINT_ARRAY_TYPE, MAKE_RESOURCE_TYPE_HINT("GoapAction")), "set_actions", "get_actions");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "goals", PROPERTY_HINT_ARRAY_TYPE, MAKE_RESOURCE_TYPE_HINT("GoapGoal")), "set_goals", "get_goals");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "world_state"), "set_world_state", "get_world_state");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "planner", PROPERTY_HINT_RESOURCE_TYPE, "GoapPlanner", PROPERTY_USAGE_NONE), "set_planner", "get_planner");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "context", PROPERTY_HINT_RESOURCE_TYPE, "GoapContext", PROPERTY_USAGE_NONE), "set_context", "get_context");

	ADD_SIGNAL(MethodInfo("plan_found", PropertyInfo(Variant::OBJECT, "plan", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT, "GoapPlan")));
	ADD_SIGNAL(MethodInfo("planning_failed"));
	ADD_SIGNAL(MethodInfo("plan_completed"));
	ADD_SIGNAL(MethodInfo("plan_aborted"));
	ADD_SIGNAL(MethodInfo("action_started", PropertyInfo(Variant::OBJECT, "action", PROPERTY_HINT_RESOURCE_TYPE, "GoapAction")));
	ADD_SIGNAL(MethodInfo("action_finished", PropertyInfo(Variant::OBJECT, "action", PROPERTY_HINT_RESOURCE_TYPE, "GoapAction"), PropertyInfo(Variant::BOOL, "succeeded")));

	BIND_ENUM_CONSTANT(PROCESS_CALLBACK_IDLE);
	BIND_ENUM_CONSTANT(PROCESS_CALLBACK_PHYSICS);
	BIND_ENUM_CONSTANT(PROCESS_CALLBACK_MANUAL);
}
