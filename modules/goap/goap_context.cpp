/**************************************************************************/
/*  goap_context.cpp                                                      */
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

#include "goap_context.h"

#include "core/object/class_db.h"
#include "scene/main/node.h"

void GoapContext::set_agent(Node *p_agent) {
	agent_id = p_agent != nullptr ? p_agent->get_instance_id() : ObjectID();
}

Node *GoapContext::get_agent() const {
	if (agent_id.is_null()) {
		return nullptr;
	}
	// Resolved through the instance ID so a freed agent reads back as `null`
	// instead of a dangling pointer.
	return ObjectDB::get_instance<Node>(agent_id);
}

void GoapContext::set_world_state(const Dictionary &p_world_state) {
	world_state = p_world_state;
}

void GoapContext::set_state(const StringName &p_key, const Variant &p_value) {
	world_state[p_key] = p_value;
}

Variant GoapContext::get_state(const StringName &p_key, const Variant &p_default) const {
	return world_state.get(p_key, p_default);
}

bool GoapContext::has_state(const StringName &p_key) const {
	return world_state.has(p_key);
}

void GoapContext::erase_state(const StringName &p_key) {
	world_state.erase(p_key);
}

void GoapContext::clear_state() {
	world_state.clear();
}

void GoapContext::set_blackboard(const Dictionary &p_blackboard) {
	blackboard = p_blackboard;
}

void GoapContext::set_var(const StringName &p_key, const Variant &p_value) {
	blackboard[p_key] = p_value;
}

Variant GoapContext::get_var(const StringName &p_key, const Variant &p_default) const {
	return blackboard.get(p_key, p_default);
}

bool GoapContext::has_var(const StringName &p_key) const {
	return blackboard.has(p_key);
}

void GoapContext::erase_var(const StringName &p_key) {
	blackboard.erase(p_key);
}

void GoapContext::clear_vars() {
	blackboard.clear();
}

void GoapContext::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_agent", "agent"), &GoapContext::set_agent);
	ClassDB::bind_method(D_METHOD("get_agent"), &GoapContext::get_agent);

	ClassDB::bind_method(D_METHOD("set_world_state", "world_state"), &GoapContext::set_world_state);
	ClassDB::bind_method(D_METHOD("get_world_state"), &GoapContext::get_world_state);
	ClassDB::bind_method(D_METHOD("set_state", "key", "value"), &GoapContext::set_state);
	ClassDB::bind_method(D_METHOD("get_state", "key", "default"), &GoapContext::get_state, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("has_state", "key"), &GoapContext::has_state);
	ClassDB::bind_method(D_METHOD("erase_state", "key"), &GoapContext::erase_state);
	ClassDB::bind_method(D_METHOD("clear_state"), &GoapContext::clear_state);

	ClassDB::bind_method(D_METHOD("set_blackboard", "blackboard"), &GoapContext::set_blackboard);
	ClassDB::bind_method(D_METHOD("get_blackboard"), &GoapContext::get_blackboard);
	ClassDB::bind_method(D_METHOD("set_var", "key", "value"), &GoapContext::set_var);
	ClassDB::bind_method(D_METHOD("get_var", "key", "default"), &GoapContext::get_var, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("has_var", "key"), &GoapContext::has_var);
	ClassDB::bind_method(D_METHOD("erase_var", "key"), &GoapContext::erase_var);
	ClassDB::bind_method(D_METHOD("clear_vars"), &GoapContext::clear_vars);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "agent", PROPERTY_HINT_NODE_TYPE, "Node", PROPERTY_USAGE_NONE), "set_agent", "get_agent");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "world_state"), "set_world_state", "get_world_state");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "blackboard"), "set_blackboard", "get_blackboard");
}
