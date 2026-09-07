/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "workbench/editor/SenpReadonlyWorkbench.h"
#include "workbench/editor/EditorCommandIds.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <map>
#include <utility>

namespace workbench::editor {
namespace {
std::uint64_t NextInstance()
{
	static std::atomic<std::uint64_t> next{ 1 };
	auto value = next.load(std::memory_order_relaxed);
	while (value != (std::numeric_limits<std::uint64_t>::max)()) {
		if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) return value;
	}
	return 0;
}
bool ValidText(std::wstring_view value, std::size_t maximum)
{
	if (value.empty() || value.size() > maximum) return false;
	for (std::size_t i = 0; i < value.size(); ++i) {
		const auto c = static_cast<unsigned int>(value[i]);
		if (c < 0x20 || c == 0x7f) return false;
		if (c >= 0xd800 && c <= 0xdbff) {
			if (++i == value.size() || value[i] < 0xdc00 || value[i] > 0xdfff) return false;
		} else if (c >= 0xdc00 && c <= 0xdfff) return false;
	}
	return true;
}
bool ValidScope(const SenpReadonlyScope& scope)
{
	return !scope.extensionId.empty() && scope.extensionId.size() <= 128
		&& std::ranges::all_of(scope.extensionId, [](char c) {
			return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
		}) && scope.ownerGeneration > 0 && scope.workspaceRevision >= 0 && scope.accountGeneration >= 0;
}
EditorDocumentIdentity Identity(const SenpReadonlyScope& scope, std::wstring_view resource)
{
	// An injective ASCII path encoding, not a native file path or a display title.
	// Generation fences keep documents from another account/cohort out of the cache.
	std::wstring uri = L"senp://" + std::wstring(scope.extensionId.begin(), scope.extensionId.end())
		+ L"/" + std::to_wstring(scope.ownerGeneration) + L"/" + std::to_wstring(scope.workspaceRevision)
		+ L"/" + std::to_wstring(scope.accountGeneration) + L"/";
	constexpr wchar_t hex[] = L"0123456789abcdef";
	for (const wchar_t c : resource) {
		for (int shift = 12; shift >= 0; shift -= 4) uri.push_back(hex[(c >> shift) & 15]);
	}
	const auto parsed = platform::uri::Uri::Parse(uri);
	return parsed ? EditorDocumentIdentity{ .resource = *parsed.value } : EditorDocumentIdentity{};
}
bool Success(const EditorOperationResult& result)
{
	return result.status == EEditorOperationStatus::Succeeded
		|| (result.status == EEditorOperationStatus::NotApplicable && result.reason == EEditorOperationReason::AlreadyActive);
}
}

struct SenpReadonlyWorkbench::Impl final {
	EditorCoreService& core;
	std::map<std::string, SenpReadonlyInput, std::less<>> inputs;
	std::uint64_t instance{ NextInstance() };
	std::uint64_t sequence{};
	bool closed{};
	bool busy{};
	explicit Impl(EditorCoreService& value) : core(value), closed(instance == 0) {}
	std::string Next(std::string_view kind) {
		if (sequence == (std::numeric_limits<std::uint64_t>::max)()) return {};
		return "senp.readonly." + std::to_string(instance) + "." + std::string(kind) + "." + std::to_string(++sequence);
	}
};
namespace {
struct BusyGuard final {
	bool& busy;
	explicit BusyGuard(bool& value) : busy(value) { busy = true; }
	~BusyGuard() { busy = false; }
};
}

SenpReadonlyWorkbench::SenpReadonlyWorkbench(EditorCoreService& core) : m_impl(std::make_unique<Impl>(core)) {}
SenpReadonlyWorkbench::~SenpReadonlyWorkbench()
{
	// The composition owner calls Shutdown while observers and native surfaces still
	// exist, and observes a failed terminal. Destruction is a final bounded best effort.
	try { (void)Shutdown(); } catch (...) { /* Core lifetime still owns any surviving input. */ }
}

SenpReadonlyOpenResult SenpReadonlyWorkbench::Open(SenpReadonlyScope scope,
	std::wstring resourceId, std::wstring title)
{
	auto& state = *m_impl;
	if (state.closed) return { SenpReadonlyStatus::Closed };
	if (state.busy) return { SenpReadonlyStatus::Conflict };
	if (!ValidScope(scope) || !ValidText(resourceId, 512) || !ValidText(title, 256)) return { SenpReadonlyStatus::Invalid };
	BusyGuard guard(state.busy);
	const auto snapshot = state.core.Snapshot();
	for (auto it = state.inputs.begin(); it != state.inputs.end();) {
		const bool exists = std::ranges::any_of(snapshot.group.inputs, [&](const auto& input) {
			return input.descriptor.inputId == it->first;
		});
		if (!exists) it = state.inputs.erase(it);
		else {
			if (it->second.scope == scope && it->second.resourceId == resourceId) {
				return { SenpReadonlyStatus::Reused, it->first };
			}
			++it;
		}
	}
	if (state.inputs.size() >= kMaximumInputs) return { SenpReadonlyStatus::LimitReached };
	const auto identity = Identity(scope, resourceId);
	if (!identity.IsValid()) return { SenpReadonlyStatus::Invalid };
	std::string id;
	do {
		id = state.Next("input");
		if (id.empty()) return { SenpReadonlyStatus::LimitReached };
	} while (std::ranges::any_of(snapshot.group.inputs, [&](const auto& input) { return input.descriptor.inputId == id; }));
	const auto operationId = state.Next("open");
	if (operationId.empty()) return { SenpReadonlyStatus::LimitReached };
	// Prepare our ownership before publishing the core input. Callbacks may inspect it
	// but cannot reenter a mutation while the core emits its post-commit notification.
	state.inputs.emplace(id, SenpReadonlyInput{ id, std::move(scope), std::move(resourceId), std::move(title) });
	const OpenResolvedInputRequest request{
		.operation = { operationId, snapshot.revision },
		.input = { id, identity },
		.resolvedDocument = ResolvedEditorDocument{ identity, 0, false },
		.activate = false,
	};
	try {
		const auto result = state.core.OpenResolvedInput(request);
		if (!Success(result)) {
			state.inputs.erase(id);
			return { SenpReadonlyStatus::Conflict };
		}
	} catch (...) {
		// A core notification/allocation may fail after commit. Retain ownership when
		// that exact input exists; the caller can bind it or explicitly close it.
		const auto after = state.core.Snapshot();
		if (!std::ranges::any_of(after.group.inputs, [&](const auto& input) { return input.descriptor.inputId == id; })) {
			state.inputs.erase(id);
			return { SenpReadonlyStatus::Failed };
		}
	}
	return { SenpReadonlyStatus::Succeeded, std::move(id) };
}

SenpReadonlyStatus SenpReadonlyWorkbench::Show(std::string_view inputId)
{
	auto& state = *m_impl;
	if (state.closed) return SenpReadonlyStatus::Closed;
	if (state.busy) return SenpReadonlyStatus::Conflict;
	BusyGuard guard(state.busy);
	const auto snapshot = state.core.Snapshot();
	const auto operationId = state.Next("show");
	if (operationId.empty()) return SenpReadonlyStatus::LimitReached;
	const auto result = state.core.ShowInput({ { operationId, snapshot.revision }, std::string(inputId) });
	return Success(result) ? SenpReadonlyStatus::Succeeded
		: result.reason == EEditorOperationReason::InputNotFound ? SenpReadonlyStatus::NotFound : SenpReadonlyStatus::Conflict;
}

SenpReadonlyStatus SenpReadonlyWorkbench::Close(std::string_view inputId)
{
	auto& state = *m_impl;
	if (state.busy) return SenpReadonlyStatus::Conflict;
	const auto found = state.inputs.find(inputId);
	if (found == state.inputs.end()) return SenpReadonlyStatus::NotFound;
	BusyGuard guard(state.busy);
	const auto snapshot = state.core.Snapshot();
	const auto operationId = state.Next("close");
	if (operationId.empty()) return SenpReadonlyStatus::LimitReached;
	const auto result = state.core.CloseInput({ { operationId, snapshot.revision }, std::string(inputId) });
	if (!Success(result) && result.reason != EEditorOperationReason::InputNotFound) return SenpReadonlyStatus::Conflict;
	state.inputs.erase(found);
	return SenpReadonlyStatus::Succeeded;
}

SenpReadonlyStatus SenpReadonlyWorkbench::Revoke(const SenpReadonlyScope& scope)
{
	if (m_impl->busy) return SenpReadonlyStatus::Conflict;
	std::vector<std::string> retiring;
	for (const auto& [id, input] : m_impl->inputs) if (input.scope == scope) retiring.push_back(id);
	auto status = SenpReadonlyStatus::Succeeded;
	for (const auto& id : retiring) {
		const auto result = Close(id);
		if (result != SenpReadonlyStatus::Succeeded && result != SenpReadonlyStatus::NotFound) status = result;
	}
	return status;
}
SenpReadonlyStatus SenpReadonlyWorkbench::Shutdown()
{
	if (m_impl->busy) return SenpReadonlyStatus::Conflict;
	m_impl->closed = true;
	std::vector<std::string> retiring;
	for (const auto& [id, input] : m_impl->inputs) retiring.push_back(id);
	auto status = SenpReadonlyStatus::Succeeded;
	for (const auto& id : retiring) {
		const auto result = Close(id);
		if (result != SenpReadonlyStatus::Succeeded && result != SenpReadonlyStatus::NotFound) status = result;
	}
	return status;
}
const SenpReadonlyInput* SenpReadonlyWorkbench::Find(std::string_view inputId) const noexcept
{
	const auto found = m_impl->inputs.find(inputId);
	return found == m_impl->inputs.end() ? nullptr : &found->second;
}
std::vector<SenpReadonlyInput> SenpReadonlyWorkbench::Inputs() const
{
	std::vector<SenpReadonlyInput> result;
	for (const auto& [id, input] : m_impl->inputs) result.push_back(input);
	return result;
}
EditorCoreSnapshot SenpReadonlyWorkbench::Snapshot() const { return m_impl->core.Snapshot(); }
SenpEditorCommandRoute SenpReadonlyWorkbench::Route(std::string_view commandId) const
{
	// Save All addresses working copies, not the active readonly document. Global
	// navigation/lifecycle commands likewise stay at the workbench composition port.
	if (commandId == command_ids::SaveAll || commandId == command_ids::NewUntitledFile
		|| commandId == command_ids::OpenFile || commandId == command_ids::OpenFolder
		|| commandId == command_ids::OpenWorkspace || commandId == command_ids::CloseWindow
		|| commandId == command_ids::Quit || commandId == command_ids::ShowCommands)
		return SenpEditorCommandRoute::Workbench;
	const auto snapshot = Snapshot();
	if (!snapshot.group.activeInputId) return SenpEditorCommandRoute::NoActiveInput;
	if (!Find(*snapshot.group.activeInputId)) return SenpEditorCommandRoute::Legacy;
	if (commandId == command_ids::CloseActiveEditor) return SenpEditorCommandRoute::CloseReadonly;
	if (commandId == "editor.action.clipboardCopyAction" || commandId == "editor.action.selectAll"
		|| commandId == "actions.find" || commandId == "editor.action.nextMatchFindAction"
		|| commandId == "editor.action.previousMatchFindAction") return SenpEditorCommandRoute::ReadonlySurface;
	return SenpEditorCommandRoute::NotApplicable;
}

} // namespace workbench::editor
