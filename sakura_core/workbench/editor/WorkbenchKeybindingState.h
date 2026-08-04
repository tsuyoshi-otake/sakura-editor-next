/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "workbench/editor/EditorCommandIds.h"

#include <cstdint>
#include <string_view>

namespace workbench::editor {

//! Modifier state sampled for one native key-down message.
//!
//! The native adapter samples Win32 keyboard state, but this value deliberately
//! has no Win32 dependency so the complete chord/shortcut contract is testable
//! without an HWND.
struct WorkbenchKeyModifiers {
	bool control = false;
	bool alt = false;
	bool shift = false;

	[[nodiscard]] constexpr bool IsControlOnly() const noexcept
	{
		return control && !alt && !shift;
	}

	[[nodiscard]] constexpr bool IsUnmodified() const noexcept
	{
		return !control && !alt && !shift;
	}
};

//! The action selected by the pending Ctrl+K chord.
enum class ECtrlKChordAction : std::uint8_t {
	None,
	OpenFolder,
	SaveAll,
	CloseFolder,
	MarkdownShowPreviewToSide,
};

//! How a pending Ctrl+K chord handles its next key-down message.
enum class ECtrlKChordKeyDecision : std::uint8_t {
	//! Modifier-only key-down; preserve the chord and let the native loop continue.
	PassThrough,
	//! A second stroke selected one stable workbench command.
	Execute,
	//! Ctrl+K re-started the chord and renews its adapter-owned timeout.
	Restart,
	//! A non-matching second stroke cleared the chord and must not reach legacy input.
	CancelAndConsume,
};

struct CtrlKChordKeyResult {
	ECtrlKChordKeyDecision decision = ECtrlKChordKeyDecision::PassThrough;
	ECtrlKChordAction action = ECtrlKChordAction::None;

	[[nodiscard]] constexpr std::string_view CommandId() const noexcept
	{
		switch (action) {
		case ECtrlKChordAction::OpenFolder: return command_ids::OpenFolder;
		case ECtrlKChordAction::SaveAll: return command_ids::SaveAll;
		case ECtrlKChordAction::CloseFolder: return command_ids::CloseFolder;
		case ECtrlKChordAction::MarkdownShowPreviewToSide: return command_ids::MarkdownShowPreviewToSide;
		case ECtrlKChordAction::None: break;
		}
		return {};
	}
};

//! HWND-free state machine for VS Code's Windows Ctrl+K chords.
//!
//! The adapter owns native timer/message handling. This class owns only the
//! bounded logical state and maps second strokes to stable command IDs.
class CtrlKChordState final
{
public:
	//! VS Code's default key-chord inactivity deadline is five seconds.
	static constexpr std::uint64_t TimeoutMs = 5000;
	using FocusToken = std::uintptr_t;

	[[nodiscard]] static constexpr bool IsModifierOnlyVirtualKey(std::uint32_t virtualKey) noexcept
	{
		switch (virtualKey) {
		case 0x10: // VK_SHIFT
		case 0x11: // VK_CONTROL
		case 0x12: // VK_MENU
		case 0x5B: // VK_LWIN
		case 0x5C: // VK_RWIN
		case 0xA0: // VK_LSHIFT
		case 0xA1: // VK_RSHIFT
		case 0xA2: // VK_LCONTROL
		case 0xA3: // VK_RCONTROL
		case 0xA4: // VK_LMENU
		case 0xA5: // VK_RMENU
			return true;
		default:
			return false;
		}
	}

	void Begin(std::uint64_t now, FocusToken focusToken = 0) noexcept
	{
		m_pending = true;
		m_startedAt = now;
		m_focusToken = focusToken;
	}

	[[nodiscard]] bool ExpireIfNeeded(std::uint64_t now) noexcept
	{
		if (!m_pending || now - m_startedAt < TimeoutMs) return false;
		Clear();
		return true;
	}

	[[nodiscard]] bool CancelIfFocusChanged(FocusToken focusToken) noexcept
	{
		if (!m_pending || m_focusToken == focusToken) return false;
		Clear();
		return true;
	}

	//! Resolves one key-down while a Ctrl+K chord is pending.
	[[nodiscard]] CtrlKChordKeyResult AdvancePendingKeyDown(
		std::uint32_t virtualKey, WorkbenchKeyModifiers modifiers) noexcept
	{
		if (!m_pending || IsModifierOnlyVirtualKey(virtualKey)) {
			return {};
		}
		if (virtualKey == static_cast<std::uint32_t>('K') && modifiers.IsControlOnly()) {
			return { ECtrlKChordKeyDecision::Restart, ECtrlKChordAction::None };
		}
		if (virtualKey == static_cast<std::uint32_t>('O') && modifiers.IsControlOnly()) {
			Clear();
			return { ECtrlKChordKeyDecision::Execute, ECtrlKChordAction::OpenFolder };
		}
		if (virtualKey == static_cast<std::uint32_t>('S') && modifiers.IsUnmodified()) {
			Clear();
			return { ECtrlKChordKeyDecision::Execute, ECtrlKChordAction::SaveAll };
		}
		if (virtualKey == static_cast<std::uint32_t>('F') && modifiers.IsUnmodified()) {
			Clear();
			return { ECtrlKChordKeyDecision::Execute, ECtrlKChordAction::CloseFolder };
		}
		if (virtualKey == static_cast<std::uint32_t>('V') && modifiers.IsUnmodified()) {
			Clear();
			return { ECtrlKChordKeyDecision::Execute, ECtrlKChordAction::MarkdownShowPreviewToSide };
		}
		Clear();
		return { ECtrlKChordKeyDecision::CancelAndConsume, ECtrlKChordAction::None };
	}

	void Clear() noexcept
	{
		m_pending = false;
		m_startedAt = 0;
		m_focusToken = 0;
	}

	[[nodiscard]] bool IsPending() const noexcept { return m_pending; }
	[[nodiscard]] std::uint64_t StartedAt() const noexcept { return m_startedAt; }

private:
	bool m_pending = false;
	std::uint64_t m_startedAt = 0;
	FocusToken m_focusToken = 0;
};

//! Result of a key-down translated by the production keybinding model.
enum class EWorkbenchKeyInputDecision : std::uint8_t {
	PassThrough,
	BeginOrRestartChordAndConsume,
	ExecuteStableCommandAndConsume,
	CancelChordAndConsume,
};

struct WorkbenchKeyInputResult {
	EWorkbenchKeyInputDecision decision = EWorkbenchKeyInputDecision::PassThrough;
	std::string_view commandId;
};

//! Pure input mapper for the File-menu bindings owned by the native adapter.
//!
//! `isCommandAvailable` is supplied by the registry-owning adapter. Therefore
//! this type has no registry or HWND dependency, while only registered stable
//! commands consume an otherwise legacy accelerator.
class WorkbenchKeybindingState final
{
public:
	using FocusToken = CtrlKChordState::FocusToken;

	[[nodiscard]] bool IsChordPending() const noexcept { return m_ctrlKChord.IsPending(); }
	[[nodiscard]] bool ExpireIfNeeded(std::uint64_t now) noexcept { return m_ctrlKChord.ExpireIfNeeded(now); }
	[[nodiscard]] bool CancelIfFocusChanged(FocusToken focusToken) noexcept
	{
		return m_ctrlKChord.CancelIfFocusChanged(focusToken);
	}
	void Clear() noexcept { m_ctrlKChord.Clear(); }

	template <class IsCommandAvailable>
	[[nodiscard]] WorkbenchKeyInputResult HandleKeyDown(std::uint32_t virtualKey,
		WorkbenchKeyModifiers modifiers, std::uint64_t now, FocusToken focusToken,
		IsCommandAvailable&& isCommandAvailable) noexcept
	{
		if (m_ctrlKChord.IsPending()) {
			const auto chord = m_ctrlKChord.AdvancePendingKeyDown(virtualKey, modifiers);
			switch (chord.decision) {
			case ECtrlKChordKeyDecision::PassThrough:
				return {};
			case ECtrlKChordKeyDecision::Restart:
				m_ctrlKChord.Begin(now, focusToken);
				return { EWorkbenchKeyInputDecision::BeginOrRestartChordAndConsume, {} };
			case ECtrlKChordKeyDecision::Execute:
				if (const auto commandId = chord.CommandId(); !commandId.empty() && isCommandAvailable(commandId)) {
					return { EWorkbenchKeyInputDecision::ExecuteStableCommandAndConsume, commandId };
				}
				return { EWorkbenchKeyInputDecision::CancelChordAndConsume, {} };
			case ECtrlKChordKeyDecision::CancelAndConsume:
				return { EWorkbenchKeyInputDecision::CancelChordAndConsume, {} };
			}
		}

		if (virtualKey == static_cast<std::uint32_t>('K') && modifiers.IsControlOnly()
			&& (isCommandAvailable(command_ids::OpenFolder)
				|| isCommandAvailable(command_ids::MarkdownShowPreviewToSide))) {
			m_ctrlKChord.Begin(now, focusToken);
			return { EWorkbenchKeyInputDecision::BeginOrRestartChordAndConsume, {} };
		}
		if (const auto commandId = DirectCommandId(virtualKey, modifiers);
			!commandId.empty() && isCommandAvailable(commandId)) {
			return { EWorkbenchKeyInputDecision::ExecuteStableCommandAndConsume, commandId };
		}
		return {};
	}

	[[nodiscard]] static constexpr std::string_view DirectCommandId(std::uint32_t virtualKey,
		WorkbenchKeyModifiers modifiers) noexcept
	{
		if (virtualKey == static_cast<std::uint32_t>('V') && modifiers.control
			&& modifiers.shift && !modifiers.alt) {
			return command_ids::MarkdownTogglePreview;
		}
		if (!modifiers.IsControlOnly()) return {};
		switch (virtualKey) {
		case 'R': return command_ids::OpenRecent;
		case 'W': return command_ids::CloseActiveEditor;
		default: return {};
		}
	}

private:
	CtrlKChordState m_ctrlKChord;
};

} // namespace workbench::editor
