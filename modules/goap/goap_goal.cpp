/**************************************************************************/
/*  goap_goal.cpp                                                         */
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

#include "goap_goal.h"

#include "goap_state.h"

#include "core/object/class_db.h" // IWYU pragma: keep. `GDVIRTUAL_BIND` macro.

void GoapGoal::set_goal_name(const StringName &p_name) {
	goal_name = p_name;
	emit_changed();
}

void GoapGoal::set_priority(double p_priority) {
	priority = p_priority;
	emit_changed();
}

void GoapGoal::set_desired_state(const Dictionary &p_desired_state) {
	desired_state = p_desired_state;
	emit_changed();
}

bool GoapGoal::is_valid(const Ref<GoapContext> &p_context) const {
	bool valid = true;
	GDVIRTUAL_CALL(_is_valid, p_context, valid);
	return valid;
}

double GoapGoal::get_effective_priority(const Ref<GoapContext> &p_context) const {
	double ret = priority;
	if (GDVIRTUAL_CALL(_get_priority, p_context, ret)) {
		return ret;
	}
	return priority;
}

Dictionary GoapGoal::get_effective_desired_state(const Ref<GoapContext> &p_context) const {
	Dictionary ret;
	if (GDVIRTUAL_CALL(_get_desired_state, p_context, ret)) {
		return ret;
	}
	return desired_state;
}

bool GoapGoal::has_dynamic_desired_state() const {
	return GDVIRTUAL_IS_OVERRIDDEN(_get_desired_state);
}

bool GoapGoal::is_satisfied(const Dictionary &p_world_state, const Ref<GoapContext> &p_context) const {
	GoapState state;
	state.from_dictionary(p_world_state);
	GoapConditions conditions;
	conditions.from_dictionary(get_effective_desired_state(p_context));
	return conditions.satisfied_by(state);
}

void GoapGoal::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_goal_name", "name"), &GoapGoal::set_goal_name);
	ClassDB::bind_method(D_METHOD("get_goal_name"), &GoapGoal::get_goal_name);
	ClassDB::bind_method(D_METHOD("set_priority", "priority"), &GoapGoal::set_priority);
	ClassDB::bind_method(D_METHOD("get_priority"), &GoapGoal::get_priority);
	ClassDB::bind_method(D_METHOD("set_desired_state", "desired_state"), &GoapGoal::set_desired_state);
	ClassDB::bind_method(D_METHOD("get_desired_state"), &GoapGoal::get_desired_state);

	ClassDB::bind_method(D_METHOD("is_valid", "context"), &GoapGoal::is_valid);
	ClassDB::bind_method(D_METHOD("get_effective_priority", "context"), &GoapGoal::get_effective_priority);
	ClassDB::bind_method(D_METHOD("get_effective_desired_state", "context"), &GoapGoal::get_effective_desired_state);
	ClassDB::bind_method(D_METHOD("is_satisfied", "world_state", "context"), &GoapGoal::is_satisfied, DEFVAL(Ref<GoapContext>()));

	GDVIRTUAL_BIND(_is_valid, "context");
	GDVIRTUAL_BIND(_get_priority, "context");
	GDVIRTUAL_BIND(_get_desired_state, "context");

	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "goal_name"), "set_goal_name", "get_goal_name");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "priority", PROPERTY_HINT_RANGE, "0,100,0.01,or_greater,or_less"), "set_priority", "get_priority");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "desired_state"), "set_desired_state", "get_desired_state");
}
