/**************************************************************************/
/*  goap_state.cpp                                                        */
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

#include "goap_state.h"

#include "core/templates/hashfuncs.h"

bool GoapState::values_match(const Variant &p_a, const Variant &p_b) {
	if (p_a.get_type() == p_b.get_type()) {
		return p_a == p_b;
	}

	// Mismatched types: let the Variant operator decide, so that comparable
	// types such as `int` and `float` still compare equal.
	Variant result;
	bool valid = false;
	Variant::evaluate(Variant::OP_EQUAL, p_a, p_b, result, valid);
	return valid && result.operator bool();
}

bool GoapState::satisfies(const GoapState &p_conditions) const {
	for (const KeyValue<StringName, Variant> &E : p_conditions.facts) {
		const Variant *value = facts.getptr(E.key);
		if (value == nullptr || !values_match(*value, E.value)) {
			return false;
		}
	}
	return true;
}

int GoapState::count_unsatisfied(const GoapState &p_conditions) const {
	int unsatisfied = 0;
	for (const KeyValue<StringName, Variant> &E : p_conditions.facts) {
		const Variant *value = facts.getptr(E.key);
		if (value == nullptr || !values_match(*value, E.value)) {
			unsatisfied++;
		}
	}
	return unsatisfied;
}

bool GoapState::conflicts_with(const GoapState &p_other) const {
	// Iterate the smaller state, the intersection is the same either way.
	if (p_other.facts.size() < facts.size()) {
		return p_other.conflicts_with(*this);
	}
	for (const KeyValue<StringName, Variant> &E : facts) {
		const Variant *value = p_other.facts.getptr(E.key);
		if (value != nullptr && !values_match(*value, E.value)) {
			return true;
		}
	}
	return false;
}

void GoapState::from_dictionary(const Dictionary &p_dictionary) {
	facts.clear();
	facts.reserve(p_dictionary.size());
	for (const KeyValue<Variant, Variant> &E : p_dictionary) {
		facts[E.key] = E.value;
	}
}

Dictionary GoapState::to_dictionary() const {
	Dictionary dictionary;
	for (const KeyValue<StringName, Variant> &E : facts) {
		dictionary[E.key] = E.value;
	}
	return dictionary;
}

uint32_t GoapState::hash() const {
	// Facts are unordered, so the per-fact hashes are combined with an
	// operation that does not depend on iteration order.
	uint32_t h = 0;
	for (const KeyValue<StringName, Variant> &E : facts) {
		h += hash_murmur3_one_32(E.key.hash(), E.value.hash());
	}
	return hash_fmix32(h ^ (uint32_t)facts.size());
}

bool GoapState::operator==(const GoapState &p_other) const {
	if (facts.size() != p_other.facts.size()) {
		return false;
	}
	return satisfies(p_other);
}
