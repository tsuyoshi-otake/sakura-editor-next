/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "StdAfx.h"
#include "accessibility/CustomUiAutomationProvider.h"

#include <oleacc.h>

#include <atomic>
#include <algorithm>
#include <new>
#include <optional>


namespace accessibility {
namespace {

constexpr int kRootNode = -1;

class CustomUiAutomationProvider final : public IRawElementProviderSimple,
	public IRawElementProviderFragment,
	public IRawElementProviderFragmentRoot,
	public IInvokeProvider {
public:
	CustomUiAutomationProvider(ICustomUiAutomationHost& host, int nodeId) noexcept
		: m_host(host), m_lifetime(host.AccessibilityLifetime()), m_nodeId(nodeId)
	{
	}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
	{
		if (object == nullptr) return E_INVALIDARG;
		*object = nullptr;
		if (iid == __uuidof(IUnknown) || iid == __uuidof(IRawElementProviderSimple)) {
			*object = static_cast<IRawElementProviderSimple*>(this);
		} else if (iid == __uuidof(IRawElementProviderFragment)) {
			*object = static_cast<IRawElementProviderFragment*>(this);
		} else if (m_nodeId == kRootNode && iid == __uuidof(IRawElementProviderFragmentRoot)) {
			*object = static_cast<IRawElementProviderFragmentRoot*>(this);
		} else if (HostAvailable() && m_nodeId != kRootNode && iid == __uuidof(IInvokeProvider) && Node().invoke) {
			*object = static_cast<IInvokeProvider*>(this);
		}
		if (*object == nullptr) return E_NOINTERFACE;
		AddRef();
		return S_OK;
	}

	ULONG STDMETHODCALLTYPE AddRef() override { return ++m_references; }
	ULONG STDMETHODCALLTYPE Release() override
	{
		const ULONG remaining = --m_references;
		if (remaining == 0) delete this;
		return remaining;
	}

	HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* options) override
	{
		if (options == nullptr) return E_INVALIDARG;
		*options = static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading);
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID patternId, IUnknown** pattern) override
	{
		if (pattern == nullptr) return E_INVALIDARG;
		*pattern = nullptr;
		if (HostAvailable() && patternId == UIA_InvokePatternId && m_nodeId != kRootNode && Node().invoke) {
			*pattern = static_cast<IInvokeProvider*>(this);
			AddRef();
		}
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID propertyId, VARIANT* value) override
	{
		if (value == nullptr) return E_INVALIDARG;
		::VariantInit(value);
		const auto node = Node();
		switch (propertyId) {
		case UIA_NamePropertyId: SetBstr(value, node.name); break;
		case UIA_AutomationIdPropertyId: SetBstr(value, node.automationId); break;
		case UIA_ControlTypePropertyId: value->vt = VT_I4; value->lVal = node.controlType; break;
		case UIA_IsEnabledPropertyId: value->vt = VT_BOOL; value->boolVal = node.enabled ? VARIANT_TRUE : VARIANT_FALSE; break;
		case UIA_HasKeyboardFocusPropertyId: value->vt = VT_BOOL; value->boolVal = node.focused ? VARIANT_TRUE : VARIANT_FALSE; break;
		case UIA_IsKeyboardFocusablePropertyId: value->vt = VT_BOOL; value->boolVal = node.enabled ? VARIANT_TRUE : VARIANT_FALSE; break;
		case UIA_IsControlElementPropertyId:
		case UIA_IsContentElementPropertyId: value->vt = VT_BOOL; value->boolVal = VARIANT_TRUE; break;
		case UIA_IsOffscreenPropertyId: value->vt = VT_BOOL; value->boolVal = IsOffscreen(node.bounds) ? VARIANT_TRUE : VARIANT_FALSE; break;
		case UIA_BoundingRectanglePropertyId: SetBoundingRectangle(value, node.bounds); break;
		case UIA_ProviderDescriptionPropertyId: SetBstr(value, L"Sakura Editor NEXT custom UI Automation provider"); break;
		default: break;
		}
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** provider) override
	{
		if (provider == nullptr) return E_INVALIDARG;
		*provider = nullptr;
		const HWND window = HostAvailable() ? m_host.AccessibilityWindow() : nullptr;
		if (m_nodeId == kRootNode && window != nullptr && ::IsWindow(window)) {
			return ::UiaHostProviderFromHwnd(window, provider);
		}
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction, IRawElementProviderFragment** provider) override
	{
		if (provider == nullptr) return E_INVALIDARG;
		*provider = nullptr;
		if (!HostAvailable()) return UIA_E_ELEMENTNOTAVAILABLE;
		int target = -2;
		switch (direction) {
		case NavigateDirection_Parent: target = m_nodeId == kRootNode ? -2 : m_host.AccessibilityParent(m_nodeId); break;
		case NavigateDirection_FirstChild: target = m_host.AccessibilityChildCount(m_nodeId) > 0 ? m_host.AccessibilityChildAt(m_nodeId, 0) : -2; break;
		case NavigateDirection_LastChild: {
			const int count = m_host.AccessibilityChildCount(m_nodeId);
			target = count > 0 ? m_host.AccessibilityChildAt(m_nodeId, count - 1) : -2;
			break;
		}
		case NavigateDirection_NextSibling:
		case NavigateDirection_PreviousSibling: {
			if (m_nodeId == kRootNode) break;
			const int parent = m_host.AccessibilityParent(m_nodeId);
			const int count = m_host.AccessibilityChildCount(parent);
			for (int index = 0; index < count; ++index) {
				if (m_host.AccessibilityChildAt(parent, index) != m_nodeId) continue;
				const int next = direction == NavigateDirection_NextSibling ? index + 1 : index - 1;
				target = next >= 0 && next < count ? m_host.AccessibilityChildAt(parent, next) : -2;
				break;
			}
			break;
		}
		default: break;
		}
		if (target != -2) *provider = NewProvider(target);
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** runtimeId) override
	{
		if (runtimeId == nullptr) return E_INVALIDARG;
		if (!HostAvailable()) return UIA_E_ELEMENTNOTAVAILABLE;
		*runtimeId = ::SafeArrayCreateVector(VT_I4, 0, 3);
		if (*runtimeId == nullptr) return E_OUTOFMEMORY;
		LONG index = 0;
		int value = UiaAppendRuntimeId;
		if (FAILED(::SafeArrayPutElement(*runtimeId, &index, &value))) return DestroyRuntimeId(runtimeId);
		++index;
		value = static_cast<int>(reinterpret_cast<INT_PTR>(m_host.AccessibilityWindow()));
		if (FAILED(::SafeArrayPutElement(*runtimeId, &index, &value))) return DestroyRuntimeId(runtimeId);
		++index;
		value = m_nodeId;
		if (FAILED(::SafeArrayPutElement(*runtimeId, &index, &value))) return DestroyRuntimeId(runtimeId);
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* rectangle) override
	{
		if (rectangle == nullptr) return E_INVALIDARG;
		*rectangle = ToUiaRect(Node().bounds);
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** roots) override
	{
		if (roots == nullptr) return E_INVALIDARG;
		*roots = nullptr;
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE SetFocus() override
	{
		if (HostAvailable() && m_nodeId != kRootNode && Node().enabled) {
			m_host.AccessibilitySetFocus(m_nodeId);
			const HWND window = m_host.AccessibilityWindow();
			if (window != nullptr && ::IsWindow(window)) ::SetFocus(window);
			RaiseFocusChanged(m_host, m_nodeId);
		}
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** root) override
	{
		if (root == nullptr) return E_INVALIDARG;
		*root = nullptr;
		IRawElementProviderFragment* provider = NewProvider(kRootNode);
		if (provider == nullptr) return E_OUTOFMEMORY;
		const HRESULT result = provider->QueryInterface(__uuidof(IRawElementProviderFragmentRoot), reinterpret_cast<void**>(root));
		provider->Release();
		return result;
	}

	HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** provider) override
	{
		if (provider == nullptr) return E_INVALIDARG;
		*provider = nullptr;
		if (!HostAvailable()) return UIA_E_ELEMENTNOTAVAILABLE;
		POINT point{ static_cast<LONG>(x), static_cast<LONG>(y) };
		const HWND window = m_host.AccessibilityWindow();
		if (window == nullptr || !::IsWindow(window)) return S_OK;
		::ScreenToClient(window, &point);
		const int node = HitTest(kRootNode, point);
		*provider = NewProvider(node);
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** provider) override
	{
		if (provider == nullptr) return E_INVALIDARG;
		if (!HostAvailable()) return UIA_E_ELEMENTNOTAVAILABLE;
		const int focused = m_host.AccessibilityFocusedNode();
		*provider = focused == -1 ? nullptr : NewProvider(focused);
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE Invoke() override
	{
		if (!HostAvailable()) return UIA_E_ELEMENTNOTAVAILABLE;
		if (m_nodeId == kRootNode || !Node().enabled || !Node().invoke) return UIA_E_NOTSUPPORTED;
		if (!m_host.AccessibilityInvoke(m_nodeId)) return UIA_E_ELEMENTNOTAVAILABLE;
		return S_OK;
	}

private:
	[[nodiscard]] bool HostAvailable() const noexcept
	{
		return m_lifetime != nullptr && m_lifetime->IsAlive();
	}

	static HRESULT DestroyRuntimeId(SAFEARRAY** runtimeId) noexcept
	{
		::SafeArrayDestroy(*runtimeId);
		*runtimeId = nullptr;
		return E_FAIL;
	}

	[[nodiscard]] CustomUiAutomationNode Node() const
	{
		if (!HostAvailable()) return { m_nodeId, L"", L"", UIA_CustomControlTypeId, {}, false, false, false };
		if (m_nodeId == kRootNode) {
			return { kRootNode, m_host.AccessibilityName(), m_host.AccessibilityAutomationId(),
				m_host.AccessibilityControlType(), ClientBounds(), true, false, false };
		}
		return m_host.AccessibilityNode(m_nodeId);
	}

	[[nodiscard]] RECT ClientBounds() const noexcept
	{
		RECT bounds{};
		if (!HostAvailable()) return bounds;
		const HWND window = m_host.AccessibilityWindow();
		if (window != nullptr && ::IsWindow(window)) ::GetClientRect(window, &bounds);
		return bounds;
	}

	[[nodiscard]] bool IsOffscreen(const RECT& rect) const noexcept
	{
		const HWND window = HostAvailable() ? m_host.AccessibilityWindow() : nullptr;
		return window == nullptr || !::IsWindow(window)
			|| IsCustomUiAutomationOffscreen(rect, ::IsWindowVisible(window) != FALSE, ::IsIconic(window) != FALSE);
	}

	[[nodiscard]] UiaRect ToUiaRect(RECT rect) const noexcept
	{
		if (!HostAvailable()) return {};
		const HWND window = m_host.AccessibilityWindow();
		POINT points[2]{ { rect.left, rect.top }, { rect.right, rect.bottom } };
		if (window != nullptr && ::IsWindow(window)) ::MapWindowPoints(window, nullptr, points, 2);
		return { static_cast<double>(points[0].x), static_cast<double>(points[0].y),
			static_cast<double>(std::max(0L, rect.right - rect.left)), static_cast<double>(std::max(0L, rect.bottom - rect.top)) };
	}

	void SetBoundingRectangle(VARIANT* value, RECT rect) const
	{
		if (!HostAvailable()) return;
		const UiaRect bounds = ToUiaRect(rect);
		SAFEARRAY* array = ::SafeArrayCreateVector(VT_R8, 0, 4);
		if (array == nullptr) return;
		double values[]{ bounds.left, bounds.top, bounds.width, bounds.height };
		for (LONG index = 0; index < 4; ++index) {
			if (FAILED(::SafeArrayPutElement(array, &index, values + index))) {
				::SafeArrayDestroy(array);
				return;
			}
		}
		value->vt = VT_R8 | VT_ARRAY;
		value->parray = array;
	}

	static void SetBstr(VARIANT* value, const std::wstring& text)
	{
		value->vt = VT_BSTR;
		value->bstrVal = ::SysAllocStringLen(text.data(), static_cast<UINT>(text.size()));
	}

	[[nodiscard]] int HitTest(int parent, POINT point) const
	{
		if (!HostAvailable()) return parent;
		const int count = m_host.AccessibilityChildCount(parent);
		for (int index = 0; index < count; ++index) {
			const int child = m_host.AccessibilityChildAt(parent, index);
			const auto node = m_host.AccessibilityNode(child);
			if (point.x < node.bounds.left || point.x >= node.bounds.right || point.y < node.bounds.top || point.y >= node.bounds.bottom) continue;
			return HitTest(child, point);
		}
		return parent;
	}

	[[nodiscard]] IRawElementProviderFragment* NewProvider(int nodeId) const noexcept
	{
		return new (std::nothrow) CustomUiAutomationProvider(m_host, nodeId);
	}

	std::atomic<ULONG> m_references{ 1 };
	ICustomUiAutomationHost& m_host;
	std::shared_ptr<CustomUiAutomationLifetime> m_lifetime;
	int m_nodeId;
};

//! MSAA facade over the same virtual nodes used by the UIA provider.  MSAA
//! child identifiers are local to each facade; complex children are returned
//! as their own IAccessible so nested structures such as the menu bar retain
//! their hierarchy.
class CustomMsaaProvider final : public IAccessible {
public:
	CustomMsaaProvider(ICustomUiAutomationHost& host, int nodeId) noexcept
		: m_host(host), m_lifetime(host.AccessibilityLifetime()), m_nodeId(nodeId)
	{
	}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
	{
		if (object == nullptr) return E_INVALIDARG;
		*object = nullptr;
		if (iid == __uuidof(IUnknown) || iid == __uuidof(IDispatch) || iid == __uuidof(IAccessible)) {
			*object = static_cast<IAccessible*>(this);
		}
		if (*object == nullptr) return E_NOINTERFACE;
		AddRef();
		return S_OK;
	}

	ULONG STDMETHODCALLTYPE AddRef() override { return ++m_references; }
	ULONG STDMETHODCALLTYPE Release() override
	{
		const ULONG remaining = --m_references;
		if (remaining == 0) delete this;
		return remaining;
	}

	HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* count) override
	{
		if (count == nullptr) return E_INVALIDARG;
		*count = 0;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override { return E_NOTIMPL; }

	HRESULT STDMETHODCALLTYPE get_accParent(IDispatch** parent) override
	{
		if (parent == nullptr) return E_INVALIDARG;
		*parent = nullptr;
		if (!HostAvailable()) return CO_E_OBJNOTCONNECTED;
		const int parentId = m_host.AccessibilityParent(m_nodeId);
		if (parentId == -2) return S_FALSE;
		return NewProvider(parentId, parent);
	}

	HRESULT STDMETHODCALLTYPE get_accChildCount(long* count) override
	{
		if (count == nullptr) return E_INVALIDARG;
		*count = 0;
		if (!HostAvailable()) return CO_E_OBJNOTCONNECTED;
		*count = m_host.AccessibilityChildCount(m_nodeId);
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE get_accChild(VARIANT child, IDispatch** result) override
	{
		if (result == nullptr) return E_INVALIDARG;
		*result = nullptr;
		int nodeId = -2;
		if (!ResolveChild(child, nodeId) || child.lVal == CHILDID_SELF) return E_INVALIDARG;
		return NewProvider(nodeId, result);
	}

	HRESULT STDMETHODCALLTYPE get_accName(VARIANT child, BSTR* name) override
	{
		return GetText(child, name, [](const CustomUiAutomationNode& node) -> const std::wstring& { return node.name; });
	}
	HRESULT STDMETHODCALLTYPE get_accValue(VARIANT, BSTR* value) override
	{
		if (value == nullptr) return E_INVALIDARG;
		*value = nullptr;
		return S_FALSE;
	}
	HRESULT STDMETHODCALLTYPE get_accDescription(VARIANT, BSTR* description) override
	{
		if (description == nullptr) return E_INVALIDARG;
		*description = nullptr;
		return S_FALSE;
	}

	HRESULT STDMETHODCALLTYPE get_accRole(VARIANT child, VARIANT* role) override
	{
		if (role == nullptr) return E_INVALIDARG;
		::VariantInit(role);
		const auto node = NodeFor(child);
		if (!node) return E_INVALIDARG;
		role->vt = VT_I4;
		role->lVal = RoleFor(node->controlType);
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE get_accState(VARIANT child, VARIANT* state) override
	{
		if (state == nullptr) return E_INVALIDARG;
		::VariantInit(state);
		const auto node = NodeFor(child);
		if (!node) return E_INVALIDARG;
		long value = 0;
		if (!node->enabled) value |= STATE_SYSTEM_UNAVAILABLE;
		if (node->enabled && node->id != kRootNode) value |= STATE_SYSTEM_FOCUSABLE;
		if (node->focused) value |= STATE_SYSTEM_FOCUSED;
		if (IsOffscreen(node->bounds)) value |= STATE_SYSTEM_INVISIBLE;
		state->vt = VT_I4;
		state->lVal = value;
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE get_accHelp(VARIANT, BSTR* help) override
	{
		if (help == nullptr) return E_INVALIDARG;
		*help = nullptr;
		return S_FALSE;
	}
	HRESULT STDMETHODCALLTYPE get_accHelpTopic(BSTR* helpFile, VARIANT, long* topic) override
	{
		if (helpFile == nullptr || topic == nullptr) return E_INVALIDARG;
		*helpFile = nullptr;
		*topic = 0;
		return S_FALSE;
	}
	HRESULT STDMETHODCALLTYPE get_accKeyboardShortcut(VARIANT, BSTR* shortcut) override
	{
		if (shortcut == nullptr) return E_INVALIDARG;
		*shortcut = nullptr;
		return S_FALSE;
	}

	HRESULT STDMETHODCALLTYPE get_accFocus(VARIANT* focus) override
	{
		if (focus == nullptr) return E_INVALIDARG;
		::VariantInit(focus);
		if (!HostAvailable()) return CO_E_OBJNOTCONNECTED;
		const int focused = m_host.AccessibilityFocusedNode();
		if (focused == -1 || !ContainsNode(m_nodeId, focused)) return S_FALSE;
		if (focused == m_nodeId) {
			focus->vt = VT_I4;
			focus->lVal = CHILDID_SELF;
			return S_OK;
		}
		IDispatch* provider = nullptr;
		const HRESULT hr = NewProvider(focused, &provider);
		if (FAILED(hr)) return hr;
		focus->vt = VT_DISPATCH;
		focus->pdispVal = provider;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE get_accSelection(VARIANT* selection) override
	{
		if (selection == nullptr) return E_INVALIDARG;
		::VariantInit(selection);
		return S_FALSE;
	}

	HRESULT STDMETHODCALLTYPE get_accDefaultAction(VARIANT child, BSTR* action) override
	{
		if (action == nullptr) return E_INVALIDARG;
		*action = nullptr;
		const auto node = NodeFor(child);
		if (!node) return E_INVALIDARG;
		if (!node->invoke) return S_FALSE;
		*action = ::SysAllocString(L"Press");
		return *action == nullptr ? E_OUTOFMEMORY : S_OK;
	}

	HRESULT STDMETHODCALLTYPE accSelect(long flags, VARIANT child) override
	{
		const auto node = NodeFor(child);
		if (!node) return E_INVALIDARG;
		if ((flags & SELFLAG_TAKEFOCUS) == 0 || !node->enabled) return S_FALSE;
		m_host.AccessibilitySetFocus(node->id);
		const HWND window = m_host.AccessibilityWindow();
		if (window != nullptr && ::IsWindow(window)) ::SetFocus(window);
		RaiseFocusChanged(m_host, node->id);
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE accLocation(long* left, long* top, long* width, long* height, VARIANT child) override
	{
		if (left == nullptr || top == nullptr || width == nullptr || height == nullptr) return E_INVALIDARG;
		const auto node = NodeFor(child);
		if (!node) return E_INVALIDARG;
		RECT bounds = node->bounds;
		const HWND window = m_host.AccessibilityWindow();
		POINT points[2]{ { bounds.left, bounds.top }, { bounds.right, bounds.bottom } };
		if (window != nullptr && ::IsWindow(window)) ::MapWindowPoints(window, nullptr, points, 2);
		*left = points[0].x;
		*top = points[0].y;
		*width = std::max(0L, points[1].x - points[0].x);
		*height = std::max(0L, points[1].y - points[0].y);
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE accNavigate(long direction, VARIANT start, VARIANT* result) override
	{
		if (result == nullptr) return E_INVALIDARG;
		::VariantInit(result);
		int nodeId = -2;
		if (!ResolveChild(start, nodeId)) return E_INVALIDARG;
		int target = -2;
		const int parent = m_host.AccessibilityParent(nodeId);
		switch (direction) {
		case NAVDIR_UP: target = parent; break;
		case NAVDIR_FIRSTCHILD: target = m_host.AccessibilityChildCount(nodeId) > 0 ? m_host.AccessibilityChildAt(nodeId, 0) : -2; break;
		case NAVDIR_LASTCHILD: {
			const int count = m_host.AccessibilityChildCount(nodeId);
			target = count > 0 ? m_host.AccessibilityChildAt(nodeId, count - 1) : -2;
			break;
		}
		case NAVDIR_NEXT:
		case NAVDIR_PREVIOUS: {
			const int count = m_host.AccessibilityChildCount(parent);
			for (int index = 0; index < count; ++index) {
				if (m_host.AccessibilityChildAt(parent, index) != nodeId) continue;
				const int sibling = direction == NAVDIR_NEXT ? index + 1 : index - 1;
				target = sibling >= 0 && sibling < count ? m_host.AccessibilityChildAt(parent, sibling) : -2;
				break;
			}
			break;
		}
		default: return E_NOTIMPL;
		}
		if (target == -2) return S_FALSE;
		IDispatch* provider = nullptr;
		const HRESULT hr = NewProvider(target, &provider);
		if (FAILED(hr)) return hr;
		result->vt = VT_DISPATCH;
		result->pdispVal = provider;
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE accHitTest(long x, long y, VARIANT* result) override
	{
		if (result == nullptr) return E_INVALIDARG;
		::VariantInit(result);
		if (!HostAvailable()) return CO_E_OBJNOTCONNECTED;
		POINT point{ x, y };
		const HWND window = m_host.AccessibilityWindow();
		if (window == nullptr || !::IsWindow(window)) return S_FALSE;
		::ScreenToClient(window, &point);
		const int nodeId = HitTest(m_nodeId, point);
		if (nodeId == m_nodeId) {
			result->vt = VT_I4;
			result->lVal = CHILDID_SELF;
			return S_OK;
		}
		IDispatch* provider = nullptr;
		const HRESULT hr = NewProvider(nodeId, &provider);
		if (FAILED(hr)) return hr;
		result->vt = VT_DISPATCH;
		result->pdispVal = provider;
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE accDoDefaultAction(VARIANT child) override
	{
		const auto node = NodeFor(child);
		if (!node) return E_INVALIDARG;
		if (!node->enabled || !node->invoke) return S_FALSE;
		return m_host.AccessibilityInvoke(node->id) ? S_OK : E_FAIL;
	}
	HRESULT STDMETHODCALLTYPE put_accName(VARIANT, BSTR) override { return E_ACCESSDENIED; }
	HRESULT STDMETHODCALLTYPE put_accValue(VARIANT, BSTR) override { return E_NOTIMPL; }

private:
	template<typename Selector>
	HRESULT GetText(VARIANT child, BSTR* value, Selector&& selector)
	{
		if (value == nullptr) return E_INVALIDARG;
		*value = nullptr;
		const auto node = NodeFor(child);
		if (!node) return E_INVALIDARG;
		const std::wstring& text = selector(*node);
		*value = ::SysAllocStringLen(text.data(), static_cast<UINT>(text.size()));
		return *value == nullptr && !text.empty() ? E_OUTOFMEMORY : S_OK;
	}

	[[nodiscard]] bool HostAvailable() const noexcept
	{
		return m_lifetime != nullptr && m_lifetime->IsAlive();
	}
	[[nodiscard]] CustomUiAutomationNode RootNode() const
	{
		RECT bounds{};
		const HWND window = HostAvailable() ? m_host.AccessibilityWindow() : nullptr;
		if (window != nullptr && ::IsWindow(window)) ::GetClientRect(window, &bounds);
		return { kRootNode, m_host.AccessibilityName(), m_host.AccessibilityAutomationId(),
			m_host.AccessibilityControlType(), bounds, true, false, false };
	}
	[[nodiscard]] std::optional<CustomUiAutomationNode> NodeFor(VARIANT child) const
	{
		int nodeId = -2;
		if (!ResolveChild(child, nodeId)) return std::nullopt;
		return nodeId == kRootNode ? RootNode() : m_host.AccessibilityNode(nodeId);
	}
	[[nodiscard]] bool ResolveChild(const VARIANT& child, int& nodeId) const noexcept
	{
		if (!HostAvailable() || child.vt != VT_I4) return false;
		if (child.lVal == CHILDID_SELF) {
			nodeId = m_nodeId;
			return true;
		}
		if (child.lVal <= CHILDID_SELF) return false;
		nodeId = m_host.AccessibilityChildAt(m_nodeId, child.lVal - 1);
		return nodeId != -1;
	}
	[[nodiscard]] static long RoleFor(CONTROLTYPEID controlType) noexcept
	{
		if (controlType == UIA_ButtonControlTypeId) return ROLE_SYSTEM_PUSHBUTTON;
		if (controlType == UIA_MenuBarControlTypeId) return ROLE_SYSTEM_MENUBAR;
		if (controlType == UIA_MenuItemControlTypeId) return ROLE_SYSTEM_MENUITEM;
		if (controlType == UIA_ToolBarControlTypeId) return ROLE_SYSTEM_TOOLBAR;
		if (controlType == UIA_WindowControlTypeId) return ROLE_SYSTEM_WINDOW;
		return ROLE_SYSTEM_CLIENT;
	}
	[[nodiscard]] bool IsOffscreen(const RECT& bounds) const noexcept
	{
		const HWND window = m_host.AccessibilityWindow();
		return window == nullptr || !::IsWindow(window)
			|| IsCustomUiAutomationOffscreen(bounds, ::IsWindowVisible(window) != FALSE, ::IsIconic(window) != FALSE);
	}
	[[nodiscard]] bool ContainsNode(int ancestor, int nodeId) const noexcept
	{
		if (ancestor == nodeId) return true;
		const int count = m_host.AccessibilityChildCount(ancestor);
		for (int index = 0; index < count; ++index) {
			if (ContainsNode(m_host.AccessibilityChildAt(ancestor, index), nodeId)) return true;
		}
		return false;
	}
	[[nodiscard]] int HitTest(int parent, POINT point) const noexcept
	{
		const int count = m_host.AccessibilityChildCount(parent);
		for (int index = 0; index < count; ++index) {
			const int child = m_host.AccessibilityChildAt(parent, index);
			const auto node = m_host.AccessibilityNode(child);
			if (point.x < node.bounds.left || point.x >= node.bounds.right || point.y < node.bounds.top || point.y >= node.bounds.bottom) continue;
			return HitTest(child, point);
		}
		return parent;
	}
	HRESULT NewProvider(int nodeId, IDispatch** result) const
	{
		if (result == nullptr) return E_INVALIDARG;
		*result = nullptr;
		if (!HostAvailable()) return CO_E_OBJNOTCONNECTED;
		auto* provider = new (std::nothrow) CustomMsaaProvider(m_host, nodeId);
		if (provider == nullptr) return E_OUTOFMEMORY;
		*result = static_cast<IDispatch*>(provider);
		return S_OK;
	}

	std::atomic<ULONG> m_references{ 1 };
	ICustomUiAutomationHost& m_host;
	std::shared_ptr<CustomUiAutomationLifetime> m_lifetime;
	int m_nodeId;
};

[[nodiscard]] IRawElementProviderSimple* NewSimpleProvider(ICustomUiAutomationHost& host, int nodeId) noexcept
{
	return new (std::nothrow) CustomUiAutomationProvider(host, nodeId);
}

void RaiseEvent(ICustomUiAutomationHost& host, int nodeId, EVENTID eventId) noexcept
{
	IRawElementProviderSimple* provider = NewSimpleProvider(host, nodeId);
	if (provider == nullptr) return;
	(void)::UiaRaiseAutomationEvent(provider, eventId);
	provider->Release();
}

} // namespace

bool IsCustomUiAutomationOffscreen(const RECT& bounds, bool ownerVisible, bool ownerIconic) noexcept
{
	return ::IsRectEmpty(&bounds) || !ownerVisible || ownerIconic;
}

LRESULT HandleGetObject(ICustomUiAutomationHost& host, WPARAM wParam, LPARAM lParam) noexcept
{
	if (host.AccessibilityWindow() == nullptr) return 0;
	if (lParam == static_cast<LPARAM>(UiaRootObjectId)) {
		IRawElementProviderSimple* provider = NewSimpleProvider(host, kRootNode);
		if (provider == nullptr) return 0;
		// UiaReturnRawElementProvider transfers the initial provider reference to UI Automation.
		return ::UiaReturnRawElementProvider(host.AccessibilityWindow(), wParam, lParam, provider);
	}
	if (lParam == static_cast<LPARAM>(OBJID_CLIENT)) {
		auto* provider = new (std::nothrow) CustomMsaaProvider(host, kRootNode);
		if (provider == nullptr) return 0;
		const LRESULT result = ::LresultFromObject(
			__uuidof(IAccessible), wParam, static_cast<IUnknown*>(static_cast<IAccessible*>(provider)));
		// LresultFromObject retains the references it marshals; this initial
		// reference belongs to the server and must be released immediately.
		provider->Release();
		return result;
	}
	return 0;
}

void RaiseFocusChanged(ICustomUiAutomationHost& host, int nodeId) noexcept
{
	RaiseEvent(host, nodeId, UIA_AutomationFocusChangedEventId);
	const HWND window = host.AccessibilityWindow();
	if (window != nullptr && ::IsWindow(window)) ::NotifyWinEvent(EVENT_OBJECT_FOCUS, window, OBJID_CLIENT, CHILDID_SELF);
}

void RaiseFocusCleared(ICustomUiAutomationHost& host, int nodeId) noexcept
{
	IRawElementProviderSimple* provider = NewSimpleProvider(host, nodeId);
	if (provider != nullptr) {
		VARIANT oldState{};
		VARIANT newState{};
		oldState.vt = newState.vt = VT_BOOL;
		oldState.boolVal = VARIANT_TRUE;
		newState.boolVal = VARIANT_FALSE;
		(void)::UiaRaiseAutomationPropertyChangedEvent(provider, UIA_HasKeyboardFocusPropertyId, oldState, newState);
		provider->Release();
	}
	const HWND window = host.AccessibilityWindow();
	if (window != nullptr && ::IsWindow(window)) ::NotifyWinEvent(EVENT_OBJECT_STATECHANGE, window, OBJID_CLIENT, CHILDID_SELF);
}

void RaiseInvoked(ICustomUiAutomationHost& host, int nodeId) noexcept
{
	RaiseEvent(host, nodeId, UIA_Invoke_InvokedEventId);
	const HWND window = host.AccessibilityWindow();
	if (window != nullptr && ::IsWindow(window)) ::NotifyWinEvent(EVENT_OBJECT_INVOKED, window, OBJID_CLIENT, CHILDID_SELF);
}

void RaiseEnabledChanged(ICustomUiAutomationHost& host, int nodeId, bool oldValue, bool newValue) noexcept
{
	IRawElementProviderSimple* provider = NewSimpleProvider(host, nodeId);
	if (provider == nullptr) return;
	VARIANT oldState{};
	VARIANT newState{};
	oldState.vt = newState.vt = VT_BOOL;
	oldState.boolVal = oldValue ? VARIANT_TRUE : VARIANT_FALSE;
	newState.boolVal = newValue ? VARIANT_TRUE : VARIANT_FALSE;
	(void)::UiaRaiseAutomationPropertyChangedEvent(provider, UIA_IsEnabledPropertyId, oldState, newState);
	provider->Release();
	const HWND window = host.AccessibilityWindow();
	if (window != nullptr && ::IsWindow(window)) ::NotifyWinEvent(EVENT_OBJECT_STATECHANGE, window, OBJID_CLIENT, CHILDID_SELF);
}

std::wstring StripMenuMnemonics(const std::wstring& value)
{
	std::wstring result;
	result.reserve(value.size());
	for (std::size_t index = 0; index < value.size(); ++index) {
		if (value[index] == L'&' && index + 1 < value.size()) {
			if (value[index + 1] == L'&') result.push_back(value[++index]);
			continue;
		}
		result.push_back(value[index]);
	}
	return result;
}

} // namespace accessibility
