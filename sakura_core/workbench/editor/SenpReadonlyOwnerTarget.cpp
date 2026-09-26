/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "workbench/editor/SenpReadonlyOwnerTarget.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace workbench::editor {
namespace {
constexpr std::size_t kCompletionMessageUnits = 200;
constexpr std::size_t kCompletionNameUnits = 64;

//! One status line out of arbitrary text. Runs of whitespace controls become a
//! single space and are dropped at both ends; every other control unit becomes
//! the visible replacement character rather than being deleted, so text that
//! carried one is never silently presented as if it had not. Truncation appends
//! an ellipsis, and drops a high surrogate it would otherwise strand alone.
std::wstring OneLine(std::wstring_view text, const std::size_t maximum)
{
	std::wstring line;
	bool space = false;
	for (const wchar_t unit : text) {
		if (unit == L' ' || unit == L'\t' || unit == L'\n' || unit == L'\r') {
			space = !line.empty();
			continue;
		}
		if (line.size() + (space ? 2 : 1) > maximum) {
			if (!line.empty() && line.back() >= 0xd800 && line.back() <= 0xdbff) line.pop_back();
			line.push_back(L'\x2026');
			return line;
		}
		if (space) { line.push_back(L' '); space = false; }
		line.push_back(unit < 0x20 || unit == 0x7f ? L'\xfffd' : unit);
	}
	return line;
}

//! What the status says happened. Succeeded has no phrase: the extension's own
//! message stands alone, and a silent success is never told at all.
std::wstring_view Phrase(const senp::effect::CompletionStatus status)
{
	switch (status) {
	case senp::effect::CompletionStatus::Succeeded: return {};
	case senp::effect::CompletionStatus::Cancelled: return L"the command was cancelled";
	case senp::effect::CompletionStatus::Failed: return L"the command failed";
	case senp::effect::CompletionStatus::TimedOut: return L"the command timed out";
	case senp::effect::CompletionStatus::HostUnavailable: return L"the command could not reach its host";
	}
	// A status this build does not name is still a status the user was owed.
	return L"the command ended for an unknown reason";
}

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
	SenpReadonlyEditorController& editors, SenpReadonlyOwnerSurface surface,
	const rendering::FrameSurfaceId firstSurfaceId, const ISenpReadonlyTextResources* resources,
	SenpTextResourceView::CopySink copy, SenpOwnerCommandCompleted commandCompleted,
	SenpOwnerResourceReleased resourceReleased, ISenpOwnerToolReads* toolReads)
	: m_owner(std::move(owner)), m_editors(editors), m_surface(std::move(surface)),
	  m_firstSurfaceId(firstSurfaceId), m_resources(resources), m_copy(std::move(copy)),
	  m_commandCompleted(std::move(commandCompleted)), m_resourceReleased(std::move(resourceReleased)),
	  m_toolReads(toolReads),
	  m_lifetime(std::make_shared<CSenpReadonlyOwnerTarget*>(this))
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
	} catch (const std::exception&) { return false; }
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
			|| m_surfaceBlocks >= SenpReadonlyWorkbench::kMaximumInputs || !m_surface.Present()
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
		if (!m_surface.Place(value->host)) {
			m_state = SenpReadonlyOwnerTargetState::HostFailed; return false;
		}
		value->host.SetStyle(m_palette, m_font, m_dpi);
		if (value->host.Sync() != SenpDocumentHostState::Ready) {
			m_state = SenpReadonlyOwnerTargetState::HostFailed; return false;
		}
		std::weak_ptr<Document> weak = value;
		m_state = SenpReadonlyOwnerTargetState::EditorOpenFailed;
		auto opened = m_editors.Open(m_scope, value->model.Input().resourceId, value->model.Input().title,
			value->host.Window(), value->host.Window(), {
			.copy = [weak] { const auto locked = weak.lock(); return locked && locked->host.Copy(); },
			.selectAll = [weak] { if (const auto locked = weak.lock()) locked->host.SelectAll(); },
			.showFind = [weak] { if (const auto locked = weak.lock()) locked->host.ShowFind(true); },
			.find = [](bool) { return false; },
			.refreshStrings = [weak] { if (const auto locked = weak.lock()) locked->host.RefreshStrings(); },
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
	} catch (const std::exception&) { return false; }
}

bool CSenpReadonlyOwnerTarget::FailDocument(const senp::effect::OperationContext& context,
	senp::InvocationStatus) noexcept
{
	if (!Matches(context)) return false;
	const auto request = m_pending.find(context.operationId);
	if (request == m_pending.end() || request->second.Context() != context) return false;
	m_pending.erase(request); return true;
}

std::wstring SenpCommandCompletionStatus(const std::wstring_view extensionId,
	const senp::effect::CompleteCommand& completion)
{
	const auto message = OneLine(completion.message, kCompletionMessageUnits);
	if (completion.status == senp::effect::CompletionStatus::Succeeded && message.empty()) return {};
	// The name is bounded well below the message so a long identity cannot crowd
	// out what the extension actually said, and an absent one is stated rather
	// than dropped: a line with no speaker is the one thing this must not be.
	auto line = OneLine(extensionId, kCompletionNameUnits);
	if (line.empty()) line = L"unknown extension";
	line.append(L": ");
	const auto phrase = Phrase(completion.status);
	if (phrase.empty()) line.append(message);
	else if (message.empty()) { line.append(phrase); line.push_back(L'.'); }
	else { line.append(phrase); line.append(L" - "); line.append(message); }
	return line;
}

bool CSenpReadonlyOwnerTarget::CompleteCommand(const senp::effect::OperationContext& context,
	senp::effect::CompleteCommand completion) noexcept
{
	if (!Matches(context) || !m_commandCompleted) return false;
	try { return m_commandCompleted(context, completion); } catch (const std::exception&) { return false; }
}

bool CSenpReadonlyOwnerTarget::ReleaseResource(std::wstring_view handle) noexcept
{
	if (m_revoked || !m_resourceReleased || handle.empty()) return false;
	try { return m_resourceReleased(handle); } catch (const std::exception&) { return false; }
}

bool CSenpReadonlyOwnerTarget::StartToolRead(const senp::effect::OperationContext& context,
	senp::effect::StartToolRead read) noexcept
{
	// The projection already refused a duplicate readId and enforced its own
	// pending bound. This check is not redundant: the target is the last owner
	// of the readId -> context map, and a map that outgrows the session bound
	// could never be drained within one lifetime.
	if (!Matches(context) || !m_toolReads) return false;
	if (read.readId.empty() || read.readId.size() > 512) return false;
	if (m_toolReadContexts.size() >= senp::CSenpRuntimeSession::kMaximumPending
		|| m_toolReadContexts.contains(read.readId)) return false;
	try {
		auto readId = read.readId;
		// Recorded before dispatch so a terminal that arrives during Start is
		// still routable. A refused start removes it again.
		if (!m_toolReadContexts.emplace(readId, context).second) return false;
		if (!m_toolReads->Start(m_owner, context, read)) {
			m_toolReadContexts.erase(readId);
			return false;
		}
		return true;
	} catch (const std::exception&) { return false; }
}

std::optional<SenpToolReadTerminal> CSenpReadonlyOwnerTarget::TakeToolRead() noexcept
{
	if (m_revoked || !m_toolReads) return {};
	// Bounded drain. A completion whose readId is unknown was cancelled while it
	// was already in flight; handing it to the projection would be reported as a
	// protocol violation and would close the owner, so it is discarded here. The
	// bound keeps a misbehaving seam from spinning the UI thread.
	for (std::size_t attempt = 0; attempt <= senp::CSenpRuntimeSession::kMaximumPending; ++attempt) {
		try {
			auto completion = m_toolReads->Take(m_owner);
			if (!completion) return {};
			const auto found = m_toolReadContexts.find(completion->readId);
			if (found == m_toolReadContexts.end()) continue;
			auto context = found->second;
			m_toolReadContexts.erase(found);
			return SenpToolReadTerminal(std::move(context), std::move(*completion));
		} catch (const std::exception&) { return {}; }
	}
	return {};
}

void CSenpReadonlyOwnerTarget::CancelToolReads(const senp::effect::OperationContext& context) noexcept
{
	// The lineage, not the operation id: one request generation can hold several
	// reads, and the projection cancels the same lineage on its own side.
	for (auto current = m_toolReadContexts.begin(); current != m_toolReadContexts.end();) {
		if (current->second.ownerGeneration == context.ownerGeneration
			&& current->second.requestGeneration == context.requestGeneration)
			current = m_toolReadContexts.erase(current);
		else ++current;
	}
	if (!m_toolReads) return;
	m_toolReads->Cancel(m_owner, context);
}

SenpReadonlyOwnerStyleSink CSenpReadonlyOwnerTarget::StyleSink() const
{
	return [lifetime = std::weak_ptr(m_lifetime)](const theme::ThemePalette& palette,
		const LOGFONT& font, unsigned int dpi) noexcept {
		const auto alive = lifetime.lock();
		if (!alive || !*alive || (*alive)->m_revoked || dpi < 48 || dpi > 768) return false;
		(*alive)->SetStyle(palette, font, dpi);
		return *alive != nullptr;
	};
}

void CSenpReadonlyOwnerTarget::SetStyle(const theme::ThemePalette& palette,
	const LOGFONT& font, const unsigned int dpi) noexcept
{
	if (m_revoked || dpi < 48 || dpi > 768) return;
	m_palette = palette; m_font = font; m_dpi = dpi;
	try {
		ReapClosed();
		for (const auto& [resource, document] : m_documents) document->host.SetStyle(m_palette, m_font, m_dpi);
	} catch (const std::exception&) { Revoke(); }
}

void CSenpReadonlyOwnerTarget::PumpText() noexcept
try {
	if (m_revoked || !m_toolReads) return;
	ReapClosed();
	SettleText();
	// Settling frees the one read this owner may have outstanding, so the turn
	// that delivers a chunk is also the turn that asks for the next one.
	if (!m_textRead) BeginText();
} catch (const std::exception&) {
	Revoke();
}

void CSenpReadonlyOwnerTarget::SettleText() noexcept
{
	if (!m_textRead) return;
	auto answer = m_toolReads->TakeResource(m_owner);
	// Nothing has settled yet. The read stays outstanding and the next turn asks
	// again; the seam owes exactly one answer, so this cannot wait forever.
	if (!answer) return;
	const auto request = *m_textRead;
	const auto resource = m_textResource;
	m_textRead.reset();
	m_textResource.clear();
	const auto found = m_documents.find(resource);
	// The document that asked is gone. There is nothing left to tell.
	if (found == m_documents.end() || found->second->closed) return;
	auto& host = found->second->host;
	if (answer->Handle() != request.Handle()
		|| answer->Offset() != static_cast<std::uint64_t>(request.Offset())) {
		// An answer naming another read cannot settle this one, and no second
		// answer is coming, so the read this document is waiting on ends here.
		host.FailText(request, senp::TextResourceEnd::Failed);
		return;
	}
	if (!answer->Chunk()) {
		// No answer about the resource arrived at all - the connection was
		// unavailable, lost or replaced. This read failed; the resource is not
		// known to be gone, so whatever already reached the screen stays.
		host.FailText(request, senp::TextResourceEnd::Failed);
		return;
	}
	switch (answer->Chunk()->result) {
	case senp::TextResourceResult::Accepted:
		// The store's own chunk, whole. Whether it fits what the surface already
		// holds is the view's judgement, not this one's.
		(void)host.ApplyText(request, *answer->Chunk());
		return;
	case senp::TextResourceResult::Expired:
		// The store let the resource go. Its bytes are no longer a partial
		// answer that something might complete, so the body is erased.
		host.FailText(request, senp::TextResourceEnd::Revoked);
		return;
	case senp::TextResourceResult::Closed:
		host.FailText(request, senp::TextResourceEnd::Cancelled);
		return;
	default:
		host.FailText(request, senp::TextResourceEnd::Failed);
		return;
	}
}

void CSenpReadonlyOwnerTarget::BeginText() noexcept
{
	for (const auto& [resource, document] : m_documents) {
		if (document->closed) continue;
		auto read = document->host.TakeTextRead();
		if (!read) continue;
		// The host has committed to this read the moment it handed it over, so a
		// seam that will not carry it must be answered here. Left alone the
		// surface would wait on a read nothing is going to serve.
		if (read->Count() > (std::numeric_limits<std::uint32_t>::max)()
			|| !m_toolReads->ReadResource(m_owner, read->Handle(), read->Offset(),
				static_cast<std::uint32_t>(read->Count()))) {
			document->host.FailText(*read, senp::TextResourceEnd::Failed);
			return;
		}
		m_textRead = std::move(read);
		m_textResource = resource;
		return;
	}
}

SenpReadonlyOwnerTextPump CSenpReadonlyOwnerTarget::TextPump() const
{
	return [lifetime = std::weak_ptr(m_lifetime)]() noexcept {
		const auto alive = lifetime.lock();
		if (!alive || !*alive || (*alive)->m_revoked) return false;
		(*alive)->PumpText();
		return *alive != nullptr;
	};
}

std::wstring CSenpReadonlyOwnerTarget::OutstandingTextResource() const
{
	return m_textResource;
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
	*m_lifetime = nullptr;
	m_revoked = true; m_pending.clear();
	m_state = SenpReadonlyOwnerTargetState::Revoked;
	// Physical cancellation before the document teardown: once this returns no
	// completion of this owner may be routed anywhere.
	m_toolReadContexts.clear();
	if (const auto reads = std::exchange(m_toolReads, nullptr)) reads->CancelAll(m_owner);
	(void)m_editors.Revoke(m_scope);
	for (const auto& [resource, document] : m_documents) {
		if (!document->closed && m_editors.Remove(document->inputId)) {
			document->closed = true; document->host.Close();
		}
	}
	m_documents.clear();
	// CancelAll above discards this owner's outstanding read and any answer it
	// had not drained, so nothing is left for the pump to settle.
	m_textRead.reset(); m_textResource.clear();
	m_copy = {}; m_commandCompleted = {}; m_resourceReleased = {};
}

} // namespace workbench::editor
