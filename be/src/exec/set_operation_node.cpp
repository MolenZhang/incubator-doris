// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "exec/set_operation_node.h"

#include "exec/hash_table.hpp"
#include "exprs/expr.h"
#include "runtime/raw_value.h"
#include "runtime/row_batch.h"
#include "runtime/runtime_state.h"

namespace doris {
SetOperationNode::SetOperationNode(ObjectPool* pool, const TPlanNode& tnode,
                                   const DescriptorTbl& descs, int tuple_id)
        : ExecNode(pool, tnode, descs), _tuple_id(tuple_id), _tuple_desc(nullptr) {}

Status SetOperationNode::init(const TPlanNode& tnode, RuntimeState* state) {
    RETURN_IF_ERROR(ExecNode::init(tnode, state));
    DCHECK_EQ(_conjunct_ctxs.size(), 0);
    DCHECK_GE(_children.size(), 2);
    return Status::OK();
}

Status SetOperationNode::prepare(RuntimeState* state) {
    RETURN_IF_ERROR(ExecNode::prepare(state));
    _tuple_desc = state->desc_tbl().get_tuple_descriptor(_tuple_id);
    DCHECK(_tuple_desc != nullptr);
    _build_pool.reset(new MemPool(mem_tracker()));
    _build_timer = ADD_TIMER(runtime_profile(), "BuildTime");
    _probe_timer = ADD_TIMER(runtime_profile(), "ProbeTime");
    SCOPED_TIMER(_runtime_profile->total_time_counter());
    for (size_t i = 0; i < _child_expr_lists.size(); ++i) {
        RETURN_IF_ERROR(Expr::prepare(_child_expr_lists[i], state, child(i)->row_desc(),
                                      expr_mem_tracker()));
        DCHECK_EQ(_child_expr_lists[i].size(), _tuple_desc->slots().size());
    }
    _build_tuple_size = child(0)->row_desc().tuple_descriptors().size();
    _build_tuple_row_size = _build_tuple_size * sizeof(Tuple*);
    _build_tuple_idx.reserve(_build_tuple_size);

    for (int i = 0; i < _build_tuple_size; ++i) {
        TupleDescriptor* build_tuple_desc = child(0)->row_desc().tuple_descriptors()[i];
        _build_tuple_idx.push_back(_row_descriptor.get_tuple_idx(build_tuple_desc->id()));
    }
    _find_nulls = std::vector<bool>();
    for (auto ctx : _child_expr_lists[0]) {
        _find_nulls.push_back(!ctx->root()->is_slotref() || ctx->is_nullable());
    }
    return Status::OK();
}

Status SetOperationNode::close(RuntimeState* state) {
    if (is_closed()) {
        return Status::OK();
    }
    for (auto& exprs : _child_expr_lists) {
        Expr::close(exprs, state);
    }

    RETURN_IF_ERROR(exec_debug_action(TExecNodePhase::CLOSE));
    // Must reset _probe_batch in close() to release resources
    _probe_batch.reset(NULL);

    if (_memory_used_counter != NULL && _hash_tbl.get() != NULL) {
        COUNTER_UPDATE(_memory_used_counter, _build_pool->peak_allocated_bytes());
        COUNTER_UPDATE(_memory_used_counter, _hash_tbl->byte_size());
    }
    if (_hash_tbl.get() != NULL) {
        _hash_tbl->close();
    }
    if (_build_pool.get() != NULL) {
        _build_pool->free_all();
    }

    return ExecNode::close(state);
}

string SetOperationNode::get_row_output_string(TupleRow* row, const RowDescriptor& row_desc) {
    std::stringstream out;
    out << "[";
    for (int i = 0; i < row_desc.tuple_descriptors().size(); ++i) {
        if (i != 0) {
            out << " ";
        }
        out << Tuple::to_string(row->get_tuple(i), *row_desc.tuple_descriptors()[i]);
    }

    out << "]";
    return out.str();
}

void SetOperationNode::create_output_row(TupleRow* input_row, RowBatch* row_batch,
                                         uint8_t* tuple_buf) {
    TupleRow* output_row = row_batch->get_row(row_batch->add_row());
    Tuple* dst_tuple = reinterpret_cast<Tuple*>(tuple_buf);
    const std::vector<ExprContext*>& exprs = _child_expr_lists[0];
    dst_tuple->materialize_exprs<false>(input_row, *_tuple_desc, exprs,
                                        row_batch->tuple_data_pool(), nullptr, nullptr);
    output_row->set_tuple(0, dst_tuple);
    row_batch->commit_last_row();
    VLOG_ROW << "commit row: " << get_row_output_string(output_row, row_desc());
}

bool SetOperationNode::equals(TupleRow* row, TupleRow* other) {
    DCHECK(!(row == nullptr && other == nullptr));
    if (row == nullptr || other == nullptr) {
        return false;
    }
    for (int i = 0; i < _child_expr_lists[0].size(); ++i) {
        void* val_row = _child_expr_lists[0][i]->get_value(row);
        void* val_other = _child_expr_lists[0][i]->get_value(other);
        if (_find_nulls[i] && val_row == nullptr && val_other == nullptr) {
            continue;
        } else if (val_row == nullptr || val_other == nullptr) {
            return false;
        } else if (!RawValue::eq(val_row, val_other, _child_expr_lists[0][i]->root()->type())) {
            return false;
        }
    }
    return true;
}
} // namespace doris
