/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "workbench/editor/SenpReadonlyOwnerTarget.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace workbench::editor {
namespace {
bool ExtensionId(std::wstring_view source, std::string& result)
{
	if (source.empty() || source.size() > 128) return false;
	result.clear(); result.reserve(source.size());
	for (const wchar_t value : source) {
		if (!((value >= L'a' && value <= L'z') || (value >= L'0' && value <= L'9')
			|| value == L'.' || value == L'-')) return false;
		result.push_back(static_cast<char>(value));
	}
	return true;
}
class EditorRegistration final {
public:
	EditorRegistration(SenpReadonlyEditorController& controller, std::string inputId)
		: m_controller(controller), m_inputId(std::move(inputId)) {}
	~EditorRegistration() { if (m_active) (void)m_controller.Close(m_inputId); }
	void Release() noexcept { m_active = false; }
private:
	SenpReadonlyEditorController& m_controller;
	std::string m_inputId;
	bool m_active{ true };
};
}

class CSenpReadonlyOwnerTarget::Pending final {
public:
	Pending(senp::effect::OperationContext context, std::wstring resource)
		: m_context(std::move(context)), m_resource(std::move(resource)) {}
	[[nodiscard]] const senp::effect::OperationContext& Context() const noexcept { return m_context; }
	[[nodiscard]] const std::wstring& Resource() const noexcept { return m_resource; }
private:
	senp::effect::OperationContext m_context;
	std::wstring m_resource;
};

class CSenpReadonlyOwnerTarget::Document final {
public:
	Document(SenpReadonlyInput input, rendering::FrameSurfaceId surface,
		const ISenpReadonlyTextResources* resources, SenpTextResourceView::CopySink copy)
		: model(std::move(input)), host(model, surface, resources, std::move(copy)) {}

private:
	friend class CSenpReadonlyOwnerTarget;
	SenpReadonlyDocument model;
	SenpReadonlyDocumentHost host;
	std::string inputId;
	bool closed{};
};

CSenpReadonlyOwnerTarget::CSenpReadonlyOwnerTarget(senp::ContributionOwnerIdentity owner,
	SenpReadonlyEditorController& editors, const HWND parent,
	const rendering::FrameSurfaceId firstSurfaceId, const ISenpReadonlyTextResources* resources,
	SenpTextResourceView::CopySink copy, SenpOwnerCommandCompleted commandCompleted,
	SenpOwnerResourceReleased resourceReleased)
	: m_owner(std::move(owner)), m_editors(editors), m_parent(parent),
	  m_firstSurfaceId(firstSurfaceId), m_resources(resources), m_copy(std::move(copy)),
	  m_commandCompleted(std::move(commandCompleted)), m_resourceReleased(std::move(resourceReleased))
{
	if (!ExtensionId(m_owner.extensionId, m_scope.extensionId)) {
		m_revoked = true;
		m_state = SenpReadonlyOwnerTargetState::Invalid;
		return;
	}
	m_scope.ownerGeneration = m_owner.generation;
	m_scope.workspaceRevision = m_owner.workspaceRevision;
	m_scope.accountGeneration = m_owner.accountGeneration;
	if (const auto stock = ::GetStockObject(DEFAULT_GUI_FONT))
		(void)::GetObjectW(stock, sizeof(m_font), &m_font);
}

CSenpReadonlyOwnerTarget::~CSenpReadonlyOwnerTarget() { Revoke(); }

bool CSenpReadonlyOwnerTarget::Matches(const senp::effect::OperationContext& context) const noexcept
{
	return !m_revoked && context.ownerGeneration == m_owner.generation
		&& context.workspaceRevision == m_owner.workspaceRevision
		&& context.accountGeneration == m_owner.accountGeneration
		&& context.requestGeneration > 0 && !context.operationId.empty();
}

void CSenpReadonlyOwnerTarget::ReapClosed() noexcept
{
	for (auto current = m_documents.begin(); current != m_documents.end();) {
		if (current->second->closed) current = m_documents.erase(current);
		else ++current;
	}
}

bool CSenpReadonlyOwnerTarget::BeginDocument(std::wstring_view resourceId,
	const senp::effect::OperationContext& context) noexcept
{
	try {
		ReapClosed();
		if (!Matches(context) || resourceId.empty() || resourceId.size() > 512
			|| m_pending.size() >= SenpReadonlyWorkbench::kMaximumInputs
			|| m_pending.contains(context.operationId)) return false;
		return m_pending.emplace(context.operationId, Pending(context, std::wstring(resourceId))).second;
	} catch (...) { return false; }
}

bool CSenpReadonlyOwnerTarget::PublishDocument(const senp::effect::OperationContext& context,
	senp::effect::PublishDocument document) noexcept
{
	try {
		ReapClosed();
		const auto request = m_pending.find(context.operationId);
		if (request == m_pending.end() || request->second.Context() != context
			|| request->second.Resource() != document.resourceId) {
			m_state = SenpReadonlyOwnerTargetState::Invalid; return false;
		}
		m_pending.erase(request);
		if (!senp::effect::ValidateDocument(document)) {
			m_state = SenpReadonlyOwnerTargetState::Invalid; return false;
		}
		m_state = SenpReadonlyOwnerTargetState::ModelBeginFailed;
		if (auto existing = m_documents.find(document.resourceId); existing != m_documents.end()) {
			auto& value = *existing->second;
			if (value.model.Input().title != document.title
				|| value.model.Begin(context).result != SenpDocumentResult::Accepted
				|| value.model.Apply(context, std::move(document)).result != SenpDocumentResult::Accepted) {
				m_state = SenpReadonlyOwnerTargetState::ModelApplyFailed; return false;
			}
			if (value.host.Sync() != SenpDocumentHostState::Ready) {
				m_state = SenpReadonlyOwnerTargetState::HostFailed; return false;
			}
			const bool shown = m_editors.Show(value.inputId, true) == SenpReadonlyStatus::Succeeded;
			m_state = shown ? SenpReadonlyOwnerTargetState::Ready : SenpReadonlyOwnerTargetState::EditorShowFailed;
			return shown;
		}
		if (m_documents.size() >= SenpReadonlyWorkbench::kMaximumInputs
			|| m_surfaceBlocks >= SenpReadonlyWorkbench::kMaximumInputs || !::IsWindow(m_parent)
			|| m_firstSurfaceId == 0 || m_surfaceBlocks > ((std::numeric_limits<rendering::FrameSurfaceId>::max)() - m_firstSurfaceId) / 32)
			{ m_state = SenpReadonlyOwnerTargetState::Invalid; return false; }
		m_state = SenpReadonlyOwnerTargetState::EditorOpenFailed;
		const auto registered = m_editors.Workbench().Open(m_scope, document.resourceId, document.title);
		if (registered.status != SenpReadonlyStatus::Succeeded) return false;
		EditorRegistration registration(m_editors, registered.inputId);
		const auto* input = m_editors.Workbench().Find(registered.inputId);
		if (!input) return false;
		m_state = SenpReadonlyOwnerTargetState::ModelBeginFailed;
		const auto surface = m_firstSurfaceId + static_cast<rendering::FrameSurfaceId>(m_surfaceBlocks++) * 32;
		auto value = std::make_shared<Document>(*input,
			surface, m_resources, m_copy);
		if (value->model.Begin(context).result != SenpDocumentResult::Accepted) return false;
		m_state = SenpReadonlyOwnerTargetState::ModelApplyFailed;
		if (value->model.Apply(context, std::move(document)).result != SenpDocumentResult::Accepted) return false;
		m_state = SenpReadonlyOwnerTargetState::HostFailed;
		if (!value->host.Create(m_parent)) {
			m_state = SenpReadonlyOwnerTargetState::HostFailed; return false;
		}
		value->host.SetStyle(m_palette, m_font, m_dpi);
		if (value->host.Sync() != SenpDocumentHostState::Ready) {
			m_state = SenpReadonlyOwnerTargetState::HostFailed; return false;
		}
		std::weak_ptr<Document> weak = value;
		m_state = SenpReadonlyOwnerTargetState::EditorOpenFailed;
		auto opened = m_editors.Open(m_scope, value->model.Input().resourceId, value->model.Input().title,
			value->host.Window(), value->host.FocusWindow(), {
			.copy = [weak] { const auto locked = weak.lock(); return locked && locked->host.Copy(); },
			.selectAll = [weak] { if (const auto locked = weak.lock()) locked->host.SelectAll(); },
			.showFind = [weak] { if (const auto locked = weak.lock()) locked->host.ShowFind(true); },
			.find = [](bool) { return false; },
			.closed = [value] { value->closed = true; value->host.Close(); },
		});
		if (opened.status != SenpReadonlyStatus::Reused)
			{ m_state = SenpReadonlyOwnerTargetState::EditorOpenFailed; return false; }
		value->inputId = opened.inputId;
		m_state = SenpReadonlyOwnerTargetState::EditorStoreFailed;
		if (!m_documents.emplace(value->model.Input().resourceId, value).second) {
			m_state = SenpReadonlyOwnerTargetState::EditorStoreFailed; return false;
		}
		m_state = SenpReadonlyOwnerTargetState::EditorShowFailed;
		if (m_editors.Show(opened.inputId, true) != SenpReadonlyStatus::Succeeded) {
			ReapClosed(); return false;
		}
		registration.Release();
		m_state = SenpReadonlyOwnerTargetState::Ready;
		return true;
	} catch (...) { return false; }
}

bool CSenpReadonlyOwnerTarget::FailDocument(const senp::effect::OperationContext& context,
	senp::InvocationStatus) noexcept
{
	if (!Matches(context)) return false;
	const auto request = m_pending.find(context.operationId);
	if (request == m_pending.end() || request->second.Context() != context) return false;
	m_pending.erase(request); return true;
}

bool CSenpReadonlyOwnerTarget::CompleteCommand(const senp::effect::OperationContext& context,
	senp::effect::CompleteCommand completion) noexcept
{
	if (!Matches(context) || !m_commandCompleted) return false;
	try { return m_commandCompleted(context, completion); } catch (...) { return false; }
}

bool CSenpReadonlyOwnerTarget::ReleaseResource(std::wstring_view handle) noexcept
{
	if (m_revoked || !m_resourceReleased || handle.empty()) return false;
	try { return m_resourceReleased(handle); } catch (...) { return false; }
}

void CSenpReadonlyOwnerTarget::SetStyle(const theme::ThemePalette& palette,
	const LOGFONT& font, const unsigned int dpi) noexcept
{
	if (m_revoked || dpi < 48 || dpi > 768) return;
	m_palette = palette; m_font = font; m_dpi = dpi;
	try {
		ReapClosed();
		for (const auto& [resource, document] : m_documents) document->host.SetStyle(m_palette, m_font, m_dpi);
	} catch (...) { Revoke(); }
}

std::optional<std::string> CSenpReadonlyOwnerTarget::InputId(std::wstring_view resourceId) const
{
	const auto found = m_documents.find(resourceId);
	return found == m_documents.end() || found->second->closed
		? std::nullopt : std::optional<std::string>(found->second->inputId);
}

SenpReadonlyDocumentHost* CSenpReadonlyOwnerTarget::Host(std::wstring_view resourceId) const noexcept
{
	const auto found = m_documents.find(resourceId);
	return found == m_documents.end() || found->second->closed ? nullptr : &found->second->host;
}

std::size_t CSenpReadonlyOwnerTarget::DocumentCount() const noexcept
{
	return std::ranges::count_if(m_documents, [](const auto& value) { return !value.second->closed; });
}

void CSenpReadonlyOwnerTarget::Revoke() noexcept
{
	if (m_revoked) return;
	m_revoked = true; m_pending.clear();
	m_state = SenpReadonlyOwnerTargetState::Revoked;
	(void)m_editors.Revoke(m_scope);
	for (const auto& [resource, document] : m_documents) {
		if (!document->closed && m_editors.Remove(document->inputId)) {
			document->closed = true; document->host.Close();
		}
	}
	m_documents.clear();
	m_copy = {}; m_commandCompleted = {}; m_resourceReleased = {};
}

} // namespace workbench::editor
