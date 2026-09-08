/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include "platform/controlipc/ControlSenpRpc.h"

namespace platform::controlipc {
namespace {
ControlSenpRpcOwner Owner()
{
	return { L"sakura.github-pull-requests", L"sha256:ab", 7, 3, 2 };
}

ControlSenpRpcRequest StartRead()
{
	ControlSenpRpcRequest request;
	request.operation = EControlSenpRpcOperation::StartRead;
	request.profileId = L"profile-1";
	request.owner = Owner();
	request.grantId = "grant-1";
	request.readId = L"issues:open:1";
	request.toolId = L"github";
	request.toolOperation = L"repositoryRead";
	request.arguments = { { L"path", L"issues" }, { L"state", L"open" } };
	return request;
}

ControlSenpRpcRequest IssueGrant()
{
	ControlSenpRpcRequest request;
	request.operation = EControlSenpRpcOperation::IssueGrant;
	request.profileId = L"profile-1";
	request.owner = Owner();
	request.capabilities = static_cast<std::uint32_t>(senp::SenpToolCapability::GitHubRepositoryRead);
	return request;
}
} // namespace

TEST(ControlSenpRpc, RoundTripsEveryRequestOperation)
{
	const auto grant = IssueGrant();
	const auto encodedGrant = EncodeControlSenpRpcRequest(grant);
	ASSERT_TRUE(encodedGrant);
	const auto decodedGrant = DecodeControlSenpRpcRequest(*encodedGrant);
	ASSERT_TRUE(decodedGrant);
	EXPECT_EQ(EControlSenpRpcOperation::IssueGrant, decodedGrant->operation);
	EXPECT_EQ(grant.capabilities, decodedGrant->capabilities);
	EXPECT_EQ(grant.owner, decodedGrant->owner);

	const auto read = StartRead();
	const auto encodedRead = EncodeControlSenpRpcRequest(read);
	ASSERT_TRUE(encodedRead);
	const auto decodedRead = DecodeControlSenpRpcRequest(*encodedRead);
	ASSERT_TRUE(decodedRead);
	EXPECT_EQ(read.readId, decodedRead->readId);
	EXPECT_EQ(read.toolId, decodedRead->toolId);
	EXPECT_EQ(read.toolOperation, decodedRead->toolOperation);
	ASSERT_EQ(2U, decodedRead->arguments.size());
	EXPECT_EQ(L"path", decodedRead->arguments[0].name);
	EXPECT_EQ(L"open", decodedRead->arguments[1].value);

	ControlSenpRpcRequest poll = read;
	poll.operation = EControlSenpRpcOperation::PollRead;
	poll.readId.clear();
	poll.toolId.clear();
	poll.toolOperation.clear();
	poll.arguments.clear();
	const auto encodedPoll = EncodeControlSenpRpcRequest(poll);
	ASSERT_TRUE(encodedPoll);
	EXPECT_TRUE(DecodeControlSenpRpcRequest(*encodedPoll));

	ControlSenpRpcRequest cancel = poll;
	cancel.operation = EControlSenpRpcOperation::CancelRead;
	cancel.readId = L"issues:open:1";
	const auto encodedCancel = EncodeControlSenpRpcRequest(cancel);
	ASSERT_TRUE(encodedCancel);
	EXPECT_TRUE(DecodeControlSenpRpcRequest(*encodedCancel));

	ControlSenpRpcRequest resource = poll;
	resource.operation = EControlSenpRpcOperation::ReadResource;
	resource.resourceHandle = L"log-1";
	resource.offset = 4096;
	resource.length = 1024;
	const auto encodedResource = EncodeControlSenpRpcRequest(resource);
	ASSERT_TRUE(encodedResource);
	const auto decodedResource = DecodeControlSenpRpcRequest(*encodedResource);
	ASSERT_TRUE(decodedResource);
	EXPECT_EQ(4096U, decodedResource->offset);
	EXPECT_EQ(1024U, decodedResource->length);

	ControlSenpRpcRequest release = poll;
	release.operation = EControlSenpRpcOperation::ReleaseResource;
	release.resourceHandle = L"log-1";
	const auto encodedRelease = EncodeControlSenpRpcRequest(release);
	ASSERT_TRUE(encodedRelease);
	EXPECT_TRUE(DecodeControlSenpRpcRequest(*encodedRelease));
}

TEST(ControlSenpRpc, RejectsMembersThatDoNotBelongToTheOperation)
{
	auto grantWithRead = IssueGrant();
	grantWithRead.readId = L"issues:open:1";
	EXPECT_FALSE(EncodeControlSenpRpcRequest(grantWithRead));

	auto pollWithTool = StartRead();
	pollWithTool.operation = EControlSenpRpcOperation::PollRead;
	EXPECT_FALSE(EncodeControlSenpRpcRequest(pollWithTool));

	auto readWithCapability = StartRead();
	readWithCapability.capabilities = 1;
	EXPECT_FALSE(EncodeControlSenpRpcRequest(readWithCapability));

	auto readWithoutGrant = StartRead();
	readWithoutGrant.grantId.clear();
	EXPECT_FALSE(EncodeControlSenpRpcRequest(readWithoutGrant));

	auto resourceWithoutLength = StartRead();
	resourceWithoutLength.operation = EControlSenpRpcOperation::ReadResource;
	resourceWithoutLength.readId.clear();
	resourceWithoutLength.toolId.clear();
	resourceWithoutLength.toolOperation.clear();
	resourceWithoutLength.arguments.clear();
	resourceWithoutLength.resourceHandle = L"log-1";
	EXPECT_FALSE(EncodeControlSenpRpcRequest(resourceWithoutLength));
}

TEST(ControlSenpRpc, RejectsUnusableOwnerIdentity)
{
	auto zeroGeneration = StartRead();
	zeroGeneration.owner.generation = 0;
	EXPECT_FALSE(EncodeControlSenpRpcRequest(zeroGeneration));

	auto noExtension = StartRead();
	noExtension.owner.extensionId.clear();
	EXPECT_FALSE(EncodeControlSenpRpcRequest(noExtension));

	auto noProfile = StartRead();
	noProfile.profileId.clear();
	EXPECT_FALSE(EncodeControlSenpRpcRequest(noProfile));

	auto negativeAccount = StartRead();
	negativeAccount.owner.accountGeneration = -1;
	EXPECT_FALSE(EncodeControlSenpRpcRequest(negativeAccount));
}

TEST(ControlSenpRpc, RejectsTruncatedAndTrailingBytes)
{
	const auto encoded = EncodeControlSenpRpcRequest(StartRead());
	ASSERT_TRUE(encoded);
	for (std::size_t length = 0; length < encoded->size(); ++length) {
		EXPECT_FALSE(DecodeControlSenpRpcRequest(std::span(encoded->data(), length))) << length;
	}
	auto trailing = *encoded;
	trailing.push_back(0);
	EXPECT_FALSE(DecodeControlSenpRpcRequest(trailing));

	auto badVersion = *encoded;
	badVersion[0] = 2;
	EXPECT_FALSE(DecodeControlSenpRpcRequest(badVersion));

	auto badOperation = *encoded;
	badOperation[1] = 99;
	EXPECT_FALSE(DecodeControlSenpRpcRequest(badOperation));
}

TEST(ControlSenpRpc, RoundTripsCompletionAndResourceResponses)
{
	ControlSenpRpcResponse granted;
	granted.status = EControlSenpRpcStatus::Succeeded;
	granted.grantId = "grant-1";
	granted.expiresAtMilliseconds = 1234567;
	const auto encodedGrant = EncodeControlSenpRpcResponse(granted);
	ASSERT_TRUE(encodedGrant);
	const auto decodedGrant = DecodeControlSenpRpcResponse(*encodedGrant);
	ASSERT_TRUE(decodedGrant);
	EXPECT_EQ("grant-1", decodedGrant->grantId);
	EXPECT_EQ(1234567U, decodedGrant->expiresAtMilliseconds);
	EXPECT_FALSE(decodedGrant->hasCompletion);

	ControlSenpRpcResponse completed;
	completed.status = EControlSenpRpcStatus::Succeeded;
	completed.hasCompletion = true;
	completed.completion = { L"issues:open:1", senp::effect::CompletionStatus::Succeeded,
		LR"({"body":[],"nextPage":2})", L"" };
	const auto encodedCompleted = EncodeControlSenpRpcResponse(completed);
	ASSERT_TRUE(encodedCompleted);
	const auto decodedCompleted = DecodeControlSenpRpcResponse(*encodedCompleted);
	ASSERT_TRUE(decodedCompleted);
	EXPECT_TRUE(decodedCompleted->hasCompletion);
	EXPECT_EQ(completed.completion, decodedCompleted->completion);

	ControlSenpRpcResponse chunk;
	chunk.status = EControlSenpRpcStatus::Succeeded;
	chunk.resourceHandle = L"log-1";
	chunk.resourceOffset = 64;
	chunk.resourceBytes = "run step output";
	chunk.resourceState = 1;
	chunk.resourceFinal = true;
	const auto encodedChunk = EncodeControlSenpRpcResponse(chunk);
	ASSERT_TRUE(encodedChunk);
	const auto decodedChunk = DecodeControlSenpRpcResponse(*encodedChunk);
	ASSERT_TRUE(decodedChunk);
	EXPECT_EQ(chunk.resourceBytes, decodedChunk->resourceBytes);
	EXPECT_EQ(64U, decodedChunk->resourceOffset);
	EXPECT_TRUE(decodedChunk->resourceFinal);
}

TEST(ControlSenpRpc, RejectsIncoherentResponses)
{
	ControlSenpRpcResponse completionWithoutFlag;
	completionWithoutFlag.status = EControlSenpRpcStatus::Succeeded;
	completionWithoutFlag.completion.readId = L"issues:open:1";
	EXPECT_FALSE(EncodeControlSenpRpcResponse(completionWithoutFlag));

	ControlSenpRpcResponse flagWithoutRead;
	flagWithoutRead.status = EControlSenpRpcStatus::Succeeded;
	flagWithoutRead.hasCompletion = true;
	EXPECT_FALSE(EncodeControlSenpRpcResponse(flagWithoutRead));

	ControlSenpRpcResponse chunkWithoutHandle;
	chunkWithoutHandle.status = EControlSenpRpcStatus::Succeeded;
	chunkWithoutHandle.resourceBytes = "bytes";
	EXPECT_FALSE(EncodeControlSenpRpcResponse(chunkWithoutHandle));

	ControlSenpRpcResponse oversizedData;
	oversizedData.status = EControlSenpRpcStatus::Succeeded;
	oversizedData.hasCompletion = true;
	oversizedData.completion.readId = L"issues:open:1";
	oversizedData.completion.data.assign(kControlSenpRpcMaximumToolDataBytes + 1, L'a');
	EXPECT_FALSE(EncodeControlSenpRpcResponse(oversizedData));
}

TEST(ControlSenpRpc, MapsOwnerIdentityWithoutLoss)
{
	const auto owner = Owner();
	const auto identity = ToContributionOwner(owner);
	EXPECT_EQ(owner.extensionId, identity.extensionId);
	EXPECT_EQ(owner.packageDigest, identity.packageDigest);
	EXPECT_EQ(owner.generation, identity.generation);
	EXPECT_EQ(owner, FromContributionOwner(identity));
}

} // namespace platform::controlipc
