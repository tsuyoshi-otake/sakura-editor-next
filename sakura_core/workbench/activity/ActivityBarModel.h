/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::activity {

//! The two physical orientations supported by the Activity Bar host.
enum class ActivityBarOrientation : std::uint8_t {
	Vertical,
	Horizontal,
};

// Keep the project-style spelling available to callers that use the E-prefixed
// workbench enums without making the model carry two independent types.
using EActivityBarOrientation = ActivityBarOrientation;

//! VS Code's Activity Bar has ViewContainers on top and a separate GlobalCompositeBar
//! (Accounts / Manage) pinned to the bottom. Those are not ViewContainers.
enum class ActivityBarEntryKind : unsigned char {
	ViewContainer,
	GlobalAction,
};

//! Upstream `ACCOUNTS_ACTIVITY_ID` / `GLOBAL_ACTIVITY_ID` from `workbench/common/activity.ts`.
inline constexpr std::string_view kAccountsActivityId = "workbench.actions.accounts";
inline constexpr std::string_view kManageActivityId = "workbench.actions.manage";

/*!
	@brief One rendered Activity Bar entry.

	For ViewContainers, identity is the ViewContainer id owned by
	workbench::layout::WorkbenchContributionRegistry. Global actions use the
	upstream activity ids (`workbench.actions.accounts` /
	`workbench.actions.manage`) and must not be treated as Side Bar containers.
*/
struct ActivityBarEntry {
	//! `workbench.view.explorer`, `workbench.actions.accounts`, etc. Empty ids are rejected.
	std::string id;
	//! Tooltip and accessible name, already localized by whoever produced the entry.
	std::wstring label;
	//! Bundled codicon name. Empty names fall back to the first letter of the label.
	std::wstring codicon;
	ActivityBarEntryKind kind = ActivityBarEntryKind::ViewContainer;
	bool enabled = true;
	//! False when the ViewContainer no longer lives in the Primary Side Bar. VS Code
	//! removes the Activity Bar entry outright rather than greying it out.
	bool visible = true;

	[[nodiscard]] bool operator==(const ActivityBarEntry&) const = default;
	[[nodiscard]] constexpr bool IsGlobalAction() const noexcept
	{
		return kind == ActivityBarEntryKind::GlobalAction;
	}
};

/*!
	@brief A ViewContainer's number badge, upstream's `NumberBadge`.

	VS Code registers a badge through `IActivityService.showViewContainerActivity`,
	which is a different producer with a different lifetime from the container list
	itself: the SCM extension publishes a change count without knowing when the
	workbench last re-projected its ViewContainers. The model stores badges
	separately for that reason -- replacing the entries must not silently drop one.

	Only `NumberBadge` is representable here. Upstream also has `IconBadge`,
	`TextBadge`, and `ProgressBadge`; a producer that needs one of those must add
	the kind explicitly rather than approximating it with a number.
*/
struct ActivityBarNumberBadge {
	//! Upstream hides the badge entirely at zero or below, so a count of 0 is not
	//! a badge reading "0" -- callers publish `std::nullopt` for "nothing to show".
	int number = 0;
	[[nodiscard]] constexpr bool operator==(const ActivityBarNumberBadge&) const noexcept = default;
};

/*!
	@brief Upstream's badge text rule from `compositeBarActions.ts`.

	Over 999 collapses to `<n/1000>K+`, over 99 to `99+`, and anything at or below
	zero renders nothing at all.
*/
[[nodiscard]] std::wstring FormatActivityBarBadge(int number);

//! A physical-pixel rectangle with no dependency on Win32 types.
struct ActivityBarRect {
	int left = 0;
	int top = 0;
	int right = 0;
	int bottom = 0;

	[[nodiscard]] constexpr int Width() const noexcept { return right - left; }
	[[nodiscard]] constexpr int Height() const noexcept { return bottom - top; }
	[[nodiscard]] constexpr bool Contains(int x, int y) const noexcept
	{
		return x >= left && x < right && y >= top && y < bottom;
	}
	[[nodiscard]] constexpr bool operator==(const ActivityBarRect&) const noexcept = default;
};

/*!
	@brief Snapshot of one logical button. Bounds are in client pixels.

	The text fields are views into the model's own entries, so painting and accessibility can
	stay `noexcept`. They are valid until the entry list is replaced.
*/
struct ActivityBarButtonInfo {
	std::string_view id;
	std::wstring_view label;
	std::wstring_view codicon;
	ActivityBarRect bounds{};
	//! Absent unless a producer published one through SetViewContainerBadge.
	std::optional<ActivityBarNumberBadge> badge;
	ActivityBarEntryKind kind = ActivityBarEntryKind::ViewContainer;
	bool selected = false;
	bool hovered = false;
	bool pressed = false;
	bool focused = false;
	bool enabled = true;
	bool visible = true;

	[[nodiscard]] constexpr bool IsGlobalAction() const noexcept
	{
		return kind == ActivityBarEntryKind::GlobalAction;
	}
	[[nodiscard]] constexpr bool operator==(const ActivityBarButtonInfo&) const noexcept = default;
};

/*!
	@brief Pure state and geometry for CActivityBar. It deliberately has no HWND, GDI, or callback ownership.

	Selection, hover, press and focus are stored as entry indices and re-resolved by id whenever
	the entry list changes, so every state mutation stays allocation-free and `noexcept` while
	the identity a caller sees remains the stable container id.
*/
class ActivityBarModel final {
public:
	static constexpr int kWidthDip = 42;
	//! The vertical Activity Bar keeps the existing 42-DIP square affordance.
	static constexpr int kButtonExtentDip = 42;
	//! The Side Bar-owned horizontal composite bar is deliberately compact.
	static constexpr int kHorizontalHeightDip = 35;
	static constexpr int kHorizontalItemExtentDip = 26;
	static constexpr int kHorizontalOuterInsetDip = 4;
	static constexpr int kVerticalIconDip = 20;
	static constexpr int kHorizontalIconDip = 16;

	//! Starts with no entry at all. A composition supplies them through SetEntries().
	ActivityBarModel() = default;
	explicit ActivityBarModel(ActivityBarOrientation orientation) noexcept
		: m_orientation(orientation)
	{
	}

	void SetOrientation(ActivityBarOrientation orientation) noexcept;
	[[nodiscard]] ActivityBarOrientation GetOrientation() const noexcept { return m_orientation; }
	//! Semantic aliases for hosts that describe this as the bar's layout direction.
	void SetLayoutOrientation(ActivityBarOrientation orientation) noexcept { SetOrientation(orientation); }
	[[nodiscard]] ActivityBarOrientation GetLayoutOrientation() const noexcept { return GetOrientation(); }

	/*!
		@brief Replaces the whole entry list.

		Per-entry enabled/visible state travels inside the entries, and selection, hover, press
		and focus survive whenever the same container id survives.
	*/
	void SetEntries(std::vector<ActivityBarEntry> entries);
	[[nodiscard]] const std::vector<ActivityBarEntry>& Entries() const noexcept { return m_entries; }

	//! Sets physical client dimensions and reflows the button strip in its current orientation.
	void SetViewport(int widthPixels, int heightPixels, unsigned int dpi = 96) noexcept;
	//! Convenience overload for callers that set orientation and viewport as one layout commit.
	void SetViewport(int widthPixels, int heightPixels, ActivityBarOrientation orientation,
		unsigned int dpi = 96) noexcept;
	//! Lets the owner reflect the currently visible workbench tool. An empty id means none.
	//! Global actions are never selected: they open menus, they do not own a ViewContainer.
	void SetSelectedItem(std::string_view id) noexcept;
	[[nodiscard]] std::string_view GetSelectedItem() const noexcept { return IdAt(m_selected); }
	//! ViewContainers are composite drag handles; GlobalCompositeBar actions are not.
	[[nodiscard]] bool IsDraggable(std::string_view id) const noexcept;

	void SetEnabled(std::string_view id, bool enabled) noexcept;
	//! Semantic name used by UI hosts and tests; retained alongside SetEnabled for low-level callers.
	void SetItemEnabled(std::string_view id, bool enabled) noexcept { SetEnabled(id, enabled); }
	[[nodiscard]] bool IsEnabled(std::string_view id) const noexcept;

	//! Adds or removes one Activity Bar entry. A ViewContainer moved to the Secondary
	//! Side Bar leaves the Activity Bar entirely, and the remaining entries close the gap.
	void SetItemVisible(std::string_view id, bool visible) noexcept;
	[[nodiscard]] bool IsVisible(std::string_view id) const noexcept;
	//! True while the model knows this container at all.
	[[nodiscard]] bool Contains(std::string_view id) const noexcept { return IndexOf(id) != kNoIndex; }

	/*!
		@brief Upstream `IActivityService.showViewContainerActivity` / its disposal.

		`std::nullopt` clears the badge, as disposing the activity does upstream. A
		badge may be published for a container the model does not know yet and
		survives SetEntries, because the producer's lifetime is independent of when
		the workbench last re-projected its ViewContainers. Global actions never
		carry one: upstream badges activities, and Accounts/Manage are not
		ViewContainers.
	*/
	void SetViewContainerBadge(std::string_view id, std::optional<ActivityBarNumberBadge> badge);
	[[nodiscard]] std::optional<ActivityBarNumberBadge> GetViewContainerBadge(std::string_view id) const;

	void SetHoveredItem(std::string_view id) noexcept;
	void SetPressedItem(std::string_view id) noexcept;
	void SetFocusedItem(std::string_view id) noexcept;
	[[nodiscard]] std::string_view GetFocusedItem() const noexcept { return IdAt(m_focused); }

	[[nodiscard]] std::size_t GetButtonCount() const noexcept { return m_entries.size(); }
	[[nodiscard]] ActivityBarButtonInfo GetButton(std::size_t index) const noexcept;
	//! Returns the id of the entry under the point, or an empty view when the point misses.
	[[nodiscard]] std::string_view HitTest(int x, int y) const noexcept;
	//! Moves focus through enabled items. A positive direction moves down/right; a negative direction moves up/left.
	[[nodiscard]] std::string_view MoveFocus(int direction) noexcept;
	//! Returns the requested item when it is enabled. The owner decides how to toggle the corresponding panel.
	[[nodiscard]] std::string_view Invoke(std::string_view id) const noexcept;
	[[nodiscard]] std::string_view InvokeFocused() const noexcept;
	//! Focuses the first or last interactive entry, matching Home and End.
	[[nodiscard]] std::string_view FocusEdge(int direction) noexcept;

	//! Position of one entry, or kNoIndex. Accessibility addresses entries by index.
	static constexpr std::size_t kNoIndex = static_cast<std::size_t>(-1);
	[[nodiscard]] std::size_t IndexOf(std::string_view id) const noexcept;
	[[nodiscard]] std::string_view IdAt(std::size_t index) const noexcept;

	[[nodiscard]] unsigned int GetDpi() const noexcept { return m_dpi; }
	[[nodiscard]] constexpr int GetIconSizeDip() const noexcept
	{
		return m_orientation == ActivityBarOrientation::Horizontal
			? kHorizontalIconDip : kVerticalIconDip;
	}
	[[nodiscard]] int GetIconSizePixels() const noexcept;
	[[nodiscard]] int GetItemFootprintPixels() const noexcept;
	[[nodiscard]] int GetOuterInsetPixels() const noexcept;
	[[nodiscard]] int GetPreferredWidthPixels() const noexcept;
	[[nodiscard]] int GetPreferredHeightPixels() const noexcept;
	//! Returns the preferred cross-axis extent of the bar.
	[[nodiscard]] int GetPreferredExtentPixels() const noexcept;

private:
	[[nodiscard]] static int ScaleDip(int dip, unsigned int dpi) noexcept;
	//! An entry only participates in hit testing, focus, and invocation when it is both
	//! present in the Activity Bar and enabled.
	[[nodiscard]] bool IsInteractive(std::size_t index) const noexcept;
	[[nodiscard]] std::size_t ResolveInteractive(std::string_view id) const noexcept;
	[[nodiscard]] std::size_t FirstEnabled(int direction) const noexcept;
	//! Drops any of the four transient states whose entry stopped being interactive.
	void ForgetUnusableStates() noexcept;
	void Reflow() noexcept;

	std::vector<ActivityBarEntry> m_entries;
	std::vector<ActivityBarRect> m_bounds;
	//! Keyed by ViewContainer id so a badge outlives any particular entry list.
	std::map<std::string, ActivityBarNumberBadge, std::less<>> m_badges;
	ActivityBarOrientation m_orientation = ActivityBarOrientation::Vertical;
	int m_widthPixels = kWidthDip;
	int m_heightPixels = 0;
	unsigned int m_dpi = 96;
	std::size_t m_selected = kNoIndex;
	std::size_t m_hovered = kNoIndex;
	std::size_t m_pressed = kNoIndex;
	std::size_t m_focused = kNoIndex;
};

} // namespace workbench::activity
