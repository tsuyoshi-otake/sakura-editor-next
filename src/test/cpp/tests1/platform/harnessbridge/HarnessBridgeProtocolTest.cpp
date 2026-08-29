#include "pch.h"

#include <sakura/harnessbridge/HarnessBridgeProtocol.h>

#include <array>

namespace platform::harnessbridge {
namespace {

HarnessBridgeFrame Frame(const EHarnessBridgeFrameKind kind, const EHarnessBridgeFrameFlags flags,
	const std::uint64_t requestId = 7, const std::uint64_t epoch = 9)
{
	return { { kHarnessBridgeMajorVersion, kHarnessBridgeMinorVersion, kind, flags, requestId, epoch }, { 0xaa, 0xbb } };
}

TEST(HarnessBridgeProtocol, EncodesAndDecodesFragmentedAndCoalescedFrames)
{
	const auto hello = EncodeHarnessBridgeFrame({ { kHarnessBridgeMajorVersion, 0,
		EHarnessBridgeFrameKind::Hello, EHarnessBridgeFrameFlags::Request, 1, 0 }, {} });
	const auto response = EncodeHarnessBridgeFrame(Frame(EHarnessBridgeFrameKind::OperationResponse,
		EHarnessBridgeFrameFlags::Response | EHarnessBridgeFrameFlags::Terminal));
	ASSERT_EQ(EHarnessBridgeEncodeOutcome::Encoded, hello.outcome);
	ASSERT_EQ(EHarnessBridgeEncodeOutcome::Encoded, response.outcome);
	std::vector<std::uint8_t> bytes = hello.bytes;
	bytes.insert(bytes.end(), response.bytes.begin(), response.bytes.end());
	CHarnessBridgeFrameDecoder decoder;
	EXPECT_EQ(EHarnessBridgeDecodeOutcome::NeedMoreData, decoder.Feed(std::span(bytes).first(3)).outcome);
	const auto result = decoder.Feed(std::span(bytes).subspan(3));
	ASSERT_EQ(EHarnessBridgeDecodeOutcome::Decoded, result.outcome);
	ASSERT_EQ(2u, result.frames.size());
	EXPECT_EQ(EHarnessBridgeFrameKind::Hello, result.frames[0].header.kind);
	EXPECT_EQ(7u, result.frames[1].header.requestId);
}

TEST(HarnessBridgeProtocol, RejectsOversizeAndStickyInvalidFrames)
{
	CHarnessBridgeFrameDecoder decoder(64);
	const std::array<std::uint8_t, 4> oversized{ 65, 0, 0, 0 };
	EXPECT_EQ(EHarnessBridgeDecodeOutcome::OversizeFrame, decoder.Feed(oversized).outcome);
	EXPECT_TRUE(decoder.IsFailed());
	EXPECT_EQ(EHarnessBridgeDecodeOutcome::MalformedFrame, decoder.Feed(std::span<const std::uint8_t>{}).outcome);
	decoder.Reset();
	EXPECT_FALSE(decoder.IsFailed());
}

TEST(HarnessBridgeProtocol, BoundsFieldsAndRejectsInvalidUtf8AndDuplicateTags)
{
	HarnessBridgeFields fields;
	EXPECT_TRUE(AddHarnessBridgeUtf8Field(fields, EHarnessBridgeFieldTag::Diagnostic, "safe"));
	EXPECT_FALSE(AddHarnessBridgeUtf8Field(fields, EHarnessBridgeFieldTag::Diagnostic, "duplicate"));
	const auto wire = EncodeHarnessBridgeFields(fields);
	ASSERT_TRUE(wire);
	const auto decoded = DecodeHarnessBridgeFields(*wire);
	ASSERT_EQ(EHarnessBridgeFieldDecodeOutcome::Decoded, decoded.outcome);
	EXPECT_EQ("safe", *GetHarnessBridgeUtf8Field(decoded.fields, EHarnessBridgeFieldTag::Diagnostic));
	HarnessBridgeFields invalid{ { static_cast<std::uint16_t>(EHarnessBridgeFieldTag::Payload), { 0xc0, 0xaf } } };
	EHarnessBridgeFieldDecodeOutcome failure = EHarnessBridgeFieldDecodeOutcome::Decoded;
	EXPECT_FALSE(GetHarnessBridgeUtf8Field(invalid, EHarnessBridgeFieldTag::Payload, &failure));
	EXPECT_EQ(EHarnessBridgeFieldDecodeOutcome::InvalidUtf8, failure);
}

} // namespace
} // namespace platform::harnessbridge
