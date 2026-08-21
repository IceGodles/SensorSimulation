#pragma once

#include "CoreMinimal.h"
#include "SimulationTypes.h"

namespace UE::SensorSimulation::LidarFormat
{
constexpr uint32 ExtendedMagic = 0x52444C53u;
constexpr uint16 ExtendedVersion = 2;
constexpr uint16 ExtendedHeaderBytes = 32;
constexpr uint32 ExtendedPointStrideBytes = 28;
constexpr uint32 ExtendedFlags = 0x7u;
constexpr uint32 SensorFluCoordinateFrame = 1u;
constexpr uint32 LittleEndianMarker = 0x01020304u;

/** 将运行时点云序列化为稳定的小端 LiDAR v2 磁盘协议。 */
SIMULATIONCORE_API void SerializeExtendedV2(const FLidarScanPayload& Scan, TArray<uint8>& OutBytes);
/** 校验 header、大小和点数，不依赖 Runtime Writer。 */
SIMULATIONCORE_API bool ValidateExtendedV2(const TArrayView<const uint8> Bytes, uint32& OutPointCount);
}
