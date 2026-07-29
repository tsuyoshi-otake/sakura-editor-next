/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "pch.h"

#include <gtest/gtest.h>

#include <oleacc.h>

#include "accessibility/CustomUiAutomationProvider.h"

#pragma comment(lib, "oleacc.lib")

namespace {

class AccessibleTestHost final : public accessibility::ICustomUiAutomationHost {
public:
	HWND window = nullptr;
	int focused = -1;
	int invoked = -1;
	int getObjectRequests = 0;

	[[nodiscard]] HWND AccessibilityWindow() const noexcept override { return window; }
	[[nodiscard]] std::shared_ptr<accessibility::CustomUiAutomationLifetime> AccessibilityLifetime() const noexcept override { return lifetime; }
	[[nodiscard]] std::wstring AccessibilityName() const override { return L"Custom test surface"; }
	[[nodiscard]] std::wstring AccessibilityAutomationId() const override { return L"Sakura.Test.Accessible"; }
	[[nodiscard]] CONTROLTYPEID AccessibilityControlType() const noexcept override { return UIA_ToolBarControlTypeId; }
	[[nodiscard]] int AccessibilityChildCount(int parentId) const noexcept override { return parentId == -1 ? 2 : 0; }
	[[nodiscard]] int AccessibilityChildAt(int parentId, int index) const noexcept override { return parentId == -1 && index >= 0 && index < 2 ? index : -1; }
	[[nodiscard]] int AccessibilityParent(int nodeId) const noexcept override { return nodeId >= 0 && nodeId < 2 ? -1 : -2; }
	[[nodiscard]] accessibility::CustomUiAutomationNode AccessibilityNode(int nodeId) const override
	{
		if (nodeId == 0) return { 0, L"Explorer", L"Sakura.Test.Explorer", UIA_ButtonControlTypeId, { 0, 0, 40, 40 }, true, focused == 0, true };
		if (nodeId == 1) return { 1, L"Terminal", L"Sakura.Test.Terminal", UIA_ButtonControlTypeId, { 40, 0, 80, 40 }, true, focused == 1, true };
		return {};
	}
	[[nodiscard]] int AccessibilityFocusedNode() const noexcept override { return focused; }
	[[nodiscard]] bool AccessibilityInvoke(int nodeId) noexcept override
	{
		if (nodeId < 0 || nodeId > 1) return false;
		invoked = nodeId;
		accessibility::RaiseInvoked(*this, nodeId);
		return true;
	}
	void AccessibilitySetFocus(int nodeId) noexcept override
	{
		if (nodeId < 0 || nodeId > 1) return;
		focused = nodeId;
	}

private:
	std::shared_ptr<accessibility::CustomUiAutomationLifetime> lifetime = std::make_shared<accessibility::CustomUiAutomationLifetime>();
};

LRESULT CALLBACK AccessibleTestWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (message == WM_NCCREATE) {
		const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
		auto* host = static_cast<AccessibleTestHost*>(create->lpCreateParams);
		::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
		if (host != nullptr) host->window = window;
	}
	auto* host = reinterpret_cast<AccessibleTestHost*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
	if (host != nullptr && message == WM_GETOBJECT) {
		++host->getObjectRequests;
		return accessibility::HandleGetObject(*host, wParam, lParam);
	}
	if (message == WM_NCDESTROY && host != nullptr) host->window = nullptr;
	return ::DefWindowProcW(window, message, wParam, lParam);
}

HWND CreateAccessibleTestWindow(AccessibleTestHost& host)
{
	constexpr wchar_t className[] = L"SakuraCustomAccessibilityTestWindow";
	static const ATOM atom = [] {
		WNDCLASSW windowClass{};
		windowClass.lpfnWndProc = AccessibleTestWindowProc;
		windowClass.hInstance = ::GetModuleHandleW(nullptr);
		windowClass.lpszClassName = L"SakuraCustomAccessibilityTestWindow";
		const ATOM result = ::RegisterClassW(&windowClass);
		return result != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS ? 1 : 0;
	}();
	if (atom == 0) return nullptr;
	return ::CreateWindowExW(0, className, L"", WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT, 160, 100, nullptr, nullptr, ::GetModuleHandleW(nullptr), &host);
}

VARIANT ChildId(long childId)
{
	VARIANT result{};
	result.vt = VT_I4;
	result.lVal = childId;
	return result;
}

} // namespace

TEST(CustomUiAutomationProvider, RemovesKeyboardMnemonicsFromAccessibleMenuNames)
{
	EXPECT_EQ(L"File", accessibility::StripMenuMnemonics(L"&File"));
	EXPECT_EQ(L"Save & Close", accessibility::StripMenuMnemonics(L"Save && &Close"));
	EXPECT_EQ(L"Trailing&", accessibility::StripMenuMnemonics(L"Trailing&"));
}

TEST(CustomUiAutomationProvider, TreatsHiddenAndIconicOwnersAsOffscreen)
{
	const RECT bounds{ 0, 0, 40, 40 };
	EXPECT_FALSE(accessibility::IsCustomUiAutomationOffscreen(bounds, true, false));
	EXPECT_TRUE(accessibility::IsCustomUiAutomationOffscreen(bounds, false, false));
	EXPECT_TRUE(accessibility::IsCustomUiAutomationOffscreen(bounds, true, true));
	const RECT emptyBounds{ 0, 0, 0, 40 };
	EXPECT_TRUE(accessibility::IsCustomUiAutomationOffscreen(emptyBounds, true, false));
}

TEST(CustomUiAutomationProvider, ExposesVirtualButtonsToMsaaThroughARealWindow)
{
	const HRESULT coInitialize = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	ASSERT_TRUE(SUCCEEDED(coInitialize) || coInitialize == RPC_E_CHANGED_MODE);
	AccessibleTestHost host;
	const HWND window = CreateAccessibleTestWindow(host);
	ASSERT_NE(nullptr, window);

	// Same-process callers intentionally send WM_GETOBJECT with wParam == 0;
	// Oleacc consequently returns its generic fallback. Exercise that public
	// entry point first, then use a nonzero client process id to validate the
	// native IAccessible object the external MSAA client receives.
	IAccessible* sameProcessObject = nullptr;
	ASSERT_HRESULT_SUCCEEDED(::AccessibleObjectFromWindow(window, OBJID_CLIENT, __uuidof(IAccessible), reinterpret_cast<void**>(&sameProcessObject)));
	ASSERT_NE(nullptr, sameProcessObject);
	sameProcessObject->Release();
	EXPECT_GT(host.getObjectRequests, 0);

	const WPARAM clientProcess = static_cast<WPARAM>(::GetCurrentProcessId());
	const LRESULT objectReference = ::SendMessageW(window, WM_GETOBJECT, clientProcess, static_cast<LPARAM>(OBJID_CLIENT));
	ASSERT_GT(objectReference, 0);
	IAccessible* root = nullptr;
	ASSERT_HRESULT_SUCCEEDED(::ObjectFromLresult(objectReference, __uuidof(IAccessible), clientProcess, reinterpret_cast<void**>(&root)));
	ASSERT_NE(nullptr, root);
	long childCount = 0;
	EXPECT_HRESULT_SUCCEEDED(root->get_accChildCount(&childCount));
	EXPECT_EQ(2, childCount);

	const VARIANT explorerId = ChildId(1);
	IDispatch* explorerDispatch = nullptr;
	ASSERT_HRESULT_SUCCEEDED(root->get_accChild(explorerId, &explorerDispatch));
	ASSERT_NE(nullptr, explorerDispatch);
	IAccessible* explorer = nullptr;
	ASSERT_HRESULT_SUCCEEDED(explorerDispatch->QueryInterface(__uuidof(IAccessible), reinterpret_cast<void**>(&explorer)));
	explorerDispatch->Release();
	ASSERT_NE(nullptr, explorer);

	const VARIANT self = ChildId(CHILDID_SELF);
	BSTR name = nullptr;
	ASSERT_HRESULT_SUCCEEDED(explorer->get_accName(self, &name));
	EXPECT_STREQ(L"Explorer", name);
	::SysFreeString(name);
	VARIANT role{};
	ASSERT_HRESULT_SUCCEEDED(explorer->get_accRole(self, &role));
	EXPECT_EQ(VT_I4, role.vt);
	EXPECT_EQ(ROLE_SYSTEM_PUSHBUTTON, role.lVal);
	::VariantClear(&role);
	VARIANT state{};
	ASSERT_HRESULT_SUCCEEDED(explorer->get_accState(self, &state));
	EXPECT_NE(0, state.lVal & STATE_SYSTEM_FOCUSABLE);
	::VariantClear(&state);
	long left = 0;
	long top = 0;
	long width = 0;
	long height = 0;
	EXPECT_HRESULT_SUCCEEDED(explorer->accLocation(&left, &top, &width, &height, self));
	EXPECT_EQ(40, width);
	EXPECT_EQ(40, height);
	BSTR action = nullptr;
	ASSERT_HRESULT_SUCCEEDED(explorer->get_accDefaultAction(self, &action));
	EXPECT_STREQ(L"Press", action);
	::SysFreeString(action);

	ASSERT_HRESULT_SUCCEEDED(explorer->accSelect(SELFLAG_TAKEFOCUS, self));
	EXPECT_EQ(0, host.focused);
	EXPECT_EQ(window, ::GetFocus());
	EXPECT_HRESULT_SUCCEEDED(explorer->accDoDefaultAction(self));
	EXPECT_EQ(0, host.invoked);

	explorer->Release();
	root->Release();
	::DestroyWindow(window);
	if (SUCCEEDED(coInitialize)) ::CoUninitialize();
}
