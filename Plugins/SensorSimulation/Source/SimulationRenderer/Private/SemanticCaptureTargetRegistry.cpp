#include "SemanticCaptureViewExtension.h"

#include "Misc/ScopeRWLock.h"

/**
 * Semantic Capture Render Target 注册表。
 *
 * Camera Rig 在游戏线程创建或销毁 Semantic Render Target 时更新该注册表；
 * Scene View Extension 在渲染相关回调中查询当前 View 使用的 Render Target 是否属于 Semantic Channel。
 */
namespace UE::SensorSimulation::SemanticCapture
{
namespace
{
/**
 * 保护 RegisteredTargets 的读写锁。
 *
 * 注册和注销会修改集合，因此获取写锁；目标识别只读取集合，因此获取读锁。
 * 多个查询可以并行执行，但查询不会与集合修改同时发生。
 */
FRWLock RegisteredTargetsLock;

/**
 * 当前由 Semantic Channel 使用的 Render Target 地址集合。
 *
 * 集合只保存非拥有型指针，不负责创建或销毁 FRenderTarget。目标拥有者必须在
 * Render Target 生命周期结束前调用 UnregisterTarget，防止留下失效地址。
 */
TSet<const FRenderTarget*> RegisteredTargets;
}

/**
 * 将一个 Render Target 标记为 Semantic Capture 输出目标。
 *
 * @param RenderTarget 要注册的非拥有型 Render Target 指针；为空时不执行操作。
 */
void RegisterTarget(const FRenderTarget* RenderTarget)
{
    if (!RenderTarget)
    {
        return;
    }

    // 修改共享集合必须独占写锁；TSet::Add 天然去重，重复注册不会产生重复条目。
    FWriteScopeLock ScopeLock(RegisteredTargetsLock);
    RegisteredTargets.Add(RenderTarget);
}

/**
 * 从 Semantic Capture 输出目标集合中移除 Render Target。
 *
 * @param RenderTarget 要注销的非拥有型 Render Target 指针；为空时不执行操作。
 */
void UnregisterTarget(const FRenderTarget* RenderTarget)
{
    if (!RenderTarget)
    {
        return;
    }

    // 注销可能与渲染侧查询并发，因此使用写锁等待正在进行的读取完成。
    FWriteScopeLock ScopeLock(RegisteredTargetsLock);
    RegisteredTargets.Remove(RenderTarget);
}

/**
 * 判断指定 Render Target 是否属于当前注册的 Semantic Channel。
 *
 * @param RenderTarget 要查询的非拥有型 Render Target 指针。
 * @return 指针非空且存在于注册集合中时返回 true，否则返回 false。
 */
bool IsRegisteredTarget(const FRenderTarget* RenderTarget)
{
    if (!RenderTarget)
    {
        return false;
    }

    // 查询只获取共享读锁，允许多个 View/渲染查询同时执行。
    FReadScopeLock ScopeLock(RegisteredTargetsLock);
    return RegisteredTargets.Contains(RenderTarget);
}
}
