/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include "workbench/SenpOwnerProjection.h"
#include "workbench/editor/SenpReadonlyDocumentHost.h"
#include "workbench/editor/SenpReadonlyEditorController.h"
#include "markdown/CMarkdownPreviewWnd.h"
#include "theme/CThemeService.h"

#include <map>

namespace workbench::editor {

using SenpOwnerCommandCompleted = std::function<bool(const senp::effect::OperationContext&,
	const senp::effect::CompleteCommand&)>;
using SenpOwnerResourceReleased = std::function<bool(std::wstring_view)>;
//! UI-thread-only observer. False means its owner has been revoked or destroyed.
using SenpReadonlyOwnerStyleSink = std::function<bool(const theme::ThemePalette&, const LOGFONT&, unsigned int)>;

/*!
	@brief Nonblocking tool-read seam between one owner target and the broker.

	Every method runs on the UI thread inside the owner projection's Pump, so an
	implementation must return without waiting on a pipe, a child process or a
	lock that is held across I/O. Start only admits a read; its terminal arrives
	later through Take. The owner identity selects the scope on every call
	because one editor process brokers reads for several owners over a single
	authenticated connection.
*/
class ISenpOwnerToolReads {
public:
	virtual ~ISenpOwnerToolReads() = default;
	[[nodiscard]] virtual bool Start(const senp::ContributionOwnerIdentity& owner,
		const senp::effect::OperationContext& context,
		const senp::effect::StartToolRead& read) noexcept = 0;
	//! Drains at most one finished terminal for this owner. An empty result means
	//! nothing has finished; it is a normal answer, not a failure.
	[[nodiscard]] virtual std::optional<senp::effect::ToolCompleted> Take(
		const senp::ContributionOwnerIdentity& owner) noexcept = 0;
	//! Cancels every read of one request lineage. A terminal already drained by
	//! the transport may still surface afterwards and must be discarded.
	virtual void Cancel(const senp::ContributionOwnerIdentity& owner,
		const senp::effect::OperationContext& context) noexcept = 0;
	//! Revocation obligation: no completion of this owner may be routed after it
	//! returns.
	virtual void CancelAll(const senp::ContributionOwnerIdentity& owner) noexcept = 0;
};

enum class SenpReadonlyOwnerTargetState : std::uint8_t {
	Ready, Invalid, ModelBeginFailed, ModelApplyFailed, HostFailed, EditorOpenFailed, EditorStoreFailed,
	EditorShowFailed, Revoked,
};

//! Native readonly-document destination for one committed SENP owner. It owns
//! document models and HWND hosts while the editor controller owns their Core
//! registrations. Controller finalizers retain each host independently, so a
//! failed/reentrant removal cannot leave a borrowed HWND dangling.
class CSenpReadonlyOwnerTarget final : public ISenpOwnerProjectionTarget {
public:
	CSenpReadonlyOwnerTarget(senp::ContributionOwnerIdentity owner,
		SenpReadonlyEditorController& editors, HWND parent,
		rendering::FrameSurfaceId firstSurfaceId,
		const ISenpReadonlyTextResources* resources = nullptr,
		SenpTextResourceView::CopySink copy = {},
		SenpOwnerCommandCompleted commandCompleted = {},
		SenpOwnerResourceReleased resourceReleased = {},
		ISenpOwnerToolReads* toolReads = nullptr
	);
	~CSenpReadonlyOwnerTarget() override;
	CSenpReadonlyOwnerTarget(const CSenpReadonlyOwnerTarget&) = delete;
	CSenpReadonlyOwnerTarget& operator=(const CSenpReadonlyOwnerTarget&) = delete;

	[[nodiscard]] bool BeginDocument(std::wstring_view resourceId,
		const senp::effect::OperationContext& context) noexcept override;
	[[nodiscard]] bool PublishDocument(const senp::effect::OperationContext& context,
		senp::effect::PublishDocument document) noexcept override;
	[[nodiscard]] bool FailDocument(const senp::effect::OperationContext& context,
		senp::InvocationStatus status) noexcept override;
	[[nodiscard]] bool CompleteCommand(const senp::effect::OperationContext& context,
		senp::effect::CompleteCommand completion) noexcept override;
	[[nodiscard]] bool ReleaseResource(std::wstring_view handle) noexcept override;
	[[nodiscard]] bool StartToolRead(const senp::effect::OperationContext& context,
		senp::effect::StartToolRead read) noexcept override;
	[[nodiscard]] std::optional<SenpToolReadTerminal> TakeToolRead() noexcept override;
	void CancelToolReads(const senp::effect::OperationContext& context) noexcept override;
	void Revoke() noexcept override;

	void SetStyle(const theme::ThemePalette& palette, const LOGFONT& font, unsigned int dpi) noexcept;
	[[nodiscard]] SenpReadonlyOwnerStyleSink StyleSink() const;
	[[nodiscard]] std::optional<std::string> InputId(std::wstring_view resourceId) const;
	[[nodiscard]] SenpReadonlyDocumentHost* Host(std::wstring_view resourceId) const noexcept;
	[[nodiscard]] std::size_t DocumentCount() const noexcept;
	[[nodiscard]] std::size_t ToolReadCount() const noexcept { return m_toolReadContexts.size(); }
	[[nodiscard]] SenpReadonlyOwnerTargetState State() const noexcept { return m_state; }

private:
	class Document;
	class Pending;
	[[nodiscard]] bool Matches(const senp::effect::OperationContext& context) const noexcept;
	void ReapClosed() noexcept;

	senp::ContributionOwnerIdentity m_owner;
	SenpReadonlyScope m_scope;
	SenpReadonlyEditorController& m_editors;
	HWND m_parent{};
	rendering::FrameSurfaceId m_firstSurfaceId{};
	const ISenpReadonlyTextResources* m_resources{};
	SenpTextResourceView::CopySink m_copy;
	SenpOwnerCommandCompleted m_commandCompleted;
	SenpOwnerResourceReleased m_resourceReleased;
	ISenpOwnerToolReads* m_toolReads{};
	//! readId -> the context that started it. A terminal whose readId is absent
	//! was cancelled or never admitted here and must never reach the projection.
	std::map<std::wstring, senp::effect::OperationContext, std::less<>> m_toolReadContexts;
	std::shared_ptr<CSenpReadonlyOwnerTarget*> m_styleLifetime;
	std::map<std::wstring, Pending, std::less<>> m_pending;
	std::map<std::wstring, std::shared_ptr<Document>, std::less<>> m_documents;
	std::size_t m_surfaceBlocks{};
	theme::ThemePalette m_palette{ theme::CThemeService::PaletteFor(theme::ThemeMode::Dark) };
	LOGFONT m_font{};
	unsigned int m_dpi{ 96 };
	bool m_revoked{};
	SenpReadonlyOwnerTargetState m_state{ SenpReadonlyOwnerTargetState::Ready };
};

} // namespace workbench::editor
