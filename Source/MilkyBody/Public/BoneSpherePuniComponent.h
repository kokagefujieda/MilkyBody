#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "BoneColliderAssetUserData.h"
#include "BoneSpherePuniComponent.generated.h"

class USkeletalMeshComponent;
class USkinnedMeshComponent;

/**
 * [日本語]
 * 任意のスケルタルメッシュ（手など）の指定ボーン群に「Milky Body Deformer の puni hold」と
 * 同じ球コライダを配置し、ターゲット側の Milky Body メッシュ（胴体など）を毎フレーム揉む
 * コンポーネント。設定は SourceMesh のアセットに付けた UBoneColliderAssetUserData が基本で、
 * 同じインスタンスにだけ追加で Spheres を足したい場合は AdditionalSpheres を併用する。
 *
 * 用途例: 「指先 10 本」を使った揉み込み。各指先ボーンに小さい球を仕込み、ターゲット表面と
 * の交差を検出した指だけが puni hold スロットを占有する (10 本同時可)。
 *
 * [English]
 * Drives sphere puni-hold colliders attached to selected bones of a SkeletalMesh
 * (e.g. a hand) so they kneading the target Milky Body mesh (e.g. a torso) each
 * frame. Sphere config is read primarily from the SourceMesh's
 * UBoneColliderAssetUserData; AdditionalSpheres is appended for instance-only
 * tweaks.
 *
 * Typical setup: ten fingertip bones each get a small sphere; only the fingers
 * whose sphere actually intersects the target body claim a puni-hold slot
 * (up to 10 simultaneously).
 */
UCLASS(ClassGroup=(MilkyBody), meta=(BlueprintSpawnableComponent), BlueprintType, Blueprintable)
class MILKYBODY_API UBoneSpherePuniComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UBoneSpherePuniComponent();

	/**
	 * [日本語] 球コライダを供給するスケルタルメッシュ (例: 手)。
	 *           未設定なら Owner Actor の最初の USkeletalMeshComponent を自動採用。
	 * [English] SkeletalMesh that provides the bone positions (e.g. the hand).
	 *           Auto-resolves to the Owner's first USkeletalMeshComponent when null.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MilkyBody|BoneSpherePuni")
	TWeakObjectPtr<USkeletalMeshComponent> SourceMesh;

	/**
	 * [日本語] 変形対象。Milky Body Deformer がアサインされた SkinnedMesh (胴体など)。
	 *           **未設定なら自動検出**: ワールド内の MilkyBodyDeformer 付き SkinnedMesh を
	 *           走査し、指球と AABB が重なるものに対して同時に適用する (SourceMesh 自身は除外)。
	 *           候補リストは TargetAutoRefreshSeconds 秒ごとに更新される。
	 * [English] Deformation target. A SkinnedMesh that has a Milky Body Deformer assigned.
	 *           **Optional**: leave null to auto-discover all MilkyBody-equipped
	 *           SkinnedMeshComponents in the world; spheres apply to whichever ones
	 *           their AABB overlaps (SourceMesh itself is excluded). The candidate
	 *           list refreshes every TargetAutoRefreshSeconds seconds.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MilkyBody|BoneSpherePuni")
	TWeakObjectPtr<USkinnedMeshComponent> TargetBody;

	/**
	 * [日本語] TargetBody 未設定時、自動検出した候補リストを再構築する間隔 (秒)。
	 *           長くするとアクター生成/破棄への追従が遅れるが、CPU コスト軽減。
	 * [English] When TargetBody is null, how often (seconds) to rebuild the
	 *           auto-discovered candidate list. Longer = cheaper but slower to
	 *           react to spawned/destroyed bodies.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MilkyBody|BoneSpherePuni",
		meta=(ClampMin="0.0"))
	float TargetAutoRefreshSeconds = 0.5f;

	/**
	 * [日本語] アセット側 UserData の Spheres に追加する個別設定。インスタンスごとの調整用。
	 * [English] Per-instance extra spheres appended on top of the asset UserData list.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MilkyBody|BoneSpherePuni")
	TArray<FBoneColliderSphere> AdditionalSpheres;

	/**
	 * [日本語] 押し込み深さ (cm)。スナップ時は内向き法線 × この値が impulse になる。
	 * [English] Push depth in cm. When surface-snapped, impulse = -normal * ImpulseDepth.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MilkyBody|BoneSpherePuni",
		meta=(ClampMin="0.0"))
	float ImpulseDepth = 5.0f;

	/**
	 * [日本語] 表面スナップを行うか。
	 *   true  → ターゲットの実スキニング頂点ポリゴンに対して球-三角形最近点判定を行い、
	 *           球内に表面が入っている指だけ、表面上の最近点に dent を寄せる (高品質)。
	 *   false → ボーンのワールド位置をそのまま dent 中心にする (低コスト・荒い)。
	 * [English] Snap each sphere to the target's actual skinned-mesh surface.
	 *   true  → Sphere-vs-triangle closest-point on CPU-skinned positions; only fingers
	 *           whose sphere intersects the surface fire, and the dent is placed at the
	 *           nearest surface point (clean look).
	 *   false → Use the raw bone world position as the dent center (cheaper, rougher).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MilkyBody|BoneSpherePuni")
	bool bSurfaceSnap = true;

	/**
	 * [日本語] Tick 毎に自動で適用するか。false なら BP から ApplyNow() を呼ぶ。
	 * [English] Auto-drive every tick. When false, drive manually from BP via ApplyNow().
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MilkyBody|BoneSpherePuni")
	bool bAutoTick = true;

	/**
	 * [日本語] デバッグ描画 (球と発火スロットを描く)。
	 * [English] Debug draw spheres and active hold slots.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MilkyBody|BoneSpherePuni")
	bool bDebugDraw = false;

	/**
	 * [日本語] 手動駆動。AutoTick=false のときに BP/C++ から呼ぶ。
	 * [English] Manual drive entry point. Call from BP/C++ when AutoTick=false.
	 */
	UFUNCTION(BlueprintCallable, Category="MilkyBody|BoneSpherePuni")
	void ApplyNow();

	/**
	 * [日本語] スナップ無効化と同等の即時停止。フェードはバネ追従で勝手に消える。
	 * [English] Stop driving; the spring naturally fades to zero.
	 */
	UFUNCTION(BlueprintCallable, Category="MilkyBody|BoneSpherePuni")
	void StopDriving();

	/**
	 * [日本語] TargetBody 自動検出のキャッシュを即時破棄。新しく Body を Spawn した直後に呼ぶ。
	 * [English] Drop the auto-discovered candidate cache. Call after spawning a new body.
	 */
	UFUNCTION(BlueprintCallable, Category="MilkyBody|BoneSpherePuni")
	void RefreshAutoTargets();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

protected:
	virtual void BeginPlay() override;

private:
	USkeletalMeshComponent* ResolveSource() const;

	// Build the list of candidate target bodies for this tick.
	// If TargetBody is set explicitly, returns just that. Otherwise scans the
	// world for SkinnedMeshComponents with a MilkyBodyDeformerInstance, caching
	// the result for TargetAutoRefreshSeconds.
	void GatherTargetCandidates(USkeletalMeshComponent* InSource, TArray<USkinnedMeshComponent*>& OutTargets);

	// Append Asset UserData spheres (from SourceMesh asset) + AdditionalSpheres into Out.
	static void GatherSpheres(
		const USkeletalMeshComponent* SourceMesh,
		const TArray<FBoneColliderSphere>& Additional,
		TArray<FBoneColliderSphere>& Out);

	// Auto-discovery cache.
	TArray<TWeakObjectPtr<USkinnedMeshComponent>> CachedAutoTargets;
	float TimeSinceTargetRefresh = 1e9f; // force refresh on first tick

	// Targets that received a BeginExternalMultiHold last frame; used so we can
	// call End on ones that no longer have any sphere intersecting them.
	TSet<TWeakObjectPtr<USkinnedMeshComponent>> PrevDrivenTargets;

	// 解決できない時の警告を 1 回だけ出すためのフラグ（Tick で連発しないように）
	mutable bool bWarnedSourceNotFound = false;
	mutable bool bWarnedTargetNotFound = false;
};
