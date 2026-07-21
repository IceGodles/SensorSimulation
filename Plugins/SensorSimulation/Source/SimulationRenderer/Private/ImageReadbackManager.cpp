#include "ImageReadbackManager.h"
#include "Engine/TextureRenderTarget2D.h"

/** 有容量上限的异步 GPU 图像读回管理器。 */
struct FImageReadbackManager::FImpl
{
/** 构造并初始化 FImpl 的默认状态。 */
    explicit FImpl(int32 InCapacity) : Capacity(FMath::Max(1, InCapacity)) {}

    /** 队列允许同时保存或等待处理的最大项目数。 */
    int32 Capacity = 4;
    /** GPU 读回完成后等待游戏线程获取的 CPU 图像队列。 */
    TQueue<FImagePayload, EQueueMode::Mpsc> Completed;
    /** 可跨线程读取的待处理或待读回任务计数。 */
    TAtomic<int32> PendingCount { 0 };
};

/** 构造并初始化 FImageReadbackManager 的默认状态。 */
FImageReadbackManager::FImageReadbackManager(int32 InCapacity)
    : Impl(MakeUnique<FImpl>(InCapacity))
{
}

/** 销毁对象并释放其持有的私有资源。 */
FImageReadbackManager::~FImageReadbackManager() = default;

/** 在容量允许时提交异步任务或数据包，失败时保持输入状态可判定。 */
bool FImageReadbackManager::Enqueue(
    UTextureRenderTarget2D* RenderTarget,
    const FCaptureRequest& Request,
    EPayloadType PayloadType)
{
    if (!RenderTarget || Impl->PendingCount.Load() >= Impl->Capacity)
    {
        return false;
    }

    // Framework boundary only. The production implementation must enqueue
    // FRHIGPUTextureReadback on the render thread and publish the owned CPU copy
    // to Impl->Completed after IsReady(). Never replace this with per-frame ReadPixels().
    return false;
}

/** 非阻塞取出一个已完成的 CPU 图像结果。 */
bool FImageReadbackManager::PollCompleted(FImagePayload& OutPayload)
{
    return Impl->Completed.Dequeue(OutPayload);
}

/** 返回当前仍在队列中等待处理的项目数。 */
int32 FImageReadbackManager::GetPendingCount() const
{
    return Impl->PendingCount.Load();
}
