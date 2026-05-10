#pragma once

#include "CoreMinimal.h"
#include "Animation/MeshDeformer.h"
#include "MilkyBodyDeformer.generated.h"

class UMilkyBodyDeformerInstance;

UCLASS(Blueprintable, BlueprintType, meta=(DisplayName="Milky Body Deformer"))
class MILKYBODY_API UMilkyBodyDeformer : public UMeshDeformer
{
	GENERATED_BODY()

public:
	UMilkyBodyDeformer();

	/**
	 * [日本語]
	 * バネの硬さ (0.0–1.0)。プニのホールド開始/解放時のレスポンス速度を決めます。
	 * 高いほど素早く凹み・素早く戻り、低いほどふわっとゆっくり追従します。
	 * 内部で 2000 倍され、典型的なバネレートに換算されます。
	 *
	 * [English]
	 * Spring stiffness (0.0–1.0) driving the puni hold/release response.
	 * Higher = snappy press and snap-back, lower = soft and delayed.
	 * Internally multiplied by 2000 to span useful spring rates.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MilkyBody",
		meta=(ClampMin="0.0", ClampMax="1.0"))
	float Stiffness = 0.8f;

	/**
	 * [日本語]
	 * バネの減衰 (0.0–1.0)。ホールド開始キックや解放後の振動を抑えます。
	 * 高いとオーバーシュートなく落ち着き、低いとぷるぷる揺れが残ります。
	 * 内部で 200 倍されます。
	 *
	 * [English]
	 * Spring damping (0.0–1.0) for the puni response.
	 * Higher = settles without overshoot, lower = wobbles before settling.
	 * Internally multiplied by 200.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MilkyBody",
		meta=(ClampMin="0.0", ClampMax="1.0"))
	float Damping = 0.1f;

	/**
	 * [日本語]
	 * 頂点位置の押し込みスケール (0.0–1.0)。実ジオメトリの凹みの深さです。
	 *   0.0 → 位置は動かない (Lumen/SSAO 由来の暗い穴は出ない)
	 *   1.0 → フル深度で押し込み (深い空洞 → AO 暗部が出る)
	 * 凹みの中が暗くなりすぎる場合は値を下げてください。
	 *
	 * [English]
	 * Position-displacement scale (0.0–1.0). Controls how deep the actual
	 * geometric cavity becomes.
	 *   0.0 → vertex positions don't move (no Lumen/SSAO darkening of the cavity)
	 *   1.0 → full physical depth (a deep concavity, AO will shade it dark)
	 * Lower this when the dent's interior is too dark.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MilkyBody",
		meta=(ClampMin="0.0", ClampMax="1.0"))
	float GeometryDepthScale = 0.3f;

	/**
	 * [日本語]
	 * 法線の傾き (F⁻ᵀ 補正) のスケール (0.0–1.0)。シェーディング上の凹み感です。
	 *   0.0 → GeometryDepthScale と同じ値が使われる (ジオメトリ追従の最低限の法線修正)
	 *   1.0 → フル傾き (凹んで見えるが、向きによって暗部が出る)
	 * 実際のシェーダーでは max(ShadingDepthScale, GeometryDepthScale) が適用され、
	 * ジオメトリが凹んでいるのに法線が平面のまま → SSAO/Lumen で黒い影が出る問題を防ぎます。
	 *
	 * [English]
	 * Normal-tilt scale (0.0–1.0) for the F⁻ᵀ correction that produces the
	 * dent's lighting cue.
	 *   0.0 → effective scale becomes GeometryDepthScale (minimum normal correction)
	 *   1.0 → full tilt (looks deeply pushed, but creates dark areas where the
	 *         tilted normal points away from light)
	 * The shader uses max(ShadingDepthScale, GeometryDepthScale) to ensure
	 * normals always follow the displaced geometry, preventing SSAO / Lumen
	 * black-flash artifacts when only GeometryDepthScale is raised.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MilkyBody",
		meta=(ClampMin="0.0", ClampMax="1.0"))
	float ShadingDepthScale = 1.0f;

	/**
	 * [日本語]
	 * Hold 中の「引っ張り」追従強度 (0.0–1.0)。Hold 開始位置はアンカーとして固定し、
	 * BeginHoldPush に渡される最新位置（カーソル位置）との差分を、押し込み中心の
	 * falloff 範囲はそのままに、横方向の引っ張り力として impulse に加算します。
	 * これにより、皮膚を掴んで引っ張るように、押し込み内部の頂点が法線方向＋カーソル
	 * 方向に合成変位し「ひっぱって動かしてる感」が出ます。
	 *   0.0 → 引っ張りなし (アンカー位置で純粋な押し込みのみ)
	 *   1.0 → 入力差分に即追従 (硬めの引っ張り)
	 * 値は per-frame lerp の係数（固定 60Hz 内部ステップ）です。
	 *
	 * [English]
	 * Drag-follow smoothing (0.0–1.0). The hold's anchor stays fixed; the diff
	 * between the latest BeginHoldPush location and the anchor is folded into
	 * the impulse as a tangential pull while the falloff region itself stays
	 * anchored. The vertices inside the dent then get displaced along the sum
	 * of (inward normal) + (drag direction), producing a "skin being pulled"
	 * effect rather than the dent sliding.
	 *   0.0 → no drag (anchored push only)
	 *   1.0 → drag tracks the input instantly (stiff pull)
	 * Per-frame lerp coefficient at the fixed 60 Hz internal step.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MilkyBody",
		meta=(ClampMin="0.0", ClampMax="1.0"))
	float HoldFollowSmoothing = 0.3f;

	/**
	 * [日本語]
	 * 引っ張りオフセットの最大振幅を、押し込み半径 (Radius) に対する比で指定 (0.0–1.0)。
	 * impulse に加算される横方向成分の長さがこの値でクランプされます。大きいほど
	 * 大きく引っ張れますが、過剰だとメッシュが破綻するので 0.5 程度を推奨。
	 *
	 * [English]
	 * Maximum drag-pull amplitude, as a fraction of the push Radius (0.0–1.0).
	 * Caps the tangential impulse component added when the cursor moves. Larger
	 * values yank harder but can distort the mesh; ~0.5 is a good default.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MilkyBody",
		meta=(ClampMin="0.0", ClampMax="1.0"))
	float HoldMaxFollowOffsetRatio = 0.5f;

	/**
	 * [日本語]
	 * 現在のプニ押し込み中心と半径をシアンのワイヤースフィアで描画します。
	 * 当たり判定や半径の調整時の確認用です。
	 *
	 * [English]
	 * Draws the current puni push center and radius as a cyan debug wire-sphere.
	 * Useful while tuning hit-test placement and radius.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MilkyBody", meta=(DisplayName="Enable Debug Draw"))
	bool bEnableDebugDraw = false;

	virtual UMeshDeformerInstanceSettings* CreateSettingsInstance(
		UMeshComponent* InMeshComponent) override;

	virtual UMeshDeformerInstance* CreateInstance(
		UMeshComponent* InComponent,
		UMeshDeformerInstanceSettings* InSettings) override;
};
