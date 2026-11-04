/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "sql/session/ob_sql_session_info.h"

namespace oceanbase::sql {
// Never exposed as a SQL variable. Scope this guard to a worker's native DML.
class AiColumnInternalWriteGuard
{
public:
  explicit AiColumnInternalWriteGuard(ObSQLSessionInfo &session, bool enabled = true)
      : session_(session), saved_(session.ai_column_internal_write_), saved_read_(session.ai_column_internal_read_),
        saved_plan_cache_(session.get_local_ob_enable_plan_cache())
  {
    session_.ai_column_internal_write_ = enabled; session_.ai_column_internal_read_ = true;
    session_.set_local_ob_enable_plan_cache(false);
  }
  ~AiColumnInternalWriteGuard()
  {
    session_.ai_column_internal_write_ = saved_; session_.ai_column_internal_read_ = saved_read_;
    session_.set_local_ob_enable_plan_cache(saved_plan_cache_);
  }
private:
  ObSQLSessionInfo &session_; bool saved_; bool saved_read_; bool saved_plan_cache_;
  DISALLOW_COPY_AND_ASSIGN(AiColumnInternalWriteGuard);
};
}
