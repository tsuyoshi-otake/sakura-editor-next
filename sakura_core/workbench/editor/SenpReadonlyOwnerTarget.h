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
		SenpOwnerResourceReleased resourceReleased = {}
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
	void Revoke() noexcept override;

	void SetStyle(const theme::ThemePalette& palette, const LOGFONT& font, unsigned int dpi) noexcept;
	[[nodiscard]] std::optional<std::string> InputId(std::wstring_view resourceId) const;
	[[nodiscard]] SenpReadonlyDocumentHost* Host(std::wstring_view resourceId) const noexcept;
	[[nodiscard]] std::size_t DocumentCount() const noexcept;
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
