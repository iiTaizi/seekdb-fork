/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
namespace oceanbase::sql {
class ObDMLStmt;
class ObLogPlan;
struct ObResolverParams;
class AiColumnStrictRead {
public:
  static int prepare(ObDMLStmt &stmt, ObResolverParams &params);
  static int wrap(ObLogPlan &plan);
};
}
