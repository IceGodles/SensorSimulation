#pragma once

#include "CoreMinimal.h"
#include "SimulationTypes.h"

/** 导出队列满载时采用的背压策略。 */
enum class EExportBackpressurePolicy : uint8
{
    RejectNewest,
    DropOldest,
    BlockDatasetClock
};

/** 接收完整帧并异步导出数据集的有界队列服务。 */
class SIMULATIONRUNTIME_API FExportService
{
public:
/** 构造并初始化 FExportService 的默认状态。 */
    explicit FExportService(int32 InCapacity = 8);
/** 销毁对象并释放其持有的私有资源。 */
    ~FExportService();

/** 设置数据集根目录并启用导出服务。 */
    bool Start(const FString& InDatasetRoot);
/** 停止接受新的导出数据包。 */
    void Stop();
/** 在容量允许时提交异步任务或数据包，失败时保持输入状态可判定。 */
    bool Enqueue(FFramePacket&& Packet, EExportBackpressurePolicy Policy);
/** 返回当前仍在队列中等待处理的项目数。 */
    int32 GetPendingCount() const;

private:
    struct FImpl;
    /** 隐藏队列、原子状态及渲染实现依赖的私有对象。 */
    TUniquePtr<FImpl> Impl;
};
