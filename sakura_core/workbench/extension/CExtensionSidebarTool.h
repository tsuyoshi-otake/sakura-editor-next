/*! @file
	@brief VS Code contributed views compatible native sidebar
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "theme/CThemeService.h"
#include "workbench/IWorkbenchTool.h"

#include <functional>
#include <memory>
#include <string_view>
#include <vector>

class CExtensionViewRegistry;
struct SExtensionViewDescriptor;

namespace workbench::extension {

class CExtensionSidebarTool final : public IWorkbenchTool {
public:
	using RequestChildrenCallback = std::function<void(std::wstring_view viewHandle, std::wstring_view parentHandle)>;
	using SelectionChangedCallback = std::function<void(std::wstring_view viewHandle, const std::vector<std::wstring>& itemHandles)>;
	using CheckboxChangedCallback = std::function<void(std::wstring_view viewHandle, std::wstring_view itemHandle, bool checked)>;
	using CommandCallback = std::function<void(std::wstring_view command, std::string_view argumentsJson,
		std::wstring_view viewHandle, std::wstring_view itemHandle)>;
	using VisibilityChangedCallback = std::function<void(bool visible)>;
	/*!
		@brief Decides whether one contributed view belongs to this tool's ViewContainer.

		A tool renders exactly one container, but which views a container owns is the pool's
		knowledge, not this control's: the fallback container has to accept everything no
		dedicated container claimed. An empty filter accepts every view, so a composition with
		a single container needs to know nothing about containers at all.
	*/
	using ViewFilter = std::function<bool(const SExtensionViewDescriptor&)>;

	explicit CExtensionSidebarTool(
		std::shared_ptr<CExtensionViewRegistry> registry, ViewFilter viewFilter = {});
	~CExtensionSidebarTool() override;
	CExtensionSidebarTool(const CExtensionSidebarTool&) = delete;
	CExtensionSidebarTool& operator=(const CExtensionSidebarTool&) = delete;

	bool Create(HWND parent) override;
	void Layout(const RECT& contentRect, unsigned int dpi) override;
	void Activate() override;
	void Deactivate() override;
	bool PreTranslateMessage(MSG& message) override;
	void Close() override;

	void SetPalette(const theme::ThemePalette& palette);
	void SetRequestChildrenCallback(RequestChildrenCallback callback);
	void SetSelectionChangedCallback(SelectionChangedCallback callback);
	void SetCheckboxChangedCallback(CheckboxChangedCallback callback);
	void SetCommandCallback(CommandCallback callback);
	void SetVisibilityChangedCallback(VisibilityChangedCallback callback);
	//! Safe to call from an RPC worker; rebuilding is marshalled to the HWND thread.
	void Refresh();
	void SetSidebarVisible(bool visible);
	[[nodiscard]] HWND GetHwnd() const noexcept;

	static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace workbench::extension
