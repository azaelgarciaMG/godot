/**************************************************************************/
/*  goap_context.h                                                        */
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

#include "core/object/ref_counted.h"
#include "core/variant/dictionary.h"

class Node;

// Everything an action or a goal is allowed to know about the character it is
// reasoning for. Passed to every virtual method of GoapAction and GoapGoal so
// that those resources stay stateless and can be shared between agents.
class GoapContext : public RefCounted {
	GDCLASS(GoapContext, RefCounted);

	ObjectID agent_id;
	Dictionary world_state;
	Dictionary blackboard;

protected:
	static void _bind_methods();

public:
	void set_agent(Node *p_agent);
	Node *get_agent() const;

	// Symbolic facts the planner reasons about. Keys should be StringNames and
	// values should be cheap, comparable Variants (bools, ints, strings).
	void set_world_state(const Dictionary &p_world_state);
	Dictionary get_world_state() const { return world_state; }
	void set_state(const StringName &p_key, const Variant &p_value);
	Variant get_state(const StringName &p_key, const Variant &p_default = Variant()) const;
	bool has_state(const StringName &p_key) const;
	void erase_state(const StringName &p_key);
	void clear_state();

	// Free-form runtime data (targets, timers, cached nodes) that the planner
	// never looks at.
	void set_blackboard(const Dictionary &p_blackboard);
	Dictionary get_blackboard() const { return blackboard; }
	void set_var(const StringName &p_key, const Variant &p_value);
	Variant get_var(const StringName &p_key, const Variant &p_default = Variant()) const;
	bool has_var(const StringName &p_key) const;
	void erase_var(const StringName &p_key);
	void clear_vars();
};
