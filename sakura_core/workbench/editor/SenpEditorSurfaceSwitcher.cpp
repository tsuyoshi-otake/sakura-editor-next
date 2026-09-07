/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "workbench/editor/SenpEditorSurfaceSwitcher.h"

#include <algorithm>
#include <map>
#include <utility>

namespace workbench::editor {
namespace { constexpr wchar_t kSurfaceOwner[] = L"Sakura.Senp.EditorSurfaceOwner"; }

struct SenpEditorSurfaceSwitcher::Impl final {
	struct Surface final { HWND window{}, focus{}; RECT bounds{}; bool positioned{}; };
	SenpReadonlyWorkbench& workbench;
	HWND parent{};
	std::map<std::string, Surface, std::less<>> surfaces;
	RECT bounds{};
	std::string presented;
	bool closed{}, visible{ true }, hasBounds{}, projecting{}, projectedVisible{};
	Impl(SenpReadonlyWorkbench& value, HWND window) : workbench(value), parent(window) {}
	bool Exists(const Surface& surface) const noexcept {
		return ::IsWindow(parent) && ::IsWindow(surface.window) && ::GetParent(surface.window) == parent;
	}
	bool Valid(const Surface& surface) const noexcept {
		return Exists(surface) && ::GetPropW(surface.window, kSurfaceOwner) == this;
	}
	bool OwnsFocus(const Surface& surface, HWND focus) const noexcept {
		return Valid(surface) && focus && (surface.window == focus || ::IsChild(surface.window, focus));
	}
	void CaptureFocus() noexcept {
		const auto found = surfaces.find(presented);
		if (found != surfaces.end() && OwnsFocus(found->second, ::GetFocus())) found->second.focus = ::GetFocus();
	}
	void HideAll() noexcept {
		CaptureFocus();
		bool ownedFocus = false;
		for (const auto& [id, surface] : surfaces) {
			ownedFocus = ownedFocus || OwnsFocus(surface, ::GetFocus());
			if (Valid(surface)) ::ShowWindow(surface.window, SW_HIDE);
		}
		if (ownedFocus && ::IsWindow(parent)) ::SetFocus(parent);
	}
	void Repaint() const noexcept {
		// Invalidate the vacated rectangle too. A new smaller/moved rectangle alone
		// cannot repair pixels outside its own bounds.
		if (::IsWindow(parent)) ::RedrawWindow(parent, nullptr, nullptr,
			RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
	}
	void Release(Surface& surface) const noexcept {
		if (Valid(surface)) ::RemovePropW(surface.window, kSurfaceOwner);
	}
	void FinishClose() noexcept {
		projecting = true;
		HideAll();
		for (auto& [id, surface] : surfaces) Release(surface);
		surfaces.clear(); presented.clear(); Repaint();
		projecting = false;
	}
	void FinishProjection() noexcept {
		projecting = false;
		if (closed) FinishClose();
	}
	SenpSurfaceProjection Project(bool focus) {
		if (closed) return SenpSurfaceProjection::Closed;
		if (projecting) return SenpSurfaceProjection::Unavailable;
		projecting = true;
		struct Reset final { Impl& state; ~Reset() { state.FinishProjection(); } } reset{ *this };
		const auto snapshot = workbench.Snapshot();
		bool changed = false;
		// Prune registrations only after the core owns a committed close. Never
		// destroy a borrowed native control while its owner may be in a callback.
		for (auto it = surfaces.begin(); it != surfaces.end();) {
			const bool exists = std::ranges::any_of(snapshot.group.inputs, [&](const auto& input) {
				return input.descriptor.inputId == it->first;
			});
			if (!exists) {
				if (OwnsFocus(it->second, ::GetFocus())) ::SetFocus(parent);
				if (Valid(it->second)) ::ShowWindow(it->second.window, SW_HIDE);
				if (closed) return SenpSurfaceProjection::Closed;
				Release(it->second); changed = true;
				it = surfaces.erase(it);
			} else ++it;
		}
		const auto target = snapshot.group.activeInputId ? surfaces.find(*snapshot.group.activeInputId) : surfaces.end();
		if (target == surfaces.end() || !Valid(target->second)) {
			changed = changed || !presented.empty();
			HideAll();
			presented.clear();
			projectedVisible = false;
			if (changed) Repaint();
			if (closed) return SenpSurfaceProjection::Closed;
			return snapshot.group.activeInputId ? SenpSurfaceProjection::Unavailable : SenpSurfaceProjection::Empty;
		}
		const auto old = surfaces.find(presented);
		const bool transferFocus = focus || (old != surfaces.end() && OwnsFocus(old->second, ::GetFocus()));
		changed = changed || presented != target->first || projectedVisible != visible;
		CaptureFocus();
		for (const auto& [id, surface] : surfaces) {
			if (Valid(surface) && (id != target->first || !visible)) ::ShowWindow(surface.window, SW_HIDE);
			if (closed) return SenpSurfaceProjection::Closed;
		}
		const bool move = hasBounds && (!target->second.positioned || !::EqualRect(&target->second.bounds, &bounds));
		if (move && !::SetWindowPos(target->second.window, nullptr, bounds.left, bounds.top,
			bounds.right - bounds.left, bounds.bottom - bounds.top,
			SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS)) {
			HideAll(); presented.clear(); Repaint();
			return SenpSurfaceProjection::Unavailable;
		}
		if (closed) return SenpSurfaceProjection::Closed;
		if (move) { target->second.bounds = bounds; target->second.positioned = true; changed = true; }
		presented = target->first;
		projectedVisible = visible;
		if (visible) {
			::ShowWindow(target->second.window, SW_SHOWNA);
			if (closed) return SenpSurfaceProjection::Closed;
			if (transferFocus) {
				const auto desired = OwnsFocus(target->second, target->second.focus)
					? target->second.focus : target->second.window;
				::SetFocus(desired);
			}
		} else if (transferFocus) ::SetFocus(parent);
		if (changed) Repaint();
		return closed ? SenpSurfaceProjection::Closed : SenpSurfaceProjection::Applied;
	}
};

SenpEditorSurfaceSwitcher::SenpEditorSurfaceSwitcher(SenpReadonlyWorkbench& workbench, HWND parent)
	: m_impl(std::make_unique<Impl>(workbench, parent)) {}
SenpEditorSurfaceSwitcher::~SenpEditorSurfaceSwitcher() { Close(); }
bool SenpEditorSurfaceSwitcher::Bind(std::string inputId, HWND surface, HWND initialFocus)
{
	auto& state = *m_impl;
	if (state.closed || state.projecting || state.surfaces.size() >= SenpReadonlyWorkbench::kMaximumInputs + 1
		|| !state.Exists({ surface, initialFocus }) || state.surfaces.contains(inputId)
		|| ::GetPropW(surface, kSurfaceOwner)) return false;
	const auto snapshot = state.workbench.Snapshot();
	if (!std::ranges::any_of(snapshot.group.inputs, [&](const auto& input) { return input.descriptor.inputId == inputId; })
		|| std::ranges::any_of(state.surfaces, [&](const auto& entry) { return entry.second.window == surface; })) return false;
	if (initialFocus && surface != initialFocus && !::IsChild(surface, initialFocus)) return false;
	if (!::SetPropW(surface, kSurfaceOwner, &state)) return false;
	try { state.surfaces.emplace(std::move(inputId), Impl::Surface{ surface, initialFocus ? initialFocus : surface }); }
	catch (...) { ::RemovePropW(surface, kSurfaceOwner); throw; }
	return true;
}
bool SenpEditorSurfaceSwitcher::Unbind(std::string_view inputId) noexcept
{
	auto& state = *m_impl;
	if (state.projecting) return false;
	const auto found = state.surfaces.find(inputId);
	if (found == state.surfaces.end()) return true;
	state.projecting = true;
	struct Reset final { Impl& state; ~Reset() { state.FinishProjection(); } } reset{ state };
	if (state.OwnsFocus(found->second, ::GetFocus())) ::SetFocus(state.parent);
	if (state.Valid(found->second)) ::ShowWindow(found->second.window, SW_HIDE);
	state.Release(found->second);
	state.surfaces.erase(found);
	if (state.presented == inputId) state.presented.clear();
	state.Repaint();
	return true;
}
SenpSurfaceProjection SenpEditorSurfaceSwitcher::Show(std::string_view inputId, bool focus) noexcept
{
	auto& state = *m_impl;
	if (state.closed) return SenpSurfaceProjection::Closed;
	const auto found = state.surfaces.find(inputId);
	if (state.projecting || found == state.surfaces.end() || !state.Valid(found->second)) return SenpSurfaceProjection::Unavailable;
	try {
		if (state.workbench.Show(inputId) != SenpReadonlyStatus::Succeeded) return SenpSurfaceProjection::Unavailable;
		return state.Project(focus);
	} catch (...) {
		Close();
		return SenpSurfaceProjection::Unavailable;
	}
}
SenpSurfaceProjection SenpEditorSurfaceSwitcher::Apply(bool focus) noexcept
{
	try { return m_impl->Project(focus); }
	catch (...) { Close(); return SenpSurfaceProjection::Unavailable; }
}
SenpSurfaceProjection SenpEditorSurfaceSwitcher::Layout(RECT bounds) noexcept
{
	if (bounds.right < bounds.left || bounds.bottom < bounds.top
		|| static_cast<long long>(bounds.right) - bounds.left > INT_MAX
		|| static_cast<long long>(bounds.bottom) - bounds.top > INT_MAX) return SenpSurfaceProjection::Unavailable;
	m_impl->bounds = bounds;
	m_impl->hasBounds = true;
	return Apply();
}
void SenpEditorSurfaceSwitcher::SetVisible(bool visible) noexcept
{
	if (m_impl->closed || m_impl->visible == visible) return;
	m_impl->visible = visible;
	(void)Apply();
}
void SenpEditorSurfaceSwitcher::Close() noexcept
{
	if (m_impl->closed) return;
	m_impl->closed = true;
	if (!m_impl->projecting) m_impl->FinishClose();
}

} // namespace workbench::editor
