/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <Windows.h>
#include <UIAutomation.h>

#include <string>
#include <memory>
#include <atomic>

namespace accessibility {

//! Shared guard retained by COM providers so a cached provider never dereferences a destroyed host.
class CustomUiAutomationLifetime final {
public:
	void Invalidate() noexcept { m_alive.store(false, std::memory_order_release); }
	[[nodiscard]] bool IsAlive() const noexcept { return m_alive.load(std::memory_order_acquire); }

private:
	std::atomic_bool m_alive{ true };
};

//! Snapshot of an app-owned element in a custom Win32 surface.
//! Node identifiers are stable for the lifetime of their owning surface.
struct CustomUiAutomationNode {
	int id = -1;
	std::wstring name;
	std::wstring automationId;
	CONTROLTYPEID controlType = UIA_CustomControlTypeId;
	RECT bounds{}; // Client coordinates of AccessibilityWindow().
	bool enabled = true;
	bool focused = false;
	bool invoke = false;
};

//! Pure visibility rule shared by UI Automation and MSAA providers.
[[nodiscard]] bool IsCustomUiAutomationOffscreen(const RECT& bounds, bool ownerVisible, bool ownerIconic) noexcept;

//! Minimal adapter implemented by custom-drawn controls. It deliberately owns no COM state.
class ICustomUiAutomationHost {
public:
	virtual ~ICustomUiAutomationHost() = default;
	[[nodiscard]] virtual HWND AccessibilityWindow() const noexcept = 0;
	[[nodiscard]] virtual std::shared_ptr<CustomUiAutomationLifetime> AccessibilityLifetime() const noexcept = 0;
	[[nodiscard]] virtual std::wstring AccessibilityName() const = 0;
	[[nodiscard]] virtual std::wstring AccessibilityAutomationId() const = 0;
	[[nodiscard]] virtual CONTROLTYPEID AccessibilityControlType() const noexcept = 0;
	[[nodiscard]] virtual int AccessibilityChildCount(int parentId) const noexcept = 0;
	[[nodiscard]] virtual int AccessibilityChildAt(int parentId, int index) const noexcept = 0;
	[[nodiscard]] virtual int AccessibilityParent(int nodeId) const noexcept = 0;
	[[nodiscard]] virtual CustomUiAutomationNode AccessibilityNode(int nodeId) const = 0;
	[[nodiscard]] virtual int AccessibilityFocusedNode() const noexcept = 0;
	[[nodiscard]] virtual bool AccessibilityInvoke(int nodeId) noexcept = 0;
	virtual void AccessibilitySetFocus(int nodeId) noexcept = 0;
};

//! Handles UIA and OBJID_CLIENT WM_GETOBJECT requests. OBJID_CLIENT is exposed through
//! IAccessible so legacy MSAA clients see the same virtual tree as UI Automation.
[[nodiscard]] LRESULT HandleGetObject(ICustomUiAutomationHost& host, WPARAM wParam, LPARAM lParam) noexcept;

//! Best-effort UIA events. These are no-ops when UIA is unavailable or the host has no HWND.
void RaiseFocusChanged(ICustomUiAutomationHost& host, int nodeId) noexcept;
//! Announces that a custom element has relinquished keyboard focus.
void RaiseFocusCleared(ICustomUiAutomationHost& host, int nodeId) noexcept;
void RaiseInvoked(ICustomUiAutomationHost& host, int nodeId) noexcept;
void RaiseEnabledChanged(ICustomUiAutomationHost& host, int nodeId, bool oldValue, bool newValue) noexcept;

//! Removes keyboard mnemonics while retaining escaped ampersands for screen-reader names.
[[nodiscard]] std::wstring StripMenuMnemonics(const std::wstring& value);

} // namespace accessibility
