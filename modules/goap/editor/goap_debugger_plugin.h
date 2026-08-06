/**************************************************************************/
/*  goap_debugger_plugin.h                                                */
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

#include "../goap_debugger.h"

#include "editor/debugger/editor_debugger_plugin.h"
#include "editor/plugins/editor_plugin.h"
#include "scene/gui/box_container.h"

class Button;
class Label;
class LineEdit;
class Tree;
class TreeItem;

// The "GOAP" tab in the debugger. Lists every agent in the running game with
// the goal it settled on, the plan it is executing and the world state it is
// reasoning over — the view you would otherwise have to build as an in-game
// HUD to work out why an agent is doing what it is doing.
class GoapDebuggerPanel : public VBoxContainer {
	GDCLASS(GoapDebuggerPanel, VBoxContainer);

	Button *capture_button = nullptr;
	LineEdit *filter = nullptr;
	Label *status = nullptr;
	Tree *agent_tree = nullptr;
	Tree *plan_tree = nullptr;
	Tree *state_tree = nullptr;

	Vector<GoapDebugger::AgentInfo> agents;
	// Kept across refreshes so a selected agent stays selected while it updates.
	String selected_path;
	bool capturing = false;

	void _capture_toggled(bool p_pressed);
	void _filter_changed(const String &p_text);
	void _agent_selected();

	void _update_agent_tree();
	void _update_details();
	const GoapDebugger::AgentInfo *_find_selected() const;

	static String _result_name(int p_result);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	// Feeds a snapshot that arrived from the running game.
	void set_agents(const Vector<GoapDebugger::AgentInfo> &p_agents);

	void started();
	void stopped();

	GoapDebuggerPanel();
};

class GoapEditorDebugger : public EditorDebuggerPlugin {
	GDCLASS(GoapEditorDebugger, EditorDebuggerPlugin);

	HashMap<int, GoapDebuggerPanel *> panels;

	void _capture_toggled(bool p_enable, int p_session_id);

protected:
	static void _bind_methods();

public:
	virtual bool has_capture(const String &p_capture) const override;
	virtual bool capture(const String &p_message, const Array &p_data, int p_session) override;
	virtual void setup_session(int p_session_id) override;
};

class GoapEditorPlugin : public EditorPlugin {
	GDCLASS(GoapEditorPlugin, EditorPlugin);

	Ref<GoapEditorDebugger> debugger;

protected:
	void _notification(int p_what);

public:
	GoapEditorPlugin();
};
