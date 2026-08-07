#include "FrameAssembler.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FFrameAssemblerDuplicateSensorNameTest,
    "SensorSimulation.Runtime.FrameAssembler.DuplicateSensorNamesUseGuid",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFrameAssemblerDuplicateSensorNameTest::RunTest(const FString& Parameters)
{
    FFrameAssembler Assembler;
    FFrameHeader Header;
    Header.FrameId = 42;
    Header.SimulationTimestampSeconds = 1.0;
    Assembler.BeginFrame(Header, EPayloadType::Rgb);

    const FGuid FirstGuid(1, 0, 0, 0);
    const FGuid SecondGuid(2, 0, 0, 0);
    const FName DuplicateName(TEXT("FrontCamera"));
    Assembler.RegisterSensor(Header.FrameId, FirstGuid, DuplicateName, EPayloadType::Rgb);
    Assembler.RegisterSensor(Header.FrameId, SecondGuid, DuplicateName, EPayloadType::Rgb);

    FImagePayload FirstImage;
    FirstImage.Header = Header;
    FirstImage.SensorName = DuplicateName;
    FirstImage.SensorGuid = FirstGuid;
    FirstImage.PayloadType = EPayloadType::Rgb;
    TestTrue(TEXT("First same-name sensor payload is accepted"), Assembler.AddImage(MoveTemp(FirstImage)));

    FFramePacket Packet;
    TestFalse(TEXT("One same-name sensor cannot complete the other sensor"), Assembler.PopCompleteFrame(Packet));

    FImagePayload SecondImage;
    SecondImage.Header = Header;
    SecondImage.SensorName = DuplicateName;
    SecondImage.SensorGuid = SecondGuid;
    SecondImage.PayloadType = EPayloadType::Rgb;
    TestTrue(TEXT("Second same-name sensor payload is accepted"), Assembler.AddImage(MoveTemp(SecondImage)));
    TestTrue(TEXT("Frame completes after both GUID identities arrive"), Assembler.PopCompleteFrame(Packet));
    TestEqual(TEXT("Both images are retained"), Packet.Images.Num(), 2);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FFrameAssemblerTerminalPolicyTest,
    "SensorSimulation.Runtime.FrameAssembler.TerminalAndLatePayloadPolicy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFrameAssemblerTerminalPolicyTest::RunTest(const FString& Parameters)
{
    const FGuid SensorGuid(7, 0, 0, 0);
    const FName SensorName(TEXT("Camera"));
    FFrameAssembler DuplicateAssembler;
    FFrameHeader DuplicateHeader;
    DuplicateHeader.FrameId = 100;
    DuplicateAssembler.BeginFrame(DuplicateHeader, EPayloadType::Rgb);
    DuplicateAssembler.RegisterSensor(100, SensorGuid, SensorName, EPayloadType::Rgb);
    FImagePayload First;
    First.Header = DuplicateHeader; First.SensorGuid = SensorGuid; First.SensorName = SensorName; First.PayloadType = EPayloadType::Rgb;
    TestTrue(TEXT("First payload is accepted"), DuplicateAssembler.AddImage(MoveTemp(First)));
    FImagePayload Duplicate;
    Duplicate.Header = DuplicateHeader; Duplicate.SensorGuid = SensorGuid; Duplicate.SensorName = SensorName; Duplicate.PayloadType = EPayloadType::Rgb;
    TestFalse(TEXT("Duplicate payload is rejected"), DuplicateAssembler.AddImage(MoveTemp(Duplicate)));
    FFramePacket Packet;
    TestTrue(TEXT("Completed frame dequeues once"), DuplicateAssembler.PopCompleteFrame(Packet));
    TestFalse(TEXT("Completed FrameId is not queued twice"), DuplicateAssembler.PopCompleteFrame(Packet));
    TestEqual(TEXT("Duplicate payload is counted"), DuplicateAssembler.GetStats().DuplicatePayloads, 1ll);

    FFrameAssembler TimeoutAssembler;
    FFrameHeader TimeoutHeader;
    TimeoutHeader.FrameId = 200; TimeoutHeader.SimulationTimestampSeconds = 1.0;
    TimeoutAssembler.BeginFrame(TimeoutHeader, EPayloadType::Rgb);
    TimeoutAssembler.RegisterSensor(200, SensorGuid, SensorName, EPayloadType::Rgb);
    TestEqual(TEXT("Frame times out"), TimeoutAssembler.PurgeTimedOutFrames(3.0, 1.0), 1);
    FImagePayload Late;
    Late.Header = TimeoutHeader; Late.SensorGuid = SensorGuid; Late.SensorName = SensorName; Late.PayloadType = EPayloadType::Rgb;
    TestFalse(TEXT("Payload after timeout is discarded"), TimeoutAssembler.AddImage(MoveTemp(Late)));
    TestEqual(TEXT("Late payload is counted"), TimeoutAssembler.GetStats().LatePayloads, 1ll);

    FFrameAssembler BusyAssembler;
    FFrameHeader BusyHeader;
    BusyHeader.FrameId = 300;
    BusyAssembler.BeginFrame(BusyHeader, EPayloadType::Rgb);
    BusyAssembler.RegisterSensor(300, SensorGuid, SensorName, EPayloadType::Rgb);
    TestTrue(TEXT("Busy immediately fails frame"), BusyAssembler.FailFrame(300, SensorGuid, SensorName, ECaptureRequestResult::Busy));
    TestEqual(TEXT("Busy frame counter"), BusyAssembler.GetStats().BusyFrames, 1ll);
    TestEqual(TEXT("Failed frame counter"), BusyAssembler.GetStats().FailedFrames, 1ll);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FFrameAssemblerSameTypeChannelsTest,
    "SensorSimulation.Runtime.FrameAssembler.SameTypeChannelsUseGuid",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** 验证同一传感器的两条 RGB 配置必须按 ChannelGuid 分别完成，不能由模态位提前完成。 */
bool FFrameAssemblerSameTypeChannelsTest::RunTest(const FString& Parameters)
{
    FFrameAssembler Assembler;
    FFrameHeader Header;
    Header.FrameId = 400;
    const FGuid SensorGuid(40, 0, 0, 0);
    const FGuid FirstChannelGuid(41, 0, 0, 0);
    const FGuid SecondChannelGuid(42, 0, 0, 0);

    TArray<FExpectedImageChannel> ExpectedChannels;
    ExpectedChannels.Add({FirstChannelGuid, EPayloadType::Rgb});
    ExpectedChannels.Add({SecondChannelGuid, EPayloadType::Rgb});
    Assembler.BeginFrame(Header, EPayloadType::Rgb);
    Assembler.RegisterSensor(
        Header.FrameId, SensorGuid, TEXT("StereoRgb"), EPayloadType::Rgb, ExpectedChannels);

    FImagePayload First;
    First.Header = Header;
    First.SensorGuid = SensorGuid;
    First.SensorName = TEXT("StereoRgb");
    First.ChannelGuid = FirstChannelGuid;
    First.PayloadType = EPayloadType::Rgb;
    TestTrue(TEXT("First RGB channel is accepted"), Assembler.AddImage(MoveTemp(First)));

    FFramePacket Complete;
    TestFalse(TEXT("One of two RGB ChannelGuids cannot complete the frame"), Assembler.PopCompleteFrame(Complete));

    FImagePayload Duplicate;
    Duplicate.Header = Header;
    Duplicate.SensorGuid = SensorGuid;
    Duplicate.SensorName = TEXT("StereoRgb");
    Duplicate.ChannelGuid = FirstChannelGuid;
    Duplicate.PayloadType = EPayloadType::Rgb;
    TestFalse(TEXT("Duplicate ChannelGuid is rejected"), Assembler.AddImage(MoveTemp(Duplicate)));

    FImagePayload Second;
    Second.Header = Header;
    Second.SensorGuid = SensorGuid;
    Second.SensorName = TEXT("StereoRgb");
    Second.ChannelGuid = SecondChannelGuid;
    Second.PayloadType = EPayloadType::Rgb;
    TestTrue(TEXT("Second RGB channel is accepted"), Assembler.AddImage(MoveTemp(Second)));
    TestTrue(TEXT("Both RGB ChannelGuids complete the frame"), Assembler.PopCompleteFrame(Complete));
    TestEqual(TEXT("Both same-type images are preserved"), Complete.Images.Num(), 2);
    TestEqual(TEXT("Duplicate is counted by ChannelGuid"), Assembler.GetStats().DuplicatePayloads, 1ll);
    return true;
}
#endif