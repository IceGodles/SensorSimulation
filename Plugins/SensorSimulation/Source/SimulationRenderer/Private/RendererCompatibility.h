#pragma once

#include "Runtime/Launch/Resources/Version.h"

#if ENGINE_MAJOR_VERSION != 5 || ENGINE_MINOR_VERSION != 7 || ENGINE_PATCH_VERSION != 2
#error "SensorSimulation R15 Renderer integration supports only UE 5.7.2. Rebase and validate Tools/EnginePatches before changing this guard."
#endif

#include "PostProcess/PostProcessing.h"

namespace SensorSimulation::RendererCompatibility
{
/**
 * 获取当前后处理调用栈借出的 Renderer 上下文。
 *
 * 这个包装函数把定制引擎 API 限制在单一兼容层内，并让缺少成员或签名漂移在编译阶段立即失败。
 * 返回指针只在当前 View Extension 回调期间有效，禁止保存到下一帧。
 */
inline const UE::Renderer::PostProcess::FExtensionContext* GetPostProcessContext_RenderThread()
{
    const UE::Renderer::PostProcess::FExtensionContext* Context =
        UE::Renderer::PostProcess::GetExtensionContext_RenderThread();

    if (Context)
    {
        // 显式类型检查会在引擎补丁成员被删除或改型时产生局部、可定位的编译错误。
        FInstanceCullingManager* InstanceCullingManager = Context->InstanceCullingManager;
        const Nanite::FRasterResults* NaniteRasterResults = Context->NaniteRasterResults;
        static_cast<void>(InstanceCullingManager);
        static_cast<void>(NaniteRasterResults);
    }

    return Context;
}
}
