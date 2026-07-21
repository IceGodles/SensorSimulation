#include "CameraChannel.h"

/** 将相机通道类型映射为帧数据模态位。 */
EPayloadType FCameraChannelConfig::ToPayloadType() const
{
    switch (ChannelType)
    {
    case ECameraChannelType::Rgb: return EPayloadType::Rgb;
    case ECameraChannelType::Semantic: return EPayloadType::Semantic;
    case ECameraChannelType::Depth: return EPayloadType::Depth;
    case ECameraChannelType::Instance: return EPayloadType::Instance;
    default: return EPayloadType::None;
    }
}
