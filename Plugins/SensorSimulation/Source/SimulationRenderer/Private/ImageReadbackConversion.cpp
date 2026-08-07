#include "ImageReadbackConversion.h"

namespace UE::SensorSimulation::ImageReadback
{
bool CopyRgba8ToCanonical(
    const uint8* Source,
    const int32 RowPitchInPixels,
    const EPixelFormat SourceFormat,
    const FIntPoint ImageSize,
    TArray<uint8>& OutBytes)
{
    constexpr int32 BytesPerPixel = 4;
    const bool bSupportedFormat =
        SourceFormat == PF_B8G8R8A8 ||
        SourceFormat == PF_R8G8B8A8;

    if (!Source ||
        !bSupportedFormat ||
        ImageSize.X <= 0 ||
        ImageSize.Y <= 0 ||
        RowPitchInPixels < ImageSize.X)
    {
        OutBytes.Reset();
        return false;
    }

    const int64 ByteCount = static_cast<int64>(ImageSize.X) * ImageSize.Y * BytesPerPixel;
    if (ByteCount > MAX_int32)
    {
        OutBytes.Reset();
        return false;
    }

    OutBytes.SetNumUninitialized(static_cast<int32>(ByteCount));
    for (int32 Y = 0; Y < ImageSize.Y; ++Y)
    {
        const uint8* SourceRow =
            Source + static_cast<int64>(Y) * RowPitchInPixels * BytesPerPixel;
        uint8* DestinationRow =
            OutBytes.GetData() + static_cast<int64>(Y) * ImageSize.X * BytesPerPixel;

        if (SourceFormat == PF_R8G8B8A8)
        {
            FMemory::Memcpy(DestinationRow, SourceRow, ImageSize.X * BytesPerPixel);
            continue;
        }

        // RHI 常把 RTF_RGBA8 落为 BGRA8；逐像素交换 R/B，保证协议始终按 RGBA 解读。
        // UE/D3D 常见的 PF_B8G8R8A8 必须交换 R/B，CPU Payload 才始终具有 RGBA 语义。
        for (int32 X = 0; X < ImageSize.X; ++X)
        {
            const uint8* SourcePixel = SourceRow + X * BytesPerPixel;
            uint8* DestinationPixel = DestinationRow + X * BytesPerPixel;
            DestinationPixel[0] = SourcePixel[2];
            DestinationPixel[1] = SourcePixel[1];
            DestinationPixel[2] = SourcePixel[0];
            DestinationPixel[3] = SourcePixel[3];
        }
    }

    return true;
}

bool CopyDepthR32FloatToMeters(
    const uint8* Source,
    const int32 RowPitchInPixels,
    const EPixelFormat SourceFormat,
    const FIntPoint ImageSize,
    TArray<uint8>& OutBytes)
{
    constexpr int32 OutputBytesPerPixel = sizeof(float);
    constexpr float UnrealCentimetersToMeters = 0.01f;
    const bool bSupportedFormat =
        SourceFormat == PF_R32_FLOAT ||
        SourceFormat == PF_A32B32G32R32F;
    const int32 SourceBytesPerPixel =
        SourceFormat == PF_A32B32G32R32F ? 4 * sizeof(float) : sizeof(float);
    if (!Source ||
        !bSupportedFormat ||
        ImageSize.X <= 0 ||
        ImageSize.Y <= 0 ||
        RowPitchInPixels < ImageSize.X)
    {
        OutBytes.Reset();
        return false;
    }

    const int64 ByteCount = static_cast<int64>(ImageSize.X) * ImageSize.Y * OutputBytesPerPixel;
    if (ByteCount > MAX_int32)
    {
        OutBytes.Reset();
        return false;
    }

    OutBytes.SetNumUninitialized(static_cast<int32>(ByteCount));
    for (int32 Y = 0; Y < ImageSize.Y; ++Y)
    {
        const uint8* SourceRow =
            Source + static_cast<int64>(Y) * RowPitchInPixels * SourceBytesPerPixel;
        uint8* DestinationRow =
            OutBytes.GetData() + static_cast<int64>(Y) * ImageSize.X * OutputBytesPerPixel;

        for (int32 X = 0; X < ImageSize.X; ++X)
        {
            float DepthCentimeters = 0.0f;
            // SCS_SceneDepth 的定义是把深度写入逻辑 R；RGBA32F 输入的其余三个 float 不进入协议。
            FMemory::Memcpy(
                &DepthCentimeters,
                SourceRow + X * SourceBytesPerPixel,
                sizeof(float));
            const float DepthMeters = DepthCentimeters * UnrealCentimetersToMeters;
            FMemory::Memcpy(DestinationRow + X * OutputBytesPerPixel, &DepthMeters, OutputBytesPerPixel);
        }
    }

    return true;
}

bool CopyR32UintToCanonical(
    const uint8* Source,
    const int32 RowPitchInPixels,
    const EPixelFormat SourceFormat,
    const FIntPoint ImageSize,
    TArray<uint8>& OutBytes)
{
    constexpr int32 BytesPerPixel = sizeof(uint32);
    if (!Source ||
        SourceFormat != PF_R32_UINT ||
        ImageSize.X <= 0 ||
        ImageSize.Y <= 0 ||
        RowPitchInPixels < ImageSize.X)
    {
        OutBytes.Reset();
        return false;
    }

    const int64 ByteCount = static_cast<int64>(ImageSize.X) * ImageSize.Y * BytesPerPixel;
    if (ByteCount > MAX_int32)
    {
        OutBytes.Reset();
        return false;
    }

    OutBytes.SetNumUninitialized(static_cast<int32>(ByteCount));
    const int32 TightRowBytes = ImageSize.X * BytesPerPixel;
    for (int32 Y = 0; Y < ImageSize.Y; ++Y)
    {
        const uint8* SourceRow =
            Source + static_cast<int64>(Y) * RowPitchInPixels * BytesPerPixel;
        uint8* DestinationRow =
            OutBytes.GetData() + static_cast<int64>(Y) * TightRowBytes;
        FMemory::Memcpy(DestinationRow, SourceRow, TightRowBytes);
    }
    return true;
}
}
