#pragma once

#include "CoreMinimal.h"
#include "SimulationTypes.h"

class UTextureRenderTarget2D;

/** 异步 GPU 图像读回管理器：
 *  有容量上限 */
class SIMULATIONRENDERER_API FImageReadbackManager
{
public:
/** 构造并初始化 FImageReadbackManager 的默认状态。 */
    explicit FImageReadbackManager(int32 InCapacity = 4);
/** 销毁对象并释放其持有的私有资源。 */
    ~FImageReadbackManager();

/** 在容量允许时提交异步任务或数据包，失败时保持输入状态可判定。 */
    bool Enqueue(
        UTextureRenderTarget2D* RenderTarget,
        const FCaptureRequest& Request,
        EPayloadType PayloadType);

/** 非阻塞取出一个已完成的 CPU 图像结果。 */
    bool PollCompleted(FImagePayload& OutPayload);
/** 返回当前仍在队列中等待处理的项目数。 */
    int32 GetPendingCount() const;

private:
    struct FImpl;
    /** 隐藏队列、原子状态及渲染实现依赖的私有对象。 */
    TUniquePtr<FImpl> Impl;
};
