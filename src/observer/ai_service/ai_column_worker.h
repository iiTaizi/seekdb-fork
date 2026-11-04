/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "share/ob_thread_pool.h"

namespace oceanbase::observer {
class AiColumnWorker : public share::ObThreadPool
{
public:
  AiColumnWorker() : share::ObThreadPool(2) {}
  ~AiColumnWorker() { stop(); wait(); }
private:
  void run1() override;
  int process_tables();
  DISALLOW_COPY_AND_ASSIGN(AiColumnWorker);
};
}
