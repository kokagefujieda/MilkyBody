# MilkyBody プラグイン クイックスタート

## 実装完了: Phase 4b GPU Compute Shader Dispatch ✓

MilkyBody プラグインの GPU PBD (Position-Based Dynamics) ソフトボディデフォーマーが完全に実装されました。

**Commit**: 82f15a0

## 動作確認方法（3ステップ）

### Step 1: エディタで DA_MilkyBodyDeformer を作成

```
1. コンテンツブラウザを開く
2. 右クリック → その他 → データアセット
3. クラス: MilkyBodyDeformer を選択
4. 名前: DA_MilkyBodyDeformer として保存
```

### Step 2: デフォーマー設定

ダブルクリックして詳細パネルを開き、以下を設定:

| パラメータ | 値 | 説明 |
|-----------|----|----|
| **VertexIndexStart** | 0 | 変形対象の最初の頂点インデックス |
| **VertexIndexEnd** | 500 | 変形対象の最後の頂点インデックス |
| **Stiffness** | 0.8 | アニメーション位置への引き戻し強度 (0.0-1.0) |
| **Damping** | 0.1 | 速度減衰 (0.0-1.0) |
| **SolverIterations** | 4 | Verlet 積分の反復回数 (1-16) |
| **GlobalBlendWeight** | 1.0 | デフォーマー全体のブレンド (0.0-1.0) |

### Step 3: Mannequin に適用して実行

```
1. DefaultLevel を開く
2. Outliner で "SK_Mannequin_C" を選択
3. 詳細パネルの Skeletal Mesh Component を探す
4. "Mesh Deformer" セクション:
   - Enabled: チェック ON
   - Deformer: DA_MilkyBodyDeformer を設定
5. Play ボタンで再生
```

## 期待される動作

- キャラクターが動く際に、指定した頂点範囲がリアルに変形
- 重力による自然な揺れ
- Stiffness でアニメーション位置に引き戻される
- Damping で速度が減衰

## パラメータ調整ガイド

### Stiffness の効果
```
0.0: 重力だけで垂れ下がる（素材が超柔らか）
0.5: バランス型（標準的な弾力性）
1.0: すぐに元に戻る（硬い素材）
```

### Damping の効果
```
0.0: 永遠に振動する（減衰なし）
0.5: 自然な減衰（推奨）
1.0: すぐに停止する（非常に粘性の高い素材）
```

### VertexIndexRange のサイズ
```
小（0-100）: ほぼ視覚的に見えない
中（0-500）: 目立つ変形
大（0-2000）: 大幅な変形（計算負荷増加）
```

## トラブルシューティング

### エディタが起動しない
```bash
# ビルドを再実行
"J:/UE_5.7/Engine/Build/BatchFiles/Build.bat" MilkemistEditor Win64 Development -Project="J:/Milkemist_plane/Milkemist/Milkemist.uproject" -WaitMutex
```

### 変形が見えない場合

1. **VertexIndexEnd が十分に大きいか確認**
   - Mannequin の胸部: 通常 500～2000 インデックス
   - 小さすぎると効果が見えない

2. **GlobalBlendWeight = 1.0 か確認**
   - 0 だと完全に効果が無くなる

3. **Stiffness が高すぎないか確認**
   - 1.0 だと頂点がアニメーション位置にロック
   - 0.5～0.8 がバランス型

4. **エディタログを確認**
   - `Saved/Logs/Milkemist.log` を確認
   - Shader 関連のエラーが出ていないか確認

## 実装の詳細

### GPU Compute Shader (HLSL)
- **File**: `Plugins/MilkyBody/Shaders/Private/MilkyBodyPBD.usf`
- **Kernel**: `[numthreads(64, 1, 1)] void MainCS()`
- **Physics**: Verlet 積分 + Gravity + Stiffness 引き戻し

### C++ RDG 統合
- **File**: `Plugins/MilkyBody/Source/MilkyBody/Private/MilkyBodyDeformerInstance.cpp`
- **Method**: `EnqueueWork()` で毎フレーム GPU dispatch
- **Buffers**: ParticleState (persistent UAV) で Verlet 履歴を保持

### Shader Registration
- **Method**: FCoreDelegates::OnPostEngineInit フック
- **Directory**: `/MilkyBodyShaders` → `Plugins/MilkyBody/Shaders`
- **Macro**: IMPLEMENT_GLOBAL_SHADER で FMilkyBodyPBDCS 登録

## 次のステップ（Phase 5）

### 距離制約（エッジ制約）
隣接頂点間の距離を保つ制約を追加
→ より堅い素材をシミュレート

### 法線再計算
変形メッシュの法線を再計算
→ 陰影が正しく更新される

### コリジョン
球体による衝突判定を追加
→ 指でプッシュして変形させる

## ライセンス

MIT License (no copyright notice required)

---

**準備完了！** エディタを開いてテストしてください。
