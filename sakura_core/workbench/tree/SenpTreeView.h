/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include "workbench/tree/SenpTreeProvider.h"
#include "workbench/viewcontainer/SenpViewContainer.h"

namespace workbench::tree {

struct SenpTreeViewOptions final {
	viewcontainer::SenpViewBodyHost host;
	std::shared_ptr<SenpTreeProvider> provider;
	std::wstring title;
	//! VS Code's workbench.tree.expandMode default is singleClick. Items with a
	//! command always reserve expansion for the twistie, independent of this option.
	bool expandOnSingleClick{ true };
	std::wstring loadingText{ L"Loading..." };
	std::wstring loadMoreText{ L"Load more..." };
	std::wstring retryText{ L"Retry" };
	std::wstring noItemsText{ L"No items" };
	std::wstring notLoadedText{ L"Not loaded" };
	std::wstring providerUnavailableText{ L"Provider unavailable" };
};

//! A real Win32 TreeView body: native hierarchy, keyboard, selection and
//! accessibility; only row painting is themed. Model/provider own identities
//! and loading; this body never invokes transport from paint or polls for data.
class CSenpTreeView final : public viewcontainer::ISenpViewBody {
public:
	[[nodiscard]] static std::unique_ptr<CSenpTreeView> Create(SenpTreeViewOptions options) noexcept;
	~CSenpTreeView() override;
	[[nodiscard]] HWND Window() const noexcept override;
	void Layout(const RECT& bounds, unsigned int dpi) noexcept override;
	void SetVisible(bool visible) noexcept override;
	void SetPalette(const theme::ThemePalette& palette, layout::EViewContainerLocation location) noexcept override;
	[[nodiscard]] bool Focus() noexcept override;
	[[nodiscard]] bool PreTranslate(MSG& message) noexcept override;
	void Close() noexcept override;
	[[nodiscard]] HWND TreeWindow() const noexcept;
	[[nodiscard]] bool IsUsable() const noexcept;
	void SetLocalizedStatusText(std::wstring loading, std::wstring loadMore,
		std::wstring retry, std::wstring noItems, std::wstring notLoaded,
		std::wstring providerUnavailable) noexcept;
private:
	struct Impl;
	explicit CSenpTreeView(SenpTreeViewOptions options);
	std::unique_ptr<Impl> m_impl;
};

} // namespace workbench::tree
