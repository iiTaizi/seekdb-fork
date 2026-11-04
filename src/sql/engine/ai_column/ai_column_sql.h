/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "common/mysqlclient/ob_isql_connection.h"
#include "lib/container/ob_se_array.h"

namespace oceanbase::share::schema { class ObTableSchema; class ObColumnSchemaV2; }
namespace oceanbase::sql {
class ObSQLSessionInfo;
struct AiColumnSqlRow { common::ObSEArray<common::ObObj, 8> values_; };
struct AiColumnSchemaChange
{
  uint64_t column_id_ = 0;
  int64_t definition_version_ = 0;
  int64_t max_retries_ = 0;
};
class AiColumnSql
{
public:
  static constexpr int64_t MAX_PARAMETERS = 128;
  static constexpr const char *PARAMETER_PREFIX = "__seekdb_aicol_p";
  static int validate_schema_change(const share::schema::ObTableSchema &before,
      const share::schema::ObTableSchema &after);
  static int append_schema_change(const share::schema::ObColumnSchemaV2 &column,
      common::ObIArray<AiColumnSchemaChange> &changes);
  static int fence_schema(common::sqlclient::ObISQLConnection &connection, ObSQLSessionInfo &session,
      const share::schema::ObTableSchema &before, const common::ObString &database,
      const common::ObIArray<AiColumnSchemaChange> &changes);
  static int read(common::sqlclient::ObISQLConnection &connection, ObSQLSessionInfo &session,
      const common::ObString &query, const common::ObIArray<common::ObObj> &parameters,
      common::ObIAllocator &allocator, common::ObIArray<AiColumnSqlRow> &rows, int64_t max_rows = 16);
  static int write(common::sqlclient::ObISQLConnection &connection, ObSQLSessionInfo &session,
      const common::ObString &query, const common::ObIArray<common::ObObj> &parameters,
      int64_t &affected_rows, bool internal_write = true);
};
}
