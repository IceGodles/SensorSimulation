#pragma once

#include "CoreMinimal.h"
#include "SimulationTypes.h"

class FInstanceCaptureTarget;
class UTextureRenderTarget2D;

/**
 * 单个 FImageReadbackManager 的运行指标快照。
 *
 * 用途：验收队列背压、Readback 资源复用情况，以及 GPU Copy/映射失败路径。
 * 所有字段均为调用 GetStats() 时的线程安全快照。
 */
struct SIMULATIONRENDERER_API FImageReadbackStats
{
    /** 当前允许同时占用容量的最大任务数。 */
    int32 Capacity = 0;
    int32 PendingCount = 0;
    int32 PeakPendingCount = 0;
    int64 EnqueuedCount = 0;
    int64 CompletedCount = 0;
    int64 RejectedCount = 0;
    int64 FailedCount = 0;
    int64 CreatedReadbackResources = 0;
    int64 ReusedReadbackResources = 0;
};

/**
 * 以 SensorGuid + ChannelGuid 为键的单通道 Readback 指标快照。
 *
 * Manager 级指标用于判断总体健康状况；通道级指标用于继续定位是哪一个传感器、
 * 哪一种图像模态造成积压、失败或延迟升高。
 */
struct SIMULATIONRENDERER_API FImageReadbackChannelStats
{
    /** 稳定归属键；同名传感器的指标不会互相覆盖。 */
    FGuid SensorGuid;
    /** 人类可读显示名称。 */
    FName SensorName = NAME_None;
    /** Camera Rig 内稳定通道身份；同模态多配置的指标据此隔离。 */
    FGuid ChannelGuid;
    EPayloadType PayloadType = EPayloadType::None;
    int32 PendingCount = 0;
    int32 PeakPendingCount = 0;
    int64 EnqueuedCount = 0;
    int64 CompletedCount = 0;
    int64 DeliveredCount = 0;
    int64 RejectedCount = 0;
    int64 FailedCount = 0;
    int64 CreatedReadbackResources = 0;
    int64 ReusedReadbackResources = 0;
    double AverageGpuLatencyMs = 0.0;
    double MaxGpuLatencyMs = 0.0;
    double AverageDeliveryLatencyMs = 0.0;
    double MaxDeliveryLatencyMs = 0.0;
};

/**
 * Renderer 全局 Readback Pump 的测试诊断快照。
 *
 * 该数据用于证明多个 Manager 由同一条渲染命令批量推进，不参与生产采集决策。
 */
struct SIMULATIONRENDERER_API FImageReadbackGlobalPumpStats
{
    int32 RegisteredManagerCount = 0;
    int32 PeakRegisteredManagerCount = 0;
    int32 PeakManagersPerPump = 0;
    int64 PumpCommandCount = 0;
    int64 PumpedManagerCount = 0;
};

/**
 * 异步 GPU 图像读回管理器。
 *
 * Enqueue() 只负责提交 GPU Copy；全局 Pump 在渲染线程检查完成状态并生成 CPU Payload；
 * PollCompleted() 在消费线程非阻塞取回结果。队列容量限制用于避免 CPU 消费速度落后时
 * 无上限占用 staging/readback 资源，内部资源池则降低连续采集中的分配抖动。
 */
class SIMULATIONRENDERER_API FImageReadbackManager
{
public:
    /** 创建 Manager，并设置“GPU 处理中 + CPU 待消费”的最大任务数。 */
    explicit FImageReadbackManager(int32 InCapacity = 4);

    /**
     * 注销该 Manager，并安全释放或延后释放仍被渲染线程引用的内部状态。
     *
     * 析构函数可能从游戏线程调用；它不会直接跨线程销毁正在使用的 Readback 资源。
     */
    ~FImageReadbackManager();

    /**
     * 安全更新后续 Enqueue 使用的容量上限。
     *
     * 缩容不会取消已经提交的任务；当 PendingCount 高于新容量时，仅拒绝新任务，
     * 直到已有结果被消费并降到限制以下。
     */
    void SetCapacity(int32 InCapacity);

    /** 返回当前生效的容量上限。 */
    int32 GetCapacity() const;

    /**
     * 把 Render Target 的当前内容提交为一次异步 GPU Texture Readback。
     *
     * @param RenderTarget 游戏线程持有的采集目标；函数内部提取其底层 FTextureRHIRef。
     * @param Request      随结果传递的传感器名、帧号、时间戳和 ViewRect 等元数据。
     * @param PayloadType  输出模态；当前读回路径支持 RGB、Semantic 和 Depth。
     * @return true 表示任务已成功排队；不表示像素已经复制到 CPU。
     */
    bool Enqueue(
        UTextureRenderTarget2D* RenderTarget,
        const FCaptureRequest& Request,
        const FGuid& ChannelGuid,
        EPayloadType PayloadType);

    /**
     * 把 Renderer 原生 PF_R32_UINT Instance 目标提交到同一异步 Readback 队列。
     *
     * SharedPtr 会被渲染命令捕获，确保热更新或组件注销不会让在途 Copy 引用悬空。
     */
    bool Enqueue(
        const TSharedPtr<FInstanceCaptureTarget, ESPMode::ThreadSafe>& RenderTarget,
        const FCaptureRequest& Request,
        const FGuid& ChannelGuid,
        EPayloadType PayloadType);

    /**
     * 非阻塞取出一个已经完成的 CPU 图像结果。
     *
     * @param OutPayload 成功时接收规范 RGBA 字节及对应采集元数据。
     * @return 有可消费结果时返回 true；队列暂时为空时返回 false。
     */
    bool PollCompleted(FImagePayload& OutPayload);

    /**
     * 返回仍占用容量的任务数。
     *
     * 该计数同时包含 GPU 中的 Pending 任务，以及 Completed 队列中尚未被消费的任务。
     */
    int32 GetPendingCount() const;

    /** 返回该 Manager 的线程安全汇总指标快照。 */
    FImageReadbackStats GetStats() const;

    /** 返回按 SensorGuid + ChannelGuid 分组的线程安全指标快照。 */
    TArray<FImageReadbackChannelStats> GetChannelStats() const;

private:
    struct FImpl;

    /** 隐藏渲染依赖、跨线程队列、原子指标和 Readback 资源池。 */
    TUniquePtr<FImpl> Impl;

#if WITH_DEV_AUTOMATION_TESTS
public:
    // -------------------- 测试辅助接口 --------------------

    /**
     * 返回所有 Manager 共享的全局 Pump 指标。
     *
     * 仅自动化测试用于验证“每轮只提交一条 Pump 命令并批量处理多个 Manager”；
     * 条件编译可避免测试诊断入口成为 Shipping/生产运行时 API。
     */
    static FImageReadbackGlobalPumpStats GetGlobalPumpStatsForTesting();
#endif
};
