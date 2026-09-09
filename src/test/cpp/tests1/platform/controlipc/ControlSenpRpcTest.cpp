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

ControlSenpRpcRequest QueryAccount()
{
	ControlSenpRpcRequest request;
	request.operation = EControlSenpRpcOperation::QueryAccount;
	request.profileId = L"profile-1";
	return request;
}

ControlSenpRpcRequest AdoptWorkspace()
{
	ControlSenpRpcRequest request;
	request.operation = EControlSenpRpcOperation::AdoptWorkspace;
	request.profileId = L"profile-1";
	request.workspace.generation = 5;
	request.workspace.revision = 11;
	request.workspace.folders = { L"file:///c:/work/repo", L"file:///c:/work/docs" };
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
	// One past whatever this build writes: a layout change has to be refused
	// rather than decoded as if the members had not moved.
	++badVersion[0];
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

TEST(ControlSenpRpc, RoundTripsTheOwnerFreeAccountQuery)
{
	const auto encoded = EncodeControlSenpRpcRequest(QueryAccount());
	ASSERT_TRUE(encoded);
	const auto decoded = DecodeControlSenpRpcRequest(*encoded);
	ASSERT_TRUE(decoded);
	EXPECT_EQ(EControlSenpRpcOperation::QueryAccount, decoded->operation);
	EXPECT_EQ(L"profile-1", decoded->profileId);
	EXPECT_TRUE(decoded->owner == ControlSenpRpcOwner{});
}

TEST(ControlSenpRpc, RejectsAnAccountQueryThatCarriesAnOwnerOrAGrant)
{
	// The editor asks this before it can know an account generation at all, so an
	// owner on it could only be a claim that nothing downstream rechecks.
	auto withOwner = QueryAccount();
	withOwner.owner = Owner();
	EXPECT_FALSE(EncodeControlSenpRpcRequest(withOwner));

	auto withGrant = QueryAccount();
	withGrant.grantId = "grant-1";
	EXPECT_FALSE(EncodeControlSenpRpcRequest(withGrant));

	auto withCapability = QueryAccount();
	withCapability.capabilities = 1;
	EXPECT_FALSE(EncodeControlSenpRpcRequest(withCapability));

	auto withoutProfile = QueryAccount();
	withoutProfile.profileId.clear();
	EXPECT_FALSE(EncodeControlSenpRpcRequest(withoutProfile));
}

TEST(ControlSenpRpc, RoundTripsTheAccountAnswerAndRefusesAnUnusableOne)
{
	ControlSenpRpcResponse answered;
	answered.status = EControlSenpRpcStatus::Succeeded;
	answered.accountGeneration = 4;
	answered.accountState = EControlSenpAccountState::ReauthenticationRequired;
	const auto encoded = EncodeControlSenpRpcResponse(answered);
	ASSERT_TRUE(encoded);
	const auto decoded = DecodeControlSenpRpcResponse(*encoded);
	ASSERT_TRUE(decoded);
	EXPECT_EQ(4, decoded->accountGeneration);
	EXPECT_EQ(EControlSenpAccountState::ReauthenticationRequired, decoded->accountState);

	auto negative = answered;
	negative.accountGeneration = -1;
	EXPECT_FALSE(EncodeControlSenpRpcResponse(negative));

	auto foreignState = answered;
	foreignState.accountState = static_cast<EControlSenpAccountState>(9);
	EXPECT_FALSE(EncodeControlSenpRpcResponse(foreignState));

	// The state is the last byte on the wire. A value outside the enum must be
	// refused rather than cast into a state the editor would then act on.
	auto badState = *encoded;
	badState.back() = 9;
	EXPECT_FALSE(DecodeControlSenpRpcResponse(badState));
}

TEST(ControlSenpRpc, RoundTripsTheOwnerFreeWorkspaceAdoption)
{
	const auto declared = AdoptWorkspace();
	const auto encoded = EncodeControlSenpRpcRequest(declared);
	ASSERT_TRUE(encoded);
	const auto decoded = DecodeControlSenpRpcRequest(*encoded);
	ASSERT_TRUE(decoded);
	EXPECT_EQ(EControlSenpRpcOperation::AdoptWorkspace, decoded->operation);
	EXPECT_EQ(L"profile-1", decoded->profileId);
	// The declaration precedes every owner and names no repository: the control
	// side derives that from the remotes it finds at these folders.
	EXPECT_TRUE(decoded->owner == ControlSenpRpcOwner{});
	EXPECT_TRUE(decoded->grantId.empty());
	EXPECT_EQ(declared.workspace, decoded->workspace);
	// Order is part of the identity of a multi-root workspace, so it is asserted
	// rather than left to the set of folders happening to match.
	ASSERT_EQ(2U, decoded->workspace.folders.size());
	EXPECT_EQ(L"file:///c:/work/repo", decoded->workspace.folders[0]);
	EXPECT_EQ(L"file:///c:/work/docs", decoded->workspace.folders[1]);

	// A window that has no folder to select from says so. That is a declaration,
	// not an absent one, and it must survive the round trip as one.
	auto empty = declared;
	empty.workspace.folders.clear();
	const auto encodedEmpty = EncodeControlSenpRpcRequest(empty);
	ASSERT_TRUE(encodedEmpty);
	const auto decodedEmpty = DecodeControlSenpRpcRequest(*encodedEmpty);
	ASSERT_TRUE(decodedEmpty);
	EXPECT_TRUE(decodedEmpty->workspace.folders.empty());
	EXPECT_EQ(11, decodedEmpty->workspace.revision);
	EXPECT_FALSE(decodedEmpty->workspace.Empty());
}

TEST(ControlSenpRpc, RejectsAWorkspaceAdoptionThatCarriesAnOwnerOrCannotBeChecked)
{
	auto withOwner = AdoptWorkspace();
	withOwner.owner = Owner();
	EXPECT_FALSE(EncodeControlSenpRpcRequest(withOwner));

	auto withGrant = AdoptWorkspace();
	withGrant.grantId = "grant-1";
	EXPECT_FALSE(EncodeControlSenpRpcRequest(withGrant));

	auto withRead = AdoptWorkspace();
	withRead.readId = L"issues:open:1";
	EXPECT_FALSE(EncodeControlSenpRpcRequest(withRead));

	auto withoutProfile = AdoptWorkspace();
	withoutProfile.profileId.clear();
	EXPECT_FALSE(EncodeControlSenpRpcRequest(withoutProfile));

	// A revision of zero observed nothing, so a staleness check against it would
	// pass for a workspace that was never captured.
	auto withoutRevision = AdoptWorkspace();
	withoutRevision.workspace.revision = 0;
	EXPECT_FALSE(EncodeControlSenpRpcRequest(withoutRevision));

	auto withoutGeneration = AdoptWorkspace();
	withoutGeneration.workspace.generation = 0;
	EXPECT_FALSE(EncodeControlSenpRpcRequest(withoutGeneration));

	auto withNamelessFolder = AdoptWorkspace();
	withNamelessFolder.workspace.folders.push_back(L"");
	EXPECT_FALSE(EncodeControlSenpRpcRequest(withNamelessFolder));

	// The control side inspects every declared folder, so the bound is what keeps
	// one declaration from becoming unbounded work on the refresh worker.
	auto tooMany = AdoptWorkspace();
	tooMany.workspace.folders.assign(kControlSenpRpcMaximumWorkspaceFolders + 1, L"file:///c:/work");
	EXPECT_FALSE(EncodeControlSenpRpcRequest(tooMany));
}

TEST(ControlSenpRpc, RejectsADeclaredFolderCountNoEncoderWouldHaveWritten)
{
	auto empty = AdoptWorkspace();
	empty.workspace.folders.clear();
	const auto encoded = EncodeControlSenpRpcRequest(empty);
	ASSERT_TRUE(encoded);
	// The folder count is the last field on the wire, so raising it past the
	// bound is exactly the payload a peer that ignored the bound would send.
	ASSERT_LE(4U, encoded->size());
	auto beyondTheBound = *encoded;
	beyondTheBound[beyondTheBound.size() - 4] =
		static_cast<std::uint8_t>(kControlSenpRpcMaximumWorkspaceFolders + 1);
	EXPECT_FALSE(DecodeControlSenpRpcRequest(beyondTheBound));

	// Within the bound but with nothing behind it: refused as truncated rather
	// than decoded into a folder list the sender never wrote.
	auto claimingOne = *encoded;
	claimingOne[claimingOne.size() - 4] = 1;
	EXPECT_FALSE(DecodeControlSenpRpcRequest(claimingOne));
}

TEST(ControlSenpRpc, RejectsAWorkspaceDeclaredOnAnOperationThatDoesNotAdoptOne)
{
	// Every other operation answers for a workspace the control side already
	// holds, so a declaration on one would restate that state where nothing
	// rechecks it.
	auto readWithWorkspace = StartRead();
	readWithWorkspace.workspace = AdoptWorkspace().workspace;
	EXPECT_FALSE(EncodeControlSenpRpcRequest(readWithWorkspace));

	auto grantWithWorkspace = IssueGrant();
	grantWithWorkspace.workspace = AdoptWorkspace().workspace;
	EXPECT_FALSE(EncodeControlSenpRpcRequest(grantWithWorkspace));

	auto accountWithWorkspace = QueryAccount();
	accountWithWorkspace.workspace = AdoptWorkspace().workspace;
	EXPECT_FALSE(EncodeControlSenpRpcRequest(accountWithWorkspace));

	// A revision alone is still a declaration, so it is refused for the same
	// reason a fully populated one is.
	auto readWithRevision = StartRead();
	readWithRevision.workspace.revision = 11;
	EXPECT_FALSE(EncodeControlSenpRpcRequest(readWithRevision));
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
