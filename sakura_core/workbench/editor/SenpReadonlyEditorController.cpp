/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "workbench/editor/SenpReadonlyEditorController.h"
#include "workbench/editor/EditorCommandIds.h"

#include <algorithm>
#include <utility>

namespace workbench::editor {

SenpReadonlyEditorController::SenpReadonlyEditorController(EditorCoreService& core, HWND parent,
	HWND legacySurface, HWND legacyFocus, std::string legacyInputId)
	: m_workbench(core), m_switcher(m_workbench, parent), m_legacySurface(legacySurface),
	  m_legacyFocus(legacyFocus), m_legacyInputId(std::move(legacyInputId))
{
}

SenpReadonlyEditorController::~SenpReadonlyEditorController() { (void)Shutdown(); }

void SenpReadonlyEditorController::EnsureLegacyBinding() noexcept
{
	if (m_closed || !::IsWindow(m_legacySurface)) return;
	const auto snapshot = m_workbench.Snapshot();
	if (std::ranges::any_of(snapshot.group.inputs, [this](const auto& input) {
		return input.descriptor.inputId == m_legacyInputId;
	})) {
		// False also means the exact surface is already bound, which is the common path.
		(void)m_switcher.Bind(m_legacyInputId, m_legacySurface, m_legacyFocus);
	}
}

bool SenpReadonlyEditorController::ReleaseSurface(
	std::map<std::string, Surface, std::less<>>::iterator found) noexcept
{
	if (found == m_surfaces.end()) return true;
	// Unbind may be refused while the switcher is synchronously processing a
	// window callback. Retain both the HWND borrow and its finalizer for the next
	// coalesced Core notification instead of making destruction appear safe.
	if (!m_switcher.Unbind(found->first)) return false;
	auto callback = std::move(found->second.m_commands.closed);
	m_surfaces.erase(found);
	if (callback && !m_callbackActive) {
		m_callbackActive = true;
		try { callback(); } catch (...) {}
		m_callbackActive = false;
	}
	return true;
}

void SenpReadonlyEditorController::RetireMissingSurfaces() noexcept
{
	const auto snapshot = m_workbench.Snapshot();
	for (auto it = m_surfaces.begin(); it != m_surfaces.end();) {
		const bool exists = std::ranges::any_of(snapshot.group.inputs, [&it](const auto& input) {
			return input.descriptor.inputId == it->first;
		});
		if (exists) { ++it; continue; }
		auto retiring = it++;
		if (!ReleaseSurface(retiring)) break;
	}
}

SenpReadonlyOpenResult SenpReadonlyEditorController::Open(SenpReadonlyScope scope,
	std::wstring resourceId, std::wstring title, HWND surface, HWND focus,
	SenpReadonlySurfaceCommands commands)
{
	if (m_closed || m_callbackActive) return { SenpReadonlyStatus::Closed };
	auto result = m_workbench.Open(std::move(scope), std::move(resourceId), std::move(title));
	if (result.status != SenpReadonlyStatus::Succeeded && result.status != SenpReadonlyStatus::Reused)
		return result;
	if (const auto existing = m_surfaces.find(result.inputId); existing != m_surfaces.end())
		return result;
	if (!m_switcher.Bind(result.inputId, surface, focus)) {
		if (result.status == SenpReadonlyStatus::Succeeded) (void)m_workbench.Close(result.inputId);
		return { SenpReadonlyStatus::Failed };
	}
	try {
		m_surfaces.emplace(result.inputId, Surface(surface, std::move(commands)));
	} catch (...) {
		(void)m_switcher.Unbind(result.inputId);
		if (result.status == SenpReadonlyStatus::Succeeded) (void)m_workbench.Close(result.inputId);
		throw;
	}
	return result;
}

SenpReadonlyStatus SenpReadonlyEditorController::Show(const std::string_view inputId, const bool focus)
{
	if (m_closed || m_callbackActive) return SenpReadonlyStatus::Closed;
	if (inputId != m_legacyInputId && !m_surfaces.contains(inputId)) return SenpReadonlyStatus::NotFound;
	return m_switcher.Show(inputId, focus) == SenpSurfaceProjection::Applied
		? SenpReadonlyStatus::Succeeded : SenpReadonlyStatus::Failed;
}

SenpReadonlyStatus SenpReadonlyEditorController::Close(const std::string_view inputId) noexcept
{
	if (m_callbackActive) return SenpReadonlyStatus::Conflict;
	SenpReadonlyStatus status;
	try { status = m_workbench.Close(inputId); }
	catch (...) { return SenpReadonlyStatus::Failed; }
	if (status != SenpReadonlyStatus::Succeeded && status != SenpReadonlyStatus::NotFound) return status;
	if (const auto found = m_surfaces.find(inputId); found != m_surfaces.end()) (void)ReleaseSurface(found);
	(void)Apply();
	return status;
}

SenpReadonlyStatus SenpReadonlyEditorController::Revoke(const SenpReadonlyScope& scope) noexcept
{
	if (m_closed || m_callbackActive) return SenpReadonlyStatus::Closed;
	std::vector<std::string> retiring;
	for (const auto& input : m_workbench.Inputs()) if (input.scope == scope) retiring.push_back(input.inputId);
	auto terminal = SenpReadonlyStatus::Succeeded;
	for (const auto& inputId : retiring) {
		const auto status = Close(inputId);
		if (status != SenpReadonlyStatus::Succeeded && status != SenpReadonlyStatus::NotFound) terminal = status;
	}
	return terminal;
}

bool SenpReadonlyEditorController::Remove(const std::string_view inputId) noexcept
{
	if (m_closed || m_callbackActive) return false;
	const auto found = m_surfaces.find(inputId);
	if (found == m_surfaces.end()) return true;
	if (!ReleaseSurface(found)) return false;
	(void)Apply();
	return true;
}

SenpSurfaceProjection SenpReadonlyEditorController::Apply(const bool focus) noexcept
{
	if (m_closed) return SenpSurfaceProjection::Closed;
	RetireMissingSurfaces();
	EnsureLegacyBinding();
	return m_switcher.Apply(focus);
}

SenpSurfaceProjection SenpReadonlyEditorController::Layout(const RECT bounds) noexcept
{
	if (m_closed) return SenpSurfaceProjection::Closed;
	RetireMissingSurfaces();
	EnsureLegacyBinding();
	return m_switcher.Layout(bounds);
}

void SenpReadonlyEditorController::SetVisible(const bool visible) noexcept
{
	if (!m_closed) m_switcher.SetVisible(visible);
}

bool SenpReadonlyEditorController::RefreshStrings() noexcept
{
	if (m_closed || m_callbackActive) return false;
	m_callbackActive = true;
	struct Reset final { bool& active; ~Reset() { active = false; } } reset{ m_callbackActive };
	bool succeeded = true;
	for (auto& [inputId, surface] : m_surfaces) {
		if (surface.m_commands.refreshStrings) {
			try { surface.m_commands.refreshStrings(); }
			catch (...) { succeeded = false; }
		}
	}
	return succeeded;
}

bool SenpReadonlyEditorController::IsReadonlyActive() const noexcept
{
	try {
		const auto snapshot = m_workbench.Snapshot();
		return snapshot.group.activeInputId && m_workbench.Find(*snapshot.group.activeInputId) != nullptr;
	} catch (...) { return false; }
}

bool SenpReadonlyEditorController::OwnsFocus(const HWND window) const noexcept
{
	if (!window) return false;
	if (m_legacySurface == window || (::IsWindow(m_legacySurface) && ::IsChild(m_legacySurface, window))) return true;
	for (const auto& [id, surface] : m_surfaces) {
		if (surface.m_window == window || (::IsWindow(surface.m_window) && ::IsChild(surface.m_window, window))) return true;
	}
	return false;
}

SenpReadonlyCommandStatus SenpReadonlyEditorController::Execute(const std::string_view commandId) noexcept
{
	if (m_closed) return SenpReadonlyCommandStatus::Closed;
	if (m_callbackActive) return SenpReadonlyCommandStatus::Failed;
	try {
		switch (m_workbench.Route(commandId)) {
		case SenpEditorCommandRoute::Legacy:
		case SenpEditorCommandRoute::Workbench:
		case SenpEditorCommandRoute::NoActiveInput:
			return SenpReadonlyCommandStatus::NotHandled;
		case SenpEditorCommandRoute::NotApplicable:
			return SenpReadonlyCommandStatus::NotApplicable;
		case SenpEditorCommandRoute::CloseReadonly: {
			const auto snapshot = m_workbench.Snapshot();
			return snapshot.group.activeInputId
				&& Close(*snapshot.group.activeInputId) == SenpReadonlyStatus::Succeeded
				? SenpReadonlyCommandStatus::Succeeded : SenpReadonlyCommandStatus::Failed;
		}
		case SenpEditorCommandRoute::ReadonlySurface:
			break;
		}
		const auto snapshot = m_workbench.Snapshot();
		if (!snapshot.group.activeInputId) return SenpReadonlyCommandStatus::Failed;
		const auto found = m_surfaces.find(*snapshot.group.activeInputId);
		if (found == m_surfaces.end() || !::IsWindow(found->second.m_window)) return SenpReadonlyCommandStatus::Failed;
		m_callbackActive = true;
		struct Reset final { bool& active; ~Reset() { active = false; } } reset{ m_callbackActive };
		auto& commands = found->second.m_commands;
		if (commandId == "editor.action.clipboardCopyAction")
			return commands.copy && commands.copy() ? SenpReadonlyCommandStatus::Succeeded : SenpReadonlyCommandStatus::Failed;
		if (commandId == "editor.action.selectAll") {
			if (!commands.selectAll) return SenpReadonlyCommandStatus::Failed;
			commands.selectAll(); return SenpReadonlyCommandStatus::Succeeded;
		}
		if (commandId == "actions.find") {
			if (!commands.showFind) return SenpReadonlyCommandStatus::Failed;
			commands.showFind(); return SenpReadonlyCommandStatus::Succeeded;
		}
		if (commandId == "editor.action.nextMatchFindAction" || commandId == "editor.action.previousMatchFindAction")
			return commands.find && commands.find(commandId == "editor.action.previousMatchFindAction")
				? SenpReadonlyCommandStatus::Succeeded : SenpReadonlyCommandStatus::Failed;
		return SenpReadonlyCommandStatus::NotApplicable;
	} catch (...) {
		m_callbackActive = false;
		return SenpReadonlyCommandStatus::Failed;
	}
}

SenpReadonlyStatus SenpReadonlyEditorController::Shutdown() noexcept
{
	if (m_closed) return SenpReadonlyStatus::Closed;
	if (m_callbackActive) return SenpReadonlyStatus::Conflict;
	SenpReadonlyStatus status;
	try { status = m_workbench.Shutdown(); }
	catch (...) { return SenpReadonlyStatus::Failed; }
	if (status != SenpReadonlyStatus::Succeeded) return status;
	while (!m_surfaces.empty()) if (!ReleaseSurface(m_surfaces.begin())) return SenpReadonlyStatus::Conflict;
	m_switcher.Close();
	m_closed = true;
	return status;
}

} // namespace workbench::editor
