/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include "platform/controlipc/ControlSenpRpc.h"

#include "senp/SenpTextResource.h"

namespace platform::controlipc {
namespace {
ControlSenpRpcOwner Owner()
{
	return { L"sakura.github-pull-requests", L"sha256:ab", 7, 3, 2 };
}

ControlSenpRpcRequest StartRead()
{
	ControlSenpRpcRequest request;
	request.SetOperation(EControlSenpRpcOperation::StartRead);
	request.SetProfileId(L"profile-1");
	request.SetOwner(Owner());
	request.SetGrantId("grant-1");
	request.SetReadId(L"issues:open:1");
	request.SetToolId(L"github");
	request.SetToolOperation(L"repositoryRead");
	request.SetArguments({ { L"path", L"issues" }, { L"state", L"open" } });
	return request;
}

ControlSenpRpcRequest IssueGrant()
{
	ControlSenpRpcRequest request;
	request.SetOperation(EControlSenpRpcOperation::IssueGrant);
	request.SetProfileId(L"profile-1");
	request.SetOwner(Owner());
	request.SetCapabilities(static_cast<std::uint32_t>(senp::SenpToolCapability::GitHubRepositoryRead));
	return request;
}

ControlSenpRpcRequest QueryAccount()
{
	ControlSenpRpcRequest request;
	request.SetOperation(EControlSenpRpcOperation::QueryAccount);
	request.SetProfileId(L"profile-1");
	return request;
}

ControlSenpRpcRequest AdoptWorkspace()
{
	ControlSenpRpcRequest request;
	request.SetOperation(EControlSenpRpcOperation::AdoptWorkspace);
	request.SetProfileId(L"profile-1");
	ControlSenpRpcWorkspace workspace;
	workspace.SetGeneration(5);
	workspace.SetRevision(11);
	workspace.SetFolders({ L"file:///c:/work/repo", L"file:///c:/work/docs" });
	request.SetWorkspace(std::move(workspace));
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
	EXPECT_EQ(EControlSenpRpcOperation::IssueGrant, decodedGrant->Operation());
	EXPECT_EQ(grant.Capabilities(), decodedGrant->Capabilities());
	EXPECT_EQ(grant.Owner(), decodedGrant->Owner());

	const auto read = StartRead();
	const auto encodedRead = EncodeControlSenpRpcRequest(read);
	ASSERT_TRUE(encodedRead);
	const auto decodedRead = DecodeControlSenpRpcRequest(*encodedRead);
	ASSERT_TRUE(decodedRead);
	EXPECT_EQ(read.ReadId(), decodedRead->ReadId());
	EXPECT_EQ(read.ToolId(), decodedRead->ToolId());
	EXPECT_EQ(read.ToolOperation(), decodedRead->ToolOperation());
	ASSERT_EQ(2U, decodedRead->Arguments().size());
	EXPECT_EQ(L"path", decodedRead->Arguments()[0].name);
	EXPECT_EQ(L"open", decodedRead->Arguments()[1].value);

	ControlSenpRpcRequest poll = read;
	poll.SetOperation(EControlSenpRpcOperation::PollRead);
	poll.SetReadId(L"");
	poll.SetToolId(L"");
	poll.SetToolOperation(L"");
	poll.SetArguments({});
	const auto encodedPoll = EncodeControlSenpRpcRequest(poll);
	ASSERT_TRUE(encodedPoll);
	EXPECT_TRUE(DecodeControlSenpRpcRequest(*encodedPoll));

	ControlSenpRpcRequest cancel = poll;
	cancel.SetOperation(EControlSenpRpcOperation::CancelRead);
	cancel.SetReadId(L"issues:open:1");
	const auto encodedCancel = EncodeControlSenpRpcRequest(cancel);
	ASSERT_TRUE(encodedCancel);
	EXPECT_TRUE(DecodeControlSenpRpcRequest(*encodedCancel));

	ControlSenpRpcRequest resource = poll;
	resource.SetOperation(EControlSenpRpcOperation::ReadResource);
	resource.SetResourceHandle(L"log-1");
	resource.SetOffset(4096);
	resource.SetLength(1024);
	const auto encodedResource = EncodeControlSenpRpcRequest(resource);
	ASSERT_TRUE(encodedResource);
	const auto decodedResource = DecodeControlSenpRpcRequest(*encodedResource);
	ASSERT_TRUE(decodedResource);
	EXPECT_EQ(4096U, decodedResource->Offset());
	EXPECT_EQ(1024U, decodedResource->Length());

	ControlSenpRpcRequest release = poll;
	release.SetOperation(EControlSenpRpcOperation::ReleaseResource);
	release.SetResourceHandle(L"log-1");
	const auto encodedRelease = EncodeControlSenpRpcRequest(release);
	ASSERT_TRUE(encodedRelease);
	EXPECT_TRUE(DecodeControlSenpRpcRequest(*encodedRelease));
}

TEST(ControlSenpRpc, RejectsMembersThatDoNotBelongToTheOperation)
{
	auto grantWithRead = IssueGrant();
	grantWithRead.SetReadId(L"issues:open:1");
	EXPECT_FALSE(EncodeControlSenpRpcRequest(grantWithRead));

	auto pollWithTool = StartRead();
	pollWithTool.SetOperation(EControlSenpRpcOperation::PollRead);
	EXPECT_FALSE(EncodeControlSenpRpcRequest(pollWithTool));

	auto readWithCapability = StartRead();
	readWithCapability.SetCapabilities(1);
	EXPECT_FALSE(EncodeControlSenpRpcRequest(readWithCapability));

	auto readWithoutGrant = StartRead();
	readWithoutGrant.SetGrantId("");
	EXPECT_FALSE(EncodeControlSenpRpcRequest(readWithoutGrant));

	auto resourceWithoutLength = StartRead();
	resourceWithoutLength.SetOperation(EControlSenpRpcOperation::ReadResource);
	resourceWithoutLength.SetReadId(L"");
	resourceWithoutLength.SetToolId(L"");
	resourceWithoutLength.SetToolOperation(L"");
	resourceWithoutLength.SetArguments({});
	resourceWithoutLength.SetResourceHandle(L"log-1");
	EXPECT_FALSE(EncodeControlSenpRpcRequest(resourceWithoutLength));
}

TEST(ControlSenpRpc, RejectsUnusableOwnerIdentity)
{
	auto zeroGeneration = StartRead();
	zeroGeneration.Owner().SetGeneration(0);
	EXPECT_FALSE(EncodeControlSenpRpcRequest(zeroGeneration));

	auto noExtension = StartRead();
	noExtension.Owner().SetExtensionId(L"");
	EXPECT_FALSE(EncodeControlSenpRpcRequest(noExtension));

	auto noProfile = StartRead();
	noProfile.SetProfileId(L"");
	EXPECT_FALSE(EncodeControlSenpRpcRequest(noProfile));

	auto negativeAccount = StartRead();
	negativeAccount.Owner().SetAccountGeneration(-1);
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
	granted.SetStatus(EControlSenpRpcStatus::Succeeded);
	granted.SetGrantId("grant-1");
	granted.SetExpiresAtMilliseconds(1234567);
	const auto encodedGrant = EncodeControlSenpRpcResponse(granted);
	ASSERT_TRUE(encodedGrant);
	const auto decodedGrant = DecodeControlSenpRpcResponse(*encodedGrant);
	ASSERT_TRUE(decodedGrant);
	EXPECT_EQ("grant-1", decodedGrant->GrantId());
	EXPECT_EQ(1234567U, decodedGrant->ExpiresAtMilliseconds());
	EXPECT_FALSE(decodedGrant->HasCompletion());

	ControlSenpRpcResponse completed;
	completed.SetStatus(EControlSenpRpcStatus::Succeeded);
	completed.SetHasCompletion(true);
	completed.SetCompletion({ L"issues:open:1", senp::effect::CompletionStatus::Succeeded,
		LR"({"body":[],"nextPage":2})", L"" });
	const auto encodedCompleted = EncodeControlSenpRpcResponse(completed);
	ASSERT_TRUE(encodedCompleted);
	const auto decodedCompleted = DecodeControlSenpRpcResponse(*encodedCompleted);
	ASSERT_TRUE(decodedCompleted);
	EXPECT_TRUE(decodedCompleted->HasCompletion());
	EXPECT_EQ(completed.Completion(), decodedCompleted->Completion());

	ControlSenpRpcResponse chunk;
	chunk.SetStatus(EControlSenpRpcStatus::Succeeded);
	chunk.SetResourceHandle(L"log-1");
	chunk.SetResourceOffset(64);
	chunk.SetResourceBytes("run step output");
	chunk.SetResourceState(static_cast<std::uint8_t>(senp::TextResourceState::Complete));
	chunk.SetResourceEnd(static_cast<std::uint8_t>(senp::TextResourceEnd::Complete));
	chunk.SetResourceLength(79);
	chunk.SetResourceRevision(11);
	const auto encodedChunk = EncodeControlSenpRpcResponse(chunk);
	ASSERT_TRUE(encodedChunk);
	const auto decodedChunk = DecodeControlSenpRpcResponse(*encodedChunk);
	ASSERT_TRUE(decodedChunk);
	EXPECT_EQ(chunk.ResourceBytes(), decodedChunk->ResourceBytes());
	EXPECT_EQ(64U, decodedChunk->ResourceOffset());
	// The editor rebuilds a whole chunk from these, so every member its text
	// surface validates has to survive the round trip - not only the bytes.
	EXPECT_EQ(chunk.ResourceState(), decodedChunk->ResourceState());
	EXPECT_EQ(chunk.ResourceEnd(), decodedChunk->ResourceEnd());
	EXPECT_EQ(79U, decodedChunk->ResourceLength());
	EXPECT_EQ(11, decodedChunk->ResourceRevision());
}

TEST(ControlSenpRpc, RejectsIncoherentResponses)
{
	ControlSenpRpcResponse completionWithoutFlag;
	completionWithoutFlag.SetStatus(EControlSenpRpcStatus::Succeeded);
	completionWithoutFlag.Completion().readId = L"issues:open:1";
	EXPECT_FALSE(EncodeControlSenpRpcResponse(completionWithoutFlag));

	ControlSenpRpcResponse flagWithoutRead;
	flagWithoutRead.SetStatus(EControlSenpRpcStatus::Succeeded);
	flagWithoutRead.SetHasCompletion(true);
	EXPECT_FALSE(EncodeControlSenpRpcResponse(flagWithoutRead));

	ControlSenpRpcResponse chunkWithoutHandle;
	chunkWithoutHandle.SetStatus(EControlSenpRpcStatus::Succeeded);
	chunkWithoutHandle.SetResourceBytes("bytes");
	EXPECT_FALSE(EncodeControlSenpRpcResponse(chunkWithoutHandle));

	// A refusal names no resource, so it may describe none either. Each of these
	// alone would let an answer about nothing be read as a chunk of whichever
	// resource the editor is currently filling.
	ControlSenpRpcResponse offsetWithoutHandle;
	offsetWithoutHandle.SetStatus(EControlSenpRpcStatus::NotFound);
	offsetWithoutHandle.SetResourceOffset(64);
	EXPECT_FALSE(EncodeControlSenpRpcResponse(offsetWithoutHandle));

	ControlSenpRpcResponse lengthWithoutHandle;
	lengthWithoutHandle.SetStatus(EControlSenpRpcStatus::NotFound);
	lengthWithoutHandle.SetResourceLength(128);
	EXPECT_FALSE(EncodeControlSenpRpcResponse(lengthWithoutHandle));

	ControlSenpRpcResponse revisionWithoutHandle;
	revisionWithoutHandle.SetStatus(EControlSenpRpcStatus::NotFound);
	revisionWithoutHandle.SetResourceRevision(11);
	EXPECT_FALSE(EncodeControlSenpRpcResponse(revisionWithoutHandle));

	ControlSenpRpcResponse endWithoutHandle;
	endWithoutHandle.SetStatus(EControlSenpRpcStatus::NotFound);
	endWithoutHandle.SetResourceState(static_cast<std::uint8_t>(senp::TextResourceState::Complete));
	endWithoutHandle.SetResourceEnd(static_cast<std::uint8_t>(senp::TextResourceEnd::Complete));
	EXPECT_FALSE(EncodeControlSenpRpcResponse(endWithoutHandle));

	// A chunk reaching past the resource it belongs to describes no resource the
	// store could have produced, and the editor would append bytes it never held.
	ControlSenpRpcResponse pastTheEnd;
	pastTheEnd.SetStatus(EControlSenpRpcStatus::Succeeded);
	pastTheEnd.SetResourceHandle(L"log-1");
	pastTheEnd.SetResourceOffset(64);
	pastTheEnd.SetResourceBytes("run step output");
	pastTheEnd.SetResourceLength(70);
	EXPECT_FALSE(EncodeControlSenpRpcResponse(pastTheEnd));

	ControlSenpRpcResponse beyondTheResourceBound;
	beyondTheResourceBound.SetStatus(EControlSenpRpcStatus::Succeeded);
	beyondTheResourceBound.SetResourceHandle(L"log-1");
	beyondTheResourceBound.SetResourceLength(kControlSenpRpcMaximumResourceBytes + 1);
	EXPECT_FALSE(EncodeControlSenpRpcResponse(beyondTheResourceBound));

	// Outside either enumeration. The editor tests a decoded state against the
	// states it handles, so one it has never heard of must not decode at all.
	ControlSenpRpcResponse unknownState;
	unknownState.SetStatus(EControlSenpRpcStatus::Succeeded);
	unknownState.SetResourceHandle(L"log-1");
	unknownState.SetResourceState(200);
	EXPECT_FALSE(EncodeControlSenpRpcResponse(unknownState));

	ControlSenpRpcResponse unknownEnd;
	unknownEnd.SetStatus(EControlSenpRpcStatus::Succeeded);
	unknownEnd.SetResourceHandle(L"log-1");
	unknownEnd.SetResourceEnd(200);
	EXPECT_FALSE(EncodeControlSenpRpcResponse(unknownEnd));

	// A negative revision would encode as an enormous unsigned value and decode
	// back as a different one, which is a resource identity, not a rounding error.
	ControlSenpRpcResponse negativeRevision;
	negativeRevision.SetStatus(EControlSenpRpcStatus::Succeeded);
	negativeRevision.SetResourceHandle(L"log-1");
	negativeRevision.SetResourceRevision(-1);
	EXPECT_FALSE(EncodeControlSenpRpcResponse(negativeRevision));

	ControlSenpRpcResponse oversizedData;
	oversizedData.SetStatus(EControlSenpRpcStatus::Succeeded);
	oversizedData.SetHasCompletion(true);
	oversizedData.Completion().readId = L"issues:open:1";
	oversizedData.Completion().data.assign(kControlSenpRpcMaximumToolDataBytes + 1, L'a');
	EXPECT_FALSE(EncodeControlSenpRpcResponse(oversizedData));
}

TEST(ControlSenpRpc, RoundTripsTheOwnerFreeAccountQuery)
{
	const auto encoded = EncodeControlSenpRpcRequest(QueryAccount());
	ASSERT_TRUE(encoded);
	const auto decoded = DecodeControlSenpRpcRequest(*encoded);
	ASSERT_TRUE(decoded);
	EXPECT_EQ(EControlSenpRpcOperation::QueryAccount, decoded->Operation());
	EXPECT_EQ(L"profile-1", decoded->ProfileId());
	EXPECT_TRUE(decoded->Owner() == ControlSenpRpcOwner{});
}

TEST(ControlSenpRpc, RejectsAnAccountQueryThatCarriesAnOwnerOrAGrant)
{
	// The editor asks this before it can know an account generation at all, so an
	// owner on it could only be a claim that nothing downstream rechecks.
	auto withOwner = QueryAccount();
	withOwner.SetOwner(Owner());
	EXPECT_FALSE(EncodeControlSenpRpcRequest(withOwner));

	auto withGrant = QueryAccount();
	withGrant.SetGrantId("grant-1");
	EXPECT_FALSE(EncodeControlSenpRpcRequest(withGrant));

	auto withCapability = QueryAccount();
	withCapability.SetCapabilities(1);
	EXPECT_FALSE(EncodeControlSenpRpcRequest(withCapability));

	auto withoutProfile = QueryAccount();
	withoutProfile.SetProfileId(L"");
	EXPECT_FALSE(EncodeControlSenpRpcRequest(withoutProfile));
}

TEST(ControlSenpRpc, RoundTripsTheAccountAnswerAndRefusesAnUnusableOne)
{
	ControlSenpRpcResponse answered;
	answered.SetStatus(EControlSenpRpcStatus::Succeeded);
	answered.SetAccountGeneration(4);
	answered.SetAccountState(EControlSenpAccountState::ReauthenticationRequired);
	const auto encoded = EncodeControlSenpRpcResponse(answered);
	ASSERT_TRUE(encoded);
	const auto decoded = DecodeControlSenpRpcResponse(*encoded);
	ASSERT_TRUE(decoded);
	EXPECT_EQ(4, decoded->AccountGeneration());
	EXPECT_EQ(EControlSenpAccountState::ReauthenticationRequired, decoded->AccountState());

	auto negative = answered;
	negative.SetAccountGeneration(-1);
	EXPECT_FALSE(EncodeControlSenpRpcResponse(negative));

	auto foreignState = answered;
	foreignState.SetAccountState(static_cast<EControlSenpAccountState>(9));
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
	EXPECT_EQ(EControlSenpRpcOperation::AdoptWorkspace, decoded->Operation());
	EXPECT_EQ(L"profile-1", decoded->ProfileId());
	// The declaration precedes every owner and names no repository: the control
	// side derives that from the remotes it finds at these folders.
	EXPECT_TRUE(decoded->Owner() == ControlSenpRpcOwner{});
	EXPECT_TRUE(decoded->GrantId().empty());
	EXPECT_EQ(declared.Workspace(), decoded->Workspace());
	// Order is part of the identity of a multi-root workspace, so it is asserted
	// rather than left to the set of folders happening to match.
	ASSERT_EQ(2U, decoded->Workspace().Folders().size());
	EXPECT_EQ(L"file:///c:/work/repo", decoded->Workspace().Folders()[0]);
	EXPECT_EQ(L"file:///c:/work/docs", decoded->Workspace().Folders()[1]);

	// A window that has no folder to select from says so. That is a declaration,
	// not an absent one, and it must survive the round trip as one.
	auto empty = declared;
	empty.Workspace().SetFolders({});
	const auto encodedEmpty = EncodeControlSenpRpcRequest(empty);
	ASSERT_TRUE(encodedEmpty);
	const auto decodedEmpty = DecodeControlSenpRpcRequest(*encodedEmpty);
	ASSERT_TRUE(decodedEmpty);
	EXPECT_TRUE(decodedEmpty->Workspace().Folders().empty());
	EXPECT_EQ(11, decodedEmpty->Workspace().Revision());
	EXPECT_FALSE(decodedEmpty->Workspace().Empty());
}

TEST(ControlSenpRpc, RejectsAWorkspaceAdoptionThatCarriesAnOwnerOrCannotBeChecked)
{
	auto withOwner = AdoptWorkspace();
	withOwner.SetOwner(Owner());
	EXPECT_FALSE(EncodeControlSenpRpcRequest(withOwner));

	auto withGrant = AdoptWorkspace();
	withGrant.SetGrantId("grant-1");
	EXPECT_FALSE(EncodeControlSenpRpcRequest(withGrant));

	auto withRead = AdoptWorkspace();
	withRead.SetReadId(L"issues:open:1");
	EXPECT_FALSE(EncodeControlSenpRpcRequest(withRead));

	auto withoutProfile = AdoptWorkspace();
	withoutProfile.SetProfileId(L"");
	EXPECT_FALSE(EncodeControlSenpRpcRequest(withoutProfile));

	// A revision of zero observed nothing, so a staleness check against it would
	// pass for a workspace that was never captured.
	auto withoutRevision = AdoptWorkspace();
	withoutRevision.Workspace().SetRevision(0);
	EXPECT_FALSE(EncodeControlSenpRpcRequest(withoutRevision));

	auto withoutGeneration = AdoptWorkspace();
	withoutGeneration.Workspace().SetGeneration(0);
	EXPECT_FALSE(EncodeControlSenpRpcRequest(withoutGeneration));

	auto withNamelessFolder = AdoptWorkspace();
	withNamelessFolder.Workspace().AddFolder(L"");
	EXPECT_FALSE(EncodeControlSenpRpcRequest(withNamelessFolder));

	// The control side inspects every declared folder, so the bound is what keeps
	// one declaration from becoming unbounded work on the refresh worker.
	auto tooMany = AdoptWorkspace();
	tooMany.Workspace().SetFolders(
		std::vector<std::wstring>(kControlSenpRpcMaximumWorkspaceFolders + 1, L"file:///c:/work"));
	EXPECT_FALSE(EncodeControlSenpRpcRequest(tooMany));
}

TEST(ControlSenpRpc, RejectsADeclaredFolderCountNoEncoderWouldHaveWritten)
{
	auto empty = AdoptWorkspace();
	empty.Workspace().SetFolders({});
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
	readWithWorkspace.SetWorkspace(AdoptWorkspace().Workspace());
	EXPECT_FALSE(EncodeControlSenpRpcRequest(readWithWorkspace));

	auto grantWithWorkspace = IssueGrant();
	grantWithWorkspace.SetWorkspace(AdoptWorkspace().Workspace());
	EXPECT_FALSE(EncodeControlSenpRpcRequest(grantWithWorkspace));

	auto accountWithWorkspace = QueryAccount();
	accountWithWorkspace.SetWorkspace(AdoptWorkspace().Workspace());
	EXPECT_FALSE(EncodeControlSenpRpcRequest(accountWithWorkspace));

	// A revision alone is still a declaration, so it is refused for the same
	// reason a fully populated one is.
	auto readWithRevision = StartRead();
	readWithRevision.Workspace().SetRevision(11);
	EXPECT_FALSE(EncodeControlSenpRpcRequest(readWithRevision));
}

TEST(ControlSenpRpc, MapsOwnerIdentityWithoutLoss)
{
	const auto owner = Owner();
	const auto identity = ToContributionOwner(owner);
	EXPECT_EQ(owner.ExtensionId(), identity.extensionId);
	EXPECT_EQ(owner.PackageDigest(), identity.packageDigest);
	EXPECT_EQ(owner.Generation(), identity.generation);
	EXPECT_EQ(owner, FromContributionOwner(identity));
}

} // namespace platform::controlipc
