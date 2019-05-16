#include "execution/operator/aggregate/physical_hash_aggregate.hpp"
#include "execution/physical_plan_generator.hpp"
#include "planner/expression/bound_reference_expression.hpp"
#include "planner/operator/logical_distinct.hpp"

using namespace duckdb;
using namespace std;

unique_ptr<PhysicalOperator> PhysicalPlanGenerator::CreateDistinct(unique_ptr<PhysicalOperator> child) {
	assert(child);
	// create a PhysicalHashAggregate that groups by the input columns
	auto &types = child->GetTypes();
	vector<unique_ptr<Expression>> groups, expressions;
	for (index_t i = 0; i < types.size(); i++) {
		groups.push_back(make_unique<BoundReferenceExpression>(types[i], i));
	}

	auto groupby =
	    make_unique<PhysicalHashAggregate>(types, move(expressions), move(groups), PhysicalOperatorType::DISTINCT);
	groupby->children.push_back(move(child));
	return move(groupby);
}

unique_ptr<PhysicalOperator> PhysicalPlanGenerator::CreateDistinctOn(unique_ptr<PhysicalOperator> child,
                                                                     vector<unique_ptr<Expression>> distinct_targets) {
	assert(child);
	assert(distinct_targets.size() > 0);

	vector<unique_ptr<Expression>> groups, expressions;
	auto &types = child->GetTypes();
	vector<TypeId> target_types;

	// create a PhysicalHashAggregate that groups by the target column(s)
	for (index_t i = 0; i < distinct_targets.size(); i++) {
		target_types.push_back(types[i]);
		groups.push_back(make_unique<BoundReferenceExpression>(types[i], i));
	}

	auto groupby =
	    make_unique<PhysicalHashAggregate>(types, move(expressions), move(groups), PhysicalOperatorType::DISTINCT);
	groupby->children.push_back(move(child));
	return move(groupby);
}

unique_ptr<PhysicalOperator> PhysicalPlanGenerator::CreatePlan(LogicalDistinct &op) {
	assert(op.children.size() == 1);
	auto plan = CreatePlan(*op.children[0]);
	if (op.distinct_targets.size() > 0) {
		return CreateDistinctOn(move(plan), move(op.distinct_targets));
	} else {
		return CreateDistinct(move(plan));
	}
}
