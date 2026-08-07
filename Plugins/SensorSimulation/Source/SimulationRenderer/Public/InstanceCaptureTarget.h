#pragma once

#include "CoreMinimal.h"
#include "RenderResource.h"
#include "UnrealClient.h"

/**
 * Renderer 自己拥有的 PF_R32_UINT 二维渲染目标。
 *
 * UTextureRenderTarget2D 在 UE 5.7 中只接受颜色/浮点白名单格式，不能合法创建
 * PF_R32_UINT。Instance Channel 因而使用独立 FRenderResource，保证像素始终是原生
 * uint32，而不是把整数伪装成 RGBA8 或浮点颜色。
 */
class SIMULATIONRENDERER_API FInstanceCaptureTarget final : public FTexture, public FRenderTarget
{
public:
    explicit FInstanceCaptureTarget(FIntPoint InSize);

    /** 在渲染线程创建可作为 RT 和 CopySource 使用的 PF_R32_UINT 纹理。 */
    virtual void InitRHI(FRHICommandListBase& RHICmdList) override;

    /** 释放 FRenderTarget 与 FTexture 共同持有的底层 RHI 引用。 */
    virtual void ReleaseRHI() override;

    /** 返回整数目标的固定像素尺寸。 */
    virtual FIntPoint GetSizeXY() const override { return Size; }
    virtual uint32 GetSizeX() const override { return static_cast<uint32>(Size.X); }
    virtual uint32 GetSizeY() const override { return static_cast<uint32>(Size.Y); }
    virtual float GetDisplayGamma() const override { return 1.0f; }
    virtual FString GetFriendlyName() const override
    {
        return TEXT("SensorSimulation.InstanceCaptureTarget");
    }

private:
    FIntPoint Size = FIntPoint::ZeroValue;
};
