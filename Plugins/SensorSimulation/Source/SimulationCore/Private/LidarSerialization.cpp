#include "LidarSerialization.h"

namespace UE::SensorSimulation::LidarFormat
{
namespace
{
void Append(TArray<uint8>& Buffer, const void* Data, const int32 Bytes)
{
    Buffer.Append(static_cast<const uint8*>(Data), Bytes);
}
}

void SerializeExtendedV2(const FLidarScanPayload& Scan, TArray<uint8>& OutBytes)
{
    OutBytes.Reset(ExtendedHeaderBytes + Scan.Points.Num() * ExtendedPointStrideBytes);
    const uint32 PointCount = static_cast<uint32>(Scan.Points.Num());
    const uint32 Reserved = 0;
    Append(OutBytes, &ExtendedMagic, sizeof(ExtendedMagic));
    Append(OutBytes, &ExtendedVersion, sizeof(ExtendedVersion));
    Append(OutBytes, &ExtendedHeaderBytes, sizeof(ExtendedHeaderBytes));
    Append(OutBytes, &ExtendedPointStrideBytes, sizeof(ExtendedPointStrideBytes));
    Append(OutBytes, &PointCount, sizeof(PointCount));
    Append(OutBytes, &ExtendedFlags, sizeof(ExtendedFlags));
    Append(OutBytes, &SensorFluCoordinateFrame, sizeof(SensorFluCoordinateFrame));
    Append(OutBytes, &LittleEndianMarker, sizeof(LittleEndianMarker));
    Append(OutBytes, &Reserved, sizeof(Reserved));
    for (const FLidarPoint& Point : Scan.Points)
    {
        const uint16 PointReserved = 0;
        Append(OutBytes, &Point.PositionMeters.X, sizeof(float));
        Append(OutBytes, &Point.PositionMeters.Y, sizeof(float));
        Append(OutBytes, &Point.PositionMeters.Z, sizeof(float));
        Append(OutBytes, &Point.Intensity, sizeof(float));
        Append(OutBytes, &Point.SemanticId, sizeof(uint16));
        Append(OutBytes, &PointReserved, sizeof(uint16));
        Append(OutBytes, &Point.InstanceId, sizeof(uint32));
        Append(OutBytes, &Point.RelativeTimeSeconds, sizeof(float));
    }
}

bool ValidateExtendedV2(const TArrayView<const uint8> Bytes, uint32& OutPointCount)
{
    OutPointCount = 0;
    if (Bytes.Num() < ExtendedHeaderBytes) return false;
    uint32 Magic = 0, Stride = 0, Flags = 0, Coordinate = 0, Endian = 0, Reserved = 1;
    uint16 Version = 0, HeaderBytes = 0;
    FMemory::Memcpy(&Magic, Bytes.GetData(), 4);
    FMemory::Memcpy(&Version, Bytes.GetData() + 4, 2);
    FMemory::Memcpy(&HeaderBytes, Bytes.GetData() + 6, 2);
    FMemory::Memcpy(&Stride, Bytes.GetData() + 8, 4);
    FMemory::Memcpy(&OutPointCount, Bytes.GetData() + 12, 4);
    FMemory::Memcpy(&Flags, Bytes.GetData() + 16, 4);
    FMemory::Memcpy(&Coordinate, Bytes.GetData() + 20, 4);
    FMemory::Memcpy(&Endian, Bytes.GetData() + 24, 4);
    FMemory::Memcpy(&Reserved, Bytes.GetData() + 28, 4);
    return Magic == ExtendedMagic && Version == ExtendedVersion
        && HeaderBytes == ExtendedHeaderBytes && Stride == ExtendedPointStrideBytes
        && Flags == ExtendedFlags && Coordinate == SensorFluCoordinateFrame
        && Endian == LittleEndianMarker && Reserved == 0
        && Bytes.Num() == static_cast<int64>(HeaderBytes) + static_cast<int64>(OutPointCount) * Stride;
}
}
