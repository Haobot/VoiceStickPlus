#pragma once

// N8：测试套件注册表（core_tests.cc 按域拆分后的唯一调用点清单）。
// 新拆套件在此声明，并在 core_tests.cc 的 main() 中各调用一次。
void RunProtocolContractTests();

// N8 cut3：协调器第一批（20 测连续段）。
void RunCoordinatorBatchTests();

// N8 cut4：协调器第二批（17 测连续段）。
void RunCoordinatorBatch2Tests();

// N8 cut5：协调器第三批（11 测连续段，注册与他域交错、按名逐删）。
void RunCoordinatorBatch3Tests();
