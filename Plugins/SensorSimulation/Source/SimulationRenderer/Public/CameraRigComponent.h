#pragma once

#include "Components/SceneComponent.h"
#include "CameraChannel.h"
#include "SimulationTypes.h"
#include "ImageReadbackManager.h"
#include "PixelFormat.h"
#include "CameraRigComponent.generated.h"

class FInstanceCaptureTarget;
class UPrimitiveComponent;
class USceneCaptureComponent2D;
class UTextureRenderTarget2D;

DECLARE_MULTICAST_DELEGATE_OneParam(FOnCaptureSubmitted, const FCaptureRequest&);
DECLARE_MULTICAST_DELEGATE(FOnCameraRigConfigurationChanged);

/** Camera Rig 运行期间的 Capture/RenderTarget 创建、复用与重建指标。 */
struct SIMULATIONRENDERER_API FCameraRigResourceStats
{
    int64 ConfigurationApplyCount = 0;
    int64 ConfigurationChangeCount = 0;
    int64 NoOpConfigurationApplyCount = 0;
    int64 CreatedCaptureComponents = 0;
    int64 ReusedCaptureComponents = 0;
    int64 DestroyedCaptureComponents = 0;
    int64 CreatedRenderTargets = 0;
    int64 ReusedRenderTargets = 0;
    int64 RebuiltRenderTargets = 0;
    int64 DestroyedRenderTargets = 0;
};

UCLASS(ClassGroup=(SensorSimulation), meta=(BlueprintSpawnableComponent))
/** 管理多模态场景捕获组件与相机标定的传感器阵列组件。
 *  UCameraRigComponent 相机成像执行层：
 *  1.相机安装位置
 *  2.管理 RGB/Semantic 等多个 Scene Capture 和 Render Target，
 *    收到采集请求后触发 GPU 渲染，再异步生成可供 CPU 使用的图像数据。*/
class SIMULATIONRENDERER_API UCameraRigComponent : public USceneComponent
{
    GENERATED_BODY()

public:
    /** 构造并初始化 UCameraRigComponent 的默认状态。 */
    UCameraRigComponent();

    /** 传感器的稳定名称，用于区分同一帧内的多个数据源。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Sensor")
    FName SensorName = TEXT("FrontCamera");

    /** 相机的水平视场角，单位为度。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Sensor", meta=(ClampMin="1.0", ClampMax="170.0"))
    float HorizontalFovDegrees = 90.0f;

    /** GPU 读回队列允许同时存在的捕获任务上限，用于施加背压。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Sensor", meta=(ClampMin="1"))
    int32 MaxPendingReadbacks = 8;

    /** 相机阵列中需要创建的输出通道配置。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Sensor")
    TArray<FCameraChannelConfig> Channels;

    /** 组件注册后根据配置创建运行时捕获通道。 */
    virtual void OnRegister() override;
    /** 组件注销前销毁动态创建的捕获通道。 */
    virtual void OnUnregister() override;

    /** 按下快门 触发所有通道延迟采集并广播本次请求。 */
    ECaptureRequestResult SubmitCapture(const FCaptureRequest& Request);

    /** 非阻塞取出一个已完成的 RGB、Semantic 或 Depth CPU 图像载荷。 */
    bool PollCompletedImage(FImagePayload& OutPayload);
    /** 返回当前启用且已支持正式读回的图像模态位集合。 */
    EPayloadType GetEnabledPayloadTypes() const;
    /** 返回当前启用通道的稳定 ChannelGuid 与模态，供请求和 FrameAssembler 精确登记。 */
    TArray<FExpectedImageChannel> GetEnabledImageChannels() const;
    /** 返回该 Camera Rig 全部图像通道的 Readback 聚合指标。 */
    FImageReadbackStats GetImageReadbackStats() const;
    /** 返回按 SensorGuid + ChannelGuid 分组的 Readback 指标。 */
    TArray<FImageReadbackChannelStats> GetImageReadbackChannelStats() const;
    /** 返回 Capture/RenderTarget 的创建、复用和选择性重建指标。 */
    FCameraRigResourceStats GetResourceStats() const { return ResourceStats; }

    /** 比较编辑器/蓝图中的最新配置与当前运行时资源，只重建真正受影响的 Render Target，尽量复用已有的 Capture 和 RT。 */
    UFUNCTION(BlueprintCallable, Category="Sensor")
    bool ApplyConfiguration();

    /** 立即执行所有有效通道的单帧采集，供编辑器人工检查。 */
    UFUNCTION(CallInEditor, BlueprintCallable, Category="Sensor|Debug", meta=(DisplayName="Capture Debug Frame"))
    void CaptureDebugFrame();

    /** 采集 Semantic 通道并把线性 RGBA8 结果保存到 Saved/SensorSimulation/Debug。 */
    UFUNCTION(CallInEditor, BlueprintCallable, Category="Sensor|Debug", meta=(DisplayName="Save Semantic Debug Image"))
    void SaveSemanticDebugImage();

    /** 采集 RGB 通道并保存为 PNG，供颜色和通道顺序验收。 */
    UFUNCTION(CallInEditor, BlueprintCallable, Category="Sensor|Debug", meta=(DisplayName="Save RGB Debug Image"))
    void SaveRgbDebugImage();

    /** 采集 R32F SceneDepth 并保存为 EXR，供浮点深度人工检查。 */
    UFUNCTION(CallInEditor, BlueprintCallable, Category="Sensor|Debug", meta=(DisplayName="Save Depth Debug Image"))
    void SaveDepthDebugImage();

    /** 按稳定 ChannelGuid 返回瞬态 Render Target；通道未创建或不是 UObject Target 时返回空。 */
    UFUNCTION(BlueprintPure, Category="Sensor|Debug")
    UTextureRenderTarget2D* GetChannelRenderTarget(FGuid ChannelGuid) const;

    /** 按稳定 ChannelGuid 返回正式输出像素格式；Instance 返回原生 PF_R32_UINT。 */
    EPixelFormat GetChannelPixelFormat(const FGuid& ChannelGuid) const;

    /** 最近一次成功保存的 Semantic 调试 PNG 绝对路径。 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Sensor|Debug")
    FString LastSemanticDebugImagePath;
    /** 最近一次成功保存的 RGB 调试 PNG 绝对路径。 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Sensor|Debug")
    FString LastRgbDebugImagePath;
    /** 最近一次成功保存的 Depth 调试 EXR 绝对路径。 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Sensor|Debug")
    FString LastDepthDebugImagePath;
    /** 由分辨率、水平视场角和持久 ChannelGuid 计算单通道针孔相机内参。 */
    FCalibration BuildCalibration(const FCameraChannelConfig& Channel) const;
    /** 返回当前真正创建的全部正式图像通道标定；跳过禁用和重复配置。 */
    TArray<FCalibration> BuildActiveCalibrations() const;

    /** 返回采集请求已提交事件，供调用方绑定监听器。 */
    FOnCaptureSubmitted& OnCaptureSubmitted() { return CaptureSubmittedDelegate; }
    /** 配置真正改变并完成资源更新后触发，Runtime 用它同步相机标定。 */
    FOnCameraRigConfigurationChanged& OnConfigurationChanged() { return ConfigurationChangedDelegate; }

#if WITH_EDITOR
    /** 编辑器属性变化后立即应用配置，避免必须重新注册组件才能看到结果。 */
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
    /** 一个已创建通道的配置、捕获组件和渲染目标。 */
    struct FChannelRuntime
    {
        /** 此运行时对象对应的可编辑通道或雷达配置。 */
        FCameraChannelConfig Config;
        /** 实际执行场景渲染的动态场景捕获组件。 */
        TObjectPtr<USceneCaptureComponent2D> Capture = nullptr;
        /** 接收 RGB/Semantic/Depth 正式输出的 UObject 渲染目标。 */
        TObjectPtr<UTextureRenderTarget2D> Target = nullptr;
        /**
         * Instance 专用的普通颜色工作目标。
         *
         * 它只负责让 SceneCapture 建立 View/SceneDepth，正式 InstanceId 写入 Target。
         */
        TObjectPtr<UTextureRenderTarget2D> CaptureTarget = nullptr;
        /** Instance 正式输出；原生 PF_R32_UINT，不受 UTextureRenderTarget2D 格式白名单限制。 */
        TSharedPtr<FInstanceCaptureTarget, ESPMode::ThreadSafe> InstanceTarget;
        /** 上一次为 RGB/Depth 临时隐藏的标签代理，用于下次提交前精确刷新。 */
        TArray<TWeakObjectPtr<UPrimitiveComponent>> HiddenLabelProxies;
    };

    /** 根据有效配置动态创建的全部运行时捕获通道。 */
    TArray<FChannelRuntime> RuntimeChannels;
    /** 采集命令提交后向外部监听器广播的多播委托。 */
    FOnCaptureSubmitted CaptureSubmittedDelegate;
    /** 资源与参数热更新完成后通知依赖标定信息的上层。 */
    FOnCameraRigConfigurationChanged ConfigurationChangedDelegate;
    /** 管理 RGB/Semantic/Depth/Instance 的异步 GPU staging copy 与 CPU 完成队列。 */
    TUniquePtr<FImageReadbackManager> ImageReadbackManager;
    /** 用指标区分真正的复用与隐藏的全量重建。 */
    FCameraRigResourceStats ResourceStats;
    /** 等待既有 GPU Readback 排空后再释放的旧 Target，防止热更新制造悬空 RHI 引用。 */
    UPROPERTY(Transient)
    TArray<TObjectPtr<UTextureRenderTarget2D>> RetiredTargets;
    /** RetiredTargets 中仍需延迟从 Semantic 注册表注销的目标。 */
    UPROPERTY(Transient)
    TArray<TObjectPtr<UTextureRenderTarget2D>> RetiredSemanticTargets;
    /** RetiredTargets 中仍需延迟从 Instance 目标对注册表注销的工作目标。 */
    UPROPERTY(Transient)
    TArray<TObjectPtr<UTextureRenderTarget2D>> RetiredInstanceCaptureTargets;
    /** 等待在途 Capture/Readback 排空后释放的原生整数输出资源。 */
    TArray<TSharedPtr<FInstanceCaptureTarget, ESPMode::ThreadSafe>> RetiredInstanceTargets;
    /** 最近一次已应用的全局配置，用于识别无需触碰 GPU 资源的空操作。 */
    FName AppliedSensorName = NAME_None;
    float AppliedHorizontalFovDegrees = 0.0f;
    /** 最近一次应用到 Readback Manager 的容量，用于识别仅容量发生变化的配置。 */
    int32 AppliedMaxPendingReadbacks = 0;
    bool bHasAppliedConfiguration = false;

    /** 为缺失或重复的通道身份生成新的 GUID；编辑器关卡中的迁移结果会被标记为待保存。 */
    void EnsureChannelGuids();
    /** 为每个有效配置创建场景捕获组件和匹配的渲染目标。 */
    void BuildChannels();
    /** 销毁所有动态捕获组件并清空运行时通道。 */
    void DestroyChannels();
    /** 销毁单个运行时通道，并按正确顺序注销 Semantic Target。 */
    void DestroyChannel(FChannelRuntime& Channel);
    /** 将旧 Target 延迟到 Readback 排空后释放，避免热更新破坏在途命令。 */
    void RetireTarget(TObjectPtr<UTextureRenderTarget2D>& Target, bool bSemanticTarget);
    /** 按通道类型注销并退休正式输出及 Instance 工作目标。 */
    void RetireChannelTargets(FChannelRuntime& Channel);
    /** 在没有 Pending Readback 时注销并释放全部退休 Target。 */
    void ReleaseRetiredTargetsIfSafe();
    /** 创建一个尚不存在的 Capture/RenderTarget 通道。 */
    void CreateChannel(FChannelRuntime& Channel, const FCameraChannelConfig& Config);
    /** 判断当前运行时状态是否已经与可编辑配置完全一致。 */
    bool IsConfigurationCurrent() const;
    /** 刷新当前通道对标签代理的可见性：RGB/Depth 隐藏，Semantic/Instance 保留。 */
    void RefreshOpaqueLabelProxyVisibility(FChannelRuntime& Channel);
    /** 根据 RGB、标签或深度模态配置场景捕获参数。 */
    void ConfigureCapture(FChannelRuntime& Channel);
    /** 仅供按模态触发的人工调试入口选择第一条通道；正式资源查询始终使用 ChannelGuid。 */
    FGuid FindFirstChannelGuid(ECameraChannelType ChannelType) const;
};
