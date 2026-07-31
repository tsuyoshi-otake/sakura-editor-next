/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "platform/controlipc/ControlIpcProtocol.h"

#include <array>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace platform::controlipc {
namespace {

ControlIpcFrame MakeFrame(EControlIpcKind kind = EControlIpcKind::StorageSnapshotRequest,
	EControlIpcFlags flags = EControlIpcFlags::Request, std::uint64_t requestId = 41,
	std::uint64_t generation = 7, std::vector<std::uint8_t> payload = {})
{
	return { { kControlIpcMajorVersion, kControlIpcMinorVersion, kind, flags, requestId, generation },
		std::move(payload) };
}

std::vector<std::uint8_t> Encode(const ControlIpcFrame& frame)
{
	auto encoded = EncodeControlIpcFrame(frame);
	EXPECT_EQ(EControlIpcEncodeOutcome::Encoded, encoded.outcome);
	return encoded.bytes;
}

void SetLe16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value)
{
	bytes[offset] = static_cast<std::uint8_t>(value);
	bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void SetLe32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
{
	for (std::size_t index = 0; index < 4; ++index) bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8));
}

TEST(ControlIpcProtocol, EncodesVersionedLittleEndianHeaderAndDecodesFragmentedInput)
{
	const auto input = MakeFrame(EControlIpcKind::StorageApplyRequest, EControlIpcFlags::Request, 0x0102030405060708ULL,
		9, { 0xaa, 0xbb });
	const auto encoded = Encode(input);

	ASSERT_EQ(kControlIpcLengthPrefixBytes + kControlIpcHeaderBytes + 2, encoded.size());
	EXPECT_EQ(kControlIpcHeaderBytes + 2, encoded[0]);
	EXPECT_EQ(0, encoded[1]);
	EXPECT_EQ('S', encoded[4]);
	EXPECT_EQ('C', encoded[5]);
	EXPECT_EQ('I', encoded[6]);
	EXPECT_EQ('P', encoded[7]);

	CControlIpcFrameDecoder decoder;
	EXPECT_EQ(EControlIpcDecodeOutcome::NeedMoreData, decoder.Feed(std::span(encoded).first(3)).outcome);
	EXPECT_EQ(EControlIpcDecodeOutcome::NeedMoreData, decoder.Feed(std::span(encoded).subspan(3, 11)).outcome);
	const auto decoded = decoder.Feed(std::span(encoded).subspan(14));
	ASSERT_EQ(EControlIpcDecodeOutcome::Decoded, decoded.outcome);
	ASSERT_EQ(1u, decoded.frames.size());
	EXPECT_EQ(input.header.requestId, decoded.frames[0].header.requestId);
	EXPECT_EQ(input.header.generation, decoded.frames[0].header.generation);
	EXPECT_EQ(input.payload, decoded.frames[0].payload);
}

TEST(ControlIpcProtocol, DecodesCoalescedFramesWithoutLosingBoundary)
{
	const auto first = Encode(MakeFrame(EControlIpcKind::Hello, EControlIpcFlags::Request, 1));
	const auto second = Encode(MakeFrame(EControlIpcKind::CancelAck,
		EControlIpcFlags::Response | EControlIpcFlags::Terminal, 2));
	std::vector<std::uint8_t> coalesced = first;
	coalesced.insert(coalesced.end(), second.begin(), second.end());

	CControlIpcFrameDecoder decoder;
	const auto decoded = decoder.Feed(coalesced);
	ASSERT_EQ(EControlIpcDecodeOutcome::Decoded, decoded.outcome);
	ASSERT_EQ(2u, decoded.frames.size());
	EXPECT_EQ(EControlIpcKind::Hello, decoded.frames[0].header.kind);
	EXPECT_EQ(EControlIpcKind::CancelAck, decoded.frames[1].header.kind);
	EXPECT_EQ(2u, decoded.frames[1].header.requestId);
}

TEST(ControlIpcProtocol, InitialHelloUsesUnknownGenerationButLaterRequestsDoNot)
{
	const auto hello = Encode(MakeFrame(EControlIpcKind::Hello, EControlIpcFlags::Request, 1, 0));
	CControlIpcFrameDecoder decoder;
	const auto decoded = decoder.Feed(hello);
	ASSERT_EQ(EControlIpcDecodeOutcome::Decoded, decoded.outcome);
	ASSERT_EQ(1u, decoded.frames.size());
	EXPECT_EQ(0u, decoded.frames[0].header.generation);

	auto later = Encode(MakeFrame());
	for (std::size_t index = 24; index < 32; ++index) later[index] = 0;
	CControlIpcFrameDecoder staleDecoder;
	EXPECT_EQ(EControlIpcDecodeOutcome::InvalidGeneration, staleDecoder.Feed(later).outcome);
}

TEST(ControlIpcProtocol, SameMajorHigherMinorRemainsForwardCompatible)
{
	auto frame = MakeFrame();
	frame.header.minorVersion = kControlIpcMinorVersion + 7;
	const auto encoded = Encode(frame);
	CControlIpcFrameDecoder decoder;
	const auto decoded = decoder.Feed(encoded);
	ASSERT_EQ(EControlIpcDecodeOutcome::Decoded, decoded.outcome);
	ASSERT_EQ(1u, decoded.frames.size());
	EXPECT_EQ(kControlIpcMinorVersion + 7, decoded.frames[0].header.minorVersion);
}

TEST(ControlIpcProtocol, RejectsZeroAndOversizedLengthBeforeFrameAllocation)
{
	CControlIpcFrameDecoder zero;
	const std::array<std::uint8_t, 4> zeroPrefix{};
	EXPECT_EQ(EControlIpcDecodeOutcome::ZeroLengthFrame, zero.Feed(zeroPrefix).outcome);
	EXPECT_TRUE(zero.IsFailed());
	EXPECT_EQ(EControlIpcDecodeOutcome::ZeroLengthFrame, zero.Feed(std::span<const std::uint8_t>{}).outcome);

	CControlIpcFrameDecoder oversize(64);
	std::array<std::uint8_t, 4> prefix{};
	prefix[0] = 65;
	EXPECT_EQ(EControlIpcDecodeOutcome::OversizeFrame, oversize.Feed(prefix).outcome);
	EXPECT_TRUE(oversize.IsFailed());
}

TEST(ControlIpcProtocol, RejectsHostileHeaderMetadataWithExplicitTerminalOutcome)
{
	const auto valid = Encode(MakeFrame());
	struct Case {
		std::vector<std::uint8_t> bytes;
		EControlIpcDecodeOutcome expected;
	};
	std::vector<Case> cases;
	auto malformedMagic = valid;
	malformedMagic[4] ^= 0xff;
	cases.push_back({ std::move(malformedMagic), EControlIpcDecodeOutcome::MalformedFrame });
	auto unsupportedMajor = valid;
	SetLe16(unsupportedMajor, 8, kControlIpcMajorVersion + 1);
	cases.push_back({ std::move(unsupportedMajor), EControlIpcDecodeOutcome::UnsupportedMajorVersion });
	auto unknownKind = valid;
	SetLe16(unknownKind, 12, 0xfffe);
	cases.push_back({ std::move(unknownKind), EControlIpcDecodeOutcome::UnknownKind });
	auto unknownRequiredFlags = valid;
	SetLe16(unknownRequiredFlags, 14, 0x8001);
	cases.push_back({ std::move(unknownRequiredFlags), EControlIpcDecodeOutcome::UnknownRequiredFlags });
	auto zeroRequestId = valid;
	for (std::size_t index = 16; index < 24; ++index) zeroRequestId[index] = 0;
	cases.push_back({ std::move(zeroRequestId), EControlIpcDecodeOutcome::InvalidRequestId });
	auto zeroGeneration = valid;
	for (std::size_t index = 24; index < 32; ++index) zeroGeneration[index] = 0;
	cases.push_back({ std::move(zeroGeneration), EControlIpcDecodeOutcome::InvalidGeneration });

	for (const auto& test : cases) {
		CControlIpcFrameDecoder decoder;
		EXPECT_EQ(test.expected, decoder.Feed(test.bytes).outcome);
		EXPECT_TRUE(decoder.IsFailed());
	}
}

TEST(ControlIpcProtocol, EncodeAllowsNonTerminalResponseButRejectsInvalidDirectionAndOversizePayload)
{
	EXPECT_EQ(EControlIpcEncodeOutcome::Encoded,
		EncodeControlIpcFrame(MakeFrame(EControlIpcKind::ProfileRequest, EControlIpcFlags::Request)).outcome);
	EXPECT_EQ(EControlIpcEncodeOutcome::Encoded,
		EncodeControlIpcFrame(MakeFrame(EControlIpcKind::ProfileResponse,
			EControlIpcFlags::Response | EControlIpcFlags::Terminal)).outcome);
	auto nonTerminalResponse = MakeFrame(EControlIpcKind::StorageSnapshotResponse, EControlIpcFlags::Response);
	EXPECT_EQ(EControlIpcEncodeOutcome::Encoded, EncodeControlIpcFrame(nonTerminalResponse).outcome);
	auto invalidFlags = MakeFrame(EControlIpcKind::StorageSnapshotResponse, EControlIpcFlags::Request);
	EXPECT_EQ(EControlIpcEncodeOutcome::InvalidFlags, EncodeControlIpcFrame(invalidFlags).outcome);
	auto nonTerminalError = MakeFrame(EControlIpcKind::Error, EControlIpcFlags::Response);
	EXPECT_EQ(EControlIpcEncodeOutcome::InvalidFlags, EncodeControlIpcFrame(nonTerminalError).outcome);
	auto oversize = MakeFrame();
	oversize.payload.resize(kControlIpcMaximumFrameBytes - kControlIpcHeaderBytes + 1);
	EXPECT_EQ(EControlIpcEncodeOutcome::PayloadTooLarge, EncodeControlIpcFrame(oversize).outcome);
}

TEST(ControlIpcProtocol, BoundedTlvAndUtf8ErrorPayloadRoundTrip)
{
	ControlIpcFields fields;
	ASSERT_TRUE(AddUtf8Field(fields, EControlIpcFieldTag::ProfileId, "default"));
	ASSERT_TRUE(AddUtf8Field(fields, EControlIpcFieldTag::Diagnostic, "configuration invalid"));
	const auto encoded = EncodeControlIpcFields(fields);
	ASSERT_TRUE(encoded);
	const auto decoded = DecodeControlIpcFields(*encoded);
	ASSERT_EQ(EControlIpcFieldDecodeOutcome::Decoded, decoded.outcome);
	const auto profileId = GetUtf8Field(decoded.fields, EControlIpcFieldTag::ProfileId);
	const auto diagnostic = GetUtf8Field(decoded.fields, EControlIpcFieldTag::Diagnostic);
	ASSERT_TRUE(profileId);
	ASSERT_TRUE(diagnostic);
	EXPECT_EQ("default", *profileId);
	EXPECT_EQ("configuration invalid", *diagnostic);

	const ControlIpcError original{ EControlIpcTerminalStatus::GenerationMismatch, "generation is stale" };
	const auto errorPayload = EncodeControlIpcError(original);
	ASSERT_TRUE(errorPayload);
	const auto restored = DecodeControlIpcError(*errorPayload);
	ASSERT_TRUE(restored);
	EXPECT_EQ(original.status, restored->status);
	EXPECT_EQ(original.diagnostic, restored->diagnostic);
}

TEST(ControlIpcProtocol, RejectsMalformedTlvAndInvalidUtf8WithoutAmbiguousFallback)
{
	std::vector<std::uint8_t> truncated{ 1, 0, 2, 0, 0, 0, 'x' };
	EXPECT_EQ(EControlIpcFieldDecodeOutcome::MalformedField, DecodeControlIpcFields(truncated).outcome);

	ControlIpcFields malformedUtf8{ { static_cast<std::uint16_t>(EControlIpcFieldTag::Diagnostic), { 0xc0, 0xaf } } };
	EControlIpcFieldDecodeOutcome failure = EControlIpcFieldDecodeOutcome::Decoded;
	EXPECT_FALSE(GetUtf8Field(malformedUtf8, EControlIpcFieldTag::Diagnostic, &failure));
	EXPECT_EQ(EControlIpcFieldDecodeOutcome::InvalidUtf8, failure);

	std::vector<std::uint8_t> malformedFrame = Encode(MakeFrame());
	SetLe32(malformedFrame, 0, kControlIpcHeaderBytes - 1);
	CControlIpcFrameDecoder decoder;
	EXPECT_EQ(EControlIpcDecodeOutcome::MalformedFrame, decoder.Feed(malformedFrame).outcome);
}

TEST(ControlIpcProtocol, RejectsReservedAndDuplicateTlvTags)
{
	ControlIpcFields reserved{ { 0, { 'x' } } };
	EXPECT_FALSE(EncodeControlIpcFields(reserved));
	std::vector<std::uint8_t> reservedWire{ 0, 0, 0, 0, 0, 0 };
	EXPECT_EQ(EControlIpcFieldDecodeOutcome::InvalidTag, DecodeControlIpcFields(reservedWire).outcome);

	const auto tag = static_cast<std::uint16_t>(EControlIpcFieldTag::ProfileId);
	ControlIpcFields duplicate{ { tag, { 'a' } }, { tag, { 'b' } } };
	EXPECT_FALSE(EncodeControlIpcFields(duplicate));
}

TEST(ControlIpcProtocol, ResetMakesAStickyTerminalDecoderUsableForANewConnection)
{
	CControlIpcFrameDecoder decoder(64);
	const std::array<std::uint8_t, 4> oversizePrefix{ 65, 0, 0, 0 };
	EXPECT_EQ(EControlIpcDecodeOutcome::OversizeFrame, decoder.Feed(oversizePrefix).outcome);
	decoder.Reset();
	EXPECT_FALSE(decoder.IsFailed());
	const auto decoded = decoder.Feed(Encode(MakeFrame()));
	EXPECT_EQ(EControlIpcDecodeOutcome::Decoded, decoded.outcome);
	ASSERT_EQ(1u, decoded.frames.size());
}

TEST(ControlIpcProtocol, RejectsMoreThanMaximumTlvFieldCount)
{
	std::vector<std::uint8_t> bytes;
	bytes.reserve((kControlIpcMaximumFieldCount + 1) * 6);
	for (std::size_t index = 0; index <= kControlIpcMaximumFieldCount; ++index) {
		bytes.push_back(1);
		bytes.push_back(0);
		bytes.insert(bytes.end(), { 0, 0, 0, 0 });
	}
	EXPECT_EQ(EControlIpcFieldDecodeOutcome::TooManyFields, DecodeControlIpcFields(bytes).outcome);
}

} // namespace
} // namespace platform::controlipc
