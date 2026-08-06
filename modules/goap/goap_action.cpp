/**************************************************************************/
/*  goap_action.cpp                                                       */
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

#include "goap_action.h"

#include "goap_state.h"

#include "core/object/class_db.h" // IWYU pragma: keep. `GDVIRTUAL_BIND` macro.

void GoapAction::set_action_name(const StringName &p_name) {
	action_name = p_name;
	emit_changed();
}

void GoapAction::set_cost(double p_cost) {
	// A negative step cost would let the A* search loop forever chasing a
	// cheaper plan, so clamp it here rather than in the planner.
	cost = MAX(0.0, p_cost);
	emit_changed();
}

void GoapAction::set_preconditions(const Dictionary &p_preconditions) {
	preconditions = p_preconditions;
	emit_changed();
}

void GoapAction::set_effects(const Dictionary &p_effects) {
	effects = p_effects;
	emit_changed();
}

void GoapAction::set_auto_apply_effects(bool p_enabled) {
	auto_apply_effects = p_enabled;
}

bool GoapAction::is_valid(const Ref<GoapContext> &p_context) const {
	bool valid = true;
	GDVIRTUAL_CALL(_is_valid, p_context, valid);
	return valid;
}

double GoapAction::get_effective_cost(const Ref<GoapContext> &p_context) const {
	double ret = cost;
	if (GDVIRTUAL_CALL(_get_cost, p_context, ret)) {
		return MAX(0.0, ret);
	}
	return cost;
}

Dictionary GoapAction::get_effective_preconditions(const Ref<GoapContext> &p_context) const {
	Dictionary ret;
	if (GDVIRTUAL_CALL(_get_preconditions, p_context, ret)) {
		return ret;
	}
	return preconditions;
}

Dictionary GoapAction::get_effective_effects(const Ref<GoapContext> &p_context) const {
	Dictionary ret;
	if (GDVIRTUAL_CALL(_get_effects, p_context, ret)) {
		return ret;
	}
	return effects;
}

bool GoapAction::check_preconditions(const Dictionary &p_world_state, const Ref<GoapContext> &p_context) const {
	GoapState state;
	state.from_dictionary(p_world_state);
	GoapState conditions;
	conditions.from_dictionary(get_effective_preconditions(p_context));
	return state.satisfies(conditions);
}

Dictionary GoapAction::apply_effects(const Dictionary &p_world_state, const Ref<GoapContext> &p_context) const {
	Dictionary result = p_world_state.duplicate();
	const Dictionary action_effects = get_effective_effects(p_context);
	for (const KeyValue<Variant, Variant> &E : action_effects) {
		result[E.key] = E.value;
	}
	return result;
}

void GoapAction::enter(const Ref<GoapContext> &p_context) {
	GDVIRTUAL_CALL(_enter, p_context);
}

GoapAction::Status GoapAction::tick(const Ref<GoapContext> &p_context, double p_delta) {
	int status = STATUS_SUCCEEDED;
	if (!GDVIRTUAL_CALL(_tick, p_context, p_delta, status)) {
		// An action without a `_tick` override is instantaneous; it exists only
		// to move the world state forward.
		return STATUS_SUCCEEDED;
	}
	ERR_FAIL_INDEX_V_MSG(status, STATUS_FAILED + 1, STATUS_FAILED, vformat("Invalid status %d returned by \"_tick\" of action \"%s\".", status, get_action_name()));
	return (Status)status;
}

void GoapAction::exit(const Ref<GoapContext> &p_context, bool p_succeeded) {
	GDVIRTUAL_CALL(_exit, p_context, p_succeeded);
}

void GoapAction::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_action_name", "name"), &GoapAction::set_action_name);
	ClassDB::bind_method(D_METHOD("get_action_name"), &GoapAction::get_action_name);
	ClassDB::bind_method(D_METHOD("set_cost", "cost"), &GoapAction::set_cost);
	ClassDB::bind_method(D_METHOD("get_cost"), &GoapAction::get_cost);
	ClassDB::bind_method(D_METHOD("set_preconditions", "preconditions"), &GoapAction::set_preconditions);
	ClassDB::bind_method(D_METHOD("get_preconditions"), &GoapAction::get_preconditions);
	ClassDB::bind_method(D_METHOD("set_effects", "effects"), &GoapAction::set_effects);
	ClassDB::bind_method(D_METHOD("get_effects"), &GoapAction::get_effects);
	ClassDB::bind_method(D_METHOD("set_auto_apply_effects", "enabled"), &GoapAction::set_auto_apply_effects);
	ClassDB::bind_method(D_METHOD("is_auto_apply_effects"), &GoapAction::is_auto_apply_effects);

	ClassDB::bind_method(D_METHOD("is_valid", "context"), &GoapAction::is_valid);
	ClassDB::bind_method(D_METHOD("get_effective_cost", "context"), &GoapAction::get_effective_cost);
	ClassDB::bind_method(D_METHOD("get_effective_preconditions", "context"), &GoapAction::get_effective_preconditions);
	ClassDB::bind_method(D_METHOD("get_effective_effects", "context"), &GoapAction::get_effective_effects);
	ClassDB::bind_method(D_METHOD("check_preconditions", "world_state", "context"), &GoapAction::check_preconditions, DEFVAL(Ref<GoapContext>()));
	ClassDB::bind_method(D_METHOD("apply_effects", "world_state", "context"), &GoapAction::apply_effects, DEFVAL(Ref<GoapContext>()));

	ClassDB::bind_method(D_METHOD("enter", "context"), &GoapAction::enter);
	ClassDB::bind_method(D_METHOD("tick", "context", "delta"), &GoapAction::tick);
	ClassDB::bind_method(D_METHOD("exit", "context", "succeeded"), &GoapAction::exit);

	GDVIRTUAL_BIND(_is_valid, "context");
	GDVIRTUAL_BIND(_get_cost, "context");
	GDVIRTUAL_BIND(_get_preconditions, "context");
	GDVIRTUAL_BIND(_get_effects, "context");
	GDVIRTUAL_BIND(_enter, "context");
	GDVIRTUAL_BIND(_tick, "context", "delta");
	GDVIRTUAL_BIND(_exit, "context", "succeeded");

	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "action_name"), "set_action_name", "get_action_name");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cost", PROPERTY_HINT_RANGE, "0,100,0.01,or_greater"), "set_cost", "get_cost");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "preconditions"), "set_preconditions", "get_preconditions");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "effects"), "set_effects", "get_effects");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "auto_apply_effects"), "set_auto_apply_effects", "is_auto_apply_effects");

	BIND_ENUM_CONSTANT(STATUS_RUNNING);
	BIND_ENUM_CONSTANT(STATUS_SUCCEEDED);
	BIND_ENUM_CONSTANT(STATUS_FAILED);
}
