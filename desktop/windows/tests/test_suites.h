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

// N8 cut6：协调器第四批（9 测，跨段依赖预检=0）。
void RunCoordinatorBatch4Tests();

// N8 cut7：小米 ATVV 会话批（8 测；6 个共用助手随预检入 test_support.h）。
void RunXiaomiAtvvBatchTests();

// N8 cut8：协调器第五批（8 测；预检含多行签名定义头感知）。
void RunCoordinatorBatch5Tests();

// N8 cut9：协调器第六批（LocalMic 组 8 测）。
void RunCoordinatorBatch6Tests();

// N8 cut10：小米 usage-tap 批（8 测；MakeTapReport 同批迁 support）。
void RunXiaomiUsageTapBatchTests();
