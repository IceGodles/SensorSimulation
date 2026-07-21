#include "ImageReadbackManager.h"

#include "Engine/TextureRenderTarget2D.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"
#include "UnrealClient.h"

DEFINE_LOG_CATEGORY_STATIC(LogImageReadback, Log, All);

namespace ImageReadback
{
/** 仅由渲染线程访问的一次 GPU 纹理读回任务。 */
struct FPendingReadback
{
    /** 保存 GPU staging texture 与完成 fence 的 RHI 读回对象。 */
    TUniquePtr<FRHIGPUTextureReadback> Readback;
    /** 生成最终载荷所需的同步帧与传感器元数据。 */
    FCaptureRequest Request;
    /** 当前纹理对应的 RGB 或 Semantic 模态。 */
    EPayloadType PayloadType = EPayloadType::None;
    /** 提交时验证过的完整纹理 View Rect 尺寸。 */
    FIntPoint ImageSize = FIntPoint::ZeroValue;
    /** GPU 纹理的实际像素格式，用于规范化通道顺序。 */
    EPixelFormat PixelFormat = PF_Unknown;
};

/** 跨游戏线程与渲染线程共享的读回状态。 */
struct FSharedState : public TSharedFromThis<FSharedState, ESPMode::ThreadSafe>
{
    /** GPU 完成后等待游戏线程提取的、拥有独立内存的 CPU 图像队列。 */
    TQueue<FImagePayload, EQueueMode::Mpsc> Completed;
    /** 仅由渲染线程创建、轮询和销毁的未完成读回任务。 */
    TArray<FPendingReadback> PendingReadbacks;
    /** 已预留容量但尚未由游戏线程取走的任务数。 */
    TAtomic<int32> PendingCount { 0 };
};

/** 将 BGRA/RGBA staging 行复制为协议统一的紧密 RGBA8 字节。 */
static void CopyToCanonicalRgba(const uint8* Source, int32 SourceRowPitchPixels, EPixelFormat PixelFormat,
    const FIntPoint ImageSize, TArray<uint8>& OutBytes)
{
    constexpr int32 BytesPerPixel = 4;
    OutBytes.SetNumUninitialized(ImageSize.X * ImageSize.Y * BytesPerPixel);
    for (int32 Y = 0; Y < ImageSize.Y; ++Y)
    {
        const uint8* SourceRow = Source + Y * SourceRowPitchPixels * BytesPerPixel;
        uint8* DestinationRow = OutBytes.GetData() + Y * ImageSize.X * BytesPerPixel;
        if (PixelFormat == PF_B8G8R8A8)
        {
            // RHI 常把 RTF_RGBA8 落为 BGRA8；逐像素交换 R/B，保证协议始终按 RGBA 解读。
            for (int32 X = 0; X < ImageSize.X; ++X)
            {
                DestinationRow[X * 4 + 0] = SourceRow[X * 4 + 2];
                DestinationRow[X * 4 + 1] = SourceRow[X * 4 + 1];
                DestinationRow[X * 4 + 2] = SourceRow[X * 4 + 0];
                DestinationRow[X * 4 + 3] = SourceRow[X * 4 + 3];
            }
        }
        else
        {
            FMemory::Memcpy(DestinationRow, SourceRow, ImageSize.X * BytesPerPixel);
        }
    }
}

/** 在渲染线程非阻塞轮询 fence，并发布已经就绪的 CPU 图像副本。 */
static void PumpReadbacks_RenderThread(const TSharedRef<FSharedState, ESPMode::ThreadSafe>& State)
{
    check(IsInRenderingThread());
    for (int32 Index = State->PendingReadbacks.Num() - 1; Index >= 0; --Index)
    {
        FPendingReadback& Pending = State->PendingReadbacks[Index];
        if (!Pending.Readback->IsReady())
        {
            continue;
        }

        int32 RowPitchPixels = 0;
        int32 BufferHeight = 0;
        const uint8* Source = static_cast<const uint8*>(Pending.Readback->Lock(RowPitchPixels, &BufferHeight));
        if (Source && RowPitchPixels >= Pending.ImageSize.X && BufferHeight >= Pending.ImageSize.Y)
        {
            FImagePayload Payload;
            Payload.Header = Pending.Request.Header;
            Payload.SensorName = Pending.Request.SensorName;
            Payload.PayloadType = Pending.PayloadType;
            Payload.ImageSize = Pending.ImageSize;
            Payload.BytesPerPixel = 4;
            CopyToCanonicalRgba(Source, RowPitchPixels, Pending.PixelFormat, Pending.ImageSize, Payload.Bytes);
            State->Completed.Enqueue(MoveTemp(Payload));
        }
        else
        {
            UE_LOG(LogImageReadback, Error,
                TEXT("GPU readback returned an invalid buffer: view=%dx%d pitch=%d height=%d."),
                Pending.ImageSize.X, Pending.ImageSize.Y, RowPitchPixels, BufferHeight);
            // 失败任务没有 Payload 可供提取，因此在此释放它预留的容量。
            State->PendingCount.DecrementExchange();
        }

        if (Source)
        {
            Pending.Readback->Unlock();
        }
        State->PendingReadbacks.RemoveAtSwap(Index, 1, EAllowShrinking::No);
    }
}
}

/** 有容量上限的异步 GPU 图像读回管理器实现。 */
struct FImageReadbackManager::FImpl
{
    /** 构造共享状态并把容量限制为至少一个任务。 */
    explicit FImpl(int32 InCapacity)
        : Capacity(FMath::Max(1, InCapacity))
        , State(MakeShared<ImageReadback::FSharedState, ESPMode::ThreadSafe>())
    {
    }

    /** 队列允许同时保存、读回或等待消费的最大任务数。 */
    int32 Capacity = 4;
    /** 保证排队渲染命令执行期间状态仍然有效的线程安全共享对象。 */
    TSharedRef<ImageReadback::FSharedState, ESPMode::ThreadSafe> State;
};

/** 构造具有指定背压容量的图像读回管理器。 */
FImageReadbackManager::FImageReadbackManager(int32 InCapacity)
    : Impl(MakeUnique<FImpl>(InCapacity))
{
}

/** 排队清理渲染线程任务；共享状态会存活到此前命令全部执行完成。 */
FImageReadbackManager::~FImageReadbackManager()
{
    const TSharedRef<ImageReadback::FSharedState, ESPMode::ThreadSafe> State = Impl->State;
    ENQUEUE_RENDER_COMMAND(ReleaseImageReadbacks)(
        [State](FRHICommandListImmediate&)
        {
            State->PendingReadbacks.Reset();
            State->PendingCount.Store(0);
        });
}

/** 验证目标属性、预留容量并在渲染线程提交完整 View Rect 的 GPU copy。 */
bool FImageReadbackManager::Enqueue(UTextureRenderTarget2D* RenderTarget, const FCaptureRequest& Request,
    EPayloadType PayloadType)
{
    if (!RenderTarget || (PayloadType != EPayloadType::Rgb && PayloadType != EPayloadType::Semantic))
    {
        UE_LOG(LogImageReadback, Warning, TEXT("Readback rejected: target is null or payload type is not RGB/Semantic."));
        return false;
    }

    const FIntPoint ImageSize(RenderTarget->SizeX, RenderTarget->SizeY);
    const EPixelFormat PixelFormat = RenderTarget->GetFormat();
    const bool bSupportedFormat = PixelFormat == PF_B8G8R8A8 || PixelFormat == PF_R8G8B8A8;
    const bool bGammaValid = PayloadType != EPayloadType::Semantic || RenderTarget->bForceLinearGamma;
    if (ImageSize.X <= 0 || ImageSize.Y <= 0 || !bSupportedFormat || !bGammaValid)
    {
        UE_LOG(LogImageReadback, Error,
            TEXT("Readback rejected: type=%u size=%dx%d format=%s forceLinearGamma=%s."),
            static_cast<uint8>(PayloadType), ImageSize.X, ImageSize.Y,
            GPixelFormats[PixelFormat].Name, RenderTarget->bForceLinearGamma ? TEXT("true") : TEXT("false"));
        return false;
    }

    int32 ExpectedCount = Impl->State->PendingCount.Load();
    do
    {
        if (ExpectedCount >= Impl->Capacity)
        {
            UE_LOG(LogImageReadback, Warning, TEXT("Readback queue is full (%d)."), Impl->Capacity);
            return false;
        }
    }
    while (!Impl->State->PendingCount.CompareExchange(ExpectedCount, ExpectedCount + 1));

    FTextureRenderTargetResource* Resource = RenderTarget->GameThread_GetRenderTargetResource();
    if (!Resource)
    {
        Impl->State->PendingCount.DecrementExchange();
        return false;
    }

    const TSharedRef<ImageReadback::FSharedState, ESPMode::ThreadSafe> State = Impl->State;
    ENQUEUE_RENDER_COMMAND(EnqueueImageReadback)(
        [State, Resource, Request, PayloadType, ImageSize, PixelFormat](FRHICommandListImmediate& RHICmdList)
        {
            const FTextureRHIRef& Texture = Resource->GetRenderTargetTexture();
            if (!Texture.IsValid() || Texture->GetSizeXY() != ImageSize || Texture->GetFormat() != PixelFormat)
            {
                UE_LOG(LogImageReadback, Error, TEXT("Readback View Rect validation failed: expected %dx%d %s."),
                    ImageSize.X, ImageSize.Y, GPixelFormats[PixelFormat].Name);
                State->PendingCount.DecrementExchange();
                return;
            }

            ImageReadback::FPendingReadback& Pending = State->PendingReadbacks.AddDefaulted_GetRef();
            Pending.Readback = MakeUnique<FRHIGPUTextureReadback>(TEXT("SimulationImageReadback"));
            Pending.Request = Request;
            Pending.PayloadType = PayloadType;
            Pending.ImageSize = ImageSize;
            Pending.PixelFormat = PixelFormat;
            // 显式传入完整 View Rect，防止 staging 行 padding 被误当作有效像素。
            Pending.Readback->EnqueueCopy(RHICmdList, Texture, FResolveRect(0, 0, ImageSize.X, ImageSize.Y));
        });
    return true;
}

/** 请求渲染线程推进 fence，并非阻塞地取出一个已完成 Payload。 */
bool FImageReadbackManager::PollCompleted(FImagePayload& OutPayload)
{
    const TSharedRef<ImageReadback::FSharedState, ESPMode::ThreadSafe> State = Impl->State;
    ENQUEUE_RENDER_COMMAND(PumpImageReadbacks)(
        [State](FRHICommandListImmediate&)
        {
            ImageReadback::PumpReadbacks_RenderThread(State);
        });

    if (!State->Completed.Dequeue(OutPayload))
    {
        return false;
    }
    State->PendingCount.DecrementExchange();
    return true;
}

/** 返回已预留、正在 GPU 读回或等待 CPU 消费的任务总数。 */
int32 FImageReadbackManager::GetPendingCount() const
{
    return Impl->State->PendingCount.Load();
}
