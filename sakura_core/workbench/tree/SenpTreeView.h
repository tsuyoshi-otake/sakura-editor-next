/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include "workbench/tree/SenpTreeProvider.h"
#include "workbench/viewcontainer/SenpViewContainer.h"
#include <array>

namespace workbench::tree {

//! Number of TreeFailureReason values; indexes SenpTreeViewOptions::failureTexts.
inline constexpr std::size_t kTreeFailureReasonCount = static_cast<std::size_t>(TreeFailureReason::LimitReached) + 1;
//! Display group for a provider-message token. The token set itself lives in
//! SenpTreeView.cpp; several wire tokens share one display group.
enum class TreeTokenText : std::uint8_t {
	RateLimited,
	AccessDenied,
	NotFound,
	InvalidRequest,
	ToolUnavailable,
	UnsupportedVersion,
	TimedOut,
	Cancelled,
	TooLarge,
	Unreadable,
	Failed,
	PageUnavailable,
	ResourceUnavailable,
	ControlUnavailable,
	ReadRefused,
	ControlBusy,
};
//! Number of TreeTokenText values; indexes SenpTreeViewOptions::tokenTexts.
inline constexpr std::size_t kTreeTokenTextCount = static_cast<std::size_t>(TreeTokenText::ControlBusy) + 1;

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
	//! Display text per host failure reason, indexed by TreeFailureReason.
	//! An empty entry and an unset table both fall back to the built-in English.
	std::array<std::wstring, kTreeFailureReasonCount> failureTexts{};
	//! Display text per token group, indexed by TreeTokenText. An exact token
	//! match shows the group's text; any other provider message is verbatim.
	//! An empty entry and an unset table both fall back to the built-in English.
	std::array<std::wstring, kTreeTokenTextCount> tokenTexts{};
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
	//! Replaces the failure-reason and provider-token display tables and
	//! repaints existing status rows in the new language.
	void SetLocalizedFailureText(std::array<std::wstring, kTreeFailureReasonCount> reasons,
		std::array<std::wstring, kTreeTokenTextCount> tokens) noexcept;
private:
	struct Impl;
	explicit CSenpTreeView(SenpTreeViewOptions options);
	std::unique_ptr<Impl> m_impl;
};

} // namespace workbench::tree
