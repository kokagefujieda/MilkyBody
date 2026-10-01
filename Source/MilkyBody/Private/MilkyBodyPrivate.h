#pragma once

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"
#include "Stats/Stats.h"

DECLARE_LOG_CATEGORY_EXTERN(LogMilkyBody, Log, All);

// `stat MilkyBody` で CPU コストを確認できる。
DECLARE_STATS_GROUP(TEXT("MilkyBody"), STATGROUP_MilkyBody, STATCAT_Advanced);

// milkybody.Enable      0 = MilkyBody を完全にバイパスしてエンジン標準スキニングに戻す (A/B 比較用)
// milkybody.DebugDraw   1 = アセット / コンポーネントの設定に関係なく全デバッグ描画を強制 ON
extern TAutoConsoleVariable<int32> CVarMilkyBodyEnable;
extern TAutoConsoleVariable<int32> CVarMilkyBodyDebugDraw;
