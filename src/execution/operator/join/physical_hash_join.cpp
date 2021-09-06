#include "duckdb/execution/operator/join/physical_hash_join.hpp"
#include "duckdb/parallel/thread_context.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/query_profiler.hpp"

namespace duckdb {

PhysicalHashJoin::PhysicalHashJoin(LogicalOperator &op, unique_ptr<PhysicalOperator> left,
                                   unique_ptr<PhysicalOperator> right, vector<JoinCondition> cond, JoinType join_type,
                                   const vector<idx_t> &left_projection_map,
                                   const vector<idx_t> &right_projection_map_p, vector<LogicalType> delim_types,
                                   idx_t estimated_cardinality)
    : PhysicalComparisonJoin(op, PhysicalOperatorType::HASH_JOIN, move(cond), join_type, estimated_cardinality),
      right_projection_map(right_projection_map_p), delim_types(move(delim_types)) {
	children.push_back(move(left));
	children.push_back(move(right));

	D_ASSERT(left_projection_map.size() == 0);
	for (auto &condition : conditions) {
		condition_types.push_back(condition.left->return_type);
	}

	// for ANTI, SEMI and MARK join, we only need to store the keys, so for these the build types are empty
	if (join_type != JoinType::ANTI && join_type != JoinType::SEMI && join_type != JoinType::MARK) {
		build_types = LogicalOperator::MapTypes(children[1]->GetTypes(), right_projection_map);
	}
}

PhysicalHashJoin::PhysicalHashJoin(LogicalOperator &op, unique_ptr<PhysicalOperator> left,
                                   unique_ptr<PhysicalOperator> right, vector<JoinCondition> cond, JoinType join_type,
                                   idx_t estimated_cardinality)
    : PhysicalHashJoin(op, move(left), move(right), move(cond), join_type, {}, {}, {}, estimated_cardinality) {
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
class HashJoinLocalState : public LocalSinkState {
public:
	DataChunk build_chunk;
	DataChunk join_keys;
	ExpressionExecutor build_executor;
};

class HashJoinGlobalState : public GlobalSinkState {
public:
	HashJoinGlobalState() {
	}

	//! The HT used by the join
	unique_ptr<JoinHashTable> hash_table;
	//! Only used for FULL OUTER JOIN: scan state of the final scan to find unmatched tuples in the build-side
	JoinHTScanState ht_scan_state;
};

unique_ptr<GlobalSinkState> PhysicalHashJoin::GetGlobalSinkState(ClientContext &context) const {
	auto state = make_unique<HashJoinGlobalState>();
	state->hash_table =
	    make_unique<JoinHashTable>(BufferManager::GetBufferManager(context), conditions, build_types, join_type);
	if (!delim_types.empty() && join_type == JoinType::MARK) {
		// correlated MARK join
		if (delim_types.size() + 1 == conditions.size()) {
			// the correlated MARK join has one more condition than the amount of correlated columns
			// this is the case in a correlated ANY() expression
			// in this case we need to keep track of additional entries, namely:
			// - (1) the total amount of elements per group
			// - (2) the amount of non-null elements per group
			// we need these to correctly deal with the cases of either:
			// - (1) the group being empty [in which case the result is always false, even if the comparison is NULL]
			// - (2) the group containing a NULL value [in which case FALSE becomes NULL]
			auto &info = state->hash_table->correlated_mark_join_info;

			vector<LogicalType> payload_types;
			vector<BoundAggregateExpression *> correlated_aggregates;
			unique_ptr<BoundAggregateExpression> aggr;

			// jury-rigging the GroupedAggregateHashTable
			// we need a count_star and a count to get counts with and without NULLs
			aggr = AggregateFunction::BindAggregateFunction(context, CountStarFun::GetFunction(), {}, nullptr, false);
			correlated_aggregates.push_back(&*aggr);
			payload_types.push_back(aggr->return_type);
			info.correlated_aggregates.push_back(move(aggr));

			auto count_fun = CountFun::GetFunction();
			vector<unique_ptr<Expression>> children;
			// this is a dummy but we need it to make the hash table understand whats going on
			children.push_back(make_unique_base<Expression, BoundReferenceExpression>(count_fun.return_type, 0));
			aggr = AggregateFunction::BindAggregateFunction(context, count_fun, move(children), nullptr, false);
			correlated_aggregates.push_back(&*aggr);
			payload_types.push_back(aggr->return_type);
			info.correlated_aggregates.push_back(move(aggr));

			info.correlated_counts = make_unique<GroupedAggregateHashTable>(
			    BufferManager::GetBufferManager(context), delim_types, payload_types, correlated_aggregates);
			info.correlated_types = delim_types;
			info.group_chunk.Initialize(delim_types);
			info.result_chunk.Initialize(payload_types);
		}
	}
	return move(state);
}

unique_ptr<LocalSinkState> PhysicalHashJoin::GetLocalSinkState(ExecutionContext &context) const {
	auto state = make_unique<HashJoinLocalState>();
	if (!right_projection_map.empty()) {
		state->build_chunk.Initialize(build_types);
	}
	for (auto &cond : conditions) {
		state->build_executor.AddExpression(*cond.right);
	}
	state->join_keys.Initialize(condition_types);
	return move(state);
}

void PhysicalHashJoin::Sink(ExecutionContext &context, GlobalSinkState &state, LocalSinkState &lstate_p,
                            DataChunk &input) const {
	auto &sink = (HashJoinGlobalState &)state;
	auto &lstate = (HashJoinLocalState &)lstate_p;
	// resolve the join keys for the right chunk
	lstate.build_executor.Execute(input, lstate.join_keys);
	// build the HT
	if (!right_projection_map.empty()) {
		// there is a projection map: fill the build chunk with the projected columns
		lstate.build_chunk.Reset();
		lstate.build_chunk.SetCardinality(input);
		for (idx_t i = 0; i < right_projection_map.size(); i++) {
			lstate.build_chunk.data[i].Reference(input.data[right_projection_map[i]]);
		}
		sink.hash_table->Build(lstate.join_keys, lstate.build_chunk);
	} else if (!build_types.empty()) {
		// there is not a projected map: place the entire right chunk in the HT
		sink.hash_table->Build(lstate.join_keys, input);
	} else {
		// there are only keys: place an empty chunk in the payload
		lstate.build_chunk.SetCardinality(input.size());
		sink.hash_table->Build(lstate.join_keys, lstate.build_chunk);
	}
}

void PhysicalHashJoin::Combine(ExecutionContext &context, GlobalSinkState &gstate, LocalSinkState &lstate) const {
	auto &state = (HashJoinLocalState &)lstate;
	context.thread.profiler.Flush(this, &state.build_executor, "build_executor", 1);
	context.client.profiler->Flush(context.thread.profiler);
}

//===--------------------------------------------------------------------===//
// Finalize
//===--------------------------------------------------------------------===//
bool PhysicalHashJoin::Finalize(Pipeline &pipeline, ClientContext &context, unique_ptr<GlobalSinkState> state) {
	auto &sink = (HashJoinGlobalState &)*state;
	sink.hash_table->Finalize();

	PhysicalOperator::Finalize(pipeline, context, move(state));
	return true;
}

//===--------------------------------------------------------------------===//
// Operator
//===--------------------------------------------------------------------===//
class PhysicalHashJoinState : public OperatorState {
public:
	DataChunk join_keys;
	ExpressionExecutor probe_executor;
	unique_ptr<JoinHashTable::ScanStructure> scan_structure;

public:
	void Finalize(PhysicalOperator *op, ExecutionContext &context) override {
		context.thread.profiler.Flush(op, &probe_executor, "probe_executor", 0);
	}
};

unique_ptr<OperatorState> PhysicalHashJoin::GetOperatorState(ClientContext &context) const {
	auto state = make_unique<PhysicalHashJoinState>();
	state->join_keys.Initialize(condition_types);
	for (auto &cond : conditions) {
		state->probe_executor.AddExpression(*cond.left);
	}
	return move(state);
}

bool PhysicalHashJoin::Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk, OperatorState &state_p) const {
	auto &state = (PhysicalHashJoinState &) state_p;
	auto &sink = (HashJoinGlobalState &)*sink_state;

	if (sink.hash_table->Count() == 0 && EmptyResultIfRHSIsEmpty()) {
		return false;
	}

	if (state.scan_structure) {
		// still have elements remaining from the previous probe (i.e. we got
		// >1024 elements in the previous probe)
		state.scan_structure->Next(state.join_keys, input, chunk);
		if (chunk.size() > 0) {
			return true;
		}
		state.scan_structure = nullptr;
		return false;
	}

	// probe the HT
	if (sink.hash_table->Count() == 0) {
		ConstructEmptyJoinResult(sink.hash_table->join_type, sink.hash_table->has_null, input, chunk);
		return false;
	}
	// resolve the join keys for the left chunk
	state.probe_executor.Execute(input, state.join_keys);

	// perform the actual probe
	state.scan_structure = sink.hash_table->Probe(state.join_keys);
	state.scan_structure->Next(state.join_keys, input, chunk);
	return true;
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
void PhysicalHashJoin::GetData(ExecutionContext &context, DataChunk &chunk, GlobalSourceState &gstate, LocalSourceState &lstate) const {
	D_ASSERT(IsRightOuterJoin(join_type));
	// check if we need to scan any unmatched tuples from the RHS for the full/right outer join
	auto &sink = (HashJoinGlobalState &)*sink_state;
	sink.hash_table->ScanFullOuter(chunk, sink.ht_scan_state);
}

} // namespace duckdb
