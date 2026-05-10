#pragma once

#include "CoreMinimal.h"
#include "Engine/AssetUserData.h"
#include "BoneColliderAssetUserData.generated.h"

/**
 * [日本語]
 * 1 ボーンに紐づくスフィア当たり判定（Milky Body Deformer の puni hold と同等の球コライダ）の設定。
 * Asset 側 (USkeletalMesh の AssetUserData) または駆動 Component 側で配列として保持される。
 *
 * [English]
 * One sphere collider tied to a bone (the same circular collision used by Milky Body
 * Deformer's puni hold). Stored in an array on the SkeletalMesh asset (via AssetUserData)
 * or on the driving component.
 */
USTRUCT(BlueprintType)
struct MILKYBODY_API FBoneColliderSphere
{
	GENERATED_BODY()

	/** ターゲットとなるボーン名 (例: index_03_r) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MilkyBody|BoneCollider")
	FName BoneName = NAME_None;

	/** スフィア半径 (cm)。puni hold の Radius と同じ意味。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MilkyBody|BoneCollider", meta=(ClampMin="0.0"))
	float Radius = 2.0f;

	/** ボーンローカル空間でのオフセット。指先ボーンが関節位置にある場合の指の腹方向への押し出し用。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MilkyBody|BoneCollider")
	FVector LocalOffset = FVector::ZeroVector;

	/** 有効化フラグ。設定を残したまま一時的に無効化したいときに使う。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MilkyBody|BoneCollider")
	bool bEnabled = true;
};

/**
 * [日本語]
 * USkeletalMesh の AssetUserData に追加してメッシュごとに「指先スフィア群」設定を保存する。
 * SkeletalMesh アセットの詳細パネル「Asset User Data」で +Add → BoneColliderAssetUserData。
 * 駆動側 Component (UBoneSpherePuniComponent) が Owner の SkeletalMeshComponent から
 * 自動で本データを読み出し、毎フレーム puni hold スロットへ供給する。
 *
 * [English]
 * AssetUserData attached to a USkeletalMesh asset to store the per-mesh fingertip-sphere
 * configuration. Add it from the SkeletalMesh detail panel under "Asset User Data".
 * The driving component (UBoneSpherePuniComponent) reads it from the owning
 * SkeletalMeshComponent each tick and feeds the spheres into the deformer's
 * external multi-hold slots.
 */
UCLASS(BlueprintType, meta=(DisplayName="Milky Body Bone Collider"))
class MILKYBODY_API UBoneColliderAssetUserData : public UAssetUserData
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MilkyBody|BoneCollider")
	TArray<FBoneColliderSphere> Spheres;
};
