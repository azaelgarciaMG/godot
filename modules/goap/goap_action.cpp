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

#include "core/object/class_db.h"

void GoapAction::set_preconditions(const TypedDictionary<StringName, bool> &p_preconditions) {
	preconditions = p_preconditions;
}

TypedDictionary<StringName, bool> GoapAction::get_preconditions() const {
	return preconditions;
}

void GoapAction::set_effects(const TypedDictionary<StringName, bool> &p_effects) {
	effects = p_effects;
}

TypedDictionary<StringName, bool> GoapAction::get_effects() const {
	return effects;
}

void GoapAction::set_cost(float p_cost) {
	cost = p_cost;
}

float GoapAction::get_cost() const {
	return cost;
}

void GoapAction::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_preconditions", "preconditions"), &GoapAction::set_preconditions);
	ClassDB::bind_method(D_METHOD("get_preconditions"), &GoapAction::get_preconditions);
	ClassDB::bind_method(D_METHOD("set_effects", "effects"), &GoapAction::set_effects);
	ClassDB::bind_method(D_METHOD("get_effects"), &GoapAction::get_effects);
	ClassDB::bind_method(D_METHOD("set_cost", "cost"), &GoapAction::set_cost);
	ClassDB::bind_method(D_METHOD("get_cost"), &GoapAction::get_cost);

	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "preconditions", PROPERTY_HINT_DICTIONARY_TYPE, "StringName;bool"), "set_preconditions", "get_preconditions");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "effects", PROPERTY_HINT_DICTIONARY_TYPE, "StringName;bool"), "set_effects", "get_effects");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cost", PROPERTY_HINT_RANGE, "0,100,0.01,or_greater"), "set_cost", "get_cost");
}
