/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include "extension/CJsonRpcFrameCodec.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string Encode(CJsonRpcFrameCodec& codec, std::string_view payload)
{
	std::string frame;
	EXPECT_TRUE(codec.Encode(payload, frame));
	return frame;
}

std::string Header(std::uint32_t payloadLength)
{
	return std::string{
		static_cast<char>((payloadLength >> 24) & 0xffu),
		static_cast<char>((payloadLength >> 16) & 0xffu),
		static_cast<char>((payloadLength >> 8) & 0xffu),
		static_cast<char>(payloadLength & 0xffu),
	};
}

} // namespace

TEST(CJsonRpcFrameCodec, Encode_UsesAnExactBigEndianLengthPrefix)
{
	CJsonRpcFrameCodec codec;
	const auto frame = Encode(codec, "abcde");

	ASSERT_EQ(9u, frame.size());
	EXPECT_EQ(0, static_cast<unsigned char>(frame[0]));
	EXPECT_EQ(0, static_cast<unsigned char>(frame[1]));
	EXPECT_EQ(0, static_cast<unsigned char>(frame[2]));
	EXPECT_EQ(5, static_cast<unsigned char>(frame[3]));
	EXPECT_EQ("abcde", std::string_view(frame).substr(4));
}

TEST(CJsonRpcFrameCodec, RoundTrip_PreservesUtf8Bytes)
{
	CJsonRpcFrameCodec encoder;
	CJsonRpcFrameCodec decoder;
	const std::u8string utf8Payload = u8"{\"message\":\"日本語😀\"}";
	const std::string payload(reinterpret_cast<const char*>(utf8Payload.data()), utf8Payload.size());
	const auto frame = Encode(encoder, payload);
	std::vector<std::string> received;

	EXPECT_TRUE(decoder.Feed(frame, received));
	ASSERT_EQ(1u, received.size());
	EXPECT_EQ(payload, received.front());
}

TEST(CJsonRpcFrameCodec, Feed_AcceptsHeaderFragments)
{
	CJsonRpcFrameCodec codec;
	std::vector<std::string> received;
	const std::string frame = Header(3) + "abc";

	EXPECT_TRUE(codec.Feed(std::string_view(frame).substr(0, 1), received));
	EXPECT_TRUE(codec.Feed(std::string_view(frame).substr(1, 2), received));
	EXPECT_TRUE(codec.Feed(std::string_view(frame).substr(3), received));
	ASSERT_EQ(1u, received.size());
	EXPECT_EQ("abc", received.front());
}

TEST(CJsonRpcFrameCodec, Feed_AcceptsPayloadFragments)
{
	CJsonRpcFrameCodec encoder;
	CJsonRpcFrameCodec decoder;
	const auto frame = Encode(encoder, "fragmented");
	std::vector<std::string> received;

	EXPECT_TRUE(decoder.Feed(std::string_view(frame).substr(0, 6), received));
	EXPECT_TRUE(received.empty());
	EXPECT_TRUE(decoder.Feed(std::string_view(frame).substr(6, 3), received));
	EXPECT_TRUE(received.empty());
	EXPECT_TRUE(decoder.Feed(std::string_view(frame).substr(9), received));
	ASSERT_EQ(1u, received.size());
	EXPECT_EQ("fragmented", received.front());
}

TEST(CJsonRpcFrameCodec, Feed_CollectsConcatenatedFrames)
{
	CJsonRpcFrameCodec encoder;
	CJsonRpcFrameCodec decoder;
	const auto first = Encode(encoder, "first");
	const auto second = Encode(encoder, "second");
	std::vector<std::string> received;

	EXPECT_TRUE(decoder.Feed(first + second, received));
	ASSERT_EQ(2u, received.size());
	EXPECT_EQ("first", received[0]);
	EXPECT_EQ("second", received[1]);
}

TEST(CJsonRpcFrameCodec, Feed_AllowsZeroLengthPayload)
{
	CJsonRpcFrameCodec codec;
	std::vector<std::string> received;
	const std::string frame = Header(0);

	EXPECT_TRUE(codec.Feed(frame, received));
	ASSERT_EQ(1u, received.size());
	EXPECT_TRUE(received.front().empty());
}

TEST(CJsonRpcFrameCodec, Feed_RejectsConfiguredOversizeBeforePayloadIsBuffered)
{
	CJsonRpcFrameCodec codec(3);
	std::vector<std::string> received;
	const std::string header = Header(4);

	EXPECT_FALSE(codec.Feed(header, received));
	EXPECT_TRUE(received.empty());
	EXPECT_EQ(EJsonRpcFrameCodecState::Failed, codec.GetState());
	EXPECT_EQ(EJsonRpcFrameCodecError::PayloadTooLarge, codec.GetError());
}

TEST(CJsonRpcFrameCodec, Feed_FailureIsStickyUntilReset)
{
	CJsonRpcFrameCodec codec(1);
	std::vector<std::string> received;
	const std::string oversizeHeader = Header(2);

	EXPECT_FALSE(codec.Feed(oversizeHeader, received));
	EXPECT_FALSE(codec.Feed(Header(1) + "x", received));
	EXPECT_TRUE(received.empty());
	EXPECT_EQ(EJsonRpcFrameCodecState::Failed, codec.GetState());
	EXPECT_EQ(EJsonRpcFrameCodecError::PayloadTooLarge, codec.GetError());
}

TEST(CJsonRpcFrameCodec, Reset_RecoversFromOversizeFailure)
{
	CJsonRpcFrameCodec codec(1);
	std::vector<std::string> received;

	EXPECT_FALSE(codec.Feed(Header(2), received));
	codec.Reset();
	EXPECT_EQ(EJsonRpcFrameCodecState::ReadingHeader, codec.GetState());
	EXPECT_EQ(EJsonRpcFrameCodecError::None, codec.GetError());
	EXPECT_TRUE(codec.Feed(Header(1) + "x", received));
	ASSERT_EQ(1u, received.size());
	EXPECT_EQ("x", received.front());
}

TEST(CJsonRpcFrameCodec, Encode_RejectsPayloadOverConfiguredLimit)
{
	CJsonRpcFrameCodec codec(3);
	std::string frame = "unchanged";

	EXPECT_FALSE(codec.Encode("four", frame));
	EXPECT_EQ("unchanged", frame);
}
