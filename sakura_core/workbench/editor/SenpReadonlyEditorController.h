/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include "workbench/editor/SenpEditorSurfaceSwitcher.h"

#include <functional>
#include <map>
#include <utility>

namespace workbench::editor {

struct SenpReadonlySurfaceCommands final {
	std::function<bool()> copy;
	std::function<void()> selectAll;
	std::function<void()> showFind;
	std::function<bool(bool previous)> find;
	std::function<void()> refreshStrings;
	//! Called after the input has left Editor Core and the HWND has been unbound.
	//! The callback may destroy the borrowed surface and must not reenter this controller.
	std::function<void()> closed;
};

enum class SenpReadonlyCommandStatus : std::uint8_t {
	NotHandled,
	Succeeded,
	Failed,
	NotApplicable,
	Closed,
};

//! Window composition for the existing Editor Core group and retained SENP HWNDs.
//! The controller owns input registrations and native projection, but borrows every
//! surface. A publisher must keep a surface alive until `closed` runs or Remove
//! succeeds. All methods are UI-thread-only and every recognized command is terminal.
class SenpReadonlyEditorController final {
public:
	SenpReadonlyEditorController(EditorCoreService& core, HWND parent,
		HWND legacySurface, HWND legacyFocus, std::string legacyInputId);
	~SenpReadonlyEditorController();
	SenpReadonlyEditorController(const SenpReadonlyEditorController&) = delete;
	SenpReadonlyEditorController& operator=(const SenpReadonlyEditorController&) = delete;
	[[nodiscard]] SenpReadonlyOpenResult Open(SenpReadonlyScope scope,
		std::wstring resourceId, std::wstring title, HWND surface, HWND focus,
		SenpReadonlySurfaceCommands commands);
	[[nodiscard]] SenpReadonlyStatus Show(std::string_view inputId, bool focus = true);
	[[nodiscard]] SenpReadonlyStatus Close(std::string_view inputId) noexcept;
	[[nodiscard]] SenpReadonlyStatus Revoke(const SenpReadonlyScope& scope) noexcept;
	//! Removes a still-open surface without closing its Core input. This is allowed
	//! only for failure recovery; projection then fails closed until a replacement binds.
	[[nodiscard]] bool Remove(std::string_view inputId) noexcept;
	[[nodiscard]] SenpSurfaceProjection Apply(bool focus = false) noexcept;
	[[nodiscard]] SenpSurfaceProjection Layout(RECT bounds) noexcept;
	void SetVisible(bool visible) noexcept;
	[[nodiscard]] bool RefreshStrings() noexcept;
	[[nodiscard]] bool IsReadonlyActive() const noexcept;
	[[nodiscard]] bool OwnsFocus(HWND window) const noexcept;
	[[nodiscard]] SenpReadonlyCommandStatus Execute(std::string_view commandId) noexcept;
	[[nodiscard]] SenpReadonlyStatus Shutdown() noexcept;
	[[nodiscard]] SenpReadonlyWorkbench& Workbench() noexcept { return m_workbench; }
	[[nodiscard]] const SenpReadonlyWorkbench& Workbench() const noexcept { return m_workbench; }
private:
	class Surface final {
	public:
		Surface(HWND window, SenpReadonlySurfaceCommands commands)
			: m_window(window), m_commands(std::move(commands)) {}
	private:
		friend class SenpReadonlyEditorController;
		HWND m_window{};
		SenpReadonlySurfaceCommands m_commands;
	};
	void EnsureLegacyBinding() noexcept;
	void RetireMissingSurfaces() noexcept;
	[[nodiscard]] bool ReleaseSurface(std::map<std::string, Surface, std::less<>>::iterator) noexcept;

	SenpReadonlyWorkbench m_workbench;
	SenpEditorSurfaceSwitcher m_switcher;
	HWND m_legacySurface{};
	HWND m_legacyFocus{};
	std::string m_legacyInputId;
	std::map<std::string, Surface, std::less<>> m_surfaces;
	bool m_closed{};
	bool m_callbackActive{};
};

} // namespace workbench::editor
