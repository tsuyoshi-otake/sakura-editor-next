/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include "config/WorkspaceTrustPromptPolicy.h"

namespace {

using config::EWorkspaceTrustStartupPrompt;
using config::EWorkspaceTrustStartupPromptDecision;
using config::EWorkspaceTrustState;
using config::EWorkspaceTrustUntrustedFiles;
using config::EWorkspaceTrustUntrustedFilesDecision;
using config::ParseWorkspaceTrustStartupPrompt;
using config::ParseWorkspaceTrustUntrustedFiles;
using config::ResolveWorkspaceTrustStartupPrompt;
using config::ResolveWorkspaceTrustUntrustedFiles;
using config::WorkspaceTrustStartupPromptRequest;
using config::WorkspaceTrustUntrustedFilesRequest;

/*!
	@brief A window that would be prompted: untrusted, feature on, something to grant.

	Every startup-prompt test below starts from this and changes exactly the one
	field under test, so a passing assertion names the field that caused it.
 */
WorkspaceTrustStartupPromptRequest PromptableRequest(EWorkspaceTrustStartupPrompt setting)
{
	WorkspaceTrustStartupPromptRequest request;
	request.setting = setting;
	request.state = EWorkspaceTrustState::Unknown;
	request.featureEnabled = true;
	request.hasGrantableOption = true;
	request.promptRecordReadable = true;
	request.promptAlreadyShown = false;
	return request;
}

//! A trusted window opening a resource its trusted list does not cover.
WorkspaceTrustUntrustedFilesRequest LooseFileRequest(EWorkspaceTrustUntrustedFiles setting)
{
	WorkspaceTrustUntrustedFilesRequest request;
	request.setting = setting;
	request.state = EWorkspaceTrustState::Trusted;
	request.featureEnabled = true;
	request.allResourcesTrusted = false;
	request.acceptRecordReadable = true;
	request.alreadyAccepted = false;
	request.newWindowSupported = false;
	return request;
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

TEST(WorkspaceTrustPromptPolicyTest, StartupPromptParsesEveryUpstreamValue)
{
	EXPECT_EQ(EWorkspaceTrustStartupPrompt::Always, ParseWorkspaceTrustStartupPrompt(L"always"));
	EXPECT_EQ(EWorkspaceTrustStartupPrompt::Once, ParseWorkspaceTrustStartupPrompt(L"once"));
	EXPECT_EQ(EWorkspaceTrustStartupPrompt::Never, ParseWorkspaceTrustStartupPrompt(L"never"));
}

/*!
	The descriptor constrains the value, but the parser must still be safe on its
	own: an unrecognized value resolves to upstream's default, which is also the
	value that cannot grant anything.
 */
TEST(WorkspaceTrustPromptPolicyTest, StartupPromptParseFailsClosedOnUnknownValue)
{
	EXPECT_EQ(EWorkspaceTrustStartupPrompt::Never, ParseWorkspaceTrustStartupPrompt(L""));
	EXPECT_EQ(EWorkspaceTrustStartupPrompt::Never, ParseWorkspaceTrustStartupPrompt(L"Always"));
	EXPECT_EQ(EWorkspaceTrustStartupPrompt::Never, ParseWorkspaceTrustStartupPrompt(L"yes"));
}

TEST(WorkspaceTrustPromptPolicyTest, UntrustedFilesParsesEveryUpstreamValue)
{
	EXPECT_EQ(EWorkspaceTrustUntrustedFiles::Prompt, ParseWorkspaceTrustUntrustedFiles(L"prompt"));
	EXPECT_EQ(EWorkspaceTrustUntrustedFiles::Open, ParseWorkspaceTrustUntrustedFiles(L"open"));
	EXPECT_EQ(EWorkspaceTrustUntrustedFiles::NewWindow, ParseWorkspaceTrustUntrustedFiles(L"newWindow"));
}

//! "prompt" asks rather than deciding for the user, so it is the safe fallback.
TEST(WorkspaceTrustPromptPolicyTest, UntrustedFilesParseFailsClosedOnUnknownValue)
{
	EXPECT_EQ(EWorkspaceTrustUntrustedFiles::Prompt, ParseWorkspaceTrustUntrustedFiles(L""));
	EXPECT_EQ(EWorkspaceTrustUntrustedFiles::Prompt, ParseWorkspaceTrustUntrustedFiles(L"newwindow"));
	EXPECT_EQ(EWorkspaceTrustUntrustedFiles::Prompt, ParseWorkspaceTrustUntrustedFiles(L"always"));
}

// ---------------------------------------------------------------------------
// Startup prompt
// ---------------------------------------------------------------------------

TEST(WorkspaceTrustPromptPolicyTest, StartupPromptAlwaysShows)
{
	EXPECT_EQ(EWorkspaceTrustStartupPromptDecision::Show,
		ResolveWorkspaceTrustStartupPrompt(PromptableRequest(EWorkspaceTrustStartupPrompt::Always)));
}

//! "always" means every launch, so a record of a previous prompt changes nothing.
TEST(WorkspaceTrustPromptPolicyTest, StartupPromptAlwaysIgnoresTheShownRecord)
{
	auto request = PromptableRequest(EWorkspaceTrustStartupPrompt::Always);
	request.promptAlreadyShown = true;
	EXPECT_EQ(EWorkspaceTrustStartupPromptDecision::Show, ResolveWorkspaceTrustStartupPrompt(request));

	request.promptRecordReadable = false;
	EXPECT_EQ(EWorkspaceTrustStartupPromptDecision::Show, ResolveWorkspaceTrustStartupPrompt(request));
}

TEST(WorkspaceTrustPromptPolicyTest, StartupPromptNeverSkips)
{
	EXPECT_EQ(EWorkspaceTrustStartupPromptDecision::SkipSettingNever,
		ResolveWorkspaceTrustStartupPrompt(PromptableRequest(EWorkspaceTrustStartupPrompt::Never)));
}

TEST(WorkspaceTrustPromptPolicyTest, StartupPromptOnceShowsWhenNothingWasRecorded)
{
	EXPECT_EQ(EWorkspaceTrustStartupPromptDecision::Show,
		ResolveWorkspaceTrustStartupPrompt(PromptableRequest(EWorkspaceTrustStartupPrompt::Once)));
}

TEST(WorkspaceTrustPromptPolicyTest, StartupPromptOnceSkipsWhenTheRecordSaysItWasShown)
{
	auto request = PromptableRequest(EWorkspaceTrustStartupPrompt::Once);
	request.promptAlreadyShown = true;
	EXPECT_EQ(EWorkspaceTrustStartupPromptDecision::SkipAlreadyShown,
		ResolveWorkspaceTrustStartupPrompt(request));
}

/*!
	The case that must never be allowed to degrade into a skip.

	"once" without a readable record cannot honour "once"; prompting is the only
	direction that withholds trust rather than assuming a prompt already happened.
	The distinct decision value exists so the caller can report the degradation.
 */
TEST(WorkspaceTrustPromptPolicyTest, StartupPromptOnceShowsWhenTheRecordCannotBeRead)
{
	auto request = PromptableRequest(EWorkspaceTrustStartupPrompt::Once);
	request.promptRecordReadable = false;
	EXPECT_EQ(EWorkspaceTrustStartupPromptDecision::ShowRecordUnreadable,
		ResolveWorkspaceTrustStartupPrompt(request));

	// An unreadable record must not be salvaged by whatever value happens to sit
	// in the field next to it.
	request.promptAlreadyShown = true;
	EXPECT_EQ(EWorkspaceTrustStartupPromptDecision::ShowRecordUnreadable,
		ResolveWorkspaceTrustStartupPrompt(request));
}

TEST(WorkspaceTrustPromptPolicyTest, StartupPromptSkipsWhenTheFeatureIsDisabled)
{
	auto request = PromptableRequest(EWorkspaceTrustStartupPrompt::Always);
	request.featureEnabled = false;
	EXPECT_EQ(EWorkspaceTrustStartupPromptDecision::SkipFeatureDisabled,
		ResolveWorkspaceTrustStartupPrompt(request));
}

TEST(WorkspaceTrustPromptPolicyTest, StartupPromptSkipsAnAlreadyTrustedWindow)
{
	auto request = PromptableRequest(EWorkspaceTrustStartupPrompt::Always);
	request.state = EWorkspaceTrustState::Trusted;
	EXPECT_EQ(EWorkspaceTrustStartupPromptDecision::SkipAlreadyTrusted,
		ResolveWorkspaceTrustStartupPrompt(request));
}

/*!
	Only Trusted is trusted.

	Untrusted is an explicit denial and Unknown is withheld trust; both are states
	the prompt exists to resolve, so neither may short-circuit it.
 */
TEST(WorkspaceTrustPromptPolicyTest, StartupPromptTreatsUntrustedAsNotTrusted)
{
	auto request = PromptableRequest(EWorkspaceTrustStartupPrompt::Always);
	request.state = EWorkspaceTrustState::Untrusted;
	EXPECT_EQ(EWorkspaceTrustStartupPromptDecision::Show, ResolveWorkspaceTrustStartupPrompt(request));
}

//! A dialog whose every button is absent reports a decision it denies the means to make.
TEST(WorkspaceTrustPromptPolicyTest, StartupPromptSkipsWhenNoGrantWouldApply)
{
	auto request = PromptableRequest(EWorkspaceTrustStartupPrompt::Always);
	request.hasGrantableOption = false;
	EXPECT_EQ(EWorkspaceTrustStartupPromptDecision::SkipNothingToGrant,
		ResolveWorkspaceTrustStartupPrompt(request));
}

/*!
	Order is part of the contract, not an implementation detail.

	The feature switch outranks the trust state, which outranks the setting, which
	outranks the grantable-option check -- mirroring upstream's showModalOnStart.
	Asserting the winner with every lower-priority skip also true is what keeps a
	reordering from passing every single-cause test above.
 */
TEST(WorkspaceTrustPromptPolicyTest, StartupPromptSkipPrecedenceIsFixed)
{
	WorkspaceTrustStartupPromptRequest request;
	request.setting = EWorkspaceTrustStartupPrompt::Never;
	request.state = EWorkspaceTrustState::Trusted;
	request.featureEnabled = false;
	request.hasGrantableOption = false;
	EXPECT_EQ(EWorkspaceTrustStartupPromptDecision::SkipFeatureDisabled,
		ResolveWorkspaceTrustStartupPrompt(request));

	request.featureEnabled = true;
	EXPECT_EQ(EWorkspaceTrustStartupPromptDecision::SkipAlreadyTrusted,
		ResolveWorkspaceTrustStartupPrompt(request));

	request.state = EWorkspaceTrustState::Unknown;
	EXPECT_EQ(EWorkspaceTrustStartupPromptDecision::SkipSettingNever,
		ResolveWorkspaceTrustStartupPrompt(request));

	request.setting = EWorkspaceTrustStartupPrompt::Always;
	EXPECT_EQ(EWorkspaceTrustStartupPromptDecision::SkipNothingToGrant,
		ResolveWorkspaceTrustStartupPrompt(request));
}

//! The default-constructed request must not prompt: it knows nothing yet.
TEST(WorkspaceTrustPromptPolicyTest, StartupPromptDefaultRequestSkips)
{
	EXPECT_EQ(EWorkspaceTrustStartupPromptDecision::SkipSettingNever,
		ResolveWorkspaceTrustStartupPrompt(WorkspaceTrustStartupPromptRequest{}));
}

// ---------------------------------------------------------------------------
// Untrusted files
// ---------------------------------------------------------------------------

TEST(WorkspaceTrustPromptPolicyTest, UntrustedFilesPromptAsksForALooseFile)
{
	EXPECT_EQ(EWorkspaceTrustUntrustedFilesDecision::Prompt,
		ResolveWorkspaceTrustUntrustedFiles(LooseFileRequest(EWorkspaceTrustUntrustedFiles::Prompt)));
}

TEST(WorkspaceTrustPromptPolicyTest, UntrustedFilesOpenDoesNotAsk)
{
	EXPECT_EQ(EWorkspaceTrustUntrustedFilesDecision::Open,
		ResolveWorkspaceTrustUntrustedFiles(LooseFileRequest(EWorkspaceTrustUntrustedFiles::Open)));
}

TEST(WorkspaceTrustPromptPolicyTest, UntrustedFilesNewWindowOpensElsewhereWhenSupported)
{
	auto request = LooseFileRequest(EWorkspaceTrustUntrustedFiles::NewWindow);
	request.newWindowSupported = true;
	EXPECT_EQ(EWorkspaceTrustUntrustedFilesDecision::OpenInNewWindow,
		ResolveWorkspaceTrustUntrustedFiles(request));
}

/*!
	The unsupported boundary, which is the whole reason newWindowSupported is an
	input rather than an assumption.

	Answering Open here would do exactly what the user configured against: pull an
	untrusted file into the trusted window. The shell must fail closed on an
	explicit typed value instead.
 */
TEST(WorkspaceTrustPromptPolicyTest, UntrustedFilesNewWindowIsUnsupportedRatherThanWidened)
{
	auto request = LooseFileRequest(EWorkspaceTrustUntrustedFiles::NewWindow);
	request.newWindowSupported = false;
	EXPECT_EQ(EWorkspaceTrustUntrustedFilesDecision::Unsupported,
		ResolveWorkspaceTrustUntrustedFiles(request));
}

//! A previous acceptance answers for the user, so the prompt is not repeated.
TEST(WorkspaceTrustPromptPolicyTest, UntrustedFilesPromptHonoursARecordedAcceptance)
{
	auto request = LooseFileRequest(EWorkspaceTrustUntrustedFiles::Prompt);
	request.alreadyAccepted = true;
	EXPECT_EQ(EWorkspaceTrustUntrustedFilesDecision::Open,
		ResolveWorkspaceTrustUntrustedFiles(request));
}

/*!
	An unreadable acceptance record must ask, not open.

	This is the one direction that would open the file without the user being
	asked, on the strength of a record nobody could actually read.
 */
TEST(WorkspaceTrustPromptPolicyTest, UntrustedFilesPromptAsksWhenTheRecordCannotBeRead)
{
	auto request = LooseFileRequest(EWorkspaceTrustUntrustedFiles::Prompt);
	request.acceptRecordReadable = false;
	request.alreadyAccepted = true;
	EXPECT_EQ(EWorkspaceTrustUntrustedFilesDecision::Prompt,
		ResolveWorkspaceTrustUntrustedFiles(request));
}

TEST(WorkspaceTrustPromptPolicyTest, UntrustedFilesOpensWhenEveryResourceIsAlreadyTrusted)
{
	auto request = LooseFileRequest(EWorkspaceTrustUntrustedFiles::Prompt);
	request.allResourcesTrusted = true;
	EXPECT_EQ(EWorkspaceTrustUntrustedFilesDecision::Open,
		ResolveWorkspaceTrustUntrustedFiles(request));
}

TEST(WorkspaceTrustPromptPolicyTest, UntrustedFilesOpensWhenTheFeatureIsDisabled)
{
	auto request = LooseFileRequest(EWorkspaceTrustUntrustedFiles::NewWindow);
	request.featureEnabled = false;
	EXPECT_EQ(EWorkspaceTrustUntrustedFilesDecision::Open,
		ResolveWorkspaceTrustUntrustedFiles(request));
}

/*!
	A window that is not trusted already runs everything in Restricted Mode, so
	there is nothing this setting could still protect. Both non-Trusted states
	behave the same way here, unlike in the startup-prompt policy where the
	distinction decides whether to ask.
 */
TEST(WorkspaceTrustPromptPolicyTest, UntrustedFilesOpensIntoAWindowThatIsNotTrusted)
{
	auto request = LooseFileRequest(EWorkspaceTrustUntrustedFiles::NewWindow);
	request.state = EWorkspaceTrustState::Unknown;
	EXPECT_EQ(EWorkspaceTrustUntrustedFilesDecision::Open,
		ResolveWorkspaceTrustUntrustedFiles(request));

	request.state = EWorkspaceTrustState::Untrusted;
	EXPECT_EQ(EWorkspaceTrustUntrustedFilesDecision::Open,
		ResolveWorkspaceTrustUntrustedFiles(request));
}

/*!
	Order is part of the contract here too: the feature switch outranks the trust
	state, which outranks the already-trusted-resources check, which outranks the
	setting. Only the last step can reach the unsupported boundary.
 */
TEST(WorkspaceTrustPromptPolicyTest, UntrustedFilesOpenPrecedenceIsFixed)
{
	WorkspaceTrustUntrustedFilesRequest request;
	request.setting = EWorkspaceTrustUntrustedFiles::NewWindow;
	request.state = EWorkspaceTrustState::Unknown;
	request.featureEnabled = false;
	request.allResourcesTrusted = true;
	request.newWindowSupported = false;
	EXPECT_EQ(EWorkspaceTrustUntrustedFilesDecision::Open,
		ResolveWorkspaceTrustUntrustedFiles(request));

	request.featureEnabled = true;
	EXPECT_EQ(EWorkspaceTrustUntrustedFilesDecision::Open,
		ResolveWorkspaceTrustUntrustedFiles(request));

	request.state = EWorkspaceTrustState::Trusted;
	EXPECT_EQ(EWorkspaceTrustUntrustedFilesDecision::Open,
		ResolveWorkspaceTrustUntrustedFiles(request));

	request.allResourcesTrusted = false;
	EXPECT_EQ(EWorkspaceTrustUntrustedFilesDecision::Unsupported,
		ResolveWorkspaceTrustUntrustedFiles(request));
}

//! The default-constructed request opens: it describes a window with nothing at stake.
TEST(WorkspaceTrustPromptPolicyTest, UntrustedFilesDefaultRequestOpens)
{
	EXPECT_EQ(EWorkspaceTrustUntrustedFilesDecision::Open,
		ResolveWorkspaceTrustUntrustedFiles(WorkspaceTrustUntrustedFilesRequest{}));
}

} // namespace
