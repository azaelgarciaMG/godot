#pragma once

#include "../goap_action.h"
#include "../goap_context.h"
#include "../goap_goal.h"
#include "../goap_plan.h"
#include "../goap_planner.h"
#include "../goap_state.h"

#include "tests/test_macros.h"

namespace TestGoapPlanner {

static Ref<GoapAction> make_action(const StringName &p_name, const Dictionary &p_preconditions, const Dictionary &p_effects, double p_cost = 1.0) {
	Ref<GoapAction> action;
	action.instantiate();
	action->set_action_name(p_name);
	action->set_preconditions(p_preconditions);
	action->set_effects(p_effects);
	action->set_cost(p_cost);
	return action;
}

static Ref<GoapGoal> make_goal(const StringName &p_name, const Dictionary &p_desired_state, double p_priority = 1.0) {
	Ref<GoapGoal> goal;
	goal.instantiate();
	goal->set_goal_name(p_name);
	goal->set_desired_state(p_desired_state);
	goal->set_priority(p_priority);
	return goal;
}

static Ref<GoapContext> make_context(const Dictionary &p_world_state) {
	Ref<GoapContext> context;
	context.instantiate();
	context->set_world_state(p_world_state);
	return context;
}

static Vector<StringName> action_names(const Ref<GoapPlan> &p_plan) {
	Vector<StringName> names;
	for (int i = 0; i < p_plan->get_action_count(); i++) {
		names.push_back(p_plan->get_action(i)->get_action_name());
	}
	return names;
}

TEST_CASE("[GoapState] Fact matching") {
	GoapState state;
	state.from_dictionary({ { "has_axe", true }, { "wood", 3 } });

	GoapState conditions;
	conditions.from_dictionary({ { "has_axe", true } });
	CHECK(state.satisfies(conditions));

	conditions.from_dictionary({ { "has_axe", false } });
	CHECK_FALSE(state.satisfies(conditions));
	CHECK(state.conflicts_with(conditions));

	conditions.from_dictionary({ { "has_rope", true } });
	CHECK_FALSE(state.satisfies(conditions));
	CHECK_FALSE_MESSAGE(state.conflicts_with(conditions), "An absent key is unknown, not contradictory.");

	SUBCASE("Numeric values compare across int and float") {
		conditions.from_dictionary({ { "wood", 3.0 } });
		CHECK(state.satisfies(conditions));
	}

	SUBCASE("Equality ignores insertion order") {
		GoapState reordered;
		reordered.from_dictionary({ { "wood", 3 }, { "has_axe", true } });
		CHECK(state == reordered);
		CHECK(state.hash() == reordered.hash());
	}

	SUBCASE("Unsatisfied conditions are counted") {
		conditions.from_dictionary({ { "has_axe", true }, { "wood", 5 }, { "has_rope", true } });
		CHECK(state.count_unsatisfied(conditions) == 2);
	}
}

TEST_CASE("[GoapPlanner] Chains actions in execution order") {
	Ref<GoapPlanner> planner;
	planner.instantiate();

	TypedArray<GoapAction> actions;
	actions.push_back(make_action("get_axe", {}, { { "has_axe", true } }));
	actions.push_back(make_action("chop_wood", { { "has_axe", true } }, { { "has_wood", true } }));

	Ref<GoapContext> context = make_context({ { "has_axe", false }, { "has_wood", false } });
	Ref<GoapPlan> plan = planner->plan_for_goal(context, actions, make_goal("gather", { { "has_wood", true } }));

	REQUIRE(plan.is_valid());
	REQUIRE(plan->get_action_count() == 2);
	const Vector<StringName> names = action_names(plan);
	CHECK(names[0] == StringName("get_axe"));
	CHECK(names[1] == StringName("chop_wood"));
	CHECK(plan->get_total_cost() == doctest::Approx(2.0));
}

TEST_CASE("[GoapPlanner] Prefers the cheapest route") {
	Ref<GoapPlanner> planner;
	planner.instantiate();

	TypedArray<GoapAction> actions;
	actions.push_back(make_action("buy_axe", { { "has_gold", true } }, { { "has_axe", true } }, 1.0));
	actions.push_back(make_action("craft_axe", {}, { { "has_axe", true } }, 10.0));
	actions.push_back(make_action("earn_gold", {}, { { "has_gold", true } }, 1.0));

	Ref<GoapContext> context = make_context({ { "has_axe", false }, { "has_gold", false } });
	Ref<GoapPlan> plan = planner->plan_for_goal(context, actions, make_goal("armed", { { "has_axe", true } }));

	REQUIRE(plan.is_valid());
	const Vector<StringName> names = action_names(plan);
	REQUIRE(names.size() == 2);
	CHECK(names[0] == StringName("earn_gold"));
	CHECK(names[1] == StringName("buy_axe"));
	CHECK(plan->get_total_cost() == doctest::Approx(2.0));
}

TEST_CASE("[GoapPlanner] Rejects actions that undo an open condition") {
	Ref<GoapPlanner> planner;
	planner.instantiate();

	TypedArray<GoapAction> actions;
	// Reaching the goal through this action is impossible: it clears the very
	// fact the goal asks for.
	actions.push_back(make_action("sell_axe", {}, { { "has_axe", false }, { "has_gold", true } }));
	actions.push_back(make_action("buy_axe", { { "has_gold", true } }, { { "has_axe", true }, { "has_gold", false } }));

	Ref<GoapContext> context = make_context({ { "has_axe", false }, { "has_gold", false } });
	Ref<GoapPlan> plan = planner->plan_for_goal(context, actions, make_goal("armed", { { "has_axe", true } }));

	CHECK_MESSAGE(plan.is_null(), "\"sell_axe\" cannot supply the gold because it also clears \"has_axe\".");
}

TEST_CASE("[GoapPlanner] Unreachable goals produce no plan") {
	Ref<GoapPlanner> planner;
	planner.instantiate();

	TypedArray<GoapAction> actions;
	actions.push_back(make_action("chop_wood", { { "has_axe", true } }, { { "has_wood", true } }));

	Ref<GoapContext> context = make_context({ { "has_axe", false } });
	Ref<GoapPlan> plan = planner->plan_for_goal(context, actions, make_goal("gather", { { "has_wood", true } }));

	CHECK(plan.is_null());
}

TEST_CASE("[GoapPlanner] Satisfied goals produce an empty plan") {
	Ref<GoapPlanner> planner;
	planner.instantiate();

	TypedArray<GoapAction> actions;
	actions.push_back(make_action("chop_wood", {}, { { "has_wood", true } }));

	Ref<GoapContext> context = make_context({ { "has_wood", true } });
	Ref<GoapPlan> plan = planner->plan_for_goal(context, actions, make_goal("gather", { { "has_wood", true } }));

	REQUIRE(plan.is_valid());
	CHECK(plan->is_empty());
	CHECK(plan->is_finished());
}

TEST_CASE("[GoapPlanner] Plan length is capped") {
	Ref<GoapPlanner> planner;
	planner.instantiate();

	// Each step unlocks the next, so the only plan is three actions long.
	TypedArray<GoapAction> actions;
	actions.push_back(make_action("step_a", {}, { { "a", true } }));
	actions.push_back(make_action("step_b", { { "a", true } }, { { "b", true } }));
	actions.push_back(make_action("step_c", { { "b", true } }, { { "c", true } }));

	Ref<GoapContext> context = make_context({});
	Ref<GoapGoal> goal = make_goal("finish", { { "c", true } });

	planner->set_max_plan_length(2);
	CHECK(planner->plan_for_goal(context, actions, goal).is_null());

	planner->set_max_plan_length(3);
	Ref<GoapPlan> plan = planner->plan_for_goal(context, actions, goal);
	REQUIRE(plan.is_valid());
	CHECK(plan->get_action_count() == 3);
}

TEST_CASE("[GoapPlanner] Highest priority reachable goal wins") {
	Ref<GoapPlanner> planner;
	planner.instantiate();

	TypedArray<GoapAction> actions;
	actions.push_back(make_action("rest", {}, { { "rested", true } }));

	TypedArray<GoapGoal> goals;
	goals.push_back(make_goal("survive", { { "safe", true } }, 10.0)); // No action provides this.
	goals.push_back(make_goal("recover", { { "rested", true } }, 5.0));

	Ref<GoapContext> context = make_context({ { "rested", false }, { "safe", false } });
	Ref<GoapPlan> plan = planner->plan(context, actions, goals);

	REQUIRE(plan.is_valid());
	CHECK(plan->get_goal()->get_goal_name() == StringName("recover"));
	CHECK(plan->get_action_count() == 1);
}

TEST_CASE("[GoapPlanner] Already satisfied goals are skipped in favor of the next one") {
	Ref<GoapPlanner> planner;
	planner.instantiate();

	TypedArray<GoapAction> actions;
	actions.push_back(make_action("eat", {}, { { "fed", true } }));

	TypedArray<GoapGoal> goals;
	goals.push_back(make_goal("stay_safe", { { "safe", true } }, 10.0));
	goals.push_back(make_goal("eat_food", { { "fed", true } }, 1.0));

	Ref<GoapContext> context = make_context({ { "safe", true }, { "fed", false } });
	Ref<GoapPlan> plan = planner->plan(context, actions, goals);

	REQUIRE(plan.is_valid());
	CHECK(plan->get_goal()->get_goal_name() == StringName("eat_food"));
}

TEST_CASE("[GoapAction] Preconditions and effects against a world state") {
	Ref<GoapAction> action = make_action("chop_wood", { { "has_axe", true } }, { { "has_wood", true } });
	Ref<GoapContext> context = make_context({});

	Dictionary world_state = { { "has_axe", false } };
	CHECK_FALSE(action->check_preconditions(world_state, context));

	world_state["has_axe"] = true;
	CHECK(action->check_preconditions(world_state, context));

	const Dictionary applied = action->apply_effects(world_state, context);
	CHECK(applied["has_wood"] == Variant(true));
	CHECK_MESSAGE(!world_state.has("has_wood"), "The source state must not be modified in place.");
}

TEST_CASE("[GoapPlan] Cursor walks the actions once") {
	Ref<GoapPlan> plan;
	plan.instantiate();

	Vector<Ref<GoapAction>> actions;
	actions.push_back(make_action("first", {}, { { "a", true } }));
	actions.push_back(make_action("second", {}, { { "b", true } }));
	plan->set_actions(actions);

	CHECK(plan->get_current_action()->get_action_name() == StringName("first"));
	plan->advance();
	CHECK(plan->get_current_action()->get_action_name() == StringName("second"));
	plan->advance();
	CHECK(plan->is_finished());
	CHECK(plan->get_current_action().is_null());

	plan->advance(); // Must not run past the end.
	CHECK(plan->get_cursor() == 2);

	plan->reset();
	CHECK_FALSE(plan->is_finished());
	CHECK(plan->get_current_action()->get_action_name() == StringName("first"));
}

} // namespace TestGoapPlanner
