/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/ai_column/ai_column_strict_read.h"
#include "sql/engine/ai_column/ai_column_internal_write_guard.h"
#include "sql/resolver/dml/ob_select_resolver.h"
#include "sql/resolver/dml/ob_stmt_expr_visitor.h"
#include "sql/resolver/ob_schema_checker.h"
#include "sql/resolver/ddl/ob_explain_stmt.h"
#include "sql/optimizer/ob_log_plan.h"
#include "sql/optimizer/ob_log_temp_table_transformation.h"
#include "sql/optimizer/ob_log_plan_factory.h"
#include "sql/ob_sql_context.h"

namespace oceanbase::sql {
using namespace common;
using namespace share::schema;
namespace {
struct StrictColumn { uint64_t table_; uint64_t column_; };
class StrictReferences {
public:
  explicit StrictReferences(ObResolverParams &params) : params_(params) {}
  int statement(ObDMLStmt &stmt, int64_t depth = 0, bool projection = true)
  {
    if (depth > 128 || ++nodes_ > 65536) { return OB_SIZE_OVERFLOW; }
    // DISTINCT (including set deduplication) consumes every projected field,
    // even if the enclosing query only requests a different output column.
    if (stmt.is_select_stmt()) {
      const auto &select=static_cast<const ObSelectStmt &>(stmt);
      projection |= select.has_distinct() || (select.is_set_stmt() && select.is_set_distinct());
    }
    int64_t found=-1;
    for (int64_t i=0; i<visited_.count(); ++i) { if (visited_.at(i).stmt_==&stmt) { found=i; break; } }
    if (found>=0 && (!projection || visited_.at(found).projection_)) { return OB_SUCCESS; }
    int ret=OB_SUCCESS; ObSEArray<ObRawExpr *,16> roots; ObStmtExprGetter visitor;
    if (found>=0) {
      visited_.at(found).projection_=true; visitor.remove_all(); visitor.add_scope(SCOPE_SELECT);
    } else {
      if (OB_FAIL(visited_.push_back({&stmt,projection}))) { return ret; }
      visitor.set_relation_scope();
      visitor.remove_scope(SCOPE_DML_COLUMN); visitor.remove_scope(SCOPE_DMLINFOS);
      visitor.remove_scope(SCOPE_DML_CONSTRAINT);
      if (!projection) { visitor.remove_scope(SCOPE_SELECT); }
    }
    if (OB_FAIL(stmt.get_relation_exprs(roots,visitor))) {}
    for (int64_t i=0; OB_SUCC(ret) && i<roots.count(); ++i) { ret=expression(stmt,roots.at(i),depth+1); }
    if (found<0) {
      ObSEArray<ObSelectStmt *,8> children;
      if (OB_SUCC(ret)) { ret=stmt.get_child_stmts(children); }
      for (int64_t i=0; OB_SUCC(ret) && i<children.count(); ++i) {
        bool derived=false;
        for (int64_t j=0; j<stmt.get_table_items().count(); ++j) {
          const auto *item=stmt.get_table_items().at(j);
          derived |= item!=nullptr && item->ref_query_==children.at(i);
        }
        ret=children.at(i)==nullptr ? OB_ERR_UNEXPECTED : statement(*children.at(i),depth+1,projection && !derived);
      }
    }
    return ret;
  }
  int expression(ObDMLStmt &stmt, ObRawExpr *expr, int64_t depth)
  {
    if (expr==nullptr) { return OB_ERR_UNEXPECTED; }
    if (depth>256 || ++nodes_>65536) { return OB_SIZE_OVERFLOW; }
    // Capture and status describe mutations; their generated inputs are not user AI reads.
    if (expr->get_expr_type()==T_FUN_SYS_AI_COLUMN_CAPTURE || expr->get_expr_type()==T_FUN_SYS_AI_COLUMN_STATUS) {
      return OB_SUCCESS;
    }
    int ret=OB_SUCCESS;
    if (expr->is_column_ref_expr()) {
      auto *ref=static_cast<ObColumnRefRawExpr *>(expr);
      const TableItem *item=stmt.get_table_item_by_id(ref->get_table_id());
      // Correlated references belong to an ancestor query block.
      for (int64_t i=0; item==nullptr && i<visited_.count(); ++i) {
        item=visited_.at(i).stmt_->get_table_item_by_id(ref->get_table_id());
      }
      if (item!=nullptr && item->ref_query_!=nullptr) {
        auto *child=item->ref_query_;
        const int64_t index=static_cast<int64_t>(ref->get_column_id())-OB_APP_MIN_COLUMN_ID;
        if (index<0 || index>=child->get_select_item_size()) { ret=OB_ERR_UNEXPECTED; }
        else if (OB_FAIL(statement(*child,depth+1,false))) {}
        else { ret=expression(*child,child->get_select_item(index).expr_,depth+1); }
      } else if (item!=nullptr && item->is_basic_table()) {
        const ObTableSchema *table=nullptr;
        if (OB_FAIL(params_.schema_checker_->get_table_schema(item->ref_id_,table))) {}
        else if (table==nullptr) { ret=OB_ERR_UNEXPECTED; }
        else {
          const auto *col=table->get_column_schema(ref->get_column_id());
          if (col!=nullptr && col->is_ai_column()) {
            bool exists=false;
            for (int64_t i=0; i<columns_.count(); ++i) {
              exists |= columns_.at(i).table_==item->ref_id_ && columns_.at(i).column_==ref->get_column_id();
            }
            if (!exists) {
              if (columns_.count()>=1024) { ret=OB_SIZE_OVERFLOW; }
              else { ret=columns_.push_back({item->ref_id_,ref->get_column_id()}); }
            }
          }
        }
      }
    }
    if (OB_SUCC(ret) && expr->is_exec_param_expr()) {
      ret=expression(stmt,static_cast<ObExecParamRawExpr *>(expr)->get_ref_expr(),depth+1);
    }
    if (OB_SUCC(ret) && expr->is_set_op_expr() && stmt.is_select_stmt()) {
      auto &select=static_cast<ObSelectStmt &>(stmt);
      const int64_t index=static_cast<ObSetOpRawExpr *>(expr)->get_idx();
      for (int64_t i=0; OB_SUCC(ret) && i<select.get_set_query().count(); ++i) {
        auto *child=select.get_set_query(i);
        if (child==nullptr || index<0 || index>=child->get_select_item_size()) { ret=OB_ERR_UNEXPECTED; }
        else if (OB_FAIL(statement(*child,depth+1,false))) {}
        else { ret=expression(*child,child->get_select_item(index).expr_,depth+1); }
      }
    }
    if (OB_SUCC(ret) && expr->is_query_ref_expr()) {
      auto *child=static_cast<ObQueryRefRawExpr *>(expr)->get_ref_stmt();
      ret=child==nullptr ? OB_ERR_UNEXPECTED : statement(*child,depth+1);
    }
    for (int64_t i=0; OB_SUCC(ret) && i<expr->get_param_count(); ++i) { ret=expression(stmt,expr->get_param_expr(i),depth+1); }
    return ret;
  }
  ObSEArray<StrictColumn,16> columns_;
private:
  struct Visited { ObDMLStmt *stmt_; bool projection_; };
  ObResolverParams &params_; int64_t nodes_=0; ObSEArray<Visited,16> visited_;
};
int identifier(ObSqlString &sql,const ObString &name)
{
  int ret=sql.append("`");
  for (int64_t i=0; OB_SUCC(ret) && i<name.length(); ++i) {
    if (name[i]=='\0') { ret=OB_INVALID_ARGUMENT; }
    else { ret=sql.append(name.ptr()+i,1); if (OB_SUCC(ret) && name[i]=='`') { ret=sql.append("`"); } }
  }
  if (OB_SUCC(ret)) { ret=sql.append("`"); } return ret;
}
}
int AiColumnStrictRead::prepare(ObDMLStmt &stmt,ObResolverParams &params)
{
  int ret=OB_SUCCESS; bool enabled=false;
  if (params.session_info_==nullptr || params.query_ctx_==nullptr || params.schema_checker_==nullptr) { return OB_INVALID_ARGUMENT; }
  if (params.session_info_->is_ai_column_internal_read()) { return OB_SUCCESS; }
  if (OB_FAIL(params.session_info_->get_sys_variable(share::SYS_VAR_AI_COLUMN_STRICT_READ,enabled))) { return ret; }
  if (!enabled) { return OB_SUCCESS; }
  ObDMLStmt *target=&stmt;
  if (stmt.is_explain_stmt()) { target=static_cast<ObExplainStmt &>(stmt).get_explain_query_stmt(); }
  if (target==nullptr) { return OB_ERR_UNEXPECTED; }
  StrictReferences refs(params);
  if (OB_FAIL(refs.statement(*target))) { return ret; }
  ObSEArray<uint64_t,8> done;
  for (int64_t i=0; OB_SUCC(ret) && i<refs.columns_.count(); ++i) {
    const uint64_t table_id=refs.columns_.at(i).table_; bool seen=false;
    for (int64_t j=0; j<done.count(); ++j) { seen |= done.at(j)==table_id; }
    if (seen) { continue; }
    if (done.count()>=64) { return OB_SIZE_OVERFLOW; }
    const ObTableSchema *table=nullptr; const ObDatabaseSchema *db=nullptr; const ObColumnSchemaV2 *state=nullptr;
    if (OB_FAIL(done.push_back(table_id))) {}
    else if (OB_FAIL(params.schema_checker_->get_table_schema(table_id,table))) {}
    else if (table==nullptr) { ret=OB_ERR_UNEXPECTED; }
    else if (OB_FAIL(params.schema_checker_->get_database_schema(table->get_database_id(),db))) {}
    else if (db==nullptr) { ret=OB_ERR_UNEXPECTED; }
    if (OB_FAIL(ret)) { break; }
    for (auto it=table->column_begin(); it!=table->column_end(); ++it) { if ((*it)->is_ai_column_outbox()) { state=*it; } }
    if (state==nullptr) { return OB_INVALID_DATA; }
    ObSqlString sql; bool first=true;
    if (OB_FAIL(sql.append("SELECT "))) { break; }
    for (int64_t j=0; OB_SUCC(ret) && j<refs.columns_.count(); ++j) {
      if (refs.columns_.at(j).table_!=table_id) { continue; }
      const auto *column=table->get_column_schema(refs.columns_.at(j).column_);
      if (column==nullptr) { ret=OB_SCHEMA_ERROR; }
      else if (!first && OB_FAIL(sql.append(","))) {}
      else if (OB_FAIL(sql.append("__ai_column_status("))) {}
      else if (OB_FAIL(identifier(sql,state->get_column_name_str()))) {}
      else { ret=sql.append_fmt(",%lu,%ld)",column->get_column_id(),column->get_schema_version()); first=false; }
    }
    if (OB_SUCC(ret) && OB_FAIL(sql.append(" FROM "))) {}
    else if (OB_SUCC(ret) && OB_FAIL(identifier(sql,db->get_database_name_str()))) {}
    else if (OB_SUCC(ret) && OB_FAIL(sql.append("."))) {}
    else if (OB_SUCC(ret) && OB_FAIL(identifier(sql,table->get_table_name_str()))) {}
    if (OB_FAIL(ret)) { break; }
    ParseResult parsed; ObParser parser(*params.allocator_,params.session_info_->get_sql_mode());
    AiColumnInternalWriteGuard permission(*params.session_info_,false);
    ObResolverParams local=params;
    if (OB_FAIL(parser.parse(sql.string(),parsed))) {}
    else if (parsed.result_tree_==nullptr || parsed.result_tree_->num_child_!=1 || parsed.result_tree_->children_[0]==nullptr) {
      ret=OB_ERR_UNEXPECTED;
    } else {
      ObSelectResolver resolver(local);
      if (OB_FAIL(resolver.resolve(*parsed.result_tree_->children_[0]))) {}
      else {
        ObSelectStmt *control=resolver.get_select_stmt();
        if (control==nullptr) { ret=OB_ERR_UNEXPECTED; }
        else {
          for (int64_t j=0; OB_SUCC(ret) && j<control->get_select_item_size(); ++j) {
            auto *expr=control->get_select_item(j).expr_;
            if (expr==nullptr || expr->get_expr_type()!=T_FUN_SYS_AI_COLUMN_STATUS) { ret=OB_ERR_UNEXPECTED; }
            else { expr->set_ai_column_strict_marker(); }
          }
          if (OB_SUCC(ret)) { ret=control->formalize_stmt(params.session_info_); }
          // The outer resolver has already initialized hints for the user tree.
          // Register only the newly resolved control blocks without renaming it.
          if (OB_SUCC(ret)) {
            ret=params.query_ctx_->get_query_hint_for_update().generate_orig_stmt_qb_name(
                *params.allocator_,control->get_stmt_id());
          }
          if (OB_SUCC(ret)) {
            ret=params.query_ctx_->get_query_hint_for_update().distribute_hint_to_orig_stmt(control);
          }
          if (OB_SUCC(ret)) { ret=params.query_ctx_->ai_column_read_controls_.push_back(control); }
        }
      }
    }
  }
  return ret;
}
int AiColumnStrictRead::wrap(ObLogPlan &plan)
{
  int ret=OB_SUCCESS; auto &ctx=plan.get_optimizer_context(); auto *query=ctx.get_query_ctx();
  const ObDMLStmt *root_stmt=ctx.get_root_stmt();
  if (root_stmt!=nullptr && root_stmt->is_explain_stmt()) {
    root_stmt=static_cast<const ObExplainStmt *>(root_stmt)->get_explain_query_stmt();
  }
  if (query==nullptr || root_stmt!=plan.get_stmt() || query->ai_column_read_controls_.empty()) { return OB_SUCCESS; }
  ObSEArray<ObLogicalOperator *,4> controls;
  for (int64_t i=0; OB_SUCC(ret) && i<query->ai_column_read_controls_.count(); ++i) {
    auto *stmt=query->ai_column_read_controls_.at(i); ObLogPlan *control=nullptr;
    if (stmt==nullptr) { ret=OB_ERR_UNEXPECTED; }
    else if ((control=ctx.get_log_plan_factory().create(ctx,*stmt))==nullptr) { ret=OB_ALLOCATE_MEMORY_FAILED; }
    else if (OB_FAIL(control->generate_raw_plan())) {}
    else {
      // A non-final SELECT retains candidate plans rather than assigning root_.
      ObLogicalOperator *best=nullptr;
      if (OB_FAIL(control->get_candidate_plans().get_best_plan(best))) {}
      else if (best==nullptr) { ret=OB_ERR_UNEXPECTED; }
      else {
        control->set_plan_root(best);
        best->mark_is_plan_root();
        ret=controls.push_back(best);
      }
    }
  }
  auto *root=plan.get_plan_root();
  if (OB_SUCC(ret)) {
    auto *gate=static_cast<ObLogTempTableTransformation *>(plan.get_log_op_factory().allocate(plan,log_op_def::LOG_TEMP_TABLE_TRANSFORMATION));
    if (gate==nullptr || root==nullptr) { ret=OB_ALLOCATE_MEMORY_FAILED; }
    else {
      gate->ai_column_strict_read_=true;
      if (OB_FAIL(gate->add_child(controls))) {}
      else if (OB_FAIL(gate->add_child(root))) {}
      else if (OB_FAIL(gate->compute_property())) {}
      else { root->set_is_plan_root(false); gate->mark_is_plan_root(); plan.set_plan_root(gate); }
    }
  }
  return ret;
}
}
