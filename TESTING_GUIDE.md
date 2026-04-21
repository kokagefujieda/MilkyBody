# MilkyBody Plugin テスト方法

## セットアップ

### 1. エディタで MilkyBody デフォーマーを適用

#### 対象メッシュ: Mannequin SkeletalMesh
- **場所**: `Content/Characters/Mannequins/Meshes/SK_Mannequin`

#### デフォーマー作成手順:

1. **MilkyBodyDeformer アセット作成**
   - コンテンツブラウザで右クリック
   - `その他` → `データアセット`
   - クラス: `MilkyBodyDeformer` を選択
   - 名前: `DA_MilkyBodyDeformer` で保存

2. **デフォーマー設定**
   - ダブルクリックで詳細パネルを開く
   - **VertexIndexStart**: `0` (胸部頂点の開始インデックス)
   - **VertexIndexEnd**: `500` (デモ用に前半の頂点範囲)
   - **Stiffness**: `0.8` (アニメーション位置への引き戻し強度)
   - **Damping**: `0.1` (速度減衰)
   - **SolverIterations**: `4` (Verlet 積分反復回数)
   - **GlobalBlendWeight**: `1.0`

3. **Mannequin に適用**
   - DefaultLevel を開く
   - Outliner で `SK_Mannequin_C` (キャラクター)を選択
   - 詳細パネルで `Skeletal Mesh Component` を探す
   - `Mesh Deformer` セクションで:
     - `Enabled`: チェック ON
     - `Deformer`: `DA_MilkyBodyDeformer` を設定

### 2. エディタで実行

1. **Play ボタン**で動作確認
2. キャラクターが上下に揺れるアニメーションを再生
3. **予期される動作**:
   - 指定した頂点範囲が Verlet 物理で変形
   - 重力（下向き）による自然な揺れ
   - Stiffness でアニメーション位置に引き戻される
   - Damping で速度が減衰

## トラブルシューティング

### エディタが起動しない場合
```bash
# Build を再実行
"J:/UE_5.7/Engine/Build/BatchFiles/Build.bat" MilkemistEditor Win64 Development -Project="J:/Milkemist_plane/Milkemist/Milkemist.uproject" -WaitMutex
```

### コンピュートシェーダーエラーが出る場合
- エディタのログを確認: `Saved/Logs/Milkemist.log`
- `RegisterShaders` が実行されたか確認
- GPU が SM5.0 以上に対応しているか確認

### 頂点変形が見えない場合
1. **VertexIndexStart/End の確認**
   - Mannequin メッシュで実際の頂点範囲を調査
   - 胸部領域: 通常 500～2000 インデックス
   
2. **アニメーション確認**
   - キャラクターのアニメーションが実行されているか確認
   - 静止している場合は Stiffness で全頂点が引き戻されるだけ

3. **Blend Weight 確認**
   - `GlobalBlendWeight = 1.0` であることを確認

## 期待される視覚効果

### Stiffness が有効な場合 (0.0 ～ 1.0)
- 小（0.0-0.2）: フワフワと揺れる（柔らかい素材）
- 中（0.4-0.6）: 適度な復帰力（標準）
- 大（0.8-1.0）: 素早く元に戻る（硬い素材）

### Damping が有効な場合 (0.0 ～ 1.0)
- 小（0.0-0.2）: 長く振動する
- 中（0.4-0.6）: 自然な減衰
- 大（0.8-1.0）: すぐに停止

### Solver Iterations (1-16)
- 少ない（1-2）: 計算が軽いが不安定
- 標準（4-6）: バランス型
- 多い（8-16）: より安定するが計算重くなる

## パフォーマンス計測

VertexCount が大きい場合の FPS 低下を測定:

```
表示中:
- VertexIndexEnd: 100   → ほぼ無視できる負荷
- VertexIndexEnd: 500   → 軽微な負荷
- VertexIndexEnd: 2000  → 計測可能な負荷

GPU コンピュートシェーダーのため、高速処理が期待できます。
```

## 次のステップ

### Phase 5 (制約・法線更新)
1. 隣接頂点間の距離制約を追加
2. 変形メッシュの法線を再計算
3. マテリアルで陰影変化を表現

### カスタマイズ
- 異なるメッシュ（キャラのバスト領域など）に適用
- リアルタイムパラメータ調整 UI を追加
- コリジョン（指タッチ）との連携

---

質問やエラーが発生した場合はログを確認してください。
