/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "platform/secrets/CSecretVaultExtensionGrantAuthority.h"

#include <utility>

namespace platform::secrets {
namespace {

constexpr std::string_view kProfileId = "0123456789abcdef0123456789abcdef";
constexpr std::uint64_t kControlGeneration = 77;

SecretVaultExtensionGrantAuthorityActivateRequest Activate(std::uint64_t expectedRevision = 0)
{
	return { expectedRevision, "host-session", 5, { "publisher.one" }, { 101 } };
}

SecretVaultExtensionGrantAuthorityIssueRequest Issue(std::string extensionId = "publisher.one")
{
	return { std::string(kProfileId), kControlGeneration, "host-session", 5, 101, std::move(extensionId) };
}

TEST(SecretVaultExtensionGrantAuthority, RequiresExactProfileGenerationHostLeaseAndCanonicalExtension)
{
	CSecretVaultExtensionGrantAuthority authority(std::string(kProfileId), kControlGeneration);
	const auto activated = authority.ActivateOrReplace(Activate());
	ASSERT_EQ(ESecretVaultExtensionGrantAuthorityStatus::Applied, activated.status);
	EXPECT_EQ(1u, activated.revision);

	const auto authorized = authority.AuthorizeIssue(Issue());
	ASSERT_EQ(ESecretVaultExtensionGrantAuthorizationStatus::Authorized, authorized.status);
	EXPECT_EQ("publisher.one", authorized.binding.extensionId);
	EXPECT_EQ("host-session", authorized.binding.session.extensionHostSessionId);
	EXPECT_EQ(101u, authorized.binding.session.clientProcessId);

	auto profileSpoof = Issue(); profileSpoof.profileId = "fedcba9876543210fedcba9876543210";
	EXPECT_EQ(ESecretVaultExtensionGrantAuthorizationStatus::AccessDenied, authority.AuthorizeIssue(profileSpoof).status);
	auto controlSpoof = Issue(); controlSpoof.controlConnectionGeneration = kControlGeneration + 1;
	EXPECT_EQ(ESecretVaultExtensionGrantAuthorizationStatus::AccessDenied, authority.AuthorizeIssue(controlSpoof).status);
	auto hostSpoof = Issue(); hostSpoof.hostSessionId = "host-session-extra";
	EXPECT_EQ(ESecretVaultExtensionGrantAuthorizationStatus::AccessDenied, authority.AuthorizeIssue(hostSpoof).status);
	auto generationSpoof = Issue(); generationSpoof.hostGeneration = 6;
	EXPECT_EQ(ESecretVaultExtensionGrantAuthorizationStatus::AccessDenied, authority.AuthorizeIssue(generationSpoof).status);
	auto pidSpoof = Issue(); pidSpoof.clientProcessId = 102;
	EXPECT_EQ(ESecretVaultExtensionGrantAuthorizationStatus::AccessDenied, authority.AuthorizeIssue(pidSpoof).status);
	EXPECT_EQ(ESecretVaultExtensionGrantAuthorizationStatus::AccessDenied, authority.AuthorizeIssue(Issue("publisher.one-extra")).status);
	EXPECT_EQ(ESecretVaultExtensionGrantAuthorizationStatus::AccessDenied, authority.AuthorizeIssue(Issue("Publisher.One")).status);
}

TEST(SecretVaultExtensionGrantAuthority, MutationsAreRevisionAndGenerationFencedAndDisableExactlyOneExtension)
{
	CSecretVaultExtensionGrantAuthority authority(std::string(kProfileId), kControlGeneration);
	const auto activated = authority.ActivateOrReplace(Activate());
	ASSERT_EQ(ESecretVaultExtensionGrantAuthorityStatus::Applied, activated.status);

	SecretVaultExtensionGrantAuthorityEditorLeaseMutation stale{ { 0, "host-session", 5 }, 102 };
	EXPECT_EQ(ESecretVaultExtensionGrantAuthorityStatus::RevisionMismatch, authority.RegisterEditorProcess(stale).status);
	SecretVaultExtensionGrantAuthorityEditorLeaseMutation spoofedHost{ { activated.revision, "host-session", 6 }, 102 };
	EXPECT_EQ(ESecretVaultExtensionGrantAuthorityStatus::GenerationMismatch, authority.RegisterEditorProcess(spoofedHost).status);
	SecretVaultExtensionGrantAuthorityApprovedExtensionsMutation replace{ { activated.revision, "host-session", 5 },
		{ "publisher.one", "publisher.two" } };
	const auto replaced = authority.ReplaceApprovedExtensions(replace);
	ASSERT_EQ(ESecretVaultExtensionGrantAuthorityStatus::Applied, replaced.status);
	EXPECT_EQ(ESecretVaultExtensionGrantAuthorizationStatus::Authorized, authority.AuthorizeIssue(Issue("publisher.two")).status);
	const auto disabled = authority.DisableExtension({ { replaced.revision, "host-session", 5 }, "publisher.one" });
	ASSERT_EQ(ESecretVaultExtensionGrantAuthorityStatus::Applied, disabled.status);
	EXPECT_EQ(ESecretVaultExtensionGrantAuthorizationStatus::AccessDenied, authority.AuthorizeIssue(Issue()).status);
	EXPECT_EQ(ESecretVaultExtensionGrantAuthorizationStatus::Authorized, authority.AuthorizeIssue(Issue("publisher.two")).status);

	EXPECT_EQ(ESecretVaultExtensionGrantAuthorityStatus::InvalidRequest,
		authority.ReplaceApprovedExtensions({ { disabled.revision, "host-session", 5 }, { "Publisher.Two" } }).status);
	EXPECT_EQ(ESecretVaultExtensionGrantAuthorityStatus::Applied,
		authority.Deactivate({ disabled.revision, "host-session", 5 }).status);
	EXPECT_EQ(ESecretVaultExtensionGrantAuthorizationStatus::AccessDenied, authority.AuthorizeIssue(Issue("publisher.two")).status);
}

TEST(SecretVaultExtensionGrantAuthority, ExplicitUnregisterOwnsEditorLeaseLifecycleAndStopIsTerminal)
{
	CSecretVaultExtensionGrantAuthority authority(std::string(kProfileId), kControlGeneration);
	const auto activated = authority.ActivateOrReplace(Activate());
	ASSERT_EQ(ESecretVaultExtensionGrantAuthorityStatus::Applied, activated.status);
	const auto authorization = authority.AuthorizeRevokeSession({ std::string(kProfileId), kControlGeneration, 101 });
	ASSERT_EQ(ESecretVaultExtensionGrantAuthorizationStatus::Authorized, authorization.status);
	EXPECT_EQ("host-session", authorization.session.extensionHostSessionId);
	EXPECT_EQ(ESecretVaultExtensionGrantAuthorityStatus::Applied,
		authority.UnregisterEditorProcess({ { activated.revision, "host-session", 5 }, 101 }).status);
	EXPECT_EQ(ESecretVaultExtensionGrantAuthorizationStatus::AccessDenied,
		authority.AuthorizeRevokeSession({ std::string(kProfileId), kControlGeneration, 101 }).status);
	EXPECT_EQ(ESecretVaultExtensionGrantAuthorityStatus::Stopped, authority.Stop());
	EXPECT_EQ(ESecretVaultExtensionGrantAuthorizationStatus::Stopped, authority.AuthorizeIssue(Issue()).status);
	EXPECT_EQ(ESecretVaultExtensionGrantAuthorityStatus::Stopped, authority.ActivateOrReplace(Activate(2)).status);
}

TEST(SecretVaultExtensionGrantAuthority, RejectsDuplicateAndBoundedActivationInputs)
{
	CSecretVaultExtensionGrantAuthority authority(std::string(kProfileId), kControlGeneration,
		{ 1, 1 });
	auto duplicateExtensions = Activate(); duplicateExtensions.approvedExtensionIds.push_back("publisher.one");
	EXPECT_EQ(ESecretVaultExtensionGrantAuthorityStatus::InvalidRequest, authority.ActivateOrReplace(duplicateExtensions).status);
	auto duplicatePids = Activate(); duplicatePids.editorProcessIds.push_back(101);
	EXPECT_EQ(ESecretVaultExtensionGrantAuthorityStatus::InvalidRequest, authority.ActivateOrReplace(duplicatePids).status);
	auto overBound = Activate(); overBound.approvedExtensionIds = { "publisher.one", "publisher.two" };
	EXPECT_EQ(ESecretVaultExtensionGrantAuthorityStatus::InvalidRequest, authority.ActivateOrReplace(overBound).status);
}

} // namespace
} // namespace platform::secrets
