#include "ImageReadbackManager.h"
#include "ImageReadbackConversion.h"
#include "InstanceCaptureTarget.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/ThreadSafeCounter64.h"
#include "Misc/ScopeLock.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"
#include "UnrealClient.h"

DEFINE_LOG_CATEGORY_STATIC(LogImageReadback, Log, All);

namespace ImageReadback
{
// ==================== 1. 内部数据模型 ====================

/** 按 SensorGuid + ChannelGuid 隔离统计；PayloadType 只作为通道的数据解释。 */
struct FChannelKey
{
    FGuid SensorGuid;
    FName SensorName = NAME_None;
    FGuid ChannelGuid;
    EPayloadType PayloadType = EPayloadType::None;

    bool operator==(const FChannelKey& Other) const
    {
        return SensorGuid == Other.SensorGuid && ChannelGuid == Other.ChannelGuid;
    }

    friend uint32 GetTypeHash(const FChannelKey& Key)
    {
        return HashCombine(GetTypeHash(Key.SensorGuid), GetTypeHash(Key.ChannelGuid));
    }
};

/** 受 ChannelStatsMutex 保护的可变累计值。 */
struct FChannelStatsAccumulator
{
    int32 PendingCount = 0;
    int32 PeakPendingCount = 0;
    int64 EnqueuedCount = 0;
    int64 CompletedCount = 0;
    int64 DeliveredCount = 0;
    int64 RejectedCount = 0;
    int64 FailedCount = 0;
    int64 CreatedReadbackResources = 0;
    int64 ReusedReadbackResources = 0;
    double TotalGpuLatencyMs = 0.0;
    double MaxGpuLatencyMs = 0.0;
    double TotalDeliveryLatencyMs = 0.0;
    double MaxDeliveryLatencyMs = 0.0;
};

/** 仅由渲染线程访问的一次尚未完成的 GPU 纹理读回任务。 */
struct FPendingReadback
{
    /** 保存 GPU staging texture 与完成 fence 的 RHI 读回对象。 */
    TUniquePtr<FRHIGPUTextureReadback> Readback;
    /** 生成最终载荷所需的同步帧与传感器元数据。 */
    FCaptureRequest Request;
    /** 当前纹理对应的 RGB、Semantic 或 Depth 模态。 */
    EPayloadType PayloadType = EPayloadType::None;
    /** 提交时验证过的完整纹理 View Rect 尺寸。 */
    FIntPoint ImageSize = FIntPoint::ZeroValue;
    /** GPU 纹理的实际像素格式，用于规范化通道顺序。 */
    EPixelFormat PixelFormat = PF_Unknown;
    /** 显示色彩或数值数据语义。 */
    EImageColorSpace ColorSpace = EImageColorSpace::Unknown;
    /** 指标归属键在提交时固定，防止后续 Sensor 配置变化改变历史归属。 */
    FChannelKey ChannelKey;
    /** 游戏线程接受任务的时间，用于统计 GPU 与端到端延迟。 */
    double EnqueueSeconds = 0.0;
};

/** 完成队列同时保存提交时间，Poll 时才能计算端到端交付延迟。 */
struct FCompletedReadback
{
    FImagePayload Payload;
    FChannelKey ChannelKey;
    double EnqueueSeconds = 0.0;
};

/**
 * 可复用的 GPU Readback 资源及其兼容性描述。
 *
 * FRHIGPUTextureReadback 内部持有 staging texture。该资源只有在尺寸和像素格式
 * 与新任务完全一致时才能安全复用，避免将错误的内存布局用于另一种 Render Target。
 */
struct FReusableReadback
{
    /** 已完成 Fence、已经 Unlock，当前没有任务占用的 RHI Readback 对象。 */
    TUniquePtr<FRHIGPUTextureReadback> Readback;
    /** staging texture 创建时对应的宽度和高度。 */
    FIntPoint ImageSize = FIntPoint::ZeroValue;
    /** staging texture 创建时对应的底层 RHI 像素格式。 */
    EPixelFormat PixelFormat = PF_Unknown;
};

/** 游戏线程与渲染线程共享的读回状态。 */
struct FSharedState : public TSharedFromThis<FSharedState, ESPMode::ThreadSafe>
{
    /** 已复制到独立 CPU 内存、等待游戏线程领取的完成队列。 */
    TQueue<FCompletedReadback, EQueueMode::Mpsc> Completed;
    /** GPU Fence 尚未完成或尚未转换为 CPU Payload 的任务；只由渲染线程访问。 */
    TArray<FPendingReadback> PendingReadbacks;
    /** Fence 完成并 Unlock 后回收的 RHI Readback 对象池；只由渲染线程访问。 */
    TArray<FReusableReadback> ReusableReadbacks;

    /** 尚未被消费的任务数，覆盖排队、GPU 执行和等待领取三个阶段。 */
    TAtomic<int32> PendingCount { 0 };
    /** 生命周期内观测到的最大 PendingCount，用于检查容量和背压配置。 */
    TAtomic<int32> PeakPendingCount { 0 };
    /** 按通道累计值会同时被游戏线程和渲染线程更新，必须用互斥锁形成一致快照。 */
    mutable FCriticalSection ChannelStatsMutex;
    TMap<FChannelKey, FChannelStatsAccumulator> ChannelStats;

    /** 成功通过验证并提交到渲染线程的任务总数。 */
    FThreadSafeCounter64 EnqueuedCount;
    /** 已成功转换并放入 Completed 队列的 Payload 总数。 */
    FThreadSafeCounter64 CompletedCount;
    /** 因参数、格式、Gamma 或容量问题而在提交阶段拒绝的任务总数。 */
    FThreadSafeCounter64 RejectedCount;
    /** 提交后因资源、View Rect 或转换失败而终止的任务总数。 */
    FThreadSafeCounter64 FailedCount;
    /** 因复用池无兼容条目而新创建的 FRHIGPUTextureReadback 总数。 */
    FThreadSafeCounter64 CreatedReadbackResources;
    /** 从复用池成功取得兼容 FRHIGPUTextureReadback 的总次数。 */
    FThreadSafeCounter64 ReusedReadbackResources;
};

// ==================== 2. 容量与按通道指标 ====================

/**
 * 使用 CAS 循环线程安全地更新历史最大 Pending 数。
 *
 * CompareExchange 失败时会把 ExpectedPeak 更新为其他线程刚写入的真实值，因此循环会
 * 重新比较，不会用较小的 Count 覆盖更大的峰值。
 */
static void UpdatePeakPending(const TSharedRef<FSharedState, ESPMode::ThreadSafe>& State, const int32 Count)
{
    int32 ExpectedPeak = State->PeakPendingCount.Load();
    while (Count > ExpectedPeak && !State->PeakPendingCount.CompareExchange(ExpectedPeak, Count))
    {
        // 空循环体是有意的：所有读取、比较和重试都在 while 条件中完成。
    }
}

/** 记录一次在进入 GPU 队列之前被拒绝的任务。 */
static void RecordRejected(const TSharedRef<FSharedState, ESPMode::ThreadSafe>& State, const FChannelKey& Key)
{
    FScopeLock Lock(&State->ChannelStatsMutex);
    ++State->ChannelStats.FindOrAdd(Key).RejectedCount;
}

/** 记录一次成功排队，并同步增加该通道的 Pending 计数。 */
static void RecordEnqueued(const TSharedRef<FSharedState, ESPMode::ThreadSafe>& State, const FChannelKey& Key)
{
    FScopeLock Lock(&State->ChannelStatsMutex);
    FChannelStatsAccumulator& Stats = State->ChannelStats.FindOrAdd(Key);
    ++Stats.EnqueuedCount;
    ++Stats.PendingCount;
    Stats.PeakPendingCount = FMath::Max(Stats.PeakPendingCount, Stats.PendingCount);
}

/** 记录本次任务创建了新 Readback 资源，还是复用了池中资源。 */
static void RecordResourceAcquired(
    const TSharedRef<FSharedState, ESPMode::ThreadSafe>& State,
    const FChannelKey& Key,
    const bool bReused)
{
    FScopeLock Lock(&State->ChannelStatsMutex);
    FChannelStatsAccumulator& Stats = State->ChannelStats.FindOrAdd(Key);
    if (bReused)
    {
        ++Stats.ReusedReadbackResources;
    }
    else
    {
        ++Stats.CreatedReadbackResources;
    }
}

/** 记录 GPU Fence 完成并成功生成 CPU Payload 的时延。 */
static void RecordCompleted(
    const TSharedRef<FSharedState, ESPMode::ThreadSafe>& State,
    const FChannelKey& Key,
    const double GpuLatencyMs)
{
    FScopeLock Lock(&State->ChannelStatsMutex);
    FChannelStatsAccumulator& Stats = State->ChannelStats.FindOrAdd(Key);
    ++Stats.CompletedCount;
    Stats.TotalGpuLatencyMs += GpuLatencyMs;
    Stats.MaxGpuLatencyMs = FMath::Max(Stats.MaxGpuLatencyMs, GpuLatencyMs);
}

/** 记录提交后的失败；bWasPending 指示是否需要释放通道级容量。 */
static void RecordFailed(
    const TSharedRef<FSharedState, ESPMode::ThreadSafe>& State,
    const FChannelKey& Key,
    const bool bWasPending)
{
    FScopeLock Lock(&State->ChannelStatsMutex);
    FChannelStatsAccumulator& Stats = State->ChannelStats.FindOrAdd(Key);
    ++Stats.FailedCount;
    if (bWasPending)
    {
        Stats.PendingCount = FMath::Max(0, Stats.PendingCount - 1);
    }
}

/** 记录 Payload 被消费线程领取，并计算从 Enqueue 到交付的端到端时延。 */
static void RecordDelivered(
    const TSharedRef<FSharedState, ESPMode::ThreadSafe>& State,
    const FChannelKey& Key,
    const double DeliveryLatencyMs)
{
    FScopeLock Lock(&State->ChannelStatsMutex);
    FChannelStatsAccumulator& Stats = State->ChannelStats.FindOrAdd(Key);
    ++Stats.DeliveredCount;
    Stats.PendingCount = FMath::Max(0, Stats.PendingCount - 1);
    Stats.TotalDeliveryLatencyMs += DeliveryLatencyMs;
    Stats.MaxDeliveryLatencyMs = FMath::Max(Stats.MaxDeliveryLatencyMs, DeliveryLatencyMs);
}

// ==================== 3. 全局 Manager 注册与 Pump 协调 ====================

/** 所有 Manager 共享一个 Pump 协调器，避免每台相机每 Tick 都提交一条渲染命令。 */
struct FGlobalPumpState : public TSharedFromThis<FGlobalPumpState, ESPMode::ThreadSafe>
{
    FCriticalSection Mutex;
    TArray<TWeakPtr<FSharedState, ESPMode::ThreadSafe>> Managers;
    bool bPumpQueued = false;
    FImageReadbackGlobalPumpStats Stats;
};

/** 返回进程内唯一的全局 Pump 状态；函数内静态对象保证首次使用时安全初始化。 */
static TSharedRef<FGlobalPumpState, ESPMode::ThreadSafe> GetGlobalPumpState()
{
    static const TSharedRef<FGlobalPumpState, ESPMode::ThreadSafe> State =
        MakeShared<FGlobalPumpState, ESPMode::ThreadSafe>();
    return State;
}

/** 登记 Manager 的弱引用，使一次全局 Pump 可以批量推进所有存活实例。 */
static void RegisterManager(const TSharedRef<FSharedState, ESPMode::ThreadSafe>& State)
{
    const TSharedRef<FGlobalPumpState, ESPMode::ThreadSafe> Global = GetGlobalPumpState();
    FScopeLock Lock(&Global->Mutex);
    Global->Managers.Add(State);
    Global->Stats.RegisteredManagerCount = Global->Managers.Num();
    Global->Stats.PeakRegisteredManagerCount = FMath::Max(
        Global->Stats.PeakRegisteredManagerCount,
        Global->Stats.RegisteredManagerCount);
}

/** 移除正在析构及已经失效的 Manager，防止后续 Pump 再获取它们。 */
static void UnregisterManager(const TSharedRef<FSharedState, ESPMode::ThreadSafe>& State)
{
    const TSharedRef<FGlobalPumpState, ESPMode::ThreadSafe> Global = GetGlobalPumpState();
    FScopeLock Lock(&Global->Mutex);
    Global->Managers.RemoveAll(
        [&State](const TWeakPtr<FSharedState, ESPMode::ThreadSafe>& Candidate)
        {
            const TSharedPtr<FSharedState, ESPMode::ThreadSafe> Pinned = Candidate.Pin();
            return !Pinned.IsValid() || Pinned.Get() == &State.Get();
        });
    Global->Stats.RegisteredManagerCount = Global->Managers.Num();
}


// ==================== 4. 渲染线程 Readback 资源池 ====================

/**
 * 获取与尺寸、像素格式兼容的 Readback 对象。
 *
 * 优先从渲染线程专属复用池移动一个兼容对象；没有匹配项时才创建新的 RHI 资源。
 * 返回 TUniquePtr 表示新任务在完成并回收前独占该 Readback。
 */
static TUniquePtr<FRHIGPUTextureReadback> AcquireReadback_RenderThread(
    const TSharedRef<FSharedState, ESPMode::ThreadSafe>& State,
    const FIntPoint ImageSize,
    const EPixelFormat PixelFormat,
    bool& bOutReused)
{
    check(IsInRenderingThread());
    bOutReused = false;
    // 在资源池中查找尺寸和格式完全一致的闲置对象；整数标签也可安全逐位复用。
    const int32 ReusableIndex = State->ReusableReadbacks.IndexOfByPredicate(
        [ImageSize, PixelFormat](const FReusableReadback& Candidate)
        {
            return Candidate.ImageSize == ImageSize && Candidate.PixelFormat == PixelFormat;
        });

    if (ReusableIndex != INDEX_NONE)
    {
        // MoveTemp 转移独占所有权；RemoveAtSwap 删除已经取走的空池条目且避免整体搬移。
        TUniquePtr<FRHIGPUTextureReadback> Result =
            MoveTemp(State->ReusableReadbacks[ReusableIndex].Readback);
        State->ReusableReadbacks.RemoveAtSwap(ReusableIndex, 1, EAllowShrinking::No);
        State->ReusedReadbackResources.Increment();
        bOutReused = true;
        return Result;
    }

    State->CreatedReadbackResources.Increment();
    // 没有兼容资源则创建
    return MakeUnique<FRHIGPUTextureReadback>(TEXT("SimulationImageReadback"));
}

/**
 * 将完成且已经 Unlock 的 Readback 对象放回复用池。
 *
 * 调用方必须保证 GPU Fence 已完成、CPU 不再访问映射内存。
 * 函数同时保存资源描述，使后续 Acquire 只能把它用于相同尺寸和像素格式的任务。
 */
static void RecycleReadback_RenderThread(
    const TSharedRef<FSharedState, ESPMode::ThreadSafe>& State,
    FPendingReadback& Pending)
{
    check(IsInRenderingThread());

    FReusableReadback& Reusable = State->ReusableReadbacks.AddDefaulted_GetRef();
    Reusable.Readback = MoveTemp(Pending.Readback);
    Reusable.ImageSize = Pending.ImageSize;
    Reusable.PixelFormat = Pending.PixelFormat;
}

// ==================== 5. GPU 完成检查与 CPU Payload 转换 ====================

/**
 * 在渲染线程非阻塞处理所有已经完成 Fence 的任务。
 *
 * 尚未 Ready 的任务保留到下一次 Pump；Ready 的任务会 Lock staging texture，
 * 根据模态转换为 FImagePayload，随后 Unlock、回收 Readback 并移除 Pending 条目。
 * 失败任务不会进入 Completed，因此必须在此递减 PendingCount 释放容量。
 */
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
        const uint8* Source =
            static_cast<const uint8*>(Pending.Readback->Lock(RowPitchPixels, &BufferHeight));

        // bSucceeded 同时表示 Lock 缓冲区有效且对应的格式转换成功。
        bool bSucceeded = false;
        if (Source && BufferHeight >= Pending.ImageSize.Y)
        {
            FImagePayload Payload;
            Payload.Header = Pending.Request.Header;
            Payload.SensorName = Pending.Request.SensorName;
            Payload.SensorGuid = Pending.Request.SensorGuid;
            Payload.ChannelGuid = Pending.ChannelKey.ChannelGuid;
            Payload.PayloadType = Pending.PayloadType;
            Payload.ImageSize = Pending.ImageSize;
            Payload.ViewRect = FIntRect(FIntPoint::ZeroValue, Pending.ImageSize);
            Payload.ColorSpace = Pending.ColorSpace;
            Payload.BytesPerPixel = 4;
            Payload.RowStrideBytes = Pending.ImageSize.X * Payload.BytesPerPixel;

            if (Pending.PayloadType == EPayloadType::Depth)
            {
                // Depth 不能按颜色字节复制：转换函数读取 R32F/Float4，并统一输出“米”。
                Payload.PixelFormat = EImagePixelFormat::R32Float;
                Payload.ValueUnit = EImageValueUnit::Meters;
                bSucceeded = UE::SensorSimulation::ImageReadback::CopyDepthR32FloatToMeters(
                    Source,
                    RowPitchPixels,
                    Pending.PixelFormat,
                    Pending.ImageSize,
                    Payload.Bytes);
            }
            else if (Pending.PayloadType == EPayloadType::Instance)
            {
                // Instance 是原生 uint32 标识符；只剥离 RowPitch，绝不经过 RGBA8 或浮点归一化。
                Payload.PixelFormat = EImagePixelFormat::R32Uint;
                Payload.ValueUnit = EImageValueUnit::Identifier;
                bSucceeded = UE::SensorSimulation::ImageReadback::CopyR32UintToCanonical(
                    Source,
                    RowPitchPixels,
                    Pending.PixelFormat,
                    Pending.ImageSize,
                    Payload.Bytes);
            }
            else
            {
                // RGB 与 Semantic 统一输出紧密 RGBA8；转换函数负责处理 RHI 的 BGRA/RGBA 差异。
                Payload.PixelFormat = EImagePixelFormat::Rgba8;
                Payload.ValueUnit = Pending.PayloadType == EPayloadType::Semantic
                    ? EImageValueUnit::Identifier
                    : EImageValueUnit::None;
                bSucceeded = UE::SensorSimulation::ImageReadback::CopyRgba8ToCanonical(
                    Source,
                    RowPitchPixels,
                    Pending.PixelFormat,
                    Pending.ImageSize,
                    Payload.Bytes);
            }
            if (bSucceeded)
            {
                FCompletedReadback Completed;
                Completed.Payload = MoveTemp(Payload);
                Completed.ChannelKey = Pending.ChannelKey;
                Completed.EnqueueSeconds = Pending.EnqueueSeconds;
                State->Completed.Enqueue(MoveTemp(Completed));
                State->CompletedCount.Increment();
                RecordCompleted(
                    State,
                    Pending.ChannelKey,
                    (FPlatformTime::Seconds() - Pending.EnqueueSeconds) * 1000.0);
            }
        }

        if (!bSucceeded)
        {
            UE_LOG(LogImageReadback, Error,
                TEXT("GPU readback returned an invalid buffer: view=%dx%d pitch=%d height=%d."),
                Pending.ImageSize.X,
                Pending.ImageSize.Y,
                RowPitchPixels,
                BufferHeight);
            // 失败任务没有 Payload 可供提取，因此在此释放它预留的容量。
            State->FailedCount.Increment();
            State->PendingCount.DecrementExchange();
            RecordFailed(State, Pending.ChannelKey, true);
        }

        if (Source)
        {
            Pending.Readback->Unlock();
        }

        // Readback 对象在 Fence 完成且 Unlock 后可以跨帧复用，减少连续采集时的 RHI 分配抖动。
        RecycleReadback_RenderThread(State, Pending);
        State->PendingReadbacks.RemoveAtSwap(Index, 1, EAllowShrinking::No);
    }
}

/**
 * 对已经完成格式验证的纹理源统一预留容量并提交 GPU Copy。
 *
 * AcquireTexture 只在渲染线程执行；它既支持 UObject RenderTargetResource，也支持
 * Renderer 自己拥有的 PF_R32_UINT FRenderResource，避免维护两套 Readback 状态机。
 */
static bool EnqueueValidatedTexture(
    const TSharedRef<FSharedState, ESPMode::ThreadSafe>& State,
    const int32 Capacity,
    const FCaptureRequest& Request,
    const FGuid& ChannelGuid,
    const EPayloadType PayloadType,
    const FIntPoint ImageSize,
    const EPixelFormat PixelFormat,
    const EImageColorSpace ColorSpace,
    TFunction<FTextureRHIRef()>&& AcquireTexture)
{
    const FChannelKey ChannelKey { Request.SensorGuid, Request.SensorName, ChannelGuid, PayloadType };
    const double EnqueueSeconds = FPlatformTime::Seconds();

    int32 ExpectedCount = State->PendingCount.Load();
    do
    {
        if (ExpectedCount >= Capacity)
        {
            State->RejectedCount.Increment();
            RecordRejected(State, ChannelKey);
            UE_LOG(LogImageReadback, Warning, TEXT("Readback queue is full (%d)."), Capacity);
            return false;
        }
    }
    while (!State->PendingCount.CompareExchange(ExpectedCount, ExpectedCount + 1));

    UpdatePeakPending(State, ExpectedCount + 1);
    State->EnqueuedCount.Increment();
    RecordEnqueued(State, ChannelKey);

    // 提交渲染线程命令
    ENQUEUE_RENDER_COMMAND(EnqueueImageReadback)(
        [State,
         Request,
         PayloadType,
         ImageSize,
         PixelFormat,
         ColorSpace,
         ChannelKey,
         EnqueueSeconds,
         AcquireTexture = MoveTemp(AcquireTexture)](FRHICommandListImmediate& RHICmdList) mutable
        {
            const FTextureRHIRef Texture = AcquireTexture();
            if (!Texture.IsValid() ||
                Texture->GetSizeXY() != ImageSize ||
                Texture->GetFormat() != PixelFormat)
            {
                UE_LOG(LogImageReadback, Error,
                    TEXT("Readback View Rect validation failed: expected %dx%d %s."),
                    ImageSize.X,
                    ImageSize.Y,
                    GPixelFormats[PixelFormat].Name);
                State->FailedCount.Increment();
                RecordFailed(State, ChannelKey, true);
                State->PendingCount.DecrementExchange();
                return;
            }

            FPendingReadback& Pending = State->PendingReadbacks.AddDefaulted_GetRef();
            bool bReusedReadback = false;
            Pending.Readback = AcquireReadback_RenderThread(
                State, ImageSize, PixelFormat, bReusedReadback);
            RecordResourceAcquired(State, ChannelKey, bReusedReadback);
            Pending.Request = Request;
            Pending.PayloadType = PayloadType;
            Pending.ImageSize = ImageSize;
            Pending.PixelFormat = PixelFormat;
            Pending.ColorSpace = ColorSpace;
            Pending.ChannelKey = ChannelKey;
            Pending.EnqueueSeconds = EnqueueSeconds;


            // FRHIGPUTextureReadback::EnqueueCopy 只负责 staging 目标的状态转换，不会转换源纹理。
            // 因此在跨 RDG 图的原始 RHI Copy 前显式进入 CopySrc；D3D11 的隐式状态会掩盖
            // 这一遗漏，而 D3D12 可能在 Instance Draw 尚未形成可复制状态时读到清屏结果。
            RHICmdList.Transition(FRHITransitionInfo(
                Texture,
                ERHIAccess::Unknown,
                ERHIAccess::CopySrc));
            // GPU Copy 只进入 staging/readback 资源；CPU 字节仍由后续 Pump 的 Lock/转换生成。
            Pending.Readback->EnqueueCopy(
                RHICmdList,
                Texture,
                FResolveRect(0, 0, ImageSize.X, ImageSize.Y));
        });
    return true;
}

/** 提交一次批量 Pump；任意 Manager 的 Poll 都只会唤醒这一全局入口。 */
static void RequestGlobalPump()
{
    const TSharedRef<FGlobalPumpState, ESPMode::ThreadSafe> Global = GetGlobalPumpState();
    TArray<TSharedRef<FSharedState, ESPMode::ThreadSafe>> Snapshot;
    {
        FScopeLock Lock(&Global->Mutex);
        if (Global->bPumpQueued)
        {
            return;
        }

        Global->Managers.RemoveAll(
            [&Snapshot](const TWeakPtr<FSharedState, ESPMode::ThreadSafe>& Candidate)
            {
                if (const TSharedPtr<FSharedState, ESPMode::ThreadSafe> Pinned = Candidate.Pin())
                {
                    Snapshot.Add(Pinned.ToSharedRef());
                    return false;
                }
                return true;
            });
        Global->Stats.RegisteredManagerCount = Global->Managers.Num();
        Global->bPumpQueued = true;
        ++Global->Stats.PumpCommandCount;
        Global->Stats.PeakManagersPerPump = FMath::Max(
            Global->Stats.PeakManagersPerPump,
            Snapshot.Num());
    }

    ENQUEUE_RENDER_COMMAND(PumpAllImageReadbacks)(
        [Global, Snapshot = MoveTemp(Snapshot)](FRHICommandListImmediate&) mutable
        {
            for (const TSharedRef<FSharedState, ESPMode::ThreadSafe>& State : Snapshot)
            {
                PumpReadbacks_RenderThread(State);
            }

            FScopeLock Lock(&Global->Mutex);
            Global->Stats.PumpedManagerCount += Snapshot.Num();
            Global->bPumpQueued = false;
        });
}
}

// ==================== 6. Manager 生产实现 ====================

/** 有容量上限的异步 GPU 图像读回管理器实现。 */
struct FImageReadbackManager::FImpl
{
    /** 构造共享状态并把容量限制为至少一个任务。 */
    explicit FImpl(const int32 InCapacity)
        : Capacity(FMath::Max(1, InCapacity))
        , State(MakeShared<ImageReadback::FSharedState, ESPMode::ThreadSafe>())
    {
    }

    /** 队列允许同时保存、读回或等待消费的最大任务数。 */
    TAtomic<int32> Capacity { 4 };
    /** 保证排队渲染命令执行期间状态仍然有效的线程安全共享对象。 */
    TSharedRef<ImageReadback::FSharedState, ESPMode::ThreadSafe> State;
};

/** 构造具有指定背压容量的图像读回管理器。 */
FImageReadbackManager::FImageReadbackManager(const int32 InCapacity)
    : Impl(MakeUnique<FImpl>(InCapacity))
{
    // 构造时同步登记弱引用，使任意相机 Poll 都能批量推进当前所有 Manager。
    ImageReadback::RegisterManager(Impl->State);
}

/** 排队清理渲染线程任务；共享状态会存活到此前命令全部执行完成。 */
FImageReadbackManager::~FImageReadbackManager()
{
    const TSharedRef<ImageReadback::FSharedState, ESPMode::ThreadSafe> State = Impl->State;
    // 先从全局快照来源移除；已经排队的命令仍由 SharedRef 保证 State 存活。
    ImageReadback::UnregisterManager(State);
    ENQUEUE_RENDER_COMMAND(ReleaseImageReadbacks)(
        [State](FRHICommandListImmediate&)
        {
            State->PendingReadbacks.Reset();
            State->ReusableReadbacks.Reset();
            State->PendingCount.Store(0);
        });
}

/** 更新后续提交使用的容量；已有任务继续完成，避免缩容引入取消和半帧结果。 */
void FImageReadbackManager::SetCapacity(const int32 InCapacity)
{
    Impl->Capacity.Store(FMath::Max(1, InCapacity));
}

/** 返回当前原子容量快照。 */
int32 FImageReadbackManager::GetCapacity() const
{
    return Impl->Capacity.Load();
}

/**
 * 验证 Render Target 和模态、原子预留队列容量，并排队 GPU 纹理读回。
 *
 * 返回 true 仅表示任务成功进入渲染命令队列，并不表示 GPU 已完成或 CPU 已获得像素。
 * RGB/Semantic 接受 8 位 RGBA/BGRA；Depth 接受 R32F 或 Float4；Instance 只接受
 * PF_R32_UINT。Semantic/Instance 必须使用 Linear Gamma，保证数值标签不受颜色编码影响。
 */
bool FImageReadbackManager::Enqueue(
    UTextureRenderTarget2D* RenderTarget,
    const FCaptureRequest& Request,
    const FGuid& ChannelGuid,
    const EPayloadType PayloadType)
{
    const TSharedRef<ImageReadback::FSharedState, ESPMode::ThreadSafe> State = Impl->State;
    const ImageReadback::FChannelKey ChannelKey { Request.SensorGuid, Request.SensorName, ChannelGuid, PayloadType };
    const double EnqueueSeconds = FPlatformTime::Seconds();
    const bool bSupportedPayload =
        PayloadType == EPayloadType::Rgb ||
        PayloadType == EPayloadType::Semantic ||
        PayloadType == EPayloadType::Depth ||
        PayloadType == EPayloadType::Instance;
    if (!RenderTarget || !bSupportedPayload)
    {
        State->RejectedCount.Increment();
        ImageReadback::RecordRejected(State, ChannelKey);
        UE_LOG(LogImageReadback, Warning, TEXT("Readback rejected: target is null or payload type is unsupported."));
        return false;
    }

    // 获取尺寸和实际像素格式
    const FIntPoint ImageSize(RenderTarget->SizeX, RenderTarget->SizeY);
    const EPixelFormat PixelFormat = RenderTarget->GetFormat();
    const bool bRgba8Format =
        PixelFormat == PF_B8G8R8A8 || PixelFormat == PF_R8G8B8A8;
    const bool bSupportedFormat =
        PayloadType == EPayloadType::Depth
            ? PixelFormat == PF_R32_FLOAT || PixelFormat == PF_A32B32G32R32F
            : PayloadType == EPayloadType::Instance
                ? PixelFormat == PF_R32_UINT
                : bRgba8Format;
    // Semantic/Instance 是数值标签，必须强制线性，防止颜色编码介入数据纹理。
    const bool bGammaValid =
        (PayloadType != EPayloadType::Semantic && PayloadType != EPayloadType::Instance) ||
        RenderTarget->bForceLinearGamma;
    if (ImageSize.X <= 0 || ImageSize.Y <= 0 || !bSupportedFormat || !bGammaValid)
    {
        State->RejectedCount.Increment();
        ImageReadback::RecordRejected(State, ChannelKey);
        UE_LOG(LogImageReadback, Error,
            TEXT("Readback rejected: type=%u size=%dx%d format=%s forceLinearGamma=%s."),
            static_cast<uint8>(PayloadType),
            ImageSize.X,
            ImageSize.Y,
            GPixelFormats[PixelFormat].Name,
            RenderTarget->bForceLinearGamma ? TEXT("true") : TEXT("false"));
        return false;
    }

    FTextureRenderTargetResource* Resource = RenderTarget->GameThread_GetRenderTargetResource();
    if (!Resource)
    {
        State->FailedCount.Increment();
        ImageReadback::RecordFailed(State, ChannelKey, false);
        return false;
    }

    // Semantic 和 Depth 是数值数据；Instance 由原生整数目标重载处理。
    const EImageColorSpace ColorSpace =
        PayloadType == EPayloadType::Semantic || PayloadType == EPayloadType::Depth
            ? EImageColorSpace::Data
            : (RenderTarget->bForceLinearGamma ? EImageColorSpace::Linear : EImageColorSpace::SRgb);

    return ImageReadback::EnqueueValidatedTexture(
        State,
        Impl->Capacity.Load(),
        Request,
        ChannelGuid,
        PayloadType,
        ImageSize,
        PixelFormat,
        ColorSpace,
        [Resource]()
        {
            return Resource->GetRenderTargetTexture();
        });
}

/** 提交原生 PF_R32_UINT Instance 目标，并由 SharedPtr 保证在途 GPU Copy 的资源寿命。 */
bool FImageReadbackManager::Enqueue(
    const TSharedPtr<FInstanceCaptureTarget, ESPMode::ThreadSafe>& RenderTarget,
    const FCaptureRequest& Request,
    const FGuid& ChannelGuid,
    const EPayloadType PayloadType)
{
    const TSharedRef<ImageReadback::FSharedState, ESPMode::ThreadSafe> State = Impl->State;
    const ImageReadback::FChannelKey ChannelKey { Request.SensorGuid, Request.SensorName, ChannelGuid, PayloadType };
    if (!RenderTarget.IsValid() ||
        PayloadType != EPayloadType::Instance ||
        RenderTarget->GetSizeXY().X <= 0 ||
        RenderTarget->GetSizeXY().Y <= 0)
    {
        State->RejectedCount.Increment();
        ImageReadback::RecordRejected(State, ChannelKey);
        UE_LOG(LogImageReadback, Warning,
            TEXT("Instance readback rejected: target is null, empty, or payload type is not Instance."));
        return false;
    }

    const FIntPoint ImageSize = RenderTarget->GetSizeXY();

    // 内部提交GPU copy
    return ImageReadback::EnqueueValidatedTexture(
        State,
        Impl->Capacity.Load(),
        Request,
        ChannelGuid,
        PayloadType,
        ImageSize,
        PF_R32_UINT,
        EImageColorSpace::Data,
        [RenderTarget]()
        {
            return RenderTarget->GetRenderTargetTexture();
        });
}

/**
 * 请求 Renderer 全局协调器推进所有 Manager，并立即尝试取出一个已完成 Payload。
 *
 * 全局 bPumpQueued 保证同一时刻最多只有一条批量 Pump 渲染命令；本次返回 false
 * 只表示 Payload 尚未交付，调用方应在后续 Tick 继续非阻塞轮询。
 */
bool FImageReadbackManager::PollCompleted(FImagePayload& OutPayload)
{
    const TSharedRef<ImageReadback::FSharedState, ESPMode::ThreadSafe> State = Impl->State;
    ImageReadback::RequestGlobalPump();

    ImageReadback::FCompletedReadback Completed;
    if (!State->Completed.Dequeue(Completed))
    {
        return false;
    }

    OutPayload = MoveTemp(Completed.Payload);
    State->PendingCount.DecrementExchange();
    ImageReadback::RecordDelivered(
        State,
        Completed.ChannelKey,
        (FPlatformTime::Seconds() - Completed.EnqueueSeconds) * 1000.0);
    return true;
}

/** 返回已预留、正在 GPU 读回或等待 CPU 消费的任务总数。 */
int32 FImageReadbackManager::GetPendingCount() const
{
    return Impl->State->PendingCount.Load();
}

/**
 * 获取当前 Manager 的线程安全统计快照。
 *
 * 原子量和线程安全计数器可能来自略有差异的瞬间，因此快照适合监控与验收，
 * 不应用作要求多个字段严格事务一致的控制逻辑。
 */
FImageReadbackStats FImageReadbackManager::GetStats() const
{
    const TSharedRef<ImageReadback::FSharedState, ESPMode::ThreadSafe> State = Impl->State;
    FImageReadbackStats Stats;
    Stats.Capacity = Impl->Capacity.Load();
    Stats.PendingCount = State->PendingCount.Load();
    Stats.PeakPendingCount = State->PeakPendingCount.Load();
    Stats.EnqueuedCount = State->EnqueuedCount.GetValue();
    Stats.CompletedCount = State->CompletedCount.GetValue();
    Stats.RejectedCount = State->RejectedCount.GetValue();
    Stats.FailedCount = State->FailedCount.GetValue();
    Stats.CreatedReadbackResources = State->CreatedReadbackResources.GetValue();
    Stats.ReusedReadbackResources = State->ReusedReadbackResources.GetValue();
    return Stats;
}

/** 返回按 SensorGuid + ChannelGuid 分组的线程安全指标快照。 */
TArray<FImageReadbackChannelStats> FImageReadbackManager::GetChannelStats() const
{
    const TSharedRef<ImageReadback::FSharedState, ESPMode::ThreadSafe> State = Impl->State;
    TArray<FImageReadbackChannelStats> Result;
    {
        FScopeLock Lock(&State->ChannelStatsMutex);
        Result.Reserve(State->ChannelStats.Num());
        for (const TPair<
            ImageReadback::FChannelKey,
            ImageReadback::FChannelStatsAccumulator>& Pair : State->ChannelStats)
        {
            const ImageReadback::FChannelStatsAccumulator& Source = Pair.Value;
            FImageReadbackChannelStats& Stats = Result.AddDefaulted_GetRef();
            Stats.SensorGuid = Pair.Key.SensorGuid;
            Stats.SensorName = Pair.Key.SensorName;
            Stats.ChannelGuid = Pair.Key.ChannelGuid;
            Stats.PayloadType = Pair.Key.PayloadType;
            Stats.PendingCount = Source.PendingCount;
            Stats.PeakPendingCount = Source.PeakPendingCount;
            Stats.EnqueuedCount = Source.EnqueuedCount;
            Stats.CompletedCount = Source.CompletedCount;
            Stats.DeliveredCount = Source.DeliveredCount;
            Stats.RejectedCount = Source.RejectedCount;
            Stats.FailedCount = Source.FailedCount;
            Stats.CreatedReadbackResources = Source.CreatedReadbackResources;
            Stats.ReusedReadbackResources = Source.ReusedReadbackResources;
            Stats.AverageGpuLatencyMs = Source.CompletedCount > 0
                ? Source.TotalGpuLatencyMs / static_cast<double>(Source.CompletedCount)
                : 0.0;
            Stats.MaxGpuLatencyMs = Source.MaxGpuLatencyMs;
            Stats.AverageDeliveryLatencyMs = Source.DeliveredCount > 0
                ? Source.TotalDeliveryLatencyMs / static_cast<double>(Source.DeliveredCount)
                : 0.0;
            Stats.MaxDeliveryLatencyMs = Source.MaxDeliveryLatencyMs;
        }
    }

    // 固定顺序让日志、JSON 和自动化断言不受 TMap 哈希顺序影响。
    Result.Sort(
        [](const FImageReadbackChannelStats& A, const FImageReadbackChannelStats& B)
        {
            const int32 GuidOrder = A.SensorGuid.ToString().Compare(B.SensorGuid.ToString());
            if (GuidOrder != 0)
            {
                return GuidOrder < 0;
            }
            const int32 ChannelOrder = A.ChannelGuid.ToString().Compare(B.ChannelGuid.ToString());
            if (ChannelOrder != 0)
            {
                return ChannelOrder < 0;
            }
            return static_cast<uint8>(A.PayloadType) < static_cast<uint8>(B.PayloadType);
        });
    return Result;
}

#if WITH_DEV_AUTOMATION_TESTS
// ==================== 7. 测试辅助实现（仅测试构建） ====================

/** 自动化测试通过该快照验证全局协调器确实批量推进了多个 Manager。 */
FImageReadbackGlobalPumpStats FImageReadbackManager::GetGlobalPumpStatsForTesting()
{
    const TSharedRef<ImageReadback::FGlobalPumpState, ESPMode::ThreadSafe> Global =
        ImageReadback::GetGlobalPumpState();
    FScopeLock Lock(&Global->Mutex);
    return Global->Stats;
}
#endif
