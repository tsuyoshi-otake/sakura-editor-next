/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include <gtest/gtest.h>

#include "debug/dap/DapProtocolCodec.h"

namespace {

using debug::dap::CDapProtocolCodec;
using debug::dap::DapEvent;
using debug::dap::DapMessage;
using debug::dap::DapProtocolCodecLimits;
using debug::dap::DapRequest;
using debug::dap::DapResponse;
using debug::dap::EDapProtocolCodecError;
using debug::dap::EDapProtocolCodecState;
using debug::dap::EDapProtocolCodecStatus;

std::string Frame(std::string_view body)
{
	return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + std::string(body);
}

TEST(DapProtocolCodec, DecodesARequestAcrossArbitraryHeaderAndBodyChunks)
{
	CDapProtocolCodec codec;
	const auto frame = Frame(R"json({"seq":1,"type":"request","command":"initialize","arguments":{"clientID":"sakura"}})json");
	std::vector<DapMessage> messages;
	for (const auto character : frame) {
		const auto result = codec.Feed(std::string_view(&character, 1), messages);
		EXPECT_TRUE(result.Succeeded());
	}
	ASSERT_EQ(1U, messages.size());
	const auto& request = std::get<DapRequest>(messages.front());
	EXPECT_EQ(1U, request.seq);
	EXPECT_EQ("initialize", request.command);
	ASSERT_TRUE(request.argumentsJson);
	EXPECT_EQ(R"json({"clientID":"sakura"})json", *request.argumentsJson);
	EXPECT_EQ(EDapProtocolCodecState::ReadingHeader, codec.GetState());
}

TEST(DapProtocolCodec, DecodesMultipleFramesAndPreservesTypedRawBodies)
{
	CDapProtocolCodec codec;
	std::vector<DapMessage> messages;
	const auto result = codec.Feed(Frame(R"json({"seq":2,"type":"request","command":"threads"})json")
		+ Frame(R"json({"seq":3,"type":"event","event":"stopped","body":{"reason":"breakpoint"}})json"), messages);
	EXPECT_EQ(EDapProtocolCodecStatus::Completed, result.status);
	EXPECT_EQ(2U, result.completedMessages);
	ASSERT_EQ(2U, messages.size());
	EXPECT_EQ("threads", std::get<DapRequest>(messages[0]).command);
	const auto& event = std::get<DapEvent>(messages[1]);
	EXPECT_EQ("stopped", event.event);
	ASSERT_TRUE(event.bodyJson);
	EXPECT_EQ(R"json({"reason":"breakpoint"})json", *event.bodyJson);
	EXPECT_EQ(R"json({"seq":3,"type":"event","event":"stopped","body":{"reason":"breakpoint"}})json", event.rawJson);
}

TEST(DapProtocolCodec, RejectsInvalidUtf8WithoutAppendingAPartialMessage)
{
	CDapProtocolCodec codec;
	std::vector<DapMessage> messages;
	const std::string invalid = "{\"seq\":1,\"type\":\"event\",\"event\":\"" + std::string(1, static_cast<char>(0xc3)) + "\"}";
	const auto result = codec.Feed(Frame(invalid), messages);
	EXPECT_EQ(EDapProtocolCodecStatus::Failed, result.status);
	EXPECT_EQ(EDapProtocolCodecError::InvalidUtf8, result.error);
	EXPECT_TRUE(messages.empty());
	EXPECT_EQ(EDapProtocolCodecState::Failed, codec.GetState());
}

TEST(DapProtocolCodec, RejectsMalformedDuplicateUnknownAndLfOnlyHeaders)
{
	struct Case { std::string bytes; EDapProtocolCodecError error; };
	const std::vector<Case> cases = {
		{ "Content-Type: application/vscode-jsonrpc\r\n\r\n", EDapProtocolCodecError::UnknownHeader },
		{ "Content-Length: 2\r\nContent-Length: 2\r\n\r\n{}", EDapProtocolCodecError::DuplicateHeader },
		{ "Content-Length: 2\n\n{}", EDapProtocolCodecError::MalformedHeader },
		{ "Content-Length: x\r\n\r\n{}", EDapProtocolCodecError::InvalidContentLength },
	};
	for (const auto& test : cases) {
		CDapProtocolCodec codec;
		std::vector<DapMessage> messages;
		const auto result = codec.Feed(test.bytes, messages);
		EXPECT_EQ(EDapProtocolCodecStatus::Failed, result.status);
		EXPECT_EQ(test.error, result.error);
		EXPECT_TRUE(messages.empty());
	}
}

TEST(DapProtocolCodec, RejectsCommentsTrailingCommasAndInvalidOrIncompleteEnvelopes)
{
	const std::vector<std::string> invalidJson = {
		R"json({"seq":1,"type":"request","command":"x",})json",
		R"json({"seq":1,"type":"request",/* not DAP JSON */"command":"x"})json",
		R"json({"seq":1,"type":"request","command":"x"} trailing)json",
	};
	for (const auto& body : invalidJson) {
		CDapProtocolCodec codec;
		std::vector<DapMessage> messages;
		EXPECT_EQ(EDapProtocolCodecError::InvalidJson, codec.Feed(Frame(body), messages).error);
	}

	const std::vector<std::string> invalidEnvelope = {
		R"json({"seq":0,"type":"request","command":"x"})json",
		R"json({"seq":1,"type":"request"})json",
		R"json({"seq":1,"type":"response","request_seq":1,"command":"x"})json",
		R"json({"seq":1,"type":"event","event":"x","body":[]})json",
	};
	for (const auto& body : invalidEnvelope) {
		CDapProtocolCodec codec;
		std::vector<DapMessage> messages;
		EXPECT_EQ(EDapProtocolCodecError::InvalidEnvelope, codec.Feed(Frame(body), messages).error);
	}

	CDapProtocolCodec duplicateMemberCodec;
	std::vector<DapMessage> duplicateMemberMessages;
	EXPECT_EQ(EDapProtocolCodecError::InvalidJson,
		duplicateMemberCodec.Feed(
			Frame(R"json({"seq":1,"seq":2,"type":"event","event":"x"})json"),
			duplicateMemberMessages).error);
}

TEST(DapProtocolCodec, EnforcesHeaderBodyAndContentLengthLimits)
{
	DapProtocolCodecLimits limits;
	limits.maximumHeaderBytes = 8;
	limits.maximumBodyBytes = 32;
	CDapProtocolCodec headerLimited(limits);
	std::vector<DapMessage> messages;
	EXPECT_EQ(EDapProtocolCodecError::HeaderTooLarge, headerLimited.Feed("Content-Length", messages).error);

	DapProtocolCodecLimits bodyLimits;
	bodyLimits.maximumBodyBytes = 8;
	CDapProtocolCodec bodyLimited(bodyLimits);
	EXPECT_EQ(EDapProtocolCodecError::BodyTooLarge, bodyLimited.Feed("Content-Length: 9\r\n\r\n", messages).error);

	CDapProtocolCodec overflow;
	EXPECT_EQ(EDapProtocolCodecError::ContentLengthOverflow,
		overflow.Feed("Content-Length: 184467440737095516160\r\n\r\n", messages).error);
}

TEST(DapProtocolCodec, FailedStreamsNeedExplicitResetAndNeverResynchronize)
{
	CDapProtocolCodec codec;
	std::vector<DapMessage> messages;
	ASSERT_EQ(EDapProtocolCodecStatus::Failed, codec.Feed("broken\n", messages).status);
	EXPECT_EQ(EDapProtocolCodecStatus::Failed,
		codec.Feed(Frame(R"json({"seq":1,"type":"event","event":"continued"})json"), messages).status);
	EXPECT_TRUE(messages.empty());
	codec.Reset();
	EXPECT_EQ(EDapProtocolCodecState::ReadingHeader, codec.GetState());
	EXPECT_EQ(EDapProtocolCodecStatus::Completed,
		codec.Feed(Frame(R"json({"seq":1,"type":"event","event":"continued"})json"), messages).status);
	ASSERT_EQ(1U, messages.size());
	EXPECT_EQ("continued", std::get<DapEvent>(messages.front()).event);
}

TEST(DapProtocolCodec, RetainsCompletedMessagesWhenALaterFrameInTheSameChunkFails)
{
	CDapProtocolCodec codec;
	std::vector<DapMessage> messages;
	const auto result = codec.Feed(Frame(R"json({"seq":1,"type":"event","event":"initialized"})json") + "broken\n", messages);
	EXPECT_EQ(EDapProtocolCodecStatus::Failed, result.status);
	EXPECT_EQ(1U, result.completedMessages);
	ASSERT_EQ(1U, messages.size());
	EXPECT_EQ("initialized", std::get<DapEvent>(messages.front()).event);
}

TEST(DapProtocolCodec, EncodesCanonicalFramesThatRoundTripAsTypedResponses)
{
	CDapProtocolCodec encoder;
	DapResponse response;
	response.seq = 5;
	response.requestSeq = 4;
	response.success = true;
	response.command = "initialize";
	response.message = "ready\nnow";
	response.bodyJson = R"json({"supportsConfigurationDoneRequest":true})json";
	std::string frame;
	EXPECT_EQ(EDapProtocolCodecStatus::Completed, encoder.Encode(response, frame).status);
	EXPECT_EQ(0U, frame.find("Content-Length: "));
	EXPECT_NE(std::string::npos, frame.find("\r\n\r\n"));

	CDapProtocolCodec decoder;
	std::vector<DapMessage> messages;
	EXPECT_EQ(EDapProtocolCodecStatus::Completed, decoder.Feed(frame, messages).status);
	ASSERT_EQ(1U, messages.size());
	const auto& decoded = std::get<DapResponse>(messages.front());
	EXPECT_EQ(5U, decoded.seq);
	EXPECT_EQ(4U, decoded.requestSeq);
	EXPECT_TRUE(decoded.success);
	EXPECT_EQ("ready\nnow", *decoded.message);
	EXPECT_EQ(*response.bodyJson, *decoded.bodyJson);
}

TEST(DapProtocolCodec, StopDiscardsPartialInputAndRequiresResetBeforeEncodingOrFeeding)
{
	CDapProtocolCodec codec;
	std::vector<DapMessage> messages;
	EXPECT_EQ(EDapProtocolCodecStatus::NeedsMore, codec.Feed("Content-Length: 50\r\n\r\n{", messages).status);
	codec.Stop();
	EXPECT_EQ(EDapProtocolCodecState::Stopped, codec.GetState());
	EXPECT_EQ(EDapProtocolCodecStatus::Stopped, codec.Feed("anything", messages).status);
	DapEvent event { 1, "terminated", std::nullopt, {} };
	std::string frame;
	EXPECT_EQ(EDapProtocolCodecStatus::Stopped, codec.Encode(event, frame).status);
	codec.Reset();
	EXPECT_EQ(EDapProtocolCodecStatus::Completed, codec.Encode(event, frame).status);
}

} // namespace
