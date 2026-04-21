# MilkyBody Plugin - Implementation Status

## Status: Phase 4b Complete (awaiting in-editor verification)

**Last Updated**: 2026-04-20

> Phase 4b.1〜4b.5 の実装とビルドは完了。残タスクは in-editor での動作確認のみ。
> 計画とログの詳細は `docs/MilkyBody_Phase4b/implementation_plan.md` を参照。

---

## What is MilkyBody?

MilkyBody is a **GPU-accelerated PBD (Position-Based Dynamics) soft body deformer plugin** for Unreal Engine 5.7.

The design goal is **vertex-level mesh deformation** on SkeletalMesh bust/breast areas, driven by a compute shader instead of bone-based approximation.

---

## Current State

| Area | State |
|------|-------|
| Plugin scaffold (`UMilkyBodyDeformer` / `UMilkyBodyDeformerInstance`) | 実装済 |
| `UMeshDeformerInstance` pure virtuals | 実装済 |
| Plugin `LoadingPhase` = `PostConfigInit`（Phase 4b.1） | 実装済 |
| Global shader registration `IMPLEMENT_GLOBAL_SHADER`（Phase 4b.2） | 実装済 |
| Instance が `UMeshComponent` を保持（Phase 4b.3） | 実装済 |
| HLSL PBD kernel `Shaders/Private/MilkyBodyPBD.usf` | 実装済 |
| HLSL Skinning kernel `Shaders/Private/MilkyBodySkin.usf`（Phase 4b.5a） | 実装済 |
| `FMilkyBodySkinCS` + 4 permutations（Phase 4b.5b） | 実装済 |
| RDG dispatch: Skin CS → PBD CS → passthrough（Phase 4b.5c） | 実装済 |
| `GetOutputBuffers()` = `SkinnedMeshPosition`（Phase 4b.5d） | 実装済 |
| PBD 範囲外 passthrough + 中間バッファ入力対応（Phase 4b.5e） | 実装済 |
| Verlet ParticleState 永続バッファ | 実装済 |
| MilkemistEditor Development ビルド | 成功 |
| In-editor 動作確認 | 未検証 |

---

## Runtime behavior

1. `MilkyBodyDeformer` を `SkeletalMeshComponent` に割当てるとクラッシュしない。
2. エンジンは MeshDeformer モードに切替わり、通常の GPU Skin Cache を走らせない。
3. `EnqueueWork` は RDG で以下を実行：
   - `AllocateVertexFactoryPositionBuffer` で passthrough position buffer を確保。
   - セクションごとに `FMilkyBodySkinCS` を dispatch して中間スキニング結果を得る。
   - `FMilkyBodyPBDCS` を dispatch して Verlet PBD を指定頂点範囲に適用、範囲外は passthrough コピー。
   - `UpdateVertexFactoryBufferOverrides` で VF に接続、`ExternalAccessQueue.Submit` → `GraphBuilder.Execute`。
4. メッシュが取得できない等のエラーパスでは `FallbackDelegate` を実行し passthrough override をリセットして通常描画にフォールバック。

---

## UE 5.7 Deformer dispatch の仕組み（背景）

1. `UMeshDeformerInstance` が `SkeletalMeshComponent` に付くと、`GetGPUSkinTechnique()` が技法を `MeshDeformer` に切替え、**GPU Skin Cache は走らなくなる**。
2. 描画経路は `FGPUSkinPassthroughVertexFactory` に切替わり、デフォーマが書き込んだ頂点バッファ（`FSkeletalMeshDeformerHelpers::AllocateVertexFactoryPositionBuffer` 経由）を読む。
3. デフォーマが何も書かなければメッシュは不可視／崩壊する。MilkyBody では自前で LBS+PBD を書き込む。

---

## Quick Start

1. **Content Browser → Right-click → Miscellaneous → Data Asset → MilkyBodyDeformer**、`DA_MilkyBodyDeformer` として保存。
2. `VertexIndexStart` / `VertexIndexEnd` / `Stiffness` / `Damping` / `SolverIterations` を設定。
3. SkeletalMeshComponent の **Mesh Deformer** に `DA_MilkyBodyDeformer` を設定。
4. Play / simulate。指定頂点範囲に柔らかい揺れが出る想定。

範囲外の頂点は LBS のまま通過する。

---

## File Structure

```
Plugins/MilkyBody/
├── MilkyBody.uplugin                             (LoadingPhase: PostConfigInit)
├── Source/MilkyBody/
│   ├── MilkyBody.Build.cs
│   ├── Public/
│   │   ├── MilkyBody.h
│   │   ├── MilkyBodyDeformer.h
│   │   └── MilkyBodyDeformerInstance.h
│   └── Private/
│       ├── MilkyBody.cpp                         (shader dir mapping)
│       ├── MilkyBodyDeformer.cpp                 (CreateInstance)
│       ├── MilkyBodyDeformerInstance.cpp         (RDG dispatch pipeline)
│       ├── MilkyBodyShaders.h                    (FMilkyBodySkinCS / FMilkyBodyPBDCS)
│       └── MilkyBodyShaders.cpp                  (IMPLEMENT_GLOBAL_SHADER x2)
├── Shaders/Private/
│   ├── MilkyBodySkin.usf                         (LBS kernel, UBI/Limited 両対応)
│   └── MilkyBodyPBD.usf                          (Verlet PBD + 範囲外 passthrough)
└── IMPLEMENTATION_COMPLETE.md                    (this file)
```
