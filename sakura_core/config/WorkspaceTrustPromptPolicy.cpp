/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"

#include "config/WorkspaceTrustPromptPolicy.h"

namespace config {

EWorkspaceTrustStartupPrompt ParseWorkspaceTrustStartupPrompt(std::wstring_view value)
{
	if (value == L"always") return EWorkspaceTrustStartupPrompt::Always;
	if (value == L"once") return EWorkspaceTrustStartupPrompt::Once;
	return EWorkspaceTrustStartupPrompt::Never;
}

EWorkspaceTrustUntrustedFiles ParseWorkspaceTrustUntrustedFiles(std::wstring_view value)
{
	if (value == L"open") return EWorkspaceTrustUntrustedFiles::Open;
	if (value == L"newWindow") return EWorkspaceTrustUntrustedFiles::NewWindow;
	return EWorkspaceTrustUntrustedFiles::Prompt;
}

EWorkspaceTrustStartupPromptDecision ResolveWorkspaceTrustStartupPrompt(
	const WorkspaceTrustStartupPromptRequest& request)
{
	// The feature switch outranks everything, exactly as it does in the trust
	// resolver: with trust disabled every workspace is already trusted.
	if (!request.featureEnabled) {
		return EWorkspaceTrustStartupPromptDecision::SkipFeatureDisabled;
	}

	// Only Trusted is trusted. Unknown is withheld trust, not granted trust,
	// and is precisely the state the prompt exists to resolve.
	if (request.state == EWorkspaceTrustState::Trusted) {
		return EWorkspaceTrustStartupPromptDecision::SkipAlreadyTrusted;
	}

	if (request.setting == EWorkspaceTrustStartupPrompt::Never) {
		return EWorkspaceTrustStartupPromptDecision::SkipSettingNever;
	}

	// A dialog whose every button is absent is worse than no dialog: it reports
	// a decision the user is being denied the means to make.
	if (!request.hasGrantableOption) {
		return EWorkspaceTrustStartupPromptDecision::SkipNothingToGrant;
	}

	if (request.setting == EWorkspaceTrustStartupPrompt::Once) {
		if (!request.promptRecordReadable) {
			return EWorkspaceTrustStartupPromptDecision::ShowRecordUnreadable;
		}
		if (request.promptAlreadyShown) {
			return EWorkspaceTrustStartupPromptDecision::SkipAlreadyShown;
		}
	}

	return EWorkspaceTrustStartupPromptDecision::Show;
}

EWorkspaceTrustUntrustedFilesDecision ResolveWorkspaceTrustUntrustedFiles(
	const WorkspaceTrustUntrustedFilesRequest& request)
{
	// With trust disabled there is no trusted/untrusted distinction to enforce.
	if (!request.featureEnabled) {
		return EWorkspaceTrustUntrustedFilesDecision::Open;
	}

	// This setting protects a *trusted* window from acquiring loose files it
	// would then execute against. A window that is not trusted already runs
	// everything in Restricted Mode, so there is nothing left to protect.
	if (request.state != EWorkspaceTrustState::Trusted) {
		return EWorkspaceTrustUntrustedFilesDecision::Open;
	}

	if (request.allResourcesTrusted) {
		return EWorkspaceTrustUntrustedFilesDecision::Open;
	}

	if (request.setting == EWorkspaceTrustUntrustedFiles::Open) {
		return EWorkspaceTrustUntrustedFilesDecision::Open;
	}

	if (request.setting == EWorkspaceTrustUntrustedFiles::NewWindow) {
		return request.newWindowSupported
			? EWorkspaceTrustUntrustedFilesDecision::OpenInNewWindow
			: EWorkspaceTrustUntrustedFilesDecision::Unsupported;
	}

	// Prompt. A readable record of a previous acceptance answers for the user;
	// an unreadable one does not, because honouring a record nobody could read
	// would be the one direction that opens the file without being asked.
	if (request.acceptRecordReadable && request.alreadyAccepted) {
		return EWorkspaceTrustUntrustedFilesDecision::Open;
	}

	return EWorkspaceTrustUntrustedFilesDecision::Prompt;
}

} // namespace config
