/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include "extension/CExtensionRpcProtocol.h"

#include <string>
#include <string_view>

namespace {

SExtensionRpcOutbound Request(
	CExtensionRpcProtocol& protocol,
	std::string_view method,
	std::string_view paramsJson = {})
{
	SExtensionRpcOutbound outbound;
	std::string errorMessage;
	EXPECT_TRUE(protocol.CreateRequest(method, paramsJson, outbound, errorMessage)) << errorMessage;
	return outbound;
}

SExtensionRpcOutbound Success(
	CExtensionRpcProtocol& protocol,
	std::string_view idJson,
	std::string_view resultJson)
{
	SExtensionRpcOutbound outbound;
	std::string errorMessage;
	EXPECT_TRUE(protocol.CreateSuccessResponse(idJson, resultJson, outbound, errorMessage)) << errorMessage;
	return outbound;
}

std::string Frame(std::string_view payload)
{
	CJsonRpcFrameCodec codec;
	std::string frame;
	EXPECT_TRUE(codec.Encode(payload, frame));
	return frame;
}

} // namespace

TEST(CExtensionRpcProtocol, FakeHost_CompletesFragmentedRequestResponseRoundTrip)
{
	CExtensionRpcProtocol editor;
	CExtensionRpcProtocol host;
	const auto request = Request(editor, "commands.execute", R"({"command":"sample.hello"})");

	const auto first = host.Feed(std::string_view(request.frame).substr(0, 2));
	EXPECT_TRUE(first.messages.empty());
	EXPECT_FALSE(first.IsTerminal());
	const auto second = host.Feed(std::string_view(request.frame).substr(2));
	ASSERT_EQ(1u, second.messages.size());
	EXPECT_EQ(EExtensionRpcMessageKind::Request, second.messages[0].eKind);
	EXPECT_EQ("commands.execute", second.messages[0].sMethod);
	EXPECT_EQ(request.sIdJson, second.messages[0].sIdJson);

	const auto response = Success(host, second.messages[0].sIdJson, R"({"handled":true})");
	const auto completed = editor.Feed(response.frame);
	ASSERT_EQ(1u, completed.messages.size());
	EXPECT_EQ(EExtensionRpcMessageKind::SuccessResponse, completed.messages[0].eKind);
	EXPECT_EQ(R"({"handled":true})", completed.messages[0].sResultJson);
	EXPECT_EQ(0u, editor.GetPendingRequestCount());
}

TEST(CExtensionRpcProtocol, FakeHost_AllowsRequestsInBothDirections)
{
	CExtensionRpcProtocol editor;
	CExtensionRpcProtocol host;
	const auto hostRequest = Request(host, "workspace.applyEdit", R"({"expectedVersion":7})");

	const auto received = editor.Feed(hostRequest.frame);
	ASSERT_EQ(1u, received.messages.size());
	EXPECT_EQ(EExtensionRpcMessageKind::Request, received.messages[0].eKind);
	EXPECT_EQ("workspace.applyEdit", received.messages[0].sMethod);

	const auto response = Success(editor, received.messages[0].sIdJson, R"({"applied":false})");
	const auto completed = host.Feed(response.frame);
	ASSERT_EQ(1u, completed.messages.size());
	EXPECT_EQ(R"({"applied":false})", completed.messages[0].sResultJson);
	EXPECT_EQ(0u, host.GetPendingRequestCount());
}

TEST(CExtensionRpcProtocol, CancelNotification_DoesNotCompleteTheOriginalRequest)
{
	CExtensionRpcProtocol editor;
	CExtensionRpcProtocol host;
	const auto request = Request(editor, "languages.format", R"({"document":"sakura://1/2"})");
	ASSERT_EQ(1u, host.Feed(request.frame).messages.size());

	SExtensionRpcOutbound cancel;
	std::string errorMessage;
	ASSERT_TRUE(editor.CreateCancelNotification(request.sIdJson, cancel, errorMessage)) << errorMessage;
	EXPECT_EQ(1u, editor.GetPendingRequestCount());
	const auto receivedCancel = host.Feed(cancel.frame);
	ASSERT_EQ(1u, receivedCancel.messages.size());
	EXPECT_EQ(EExtensionRpcMessageKind::Notification, receivedCancel.messages[0].eKind);
	EXPECT_EQ("$/cancelRequest", receivedCancel.messages[0].sMethod);
	EXPECT_EQ(std::string("{\"id\":") + request.sIdJson + "}", receivedCancel.messages[0].sParamsJson);

	SExtensionRpcOutbound cancelledResponse;
	ASSERT_TRUE(host.CreateErrorResponse(request.sIdJson, -32800, "request cancelled", {}, cancelledResponse, errorMessage)) << errorMessage;
	const auto completed = editor.Feed(cancelledResponse.frame);
	ASSERT_EQ(1u, completed.messages.size());
	EXPECT_EQ(EExtensionRpcMessageKind::ErrorResponse, completed.messages[0].eKind);
	EXPECT_EQ(-32800, completed.messages[0].error.nCode);
	EXPECT_EQ(0u, editor.GetPendingRequestCount());
}

TEST(CExtensionRpcProtocol, HostLost_FailsEveryPendingRequestExactlyOnce)
{
	CExtensionRpcProtocol editor;
	const auto first = Request(editor, "one");
	const auto second = Request(editor, "two");

	const auto lost = editor.CloseHostLost("fake host disconnected");
	EXPECT_EQ(EExtensionRpcTerminalReason::HostLost, lost.terminalReason);
	EXPECT_EQ("fake host disconnected", lost.diagnostic);
	ASSERT_EQ(2u, lost.failedRequests.size());
	EXPECT_EQ(first.sIdJson, lost.failedRequests[0].sIdJson);
	EXPECT_EQ(second.sIdJson, lost.failedRequests[1].sIdJson);
	EXPECT_EQ(EExtensionRpcProtocolState::Closed, editor.GetState());
	EXPECT_EQ(0u, editor.GetPendingRequestCount());

	const auto repeated = editor.CloseHostLost();
	EXPECT_TRUE(repeated.failedRequests.empty());
	EXPECT_EQ(EExtensionRpcTerminalReason::HostLost, repeated.terminalReason);
	SExtensionRpcOutbound rejected;
	std::string errorMessage;
	EXPECT_FALSE(editor.CreateRequest("after.close", {}, rejected, errorMessage));
	EXPECT_FALSE(errorMessage.empty());
}

TEST(CExtensionRpcProtocol, MalformedEnvelope_FailsPendingRequestsAndStaysTerminal)
{
	CExtensionRpcProtocol editor;
	Request(editor, "pending");

	const auto invalid = editor.Feed(Frame(R"({"jsonrpc":"2.0","method":7})"));
	EXPECT_EQ(EExtensionRpcTerminalReason::ProtocolError, invalid.terminalReason);
	ASSERT_EQ(1u, invalid.failedRequests.size());
	EXPECT_FALSE(invalid.diagnostic.empty());
	EXPECT_EQ(EExtensionRpcProtocolState::Failed, editor.GetState());

	const auto ignored = editor.Feed(Frame(R"({"jsonrpc":"2.0","method":"valid"})"));
	EXPECT_TRUE(ignored.messages.empty());
	EXPECT_TRUE(ignored.failedRequests.empty());
	EXPECT_EQ(EExtensionRpcTerminalReason::ProtocolError, ignored.terminalReason);
}

TEST(CExtensionRpcProtocol, UnknownResponseId_IsAProtocolError)
{
	CExtensionRpcProtocol editor;
	CExtensionRpcProtocol host;
	Request(editor, "pending");
	const auto unknown = Success(host, R"("missing")", "null");

	const auto result = editor.Feed(unknown.frame);
	EXPECT_EQ(EExtensionRpcTerminalReason::ProtocolError, result.terminalReason);
	ASSERT_EQ(1u, result.failedRequests.size());
	EXPECT_EQ(EExtensionRpcProtocolState::Failed, editor.GetState());
}

TEST(CExtensionRpcProtocol, OversizeFrame_FailsBeforeReceivingPayload)
{
	CExtensionRpcProtocol editor(256);
	Request(editor, "x");
	const std::string oversizedHeader{ '\0', '\0', '\1', '\1' };

	const auto result = editor.Feed(oversizedHeader);
	EXPECT_EQ(EExtensionRpcTerminalReason::ProtocolError, result.terminalReason);
	ASSERT_EQ(1u, result.failedRequests.size());
	EXPECT_EQ(EExtensionRpcProtocolState::Failed, editor.GetState());
}

TEST(CExtensionRpcProtocol, InvalidOutboundParams_AreRejectedWithoutCreatingPendingWork)
{
	CExtensionRpcProtocol editor;
	SExtensionRpcOutbound outbound;
	std::string errorMessage;

	EXPECT_FALSE(editor.CreateRequest("sample", "42", outbound, errorMessage));
	EXPECT_FALSE(errorMessage.empty());
	EXPECT_TRUE(outbound.frame.empty());
	EXPECT_EQ(0u, editor.GetPendingRequestCount());
	EXPECT_EQ(EExtensionRpcProtocolState::Open, editor.GetState());
}
