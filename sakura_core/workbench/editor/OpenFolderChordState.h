/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include <cstdint>

namespace workbench::editor {

//! Modifier state observed for one native key-down message.
//!
//! This deliberately models only the modifiers bound by Ctrl+K Ctrl+O.  The
//! HWND-owning adapter samples the native keyboard state and passes this value
//! in, which keeps the transition contract directly testable without an HWND.
struct OpenFolderChordModifiers {
	bool control = false;
	bool alt = false;
	bool shift = false;

	[[nodiscard]] constexpr bool IsControlOnly() const noexcept
	{
		return control && !alt && !shift;
	}
};

//! How a pending Ctrl+K chord handles its next key-down message.
enum class EOpenFolderChordKeyDecision : std::uint8_t {
	//! A modifier-only key-down keeps the chord pending and reaches normal dispatch.
	PassThrough,
	//! Ctrl+O matches the configured second stroke and must be handled by the adapter.
	Execute,
	//! Ctrl+K starts the chord again and renews the adapter-owned timeout.
	Restart,
	//! A non-matching second stroke cleared the chord and must not fall through.
	CancelAndConsume,
};

//! HWND-free state machine for VS Code's Windows Open Folder key chord.
//!
//! The native adapter owns the timer and message classification.  This type
//! owns only the bounded logical state so timeout and cancellation semantics
//! can be tested without constructing an editor window.
class OpenFolderChordState final
{
public:
	//! VS Code's default key-chord inactivity deadline is five seconds.
	static constexpr std::uint64_t TimeoutMs = 5000;
	using FocusToken = std::uintptr_t;

	//! Virtual-key values are kept here so the state model remains HWND-free.
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

	void Refresh(std::uint64_t now) noexcept
	{
		if (m_pending) m_startedAt = now;
	}

	//! Expires the state when the monotonic deadline has elapsed.
	//! Returns true when this call cleared a pending chord.
	[[nodiscard]] bool ExpireIfNeeded(std::uint64_t now) noexcept
	{
		if (!m_pending || now - m_startedAt < TimeoutMs) return false;
		Clear();
		return true;
	}

	//! Cancels the chord when the native focus identity changes.
	//! The token is intentionally opaque so this model remains HWND-free.
	[[nodiscard]] bool CancelIfFocusChanged(FocusToken focusToken) noexcept
	{
		if (!m_pending || m_focusToken == focusToken) return false;
		Clear();
		return true;
	}

	//! Advances a pending chord for a key-down message.
	//!
	//! A modifier-only key-down is intentionally passed through: releasing and
	//! re-pressing Ctrl between strokes is valid, and no key-up needs to be
	//! swallowed. Every other non-matching key clears the state before returning
	//! CancelAndConsume so the native adapter cannot leak it into editor input.
	[[nodiscard]] EOpenFolderChordKeyDecision AdvancePendingKeyDown(
		std::uint32_t virtualKey, OpenFolderChordModifiers modifiers) noexcept
	{
		if (!m_pending || IsModifierOnlyVirtualKey(virtualKey)) {
			return EOpenFolderChordKeyDecision::PassThrough;
		}
		if (!modifiers.IsControlOnly()) {
			Clear();
			return EOpenFolderChordKeyDecision::CancelAndConsume;
		}
		if (virtualKey == static_cast<std::uint32_t>('O')) {
			Clear();
			return EOpenFolderChordKeyDecision::Execute;
		}
		if (virtualKey == static_cast<std::uint32_t>('K')) {
			return EOpenFolderChordKeyDecision::Restart;
		}
		Clear();
		return EOpenFolderChordKeyDecision::CancelAndConsume;
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

} // namespace workbench::editor
