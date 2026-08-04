/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace markdown {

enum class MarkdownPreviewCommand : std::uint8_t {
	ShowPreview,
	ShowPreviewToSide,
	ShowLockedPreviewToSide,
	ShowSource,
	ShowPreviewSecuritySelector,
	Refresh,
	ToggleLock,
	ReopenAsPreview,
	ReopenAsSource,
	TogglePreview,
};

enum class MarkdownPreviewPlacement : std::uint8_t {
	CurrentEditorGroup,
	SideEditorGroup,
	//! Sakura-specific legacy split hosted inside one EditorGroup. Stable VS Code
	//! to-side commands must never report this as SideEditorGroup capability.
	NativeSiblingPane,
};

enum class MarkdownPreviewPresentation : std::uint8_t {
	Source,
	Preview,
};

enum class MarkdownPreviewCommandOutcome : std::uint8_t {
	Applied,
	RefreshRequested,
	NotApplicable,
	UnsupportedSideEditorGroup,
	UnsupportedSecuritySelector,
	UnavailableLockedSource,
};

struct MarkdownPreviewHostCapabilities final {
	bool sideEditorGroup = false;
	bool securitySelector = false;
};

struct MarkdownPreviewCommandResult final {
	MarkdownPreviewCommandOutcome outcome = MarkdownPreviewCommandOutcome::NotApplicable;
	bool presentationChanged = false;
	bool identityChanged = false;
	bool lockChanged = false;
};

//! Pure command state for the native preview projection. Exact VS Code commands
//! either mutate their own concept or terminate as a typed unsupported outcome;
//! they never alias to the Sakura-specific sibling-pane toggle.
class MarkdownPreviewCommandState final {
public:
	[[nodiscard]] MarkdownPreviewCommandResult Apply(
		MarkdownPreviewCommand command,
		std::wstring activeSourceIdentity,
		bool activeSourceIsMarkdown,
		MarkdownPreviewHostCapabilities capabilities = {})
	{
		switch (command) {
		case MarkdownPreviewCommand::ShowPreview:
		case MarkdownPreviewCommand::ReopenAsPreview:
			return ShowCurrent(std::move(activeSourceIdentity), activeSourceIsMarkdown);

		case MarkdownPreviewCommand::ShowPreviewToSide:
		case MarkdownPreviewCommand::ShowLockedPreviewToSide:
			if (!activeSourceIsMarkdown || activeSourceIdentity.empty()) return {};
			if (!capabilities.sideEditorGroup) {
				return { MarkdownPreviewCommandOutcome::UnsupportedSideEditorGroup };
			}
			return ShowSide(std::move(activeSourceIdentity),
				command == MarkdownPreviewCommand::ShowLockedPreviewToSide);

		case MarkdownPreviewCommand::ShowSource:
		case MarkdownPreviewCommand::ReopenAsSource:
			if (m_presentation != MarkdownPreviewPresentation::Preview) return {};
			{
			const bool wasLocked = m_locked;
			m_presentation = MarkdownPreviewPresentation::Source;
			m_locked = false;
			return { MarkdownPreviewCommandOutcome::Applied, true, false, wasLocked };
			}

		case MarkdownPreviewCommand::ShowPreviewSecuritySelector:
			return capabilities.securitySelector
				? MarkdownPreviewCommandResult{ MarkdownPreviewCommandOutcome::Applied }
				: MarkdownPreviewCommandResult{ MarkdownPreviewCommandOutcome::UnsupportedSecuritySelector };

		case MarkdownPreviewCommand::Refresh:
			if (m_presentation != MarkdownPreviewPresentation::Preview) return {};
			if (m_locked && (!activeSourceIsMarkdown || activeSourceIdentity != m_sourceIdentity)) {
				return { MarkdownPreviewCommandOutcome::UnavailableLockedSource };
			}
			return { MarkdownPreviewCommandOutcome::RefreshRequested };

		case MarkdownPreviewCommand::ToggleLock:
			if (m_presentation != MarkdownPreviewPresentation::Preview) return {};
			if (!m_locked) {
				m_locked = true;
				return { MarkdownPreviewCommandOutcome::Applied, false, false, true };
			}
			m_locked = false;
			if (!activeSourceIsMarkdown || activeSourceIdentity.empty()) {
				m_presentation = MarkdownPreviewPresentation::Source;
				return { MarkdownPreviewCommandOutcome::Applied, true, false, true };
			}
			{
				const bool identityChanged = m_sourceIdentity != activeSourceIdentity;
				m_sourceIdentity = std::move(activeSourceIdentity);
				return { MarkdownPreviewCommandOutcome::Applied, false, identityChanged, true };
			}

		case MarkdownPreviewCommand::TogglePreview:
			if (m_presentation == MarkdownPreviewPresentation::Preview) {
				const bool wasLocked = m_locked;
				m_presentation = MarkdownPreviewPresentation::Source;
				m_locked = false;
				return { MarkdownPreviewCommandOutcome::Applied, true, false, wasLocked };
			}
			return ShowCurrent(std::move(activeSourceIdentity), activeSourceIsMarkdown);
		}
		return {};
	}

	//! Applies the project-specific physical sibling pane without claiming a
	//! second EditorGroup. This is used only by the legacy Sakura function code.
	[[nodiscard]] MarkdownPreviewCommandResult ToggleNativeSibling(
		std::wstring activeSourceIdentity, bool activeSourceIsMarkdown)
	{
		if (m_presentation == MarkdownPreviewPresentation::Preview
			&& m_placement == MarkdownPreviewPlacement::NativeSiblingPane) {
			const bool wasLocked = m_locked;
			m_presentation = MarkdownPreviewPresentation::Source;
			m_locked = false;
			return { MarkdownPreviewCommandOutcome::Applied, true, false, wasLocked };
		}
		if (!activeSourceIsMarkdown || activeSourceIdentity.empty()) return {};
		const bool identityChanged = m_sourceIdentity != activeSourceIdentity;
		m_sourceIdentity = std::move(activeSourceIdentity);
		m_presentation = MarkdownPreviewPresentation::Preview;
		m_placement = MarkdownPreviewPlacement::NativeSiblingPane;
		m_locked = false;
		return { MarkdownPreviewCommandOutcome::Applied, true, identityChanged, false };
	}

	//! Dynamic preview follows the active Markdown resource; locked preview owns
	//! its previous identity. Returns true only when a rerender is required.
	[[nodiscard]] bool ObserveActiveSource(std::wstring activeSourceIdentity, bool activeSourceIsMarkdown)
	{
		if (m_presentation != MarkdownPreviewPresentation::Preview || m_locked) return false;
		if (!activeSourceIsMarkdown || activeSourceIdentity.empty()) {
			m_presentation = MarkdownPreviewPresentation::Source;
			m_locked = false;
			return true;
		}
		if (m_sourceIdentity == activeSourceIdentity) return false;
		m_sourceIdentity = std::move(activeSourceIdentity);
		return true;
	}

	void Reset() noexcept
	{
		m_presentation = MarkdownPreviewPresentation::Source;
		m_placement = MarkdownPreviewPlacement::CurrentEditorGroup;
		m_locked = false;
		m_sourceIdentity.clear();
	}

	[[nodiscard]] bool IsVisible() const noexcept
	{
		return m_presentation == MarkdownPreviewPresentation::Preview;
	}
	[[nodiscard]] bool IsLocked() const noexcept { return m_locked; }
	[[nodiscard]] MarkdownPreviewPlacement Placement() const noexcept { return m_placement; }
	[[nodiscard]] const std::wstring& SourceIdentity() const noexcept { return m_sourceIdentity; }

private:
	[[nodiscard]] MarkdownPreviewCommandResult ShowCurrent(
		std::wstring identity, bool isMarkdown)
	{
		if (!isMarkdown || identity.empty()) return {};
		const bool presentationChanged = m_presentation != MarkdownPreviewPresentation::Preview
			|| m_placement != MarkdownPreviewPlacement::CurrentEditorGroup;
		const bool identityChanged = m_sourceIdentity != identity;
		const bool lockChanged = m_locked;
		m_sourceIdentity = std::move(identity);
		m_presentation = MarkdownPreviewPresentation::Preview;
		m_placement = MarkdownPreviewPlacement::CurrentEditorGroup;
		m_locked = false;
		return { MarkdownPreviewCommandOutcome::Applied,
			presentationChanged, identityChanged, lockChanged };
	}

	[[nodiscard]] MarkdownPreviewCommandResult ShowSide(std::wstring identity, bool locked)
	{
		const bool presentationChanged = m_presentation != MarkdownPreviewPresentation::Preview
			|| m_placement != MarkdownPreviewPlacement::SideEditorGroup;
		const bool identityChanged = m_sourceIdentity != identity;
		const bool lockChanged = m_locked != locked;
		m_sourceIdentity = std::move(identity);
		m_presentation = MarkdownPreviewPresentation::Preview;
		m_placement = MarkdownPreviewPlacement::SideEditorGroup;
		m_locked = locked;
		return { MarkdownPreviewCommandOutcome::Applied,
			presentationChanged, identityChanged, lockChanged };
	}

	MarkdownPreviewPresentation m_presentation = MarkdownPreviewPresentation::Source;
	MarkdownPreviewPlacement m_placement = MarkdownPreviewPlacement::CurrentEditorGroup;
	bool m_locked = false;
	std::wstring m_sourceIdentity;
};

} // namespace markdown
