#include "InstanceCaptureTarget.h"

#include "RHICommandList.h"

FInstanceCaptureTarget::FInstanceCaptureTarget(const FIntPoint InSize)
    : Size(InSize)
{
}

/** 创建原生 32 位无符号整数纹理；CopySrc 允许异步 GPU Readback 直接复制。 */
void FInstanceCaptureTarget::InitRHI(FRHICommandListBase& RHICmdList)
{
    check(Size.X > 0 && Size.Y > 0);

    const FRHITextureCreateDesc Desc =
        FRHITextureCreateDesc::Create2D(
            TEXT("SensorSimulation.InstanceCaptureTarget"),
            Size.X,
            Size.Y,
            PF_R32_UINT)
        .SetClearValue(FClearValueBinding::Black)
        .SetFlags(
            ETextureCreateFlags::RenderTargetable |
            ETextureCreateFlags::ShaderResource)
        .SetInitialState(ERHIAccess::SRVMask);

    RenderTargetTextureRHI = TextureRHI = RHICmdList.CreateTexture(Desc);
}

/** 先释放 RenderTarget 别名，再交给 FTexture 清理纹理和采样器引用。 */
void FInstanceCaptureTarget::ReleaseRHI()
{
    RenderTargetTextureRHI.SafeRelease();
    FTexture::ReleaseRHI();
}
