/**************************************************************************/
/*  goap_debugger_plugin.cpp                                              */
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

#include "goap_debugger_plugin.h"

#include "../goap_planner.h"

#include "core/object/callable_mp.h"
#include "editor/editor_string_names.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/split_container.h"
#include "scene/gui/tree.h"

/// GoapDebuggerPanel

GoapDebuggerPanel::GoapDebuggerPanel() {
	set_h_size_flags(SIZE_EXPAND_FILL);
	set_v_size_flags(SIZE_EXPAND_FILL);

	HBoxContainer *toolbar = memnew(HBoxContainer);
	toolbar->add_theme_constant_override(SNAME("separation"), 8 * EDSCALE);
	add_child(toolbar);

	capture_button = memnew(Button);
	capture_button->set_toggle_mode(true);
	capture_button->set_text(TTRC("Capture"));
	capture_button->set_disabled(true);
	capture_button->set_tooltip_text(TTRC("Stream the state of every GoapAgent in the running project."));
	capture_button->connect(SceneStringName(toggled), callable_mp(this, &GoapDebuggerPanel::_capture_toggled));
	toolbar->add_child(capture_button);

	filter = memnew(LineEdit);
	filter->set_placeholder(TTRC("Filter agents by path or goal"));
	filter->set_clear_button_enabled(true);
	filter->set_h_size_flags(SIZE_EXPAND_FILL);
	filter->connect(SceneStringName(text_changed), callable_mp(this, &GoapDebuggerPanel::_filter_changed));
	toolbar->add_child(filter);

	status = memnew(Label);
	toolbar->add_child(status);

	HSplitContainer *split = memnew(HSplitContainer);
	split->set_h_size_flags(SIZE_EXPAND_FILL);
	split->set_v_size_flags(SIZE_EXPAND_FILL);
	add_child(split);

	agent_tree = memnew(Tree);
	agent_tree->set_custom_minimum_size(Size2(320, 0) * EDSCALE);
	agent_tree->set_v_size_flags(SIZE_EXPAND_FILL);
	agent_tree->set_hide_root(true);
	agent_tree->set_columns(3);
	agent_tree->set_column_titles_visible(true);
	agent_tree->set_column_title(0, TTRC("Agent"));
	agent_tree->set_column_title(1, TTRC("Goal"));
	agent_tree->set_column_title(2, TTRC("State"));
	agent_tree->set_column_expand(1, true);
	agent_tree->set_column_expand(2, false);
	agent_tree->set_column_custom_minimum_width(2, 110 * EDSCALE);
	agent_tree->connect(SceneStringName(item_selected), callable_mp(this, &GoapDebuggerPanel::_agent_selected));
	split->add_child(agent_tree);

	VBoxContainer *details = memnew(VBoxContainer);
	details->set_h_size_flags(SIZE_EXPAND_FILL);
	details->set_v_size_flags(SIZE_EXPAND_FILL);
	split->add_child(details);

	Label *plan_label = memnew(Label);
	plan_label->set_text(TTRC("Plan"));
	details->add_child(plan_label);

	plan_tree = memnew(Tree);
	plan_tree->set_v_size_flags(SIZE_EXPAND_FILL);
	plan_tree->set_hide_root(true);
	plan_tree->set_columns(2);
	plan_tree->set_column_titles_visible(true);
	plan_tree->set_column_title(0, TTRC("Step"));
	plan_tree->set_column_title(1, TTRC("Action"));
	plan_tree->set_column_expand(0, false);
	plan_tree->set_column_custom_minimum_width(0, 60 * EDSCALE);
	details->add_child(plan_tree);

	Label *state_label = memnew(Label);
	state_label->set_text(TTRC("World State"));
	details->add_child(state_label);

	state_tree = memnew(Tree);
	state_tree->set_v_size_flags(SIZE_EXPAND_FILL);
	state_tree->set_hide_root(true);
	state_tree->set_columns(2);
	state_tree->set_column_titles_visible(true);
	state_tree->set_column_title(0, TTRC("Fact"));
	state_tree->set_column_title(1, TTRC("Value"));
	details->add_child(state_tree);
}

void GoapDebuggerPanel::_bind_methods() {
	ADD_SIGNAL(MethodInfo("capture_toggled", PropertyInfo(Variant::BOOL, "enabled")));
}

void GoapDebuggerPanel::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			capture_button->set_button_icon(get_theme_icon(
					capture_button->is_pressed() ? SNAME("Stop") : SNAME("Play"), EditorStringName(EditorIcons)));
		} break;
	}
}

String GoapDebuggerPanel::_result_name(int p_result) {
	switch (p_result) {
		case GoapPlanner::PLAN_RESULT_SUCCESS:
			return TTR("Executing");
		case GoapPlanner::PLAN_RESULT_IDLE:
			return TTR("Idle");
		case GoapPlanner::PLAN_RESULT_NO_GOALS:
			return TTR("No goals");
		case GoapPlanner::PLAN_RESULT_UNREACHABLE:
			return TTR("Unreachable");
		case GoapPlanner::PLAN_RESULT_BUDGET_EXHAUSTED:
			return TTR("Out of budget");
		case GoapPlanner::PLAN_RESULT_DEPTH_EXCEEDED:
			return TTR("Too deep");
		default:
			return TTR("Unknown");
	}
}

void GoapDebuggerPanel::_capture_toggled(bool p_pressed) {
	capturing = p_pressed;
	capture_button->set_button_icon(get_theme_icon(p_pressed ? SNAME("Stop") : SNAME("Play"), EditorStringName(EditorIcons)));
	emit_signal(SNAME("capture_toggled"), p_pressed);
}

void GoapDebuggerPanel::_filter_changed(const String &p_text) {
	_update_agent_tree();
}

void GoapDebuggerPanel::_agent_selected() {
	TreeItem *selected = agent_tree->get_selected();
	selected_path = selected == nullptr ? String() : selected->get_metadata(0).operator String();
	_update_details();
}

void GoapDebuggerPanel::set_agents(const Vector<GoapDebugger::AgentInfo> &p_agents) {
	agents = p_agents;
	_update_agent_tree();
	_update_details();
}

void GoapDebuggerPanel::started() {
	capture_button->set_disabled(false);
	agents.clear();
	_update_agent_tree();
	_update_details();
}

void GoapDebuggerPanel::stopped() {
	capture_button->set_disabled(true);
	// No signal: the session is already gone, there is nothing left to tell.
	capture_button->set_pressed_no_signal(false);
	capturing = false;
	agents.clear();
	_update_agent_tree();
	_update_details();
}

const GoapDebugger::AgentInfo *GoapDebuggerPanel::_find_selected() const {
	for (const GoapDebugger::AgentInfo &info : agents) {
		if (info.path == selected_path) {
			return &info;
		}
	}
	return nullptr;
}

void GoapDebuggerPanel::_update_agent_tree() {
	agent_tree->clear();
	TreeItem *root = agent_tree->create_item();

	const String needle = filter->get_text().strip_edges().to_lower();
	int shown = 0;
	for (const GoapDebugger::AgentInfo &info : agents) {
		if (!needle.is_empty() && !info.path.to_lower().contains(needle) && !info.goal.to_lower().contains(needle)) {
			continue;
		}
		shown++;

		TreeItem *item = agent_tree->create_item(root);
		// The full path is the identity; the leaf name is what fits on screen.
		item->set_text(0, info.path.get_file().is_empty() ? info.path : info.path.get_file());
		item->set_tooltip_text(0, info.path);
		item->set_metadata(0, info.path);
		item->set_text(1, info.goal.is_empty() ? String("-") : info.goal);

		String state;
		if (!info.active) {
			state = TTR("Inactive");
		} else if (info.planning) {
			state = TTR("Planning");
		} else {
			state = _result_name(info.last_result);
		}
		item->set_text(2, state);

		// Anything that is not "getting on with it" is worth the eye drawn to it.
		const bool healthy = info.active && (info.planning || info.last_result == GoapPlanner::PLAN_RESULT_SUCCESS || info.idle);
		if (!healthy) {
			item->set_custom_color(2, get_theme_color(SNAME("error_color"), EditorStringName(Editor)));
		} else if (info.idle) {
			item->set_custom_color(2, get_theme_color(SNAME("disabled_font_color"), EditorStringName(Editor)));
		}

		if (info.path == selected_path) {
			item->select(0);
		}
	}

	if (agents.is_empty()) {
		status->set_text(capturing ? TTR("No agents") : TTR("Not capturing"));
	} else if (shown == agents.size()) {
		status->set_text(vformat(TTR("%d agents"), agents.size()));
	} else {
		status->set_text(vformat(TTR("%d of %d agents"), shown, agents.size()));
	}
}

void GoapDebuggerPanel::_update_details() {
	plan_tree->clear();
	state_tree->clear();

	const GoapDebugger::AgentInfo *info = _find_selected();
	if (info == nullptr) {
		return;
	}

	TreeItem *plan_root = plan_tree->create_item();
	if (info->plan.is_empty()) {
		TreeItem *item = plan_tree->create_item(plan_root);
		item->set_text(1, info->planning ? TTR("Planning...") : TTR("No plan"));
		item->set_selectable(0, false);
		item->set_selectable(1, false);
	} else {
		for (int i = 0; i < info->plan.size(); i++) {
			TreeItem *item = plan_tree->create_item(plan_root);
			item->set_text(0, itos(i + 1));
			item->set_text(1, info->plan[i]);
			if (i < info->cursor) {
				// Already executed.
				item->set_custom_color(1, get_theme_color(SNAME("disabled_font_color"), EditorStringName(Editor)));
			} else if (i == info->cursor) {
				item->set_text(0, ">");
				item->set_custom_color(1, get_theme_color(SNAME("accent_color"), EditorStringName(Editor)));
			}
		}

		TreeItem *summary = plan_tree->create_item(plan_root);
		summary->set_text(1, vformat(TTR("Cost %s, %d nodes explored"), rtos(info->plan_cost), info->last_iterations));
		summary->set_custom_color(1, get_theme_color(SNAME("disabled_font_color"), EditorStringName(Editor)));
		summary->set_selectable(0, false);
		summary->set_selectable(1, false);
	}

	TreeItem *state_root = state_tree->create_item();
	// Dictionaries keep insertion order, which is not a useful order to read in.
	Vector<String> names;
	for (const Variant &key : info->world_state.get_key_list()) {
		names.push_back(key);
	}
	names.sort();

	for (const String &name : names) {
		TreeItem *item = state_tree->create_item(state_root);
		item->set_text(0, name);
		item->set_text(1, info->world_state[name].operator String());
	}
}

/// GoapEditorDebugger

void GoapEditorDebugger::_bind_methods() {
}

bool GoapEditorDebugger::has_capture(const String &p_capture) const {
	return p_capture == "goap";
}

bool GoapEditorDebugger::capture(const String &p_message, const Array &p_data, int p_session) {
	ERR_FAIL_COND_V(!panels.has(p_session), false);
	if (p_message != "goap:agents") {
		return false;
	}

	GoapDebugger::AgentFrame frame;
	if (!frame.deserialize(p_data)) {
		return false;
	}
	panels[p_session]->set_agents(frame.infos);
	return true;
}

void GoapEditorDebugger::_capture_toggled(bool p_enable, int p_session_id) {
	Ref<EditorDebuggerSession> session = get_session(p_session_id);
	ERR_FAIL_COND(session.is_null());
	session->toggle_profiler("goap", p_enable);
}

void GoapEditorDebugger::setup_session(int p_session_id) {
	Ref<EditorDebuggerSession> session = get_session(p_session_id);
	ERR_FAIL_COND(session.is_null());

	GoapDebuggerPanel *panel = memnew(GoapDebuggerPanel);
	panel->set_name(TTRC("GOAP"));
	panel->connect("capture_toggled", callable_mp(this, &GoapEditorDebugger::_capture_toggled).bind(p_session_id));
	session->connect("started", callable_mp(panel, &GoapDebuggerPanel::started));
	session->connect("stopped", callable_mp(panel, &GoapDebuggerPanel::stopped));
	session->add_session_tab(panel);
	panels[p_session_id] = panel;
}

/// GoapEditorPlugin

GoapEditorPlugin::GoapEditorPlugin() {
	debugger.instantiate();
}

void GoapEditorPlugin::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			add_debugger_plugin(debugger);
		} break;

		case NOTIFICATION_EXIT_TREE: {
			remove_debugger_plugin(debugger);
		} break;
	}
}
