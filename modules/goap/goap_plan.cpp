/**************************************************************************/
/*  goap_plan.cpp                                                         */
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

#include "goap_plan.h"

#include "core/object/class_db.h"

void GoapPlan::set_actions(const Vector<Ref<GoapAction>> &p_actions) {
	actions = p_actions;
	cursor = 0;
}

TypedArray<GoapAction> GoapPlan::get_actions() const {
	TypedArray<GoapAction> result;
	result.resize(actions.size());
	for (int i = 0; i < actions.size(); i++) {
		result[i] = actions[i];
	}
	return result;
}

void GoapPlan::set_actions_typed(const TypedArray<GoapAction> &p_actions) {
	Vector<Ref<GoapAction>> new_actions;
	new_actions.resize(p_actions.size());
	for (int i = 0; i < p_actions.size(); i++) {
		new_actions.write[i] = p_actions[i];
	}
	set_actions(new_actions);
}

Ref<GoapAction> GoapPlan::get_action(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, actions.size(), Ref<GoapAction>());
	return actions[p_index];
}

Ref<GoapAction> GoapPlan::get_current_action() const {
	if (is_finished()) {
		return Ref<GoapAction>();
	}
	return actions[cursor];
}

void GoapPlan::advance() {
	if (!is_finished()) {
		cursor++;
	}
}

void GoapPlan::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_goal", "goal"), &GoapPlan::set_goal);
	ClassDB::bind_method(D_METHOD("get_goal"), &GoapPlan::get_goal);
	ClassDB::bind_method(D_METHOD("set_actions", "actions"), &GoapPlan::set_actions_typed);
	ClassDB::bind_method(D_METHOD("get_actions"), &GoapPlan::get_actions);
	ClassDB::bind_method(D_METHOD("set_total_cost", "cost"), &GoapPlan::set_total_cost);
	ClassDB::bind_method(D_METHOD("get_total_cost"), &GoapPlan::get_total_cost);

	ClassDB::bind_method(D_METHOD("get_action_count"), &GoapPlan::get_action_count);
	ClassDB::bind_method(D_METHOD("get_action", "index"), &GoapPlan::get_action);
	ClassDB::bind_method(D_METHOD("get_current_action"), &GoapPlan::get_current_action);
	ClassDB::bind_method(D_METHOD("get_cursor"), &GoapPlan::get_cursor);
	ClassDB::bind_method(D_METHOD("advance"), &GoapPlan::advance);
	ClassDB::bind_method(D_METHOD("reset"), &GoapPlan::reset);
	ClassDB::bind_method(D_METHOD("is_finished"), &GoapPlan::is_finished);
	ClassDB::bind_method(D_METHOD("is_empty"), &GoapPlan::is_empty);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "goal", PROPERTY_HINT_RESOURCE_TYPE, "GoapGoal"), "set_goal", "get_goal");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "actions", PROPERTY_HINT_ARRAY_TYPE, MAKE_RESOURCE_TYPE_HINT("GoapAction")), "set_actions", "get_actions");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "total_cost"), "set_total_cost", "get_total_cost");
}
