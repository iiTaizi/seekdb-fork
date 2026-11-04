/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "common/mysqlclient/ob_isql_connection.h"
#include "share/ai_column/ai_column_state.h"
#include "share/ai_column/ai_column_definition.h"

namespace oceanbase::share::schema { class ObColumnSchemaV2; }
namespace oceanbase::sql {
class ObSQLSessionInfo;
class AiColumnModelEvaluator
{
public:
  // The caller must commit its claim before entering this function.
  static int evaluate(common::sqlclient::ObISQLConnection &connection, ObSQLSessionInfo &session,
      const share::schema::ObColumnSchemaV2 &column, const share::AiColumnDefinition &definition,
      const share::AiColumnTask &task, common::ObIAllocator &allocator, common::ObObj &result);
};
}
