#include "ExportService.h"

/** 接收完整帧并异步导出数据集的有界队列服务。 */
struct FExportService::FImpl
{
/** 构造并初始化 FImpl 的默认状态。 */
    explicit FImpl(int32 InCapacity) : Capacity(FMath::Max(1, InCapacity)) {}

    /** 队列允许同时保存或等待处理的最大项目数。 */
    int32 Capacity = 8;
    /** 数据集文件的输出根目录。 */
    FString DatasetRoot;
    /** 等待后台导出处理的完整帧多生产者单消费者队列。 */
    TQueue<FFramePacket, EQueueMode::Mpsc> Pending;
    /** 可跨线程读取的待处理或待读回任务计数。 */
    TAtomic<int32> PendingCount { 0 };
    /** 标识导出服务当前是否接受新的数据包。 */
    TAtomic<bool> bRunning { false };
};

/** 构造并初始化 FExportService 的默认状态。 */
FExportService::FExportService(int32 InCapacity)
    : Impl(MakeUnique<FImpl>(InCapacity))
{
}

/** 销毁对象并释放其持有的私有资源。 */
FExportService::~FExportService()
{
    Stop();
}

/** 设置数据集根目录并启用导出服务。 */
bool FExportService::Start(const FString& InDatasetRoot)
{
    Impl->DatasetRoot = InDatasetRoot;
    Impl->bRunning.Store(true);
    return true;
}

/** 停止接受新的导出数据包。 */
void FExportService::Stop()
{
    Impl->bRunning.Store(false);
}

/** 在容量允许时提交异步任务或数据包，失败时保持输入状态可判定。 */
bool FExportService::Enqueue(FFramePacket&& Packet, EExportBackpressurePolicy Policy)
{
    if (!Impl->bRunning.Load() || !Packet.IsComplete() || Impl->PendingCount.Load() >= Impl->Capacity)
    {
        return false;
    }
    Impl->Pending.Enqueue(MoveTemp(Packet));
    ++Impl->PendingCount;
    return true;
}

/** 返回当前仍在队列中等待处理的项目数。 */
int32 FExportService::GetPendingCount() const
{
    return Impl->PendingCount.Load();
}
