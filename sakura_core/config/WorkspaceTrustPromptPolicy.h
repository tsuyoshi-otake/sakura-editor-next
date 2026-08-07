/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "config/WorkspaceContextTypes.h"

#include <cstdint>
#include <string_view>

namespace config {

/*!
	@brief @c security.workspace.trust.startupPrompt

	Verified against upstream's workspaceTrust contribution: enum
	@c ["always", "once", "never"], default @c "never".
 */
enum class EWorkspaceTrustStartupPrompt : std::uint8_t {
	Always,
	Once,
	Never,
};

/*!
	@brief @c security.workspace.trust.untrustedFiles

	Verified against the same upstream contribution: enum
	@c ["prompt", "open", "newWindow"], default @c "prompt".
 */
enum class EWorkspaceTrustUntrustedFiles : std::uint8_t {
	Prompt,
	Open,
	NewWindow,
};

/*!
	@brief Parse the setting value, failing closed on anything unrecognized.

	The descriptor constrains the value, but a caller must still be handed one
	concrete policy rather than an empty optional it would have to guess about.
	An unrecognized value therefore resolves to the upstream default, which is
	also the conservative one: @c Never does not prompt and so cannot grant, and
	@c Prompt asks rather than deciding on the user's behalf.
 */
[[nodiscard]] EWorkspaceTrustStartupPrompt ParseWorkspaceTrustStartupPrompt(std::wstring_view value);
[[nodiscard]] EWorkspaceTrustUntrustedFiles ParseWorkspaceTrustUntrustedFiles(std::wstring_view value);

//! Everything the startup-prompt policy is allowed to look at.
struct WorkspaceTrustStartupPromptRequest final {
	EWorkspaceTrustStartupPrompt setting = EWorkspaceTrustStartupPrompt::Never;
	//! The already-resolved trust state. This policy never re-resolves trust.
	EWorkspaceTrustState state = EWorkspaceTrustState::Unknown;
	//! @c security.workspace.trust.enabled
	bool featureEnabled = true;
	/*!
		Whether the prompt would have anything to offer.

		Derived from the same pure @c BuildTrustGrantEntries the prompt and the
		grant already share. A window with no grantable option -- an empty window
		above all -- must not be shown a dialog whose every button is absent.
	 */
	bool hasGrantableOption = false;
	//! Whether the durable "this workspace was already prompted" record could be read.
	bool promptRecordReadable = false;
	//! The record's value. Meaningful only when @c promptRecordReadable.
	bool promptAlreadyShown = false;
};

//! Why the startup-prompt policy answered the way it did.
enum class EWorkspaceTrustStartupPromptDecision : std::uint8_t {
	//! Show the prompt. @c always, or @c once with no record of having shown it.
	Show,
	/*!
		Show the prompt because the durable record could not be read.

		Distinct from @c Show so the caller can report it. Prompting is the safe
		direction: a prompt withholds trust until the user grants it, whereas
		silently skipping would honour a record nobody could actually read.
	 */
	ShowRecordUnreadable,
	//! @c security.workspace.trust.enabled is false, so everything is already trusted.
	SkipFeatureDisabled,
	//! The window resolved @c Trusted. There is nothing to ask for.
	SkipAlreadyTrusted,
	//! @c security.workspace.trust.startupPrompt is @c never.
	SkipSettingNever,
	//! @c once, and the durable record says this workspace was already prompted.
	SkipAlreadyShown,
	//! No grant would apply to this window, so a prompt could offer no choice.
	SkipNothingToGrant,
};

/*!
	@brief Decide whether the startup trust prompt is shown.

	Pure: no I/O, no clock, no global state, no window. It consumes an
	already-resolved trust state and never produces one.

	Order mirrors upstream's @c showModalOnStart: the feature switch and an
	already-trusted window short-circuit first, then @c never, then @c once
	against its durable record.
 */
[[nodiscard]] EWorkspaceTrustStartupPromptDecision ResolveWorkspaceTrustStartupPrompt(
	const WorkspaceTrustStartupPromptRequest& request);

//! Everything the untrusted-files policy is allowed to look at.
struct WorkspaceTrustUntrustedFilesRequest final {
	EWorkspaceTrustUntrustedFiles setting = EWorkspaceTrustUntrustedFiles::Prompt;
	//! The already-resolved trust state of the window doing the opening.
	EWorkspaceTrustState state = EWorkspaceTrustState::Unknown;
	//! @c security.workspace.trust.enabled
	bool featureEnabled = true;
	//! Whether every resource being opened is already covered by the trusted list.
	bool allResourcesTrusted = true;
	//! Whether the durable per-workspace acceptance record could be read.
	bool acceptRecordReadable = false;
	//! The record's value. Meaningful only when @c acceptRecordReadable.
	bool alreadyAccepted = false;
	/*!
		Whether this shell can actually open the resource in a separate window
		that is genuinely in Restricted Mode.

		An input rather than an assumption, so that a shell which cannot honour
		@c newWindow resolves an explicit @c Unsupported instead of quietly
		widening the decision into @c Open.
	 */
	bool newWindowSupported = false;
};

//! What to do with a loose file being opened into a trusted window.
enum class EWorkspaceTrustUntrustedFilesDecision : std::uint8_t {
	//! Open here. Either nothing is at stake, or the user already said so.
	Open,
	//! Ask the user.
	Prompt,
	//! Open in a separate Restricted Mode window.
	OpenInNewWindow,
	/*!
		@c newWindow was requested and this shell cannot honour it.

		Never silently degraded to @c Open: the setting's whole purpose is to
		keep the file out of the trusted window, so answering @c Open would do
		the one thing the user configured against.
	 */
	Unsupported,
};

/*!
	@brief Decide how a loose file opened into a trusted window is handled.

	Pure: no I/O, no clock, no global state, no window.

	Order mirrors upstream's @c requestOpenFilesTrust: a window that is not
	trusted has nothing to protect, resources already covered by the trusted
	list are not loose, an explicit non-@c prompt setting decides without
	asking, and a durable per-workspace acceptance answers before the prompt.
 */
[[nodiscard]] EWorkspaceTrustUntrustedFilesDecision ResolveWorkspaceTrustUntrustedFiles(
	const WorkspaceTrustUntrustedFilesRequest& request);

} // namespace config
