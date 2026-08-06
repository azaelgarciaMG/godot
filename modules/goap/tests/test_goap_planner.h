#pragma once

#include "../goap_action.h"
#include "../goap_agent.h"
#include "../goap_context.h"
#include "../goap_goal.h"
#include "../goap_plan.h"
#include "../goap_planner.h"
#include "../goap_state.h"

#include "tests/signal_watcher.h"
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

	GoapConditions conditions;
	conditions.from_dictionary({ { "has_axe", true } });
	CHECK(conditions.satisfied_by(state));

	conditions.from_dictionary({ { "has_axe", false } });
	CHECK_FALSE(conditions.satisfied_by(state));

	conditions.from_dictionary({ { "has_rope", true } });
	CHECK_FALSE(conditions.satisfied_by(state));

	SUBCASE("Numeric values compare across int and float") {
		conditions.from_dictionary({ { "wood", 3.0 } });
		CHECK(conditions.satisfied_by(state));
	}

	SUBCASE("Equality ignores insertion order") {
		GoapConditions a;
		a.from_dictionary({ { "wood", 3 }, { "has_axe", true } });
		GoapConditions b;
		b.from_dictionary({ { "has_axe", true }, { "wood", 3 } });
		CHECK(a == b);
		CHECK(a.hash() == b.hash());
	}

	SUBCASE("Numerically equal values of different types hash alike") {
		GoapConditions a;
		a.from_dictionary({ { "wood", 3 } });
		GoapConditions b;
		b.from_dictionary({ { "wood", 3.0 } });
		CHECK(a == b);
		CHECK_MESSAGE(a.hash() == b.hash(), "A hash that disagrees with equality would break the planner's visited set.");
	}

	SUBCASE("Unsatisfied conditions are counted") {
		conditions.from_dictionary({ { "has_axe", true }, { "wood", 5 }, { "has_rope", true } });
		CHECK(conditions.count_unsatisfied(state) == 2);
	}
}

TEST_CASE("[GoapCondition] Comparison strings") {
	GoapState state;
	state.from_dictionary({ { "fuel", 3 }, { "label", "idle" } });

	GoapConditions conditions;

	SUBCASE("Inclusive and exclusive bounds") {
		conditions.from_dictionary({ { "fuel", ">=3" } });
		CHECK(conditions.satisfied_by(state));
		conditions.from_dictionary({ { "fuel", ">3" } });
		CHECK_FALSE(conditions.satisfied_by(state));
		conditions.from_dictionary({ { "fuel", "<=3" } });
		CHECK(conditions.satisfied_by(state));
		conditions.from_dictionary({ { "fuel", "<3" } });
		CHECK_FALSE(conditions.satisfied_by(state));
		conditions.from_dictionary({ { "fuel", "==3" } });
		CHECK(conditions.satisfied_by(state));
	}

	SUBCASE("A missing counter reads as zero") {
		conditions.from_dictionary({ { "ammo", ">=0" } });
		CHECK_MESSAGE(conditions.satisfied_by(state), "Counters should not have to be seeded in the world state.");
		conditions.from_dictionary({ { "ammo", ">=1" } });
		CHECK_FALSE(conditions.satisfied_by(state));
	}

	SUBCASE("Plain strings stay literal") {
		conditions.from_dictionary({ { "label", "idle" } });
		CHECK(conditions.satisfied_by(state));
		// Not a comparison: no number follows the operator.
		conditions.from_dictionary({ { "label", ">busy" } });
		CHECK_FALSE(conditions.satisfied_by(state));
		CHECK(GoapCondition::is_malformed(">busy"));
		CHECK_FALSE(GoapCondition::is_malformed("idle"));
		CHECK_FALSE(GoapCondition::is_malformed(">=3"));
	}

	SUBCASE("A non-numeric fact never sits inside a range") {
		conditions.from_dictionary({ { "label", ">=1" } });
		CHECK_FALSE(conditions.satisfied_by(state));
	}
}

TEST_CASE("[GoapEffect] Delta effects") {
	Ref<GoapAction> action = make_action("chop", {}, { { "wood", "+=2" }, { "tired", true } });
	Ref<GoapContext> context = make_context({});

	SUBCASE("Deltas accumulate onto the world state") {
		Dictionary world_state = { { "wood", 1 } };
		Dictionary applied = action->apply_effects(world_state, context);
		CHECK(applied["wood"] == Variant(3));
		CHECK(applied["tired"] == Variant(true));
		CHECK_MESSAGE(world_state["wood"] == Variant(1), "The source state must not be modified in place.");

		applied = action->apply_effects(applied, context);
		CHECK(applied["wood"] == Variant(5));
	}

	SUBCASE("An absent counter starts from zero") {
		const Dictionary applied = action->apply_effects({}, context);
		CHECK(applied["wood"] == Variant(2));
	}

	SUBCASE("Subtraction and malformed operators") {
		Ref<GoapAction> spend = make_action("spend", {}, { { "gold", "-=2" } });
		const Dictionary applied = spend->apply_effects({ { "gold", 5 } }, context);
		CHECK(applied["gold"] == Variant(3));

		CHECK(GoapEffect::is_malformed("+=lots"));
		CHECK_FALSE(GoapEffect::is_malformed("+=1"));
		CHECK_FALSE(GoapEffect::is_malformed("idle"));
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

TEST_CASE("[GoapPlanner] Cheap actions are not passed over for one expensive shortcut") {
	// Regression test for an inadmissible heuristic. Counting one unit per unmet
	// fact overestimates the remaining cost as soon as an action is cheaper than
	// 1.0, which used to make the search settle for the first plan it popped.
	Ref<GoapPlanner> planner;
	planner.instantiate();

	TypedArray<GoapAction> actions;
	actions.push_back(make_action("everything", {}, { { "a", true }, { "b", true }, { "c", true } }, 2.0));
	actions.push_back(make_action("cheap_a", {}, { { "a", true } }, 0.1));
	actions.push_back(make_action("cheap_b", {}, { { "b", true } }, 0.1));
	actions.push_back(make_action("cheap_c", {}, { { "c", true } }, 0.1));

	Ref<GoapContext> context = make_context({ { "a", false }, { "b", false }, { "c", false } });
	Ref<GoapGoal> goal = make_goal("all", { { "a", true }, { "b", true }, { "c", true } });

	Ref<GoapPlan> plan = planner->plan_for_goal(context, actions, goal);
	REQUIRE(plan.is_valid());
	CHECK(plan->get_action_count() == 3);
	CHECK(plan->get_total_cost() == doctest::Approx(0.3));
	CHECK(planner->get_last_result() == GoapPlanner::PLAN_RESULT_SUCCESS);

	SUBCASE("The greedy heuristic still returns a usable plan") {
		planner->set_heuristic_mode(GoapPlanner::HEURISTIC_GREEDY);
		Ref<GoapPlan> greedy = planner->plan_for_goal(context, actions, goal);
		REQUIRE(greedy.is_valid());
		CHECK(greedy->get_action_count() >= 1);
	}
}

TEST_CASE("[GoapPlanner] Reports why planning failed") {
	Ref<GoapPlanner> planner;
	planner.instantiate();

	TypedArray<GoapAction> actions;
	actions.push_back(make_action("step_a", {}, { { "a", true } }));
	actions.push_back(make_action("step_b", { { "a", true } }, { { "b", true } }));
	actions.push_back(make_action("step_c", { { "b", true } }, { { "c", true } }));

	Ref<GoapContext> context = make_context({});
	Ref<GoapGoal> goal = make_goal("finish", { { "c", true } });

	SUBCASE("Depth cutoff") {
		planner->set_max_plan_length(2);
		CHECK(planner->plan_for_goal(context, actions, goal).is_null());
		CHECK(planner->get_last_result() == GoapPlanner::PLAN_RESULT_DEPTH_EXCEEDED);
	}

	SUBCASE("Iteration budget") {
		planner->set_max_iterations(1);
		CHECK(planner->plan_for_goal(context, actions, goal).is_null());
		CHECK(planner->get_last_result() == GoapPlanner::PLAN_RESULT_BUDGET_EXHAUSTED);
	}

	SUBCASE("Genuinely unreachable") {
		Ref<GoapGoal> impossible = make_goal("fly", { { "airborne", true } });
		CHECK(planner->plan_for_goal(context, actions, impossible).is_null());
		CHECK(planner->get_last_result() == GoapPlanner::PLAN_RESULT_UNREACHABLE);
	}

	SUBCASE("Nothing to do is not a failure") {
		TypedArray<GoapGoal> goals;
		goals.push_back(make_goal("rest", { { "rested", true } }));
		Ref<GoapContext> rested = make_context({ { "rested", true } });
		CHECK(planner->plan(rested, actions, goals).is_null());
		CHECK(planner->get_last_result() == GoapPlanner::PLAN_RESULT_IDLE);
	}

	SUBCASE("No valid goals at all") {
		CHECK(planner->plan(context, actions, TypedArray<GoapGoal>()).is_null());
		CHECK(planner->get_last_result() == GoapPlanner::PLAN_RESULT_NO_GOALS);
	}
}

TEST_CASE("[GoapPlanner] Numeric goals are reached by repeating a delta action") {
	Ref<GoapPlanner> planner;
	planner.instantiate();

	TypedArray<GoapAction> actions;
	actions.push_back(make_action("chop", {}, { { "wood", "+=1" } }));

	Ref<GoapContext> context = make_context({ { "wood", 0 } });
	Ref<GoapPlan> plan = planner->plan_for_goal(context, actions, make_goal("stock", { { "wood", ">=3" } }));

	REQUIRE(plan.is_valid());
	CHECK_MESSAGE(plan->get_action_count() == 3, "Three chops are needed to go from 0 to 3.");
	CHECK(plan->get_total_cost() == doctest::Approx(3.0));

	SUBCASE("A larger delta needs fewer steps") {
		TypedArray<GoapAction> bigger;
		bigger.push_back(make_action("fell_tree", {}, { { "wood", "+=2" } }));
		Ref<GoapPlan> fewer = planner->plan_for_goal(context, bigger, make_goal("stock", { { "wood", ">=3" } }));
		REQUIRE(fewer.is_valid());
		CHECK(fewer->get_action_count() == 2);
	}

	SUBCASE("An already stocked agent needs no plan") {
		Ref<GoapContext> stocked = make_context({ { "wood", 5 } });
		Ref<GoapPlan> none = planner->plan_for_goal(stocked, actions, make_goal("stock", { { "wood", ">=3" } }));
		REQUIRE(none.is_valid());
		CHECK(none->is_empty());
	}
}

TEST_CASE("[GoapPlanner] Regression tracks the resources an action consumes") {
	Ref<GoapPlanner> planner;
	planner.instantiate();

	// Each trade spends a wood to gain a gold. Planning backwards, the wood the
	// later trades need has to accumulate into the earlier ones' requirements.
	TypedArray<GoapAction> actions;
	actions.push_back(make_action("trade", { { "wood", ">=1" } }, { { "wood", "-=1" }, { "gold", "+=1" } }));

	Ref<GoapGoal> goal = make_goal("rich", { { "gold", ">=2" } });

	SUBCASE("Enough wood in stock") {
		Ref<GoapContext> context = make_context({ { "wood", 5 }, { "gold", 0 } });
		Ref<GoapPlan> plan = planner->plan_for_goal(context, actions, goal);
		REQUIRE(plan.is_valid());
		CHECK(plan->get_action_count() == 2);
	}

	SUBCASE("Not enough wood for both trades") {
		Ref<GoapContext> context = make_context({ { "wood", 1 }, { "gold", 0 } });
		Ref<GoapPlan> plan = planner->plan_for_goal(context, actions, goal);
		CHECK_MESSAGE(plan.is_null(), "Two trades cost two wood, and only one is in stock.");
	}

	SUBCASE("A refill action makes it reachable again") {
		TypedArray<GoapAction> with_refill = actions.duplicate();
		with_refill.push_back(make_action("chop", {}, { { "wood", "+=1" } }));
		Ref<GoapContext> context = make_context({ { "wood", 1 }, { "gold", 0 } });
		Ref<GoapPlan> plan = planner->plan_for_goal(context, with_refill, goal);
		REQUIRE(plan.is_valid());
		CHECK_MESSAGE(plan->get_action_count() == 3, "One chop to cover the shortfall, then two trades.");
	}
}

TEST_CASE("[GoapPlanner] A time-sliced search matches the blocking one") {
	Ref<GoapPlanner> planner;
	planner.instantiate();

	TypedArray<GoapAction> actions;
	actions.push_back(make_action("step_a", {}, { { "a", true } }));
	actions.push_back(make_action("step_b", { { "a", true } }, { { "b", true } }));
	actions.push_back(make_action("step_c", { { "b", true } }, { { "c", true } }));

	TypedArray<GoapGoal> goals;
	goals.push_back(make_goal("finish", { { "c", true } }));
	Ref<GoapContext> context = make_context({});

	planner->plan_begin(context, actions, goals);
	CHECK(planner->is_planning());

	// One node per slice, with a guard so a broken search fails the test rather
	// than hanging it.
	int slices = 0;
	while (!planner->plan_step(1)) {
		slices++;
		REQUIRE(slices < 1000);
	}
	CHECK_MESSAGE(slices > 0, "A three step chain should not resolve in a single node.");

	Ref<GoapPlan> plan = planner->take_plan();
	REQUIRE(plan.is_valid());
	CHECK(planner->is_planning() == false);
	const Vector<StringName> names = action_names(plan);
	REQUIRE(names.size() == 3);
	CHECK(names[0] == StringName("step_a"));
	CHECK(names[2] == StringName("step_c"));

	SUBCASE("Cancelling drops the search") {
		planner->plan_begin(context, actions, goals);
		planner->plan_cancel();
		CHECK_FALSE(planner->is_planning());
		CHECK(planner->take_plan().is_null());
	}
}

TEST_CASE("[GoapPlan] Prints its goal, cost and cursor") {
	Ref<GoapPlan> plan;
	plan.instantiate();
	plan->set_goal(make_goal("gather", { { "has_wood", true } }));

	Vector<Ref<GoapAction>> actions;
	actions.push_back(make_action("get_axe", {}, { { "has_axe", true } }));
	actions.push_back(make_action("chop_wood", {}, { { "has_wood", true } }));
	plan->set_actions(actions);
	plan->set_total_cost(2.0);

	const String text = plan->to_string();
	CHECK(text.contains("gather"));
	CHECK(text.contains("get_axe"));
	CHECK(text.contains("chop_wood"));
	CHECK_MESSAGE(text.contains("*get_axe"), "The action the cursor is on should be marked.");

	plan->advance();
	CHECK(plan->to_string().contains("*chop_wood"));
}

TEST_CASE("[GoapAgent] Validation catches unreachable facts") {
	GoapAgent *agent = memnew(GoapAgent);

	TypedArray<GoapAction> actions;
	actions.push_back(make_action("chop_wood", {}, { { "has_wood", true } }));
	agent->set_actions(actions);

	SUBCASE("A typo in the goal is reported") {
		TypedArray<GoapGoal> goals;
		goals.push_back(make_goal("gather", { { "have_wood", true } })); // "has_wood".
		agent->set_goals(goals);

		const PackedStringArray warnings = agent->validate();
		bool found = false;
		for (const String &warning : warnings) {
			found = found || warning.contains("have_wood");
		}
		CHECK_MESSAGE(found, "A goal fact no action produces should be reported.");
	}

	SUBCASE("A spelled correctly goal is clean") {
		TypedArray<GoapGoal> goals;
		goals.push_back(make_goal("gather", { { "has_wood", true } }));
		agent->set_goals(goals);
		CHECK(agent->validate().is_empty());
	}

	SUBCASE("A fact the world state defines is accepted") {
		TypedArray<GoapGoal> goals;
		goals.push_back(make_goal("stay", { { "safe", true } }));
		agent->set_goals(goals);
		CHECK_FALSE(agent->validate().is_empty());

		agent->set_world_state({ { "safe", false } });
		CHECK(agent->validate().is_empty());
	}

	SUBCASE("Malformed comparison strings are reported") {
		TypedArray<GoapGoal> goals;
		goals.push_back(make_goal("stock", { { "has_wood", ">=lots" } }));
		agent->set_goals(goals);

		const PackedStringArray warnings = agent->validate();
		bool found = false;
		for (const String &warning : warnings) {
			found = found || warning.contains("has_wood");
		}
		CHECK(found);
	}

	memdelete(agent);
}

TEST_CASE("[GoapAgent] Idle is not the same as unreachable") {
	GoapAgent *agent = memnew(GoapAgent);
	agent->set_process_callback(GoapAgent::PROCESS_CALLBACK_MANUAL);

	TypedArray<GoapAction> actions;
	actions.push_back(make_action("eat", {}, { { "fed", true } }));
	agent->set_actions(actions);

	SUBCASE("Everything already done") {
		TypedArray<GoapGoal> goals;
		goals.push_back(make_goal("eat_food", { { "fed", true } }));
		agent->set_goals(goals);
		agent->set_world_state({ { "fed", true } });

		CHECK_FALSE(agent->request_plan());
		CHECK(agent->get_last_plan_result() == GoapPlanner::PLAN_RESULT_IDLE);
		CHECK(agent->is_idle());
	}

	SUBCASE("Goal out of reach") {
		TypedArray<GoapGoal> goals;
		goals.push_back(make_goal("fly", { { "airborne", true } }));
		agent->set_goals(goals);

		CHECK_FALSE(agent->request_plan());
		CHECK(agent->get_last_plan_result() == GoapPlanner::PLAN_RESULT_UNREACHABLE);
		CHECK_FALSE_MESSAGE(agent->is_idle(), "An unreachable goal means the agent is stuck, not idle.");
	}

	SUBCASE("A plan is found") {
		TypedArray<GoapGoal> goals;
		goals.push_back(make_goal("eat_food", { { "fed", true } }));
		agent->set_goals(goals);
		agent->set_world_state({ { "fed", false } });

		CHECK(agent->request_plan());
		CHECK(agent->get_last_plan_result() == GoapPlanner::PLAN_RESULT_SUCCESS);
		CHECK_FALSE(agent->is_idle());
		REQUIRE(agent->get_plan().is_valid());
		CHECK(agent->get_current_action()->get_action_name() == StringName("eat"));
	}

	memdelete(agent);
}

TEST_CASE("[GoapAgent] Signals carry why the plan stopped") {
	GoapAgent *agent = memnew(GoapAgent);
	agent->set_process_callback(GoapAgent::PROCESS_CALLBACK_MANUAL);

	TypedArray<GoapAction> actions;
	actions.push_back(make_action("eat", {}, { { "fed", true } }));
	agent->set_actions(actions);

	TypedArray<GoapGoal> goals;
	goals.push_back(make_goal("eat_food", { { "fed", true } }));
	agent->set_goals(goals);
	agent->set_world_state({ { "fed", false } });

	SIGNAL_WATCH(agent, "plan_aborted");
	SIGNAL_WATCH(agent, "planning_failed");

	REQUIRE(agent->request_plan());
	agent->abort_plan(GoapAgent::ABORT_REASON_PRECONDITIONS_STALE);
	SIGNAL_CHECK("plan_aborted", { { (int)GoapAgent::ABORT_REASON_PRECONDITIONS_STALE } });

	agent->set_world_state({ { "fed", true } });
	CHECK_FALSE(agent->request_plan());
	SIGNAL_CHECK("planning_failed", { { (int)GoapPlanner::PLAN_RESULT_IDLE } });

	SIGNAL_UNWATCH(agent, "plan_aborted");
	SIGNAL_UNWATCH(agent, "planning_failed");
	memdelete(agent);
}

} // namespace TestGoapPlanner
