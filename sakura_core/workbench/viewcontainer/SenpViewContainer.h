/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include "workbench/layout/WorkbenchLayoutStateTypes.h"
#include "workbench/viewcontainer/IViewContainerPageProjection.h"
#include "workbench/viewcontainer/ViewContainerPageRegistry.h"
#include "workbench/viewcontainer/ViewPaneLayout.h"
#include <functional>

namespace workbench::viewcontainer {

//! Native body implementation selected by product composition. A package never
//! receives this port or an HWND. A body owns its root and all descendants;
//! Close is idempotent, and destruction completes cleanup after failed Create.
class ISenpViewBody {
public:
	virtual ~ISenpViewBody() = default;
	[[nodiscard]] virtual HWND Window() const noexcept = 0;
	virtual void Layout(const RECT& bounds, unsigned int dpi) noexcept = 0;
	virtual void SetVisible(bool visible) noexcept = 0;
	virtual void SetPalette(const theme::ThemePalette& palette, layout::EViewContainerLocation location) noexcept = 0;
	[[nodiscard]] virtual bool Focus() noexcept = 0;
	[[nodiscard]] virtual bool PreTranslate(MSG& message) noexcept = 0;
	virtual void Close() noexcept = 0;
};

struct SenpViewTitleAction final {
	std::string commandId;
	std::wstring title;
	std::wstring icon;
	bool enabled{ true };
};
struct SenpViewBodyHost final {
	HWND parent{};
	//! Report focus and pointer entry/exit from any descendant after its native
	//! state changes. The projection coalesces this into one posted UI update.
	//! The body must discard this callback on Close; it never outlives the body.
	std::function<void()> interactionChanged;
	//! Report a fatal native-body failure. The native callback must not throw
	//! or destroy the calling body; it marks the publication cohort unusable.
	//! Discard on Close together with interactionChanged.
	std::function<void()> projectionFailed;
};
using SenpViewBodyFactory = std::function<std::unique_ptr<ISenpViewBody>(SenpViewBodyHost host)>;
struct SenpNativeViewDefinition final {
	layout::WorkbenchViewDescriptor descriptor;
	SenpViewBodyFactory createBody;
	std::vector<SenpViewTitleAction> actions;
	bool initiallyCollapsed{};
	int preferredBodyDip{ 180 };
	int minimumBodyDip{ kViewPaneMinimumBodyDip };
};
struct SenpViewContainerOptions final {
	HWND parkingParent{};
	layout::WorkbenchContributionOwner owner;
	std::vector<layout::WorkbenchViewContainerDescriptor> containers;
	std::vector<SenpNativeViewDefinition> views;
	//! Synchronous native command/context adapters. False rejects the gesture.
	//! They must not pump messages or release the last owning projection reference.
	std::function<bool(std::string_view viewId)> requestFocus;
	std::function<bool(std::string_view viewId, std::string_view commandId)> execute;
};

enum class ESenpViewProjectionStatus : std::uint8_t {
	Applied, Invalid, Unsupported, Conflict, Failed, Stopped,
};
struct SenpViewPaneSnapshot final {
	std::string viewId;
	std::string containerId;
	HWND pane{};
	HWND header{};
	HWND body{};
	bool collapsed{};
	bool visible{};
	int preferredBodyDip{};
};

//! UI-thread-owned native View cohort for one contribution generation. Views
//! retain their bodies across collapse, visibility changes and container moves.
//! Containers own only their mounting surfaces; closing one never destroys a
//! View already moved elsewhere. No network/runtime polling is performed here.
class CSenpViewContainers final : public std::enable_shared_from_this<CSenpViewContainers> {
public:
	//! Prepares all native resources hidden. Missing bodies fail closed; there is
	//! no placeholder provider. Factories and callbacks remain native authority.
	[[nodiscard]] static std::shared_ptr<CSenpViewContainers> Create(SenpViewContainerOptions options) noexcept;
	~CSenpViewContainers();
	CSenpViewContainers(const CSenpViewContainers&) = delete;
	CSenpViewContainers& operator=(const CSenpViewContainers&) = delete;
	[[nodiscard]] std::vector<ViewContainerPageDescriptor> PageDescriptors();
	[[nodiscard]] bool IsUsable() const noexcept;
	//! Consumes a committed workbench placement/visibility snapshot. Unsupported
	//! destinations change nothing. Native failure compensates touched parents;
	//! failed compensation closes this generation instead of exposing partial UI.
	[[nodiscard]] ESenpViewProjectionStatus ApplyLayout(const layout::WorkbenchLayoutStateSnapshot& snapshot) noexcept;
	[[nodiscard]] bool FocusView(std::string_view viewId) noexcept;
	[[nodiscard]] bool SetCollapsed(std::string_view viewId, bool collapsed) noexcept;
	[[nodiscard]] ESenpViewProjectionStatus SetTitleActions(std::string_view viewId,
		std::vector<SenpViewTitleAction> actions) noexcept;
	[[nodiscard]] std::optional<SenpViewPaneSnapshot> Snapshot(std::string_view viewId) const;
	//! Revokes callbacks before destroying body resources; retained page factories
	//! and focus tokens cannot resurrect a closed contribution generation.
	void Close() noexcept;
private:
	struct Impl;
	class Page;
	explicit CSenpViewContainers(SenpViewContainerOptions options);
	std::unique_ptr<Impl> m_impl;
};

} // namespace workbench::viewcontainer
