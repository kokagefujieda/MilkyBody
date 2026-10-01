# MilkyBody クイックスタート

MilkyBody は SkeletalMesh の表面を「プニッ」と凹ませる GPU デフォーマです（UE 5.7）。
スキニングと凹みを 1 本のコンピュートシェーダーで処理します。カーソルでの押し込み・ドラッグ・こね・揉み・指先コライダーを、同じメッシュに同時に重ねられます。

## 1. デフォーマアセットを作る

1. コンテンツブラウザで右クリック → **Animation → Deformers → Milky Body Deformer**
2. 名前を付けて保存（例: `DA_MilkyBody`）

## 2. メッシュに割り当てる

SkeletalMeshComponent の詳細パネル → **Mesh Deformer** に `DA_MilkyBody` を設定します。

- デフォーマが効くのは **PIE やゲーム中だけ** です。エディタビューポートやアセットプレビューでは、通常のスキニングに戻ります（クラッシュ回避のため）。

## 3. Blueprint から凹ませる

| やりたいこと | ノード |
|---|---|
| クリックした所を一瞬凹ませる | `Apply Puni Under Cursor` |
| 押している間ずっと凹ませる（ドラッグで皮膚を引っ張る） | 毎フレーム `Set Hold Puni Under Cursor`、離したら `Release Hold Puni` か `Release All Puni In World` |
| こねる（指がランダムに動く） | `Begin Kneading Under Cursor` → `End Kneading` |
| 揉む（円運動） | `Begin Momi Under Cursor` → `End Momi` |
| 座標を直接指定 | `Apply Puni` / `Set Hold Puni` / `Begin Kneading` / `Begin Momi` |
| 凹み量でサウンドや演出を動かす | `Get Puni Amount` / `Is Puni Active` |
| 全部解除 | `Release All Puni`（メッシュ単位）/ `Release All Puni In World` |

これらは全部 **同時に重ねられます**（最大 32 箇所）。たとえば、ホールドしながら指コライダーで触ることもできます。

ホールドは、更新が `HoldAutoReleaseSeconds`（既定 0.15 秒）途切れると自動で解除されます。カーソルがメッシュの外に出ても、凹みは残りません。

## 4. 指先で揉む（BoneSpherePuniComponent）

1. 手の SkeletalMesh アセットを開く → **Asset User Data** → `+` → **Milky Body Bone Collider**
2. `Spheres` に指先ボーン名・半径・オフセットを登録する
3. 手のアクターに **BoneSpherePuni** コンポーネントを追加する（ターゲットは自動で検出されます）

- 指ごとに独立したバネを持つので、離れた指から順に、なめらかに戻ります。
- `bDepthFromPenetration`（既定 ON）: 食い込んだ量だけ深く凹みます。`ImpulseDepth` が深さの上限です。
- 両手（コンポーネント 2 つ）で同じ体を触っても、上書きし合いません。

## 5. 凹む部位を限定する（マスク）

デフォーマアセットの **Mask** カテゴリで設定します。

- `DefaultBoneSoftness` + `BoneSoftness`: ボーンごとに柔らかさ (0–1) を設定します（子ボーンにも継承できます）。
  - 例: Default=1 にして、`head` と `hand_l` / `hand_r` を 0 にすると、顔と手が凹まなくなります。
- `VertexColorMaskChannel`: 頂点カラーの R/G/B/A のどれかを掛け合わせます（白 = 凹む）。マテリアルで使っていないチャンネルを選んでください。

## 6. 主なパラメータ（デフォーマアセット）

| 項目 | 意味 |
|---|---|
| Stiffness | バネの硬さ。高いほど素早く凹み、素早く戻る |
| Damping | 揺れの減衰。低いほどプルプルが残る（0–1 の全範囲で安定） |
| GeometryDepthScale | 実際に頂点を動かす量。凹みの中が暗くなりすぎるなら下げる |
| ShadingDepthScale | 法線の傾き（陰影上の凹み感） |
| HoldFollowSmoothing / HoldMaxFollowOffsetRatio | ドラッグで引っ張るときの追従具合 |
| HoldAutoReleaseSeconds | ホールドの自動解除までの秒数（0 で無効） |
| Enable Debug Draw | 凹みの位置を表示する（ホールド=シアン / ApplyPuni=黄 / こね=マゼンタ / 揉み=オレンジ / 指=緑） |

## 7. デバッグ用コンソールコマンド

| コマンド | 効果 |
|---|---|
| `milkybody.Enable 0` | MilkyBody を完全にバイパスして、標準スキニングに戻す（見比べ用） |
| `milkybody.DebugDraw 1` | 全デバッグ描画を強制 ON にする |
| `stat MilkyBody` | CPU コストを表示する（Tick / CPU スキニング / ポリゴントレース など） |
| `ProfileGPU` | GPU コストを表示する（「MilkyBody」スコープ） |

## 8. パッケージ版での注意

ポリゴン精密トレースと指先の表面スナップは、CPU 側の頂点データを使います。パッケージ版では、対象 SkeletalMesh の LOD 設定で **Allow CPU Access** を ON にしてください。OFF のままだと、その機能だけスキップされます（ログに 1 回警告が出ます）。

Physics Asset があれば、カーソル系のトレースはそちらを優先して使います。
