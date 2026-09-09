/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include "workbench/editor/SenpReadonlyDocumentView.h"
#include "workbench/editor/SenpTextResourceView.h"

namespace workbench::editor {

//! Borrowed UI-thread authorization projection. Calls perform no I/O and may
//! not reenter the host. Composition calls Sync on every authority invalidation.
//! Resolve supplies the broker's resource revision, never the document revision.
class ISenpReadonlyTextResources {
public:
	virtual ~ISenpReadonlyTextResources() = default;
	[[nodiscard]] virtual std::optional<senp::TextResourceScope> Resolve(const SenpReadonlyScope& document,
		const senp::effect::TextResourceSection& section) const = 0;
	[[nodiscard]] virtual bool IsCurrent(const senp::TextResourceScope& scope, std::wstring_view handle) const = 0;
};
enum class SenpDocumentHostState : std::uint8_t { Unavailable, Ready, Denied, Unsupported, Failed, Closed };
enum class SenpDocumentPageKind : std::uint8_t { Structured, TextResource };
class SenpDocumentPage final {
public:
	SenpDocumentPage(SenpDocumentPageKind kind, std::wstring label, SenpStructuredSectionRange sections)
		: m_kind(kind), m_label(std::move(label)), m_sections(sections) {}
	[[nodiscard]] SenpDocumentPageKind Kind() const noexcept { return m_kind; }
	[[nodiscard]] const std::wstring& Label() const noexcept { return m_label; }
	[[nodiscard]] SenpStructuredSectionRange Sections() const noexcept { return m_sections; }
private:
	SenpDocumentPageKind m_kind;
	std::wstring m_label;
	SenpStructuredSectionRange m_sections;
};
class SenpDocumentTextRead final {
public:
	SenpDocumentTextRead(std::uint64_t generation, std::uint64_t ticket, senp::TextResourceScope scope,
		std::wstring handle, std::size_t offset, std::size_t count)
		: m_generation(generation), m_ticket(ticket), m_scope(std::move(scope)), m_handle(std::move(handle)),
		m_offset(offset), m_count(count) {}
	[[nodiscard]] std::uint64_t Generation() const noexcept { return m_generation; }
	[[nodiscard]] std::uint64_t Ticket() const noexcept { return m_ticket; }
	[[nodiscard]] const senp::TextResourceScope& Scope() const noexcept { return m_scope; }
	[[nodiscard]] const std::wstring& Handle() const noexcept { return m_handle; }
	[[nodiscard]] std::size_t Offset() const noexcept { return m_offset; }
	[[nodiscard]] std::size_t Count() const noexcept { return m_count; }
private:
	std::uint64_t m_generation, m_ticket;
	senp::TextResourceScope m_scope;
	std::wstring m_handle;
	std::size_t m_offset, m_count;
};
[[nodiscard]] inline bool operator==(const SenpDocumentTextRead& left, const SenpDocumentTextRead& right) noexcept {
	return left.Generation() == right.Generation() && left.Ticket() == right.Ticket() && left.Scope() == right.Scope()
		&& left.Handle() == right.Handle() && left.Offset() == right.Offset() && left.Count() == right.Count();
}

//! One native surface for a generic readonly input, with ordered internal
//! sections. It owns no Editor tabs, active-input state, I/O or polling.
//! Model and authorization projection outlive this object. Reserve 32 distinct
//! frame IDs starting at firstSurfaceId. Bind its root to the surface switcher.
class SenpReadonlyDocumentHost final {
public:
	SenpReadonlyDocumentHost(SenpReadonlyDocument& model, rendering::FrameSurfaceId firstSurfaceId,
		const ISenpReadonlyTextResources* resources = nullptr, SenpTextResourceView::CopySink copy = {});
	~SenpReadonlyDocumentHost();
	SenpReadonlyDocumentHost(const SenpReadonlyDocumentHost&) = delete;
	SenpReadonlyDocumentHost& operator=(const SenpReadonlyDocumentHost&) = delete;
	[[nodiscard]] bool Create(HWND parent);
	[[nodiscard]] SenpDocumentHostState Sync();
	void SetStyle(const theme::ThemePalette& palette, const LOGFONT& font, unsigned int dpi);
	void Layout(const RECT& bounds, unsigned int dpi);
	void Show(bool visible) noexcept;
	void Close() noexcept;
	[[nodiscard]] bool SelectPage(std::size_t index, bool focus = false);
	[[nodiscard]] std::vector<SenpDocumentPage> Pages() const;
	[[nodiscard]] std::optional<std::size_t> ActivePage() const noexcept;
	[[nodiscard]] SenpDocumentHostState State() const noexcept;
	//! At most one outstanding read. Empty Loading chunks wait for an explicit
	//! producer notification. The caller owns bounded execution and finalization
	//! even if this host replaces the generation or closes before the response.
	[[nodiscard]] std::optional<SenpDocumentTextRead> TakeTextRead();
	[[nodiscard]] SenpTextViewResult ApplyText(const SenpDocumentTextRead& request, const senp::TextResourceChunk& chunk);
	//! Settles an outstanding read that produced no chunk. Revoked says the
	//! resource itself is gone rather than that this read failed, so the body
	//! already shown is erased: bytes of a resource nobody may read again are
	//! not a partial answer, they are a resource that is no longer there. Every
	//! other reason leaves what did arrive on screen under a failed status.
	void FailText(const SenpDocumentTextRead& request, senp::TextResourceEnd reason = senp::TextResourceEnd::Failed) noexcept;
	void NotifyTextChanged(const senp::TextResourceScope& scope, std::wstring_view handle);
	void SelectAll();
	[[nodiscard]] std::wstring SelectedText();
	[[nodiscard]] bool Copy();
	void ShowFind(bool visible);
	[[nodiscard]] markdown::PreviewFindResult Find(std::wstring_view query, bool previous = false, bool matchCase = false);
	[[nodiscard]] HWND Window() const noexcept;
	//! Stable focus proxy: its WM_SETFOCUS resolves the currently selected body.
	[[nodiscard]] HWND FocusWindow() const noexcept;
private:
	class Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace workbench::editor
