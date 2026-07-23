#include "ImageReadbackManager.h"

#include "Engine/TextureRenderTarget2D.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"
#include "UnrealClient.h"

DEFINE_LOG_CATEGORY_STATIC(LogImageReadback, Log, All);

namespace ImageReadback
{
/** 仅由渲染线程访问的一次尚未完成的 GPU 纹理读回任务。 */
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
    // 保存已经复制到独立 CPU 内存、等待游戏线程领取的图像。
    TQueue<FImagePayload, EQueueMode::Mpsc> Completed;
    // 保存 GPU 尚未完成或尚未处理的任务
    TArray<FPendingReadback> PendingReadbacks;
    // 统计所有占用容量、但尚未被消费的任务
    TAtomic<int32> PendingCount { 0 };
};

/** GPU Copy 通常保持底层像素格式 BGRA/RGBA
 * 将 BGRA/RGBA staging 行复制为协议统一的紧密 RGBA8 字节。 */
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

// 检查 Fence 并生成 CPU Payload
// 在渲染线程遍历所有 Pending Readback；
// 如果 GPU Fence 已完成，就 Lock staging buffer，把数据复制进独立 CPU 内存，然后放入 Completed 队列。
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
        
        // GPU Copy 完成后，Lock() 返回 CPU 可读取地址
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

// 把结果从 GPU显存的 Render Target 异步回读到 CPU FImagePayload::Bytes
bool FImageReadbackManager::Enqueue(UTextureRenderTarget2D* RenderTarget, const FCaptureRequest& Request,
    EPayloadType PayloadType)
{
    if (!RenderTarget || (PayloadType != EPayloadType::Rgb && PayloadType != EPayloadType::Semantic))
    {
        UE_LOG(LogImageReadback, Warning, TEXT("Readback rejected: target is null or payload type is not RGB/Semantic."));
        return false;
    }

    // 获取尺寸和实际像素格式
    const FIntPoint ImageSize(RenderTarget->SizeX, RenderTarget->SizeY);
    const EPixelFormat PixelFormat = RenderTarget->GetFormat();
    const bool bSupportedFormat = PixelFormat == PF_B8G8R8A8 || PixelFormat == PF_R8G8B8A8;
    // Semantic 标签不能启用 Linear Gamma
    const bool bGammaValid = PayloadType != EPayloadType::Semantic || RenderTarget->bForceLinearGamma;
    if (ImageSize.X <= 0 || ImageSize.Y <= 0 || !bSupportedFormat || !bGammaValid)
    {
        UE_LOG(LogImageReadback, Error,
            TEXT("Readback rejected: type=%u size=%dx%d format=%s forceLinearGamma=%s."),
            static_cast<uint8>(PayloadType), ImageSize.X, ImageSize.Y,
            GPixelFormats[PixelFormat].Name, RenderTarget->bForceLinearGamma ? TEXT("true") : TEXT("false"));
        return false;
    }

    // 预留容量，原子的“检查并加一”，保证多线程并发时变量安全
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

    // 获取 Render Target 的渲染资源
    // UTextureRenderTarget2D 是游戏线程侧的 UObject 包装。
    // GPU Copy 真正需要的是底层： FTextureRenderTargetResource → FTextureRHIRef
    FTextureRenderTargetResource* Resource = RenderTarget->GameThread_GetRenderTargetResource();
    if (!Resource)
    {
        Impl->State->PendingCount.DecrementExchange();
        return false;
    }

    const TSharedRef<ImageReadback::FSharedState, ESPMode::ThreadSafe> State = Impl->State;
    // 这个 Lambda 放进渲染线程命令队列，稍后在渲染线程执行
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

            // 在渲染线程专属的数组中增加一个任务。
            ImageReadback::FPendingReadback& Pending = State->PendingReadbacks.AddDefaulted_GetRef();
            Pending.Readback = MakeUnique<FRHIGPUTextureReadback>(TEXT("SimulationImageReadback"));
            Pending.Request = Request;
            Pending.PayloadType = PayloadType;
            Pending.ImageSize = ImageSize;
            Pending.PixelFormat = PixelFormat;
            
            // Render Target Texture
            //        ↓ GPU Copy 复制的是完整View Rect
            // FRHIGPUTextureReadback 内部 staging resource
            // EnqueueCopy 完成 GPU 资源之间的复制命令提交，并没有立即把像素复制进 FImagePayload::Bytes。
            // 真正生成 TArray<uint8> 是之后：Lock() → CopyToCanonicalRgba()
            Pending.Readback->EnqueueCopy(RHICmdList, Texture, FResolveRect(0, 0, ImageSize.X, ImageSize.Y));
        });
    return true;
}

// 请求渲染线程检查所有 GPU Readback，同时立即尝试从 Completed 队列取出一个已经完成的 Payload。
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
