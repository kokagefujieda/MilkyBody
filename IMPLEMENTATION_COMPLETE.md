# MilkyBody 実装状況

**最終更新**: 2026-10-01

使い方は `QUICK_START.md`、確認手順は `TESTING_GUIDE.md`、今回の計画・決定事項・保留メモは `docs/PLAN_2026-10-01.md` を参照してください。

> 旧版の PBD（VertexIndexStart / SolverIterations など）は撤去済みです。現行は「LBS スキニング + 解析的な凹み（プニ）」です。

## 構成

| ファイル | 役割 |
|---|---|
| `Shaders/Private/MilkyBodySkin.usf` | LBS スキニング + 最大 32 箇所の凹み + 法線補正（Sherman–Morrison）+ マスク |
| `Source/MilkyBody/Private/MilkyBodyShaders.h/.cpp` | `FMilkyBodySkinCS`（16 パーミュテーション） |
| `Source/MilkyBody/Public/MilkyBodyDeformer.h` | デフォーマアセット（パラメータ・マスク設定） |
| `Source/MilkyBody/Public/MilkyBodyDeformerInstance.h` / `.cpp` | 接触モデル・バネシミュレーション（Tick）・RDG ディスパッチ（EnqueueWork） |
| `Source/MilkyBody/Public/MilkyBodyBlueprintLibrary.h` / `.cpp` | BP API（トレース・ホールド・こね・揉み・状態取得） |
| `Source/MilkyBody/Public/BoneSpherePuniComponent.h` / `.cpp` | 指先スフィアで揉むコンポーネント |
| `Source/MilkyBody/Public/BoneColliderAssetUserData.h` | 指先スフィア設定（メッシュアセットに保存） |
| `Source/MilkyBody/Private/MilkyBodyCPUSkinning.h/.cpp` | CPU スキニングの共通処理（ポリゴントレース・表面スナップ） |
| `Source/MilkyBody/Private/MilkyBodyPrivate.h` | ログカテゴリ・stat グループ・コンソール変数 |
| `Source/MilkyBodyEditor/` | アセット作成メニュー |

## 実行の流れ（1 フレーム）

1. **アクター Tick**: BP や BoneSpherePuni が `BeginHoldPush` / `UpdateExternalContacts` などで接触を更新する
2. **`UMilkyBodyDeformerInstance::Tick`**（FTickableGameObject、ゲームスレッド）
   - 接触ごとのバネを実際の dt で進める（240Hz サブステップ）
   - ホールドの自動解除・ApplyPuni の保持時間・指のフェードを処理する
   - コンポーネント空間のスロット（位置・半径・impulse × 押し込み量）を詰める
   - デバッグ描画を行い、動いている間は `MarkRenderDynamicDataDirty`
3. **`EnqueueWork`**（エンドオブフレーム）: スロットとマスクを RDG に渡し、セクションごとにスキニング + 凹みの CS を流す

## 制約

- エディタのプレビュー（ゲームワールド以外）では、通常のスキニングにフォールバックする。
- MilkyBody はスキニングを自前で行うので、デフォーマ適用中はモーフターゲットとクロスが反映されない（F7 として将来の対応候補）。
- 凹みスロットは 1 メッシュあたり最大 32。超えた分は、押し込み量の小さい指から捨てる。
- パッケージ版でポリゴントレースや表面スナップを使うには、メッシュ LOD の Allow CPU Access が必要。
