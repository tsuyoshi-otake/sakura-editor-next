/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "StdAfx.h"
#include "workbench/editor/persistence/EditorCoreRecoveredInputAdopter.h"

#include "util/string_ex.h"
#include "workbench/editor/persistence/WorkingCopyPersistenceCodec.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

namespace workbench::editor::persistence {
namespace {

constexpr std::string_view kOperationPrefix = "recovered-input-adoption-";
std::atomic<std::uint64_t> g_nextOperation{ 1 };

bool IdentityMatches(const WorkingCopyPersistenceIdentity& persisted,
	const EditorDocumentIdentity& core) noexcept
{
	try {
		if (persisted.opaqueId) return core.opaqueId && *core.opaqueId == *persisted.opaqueId;
		if (!persisted.canonicalResource || !core.resource) return false;
		const auto parsed = platform::uri::Uri::Parse(u8stowcs(*persisted.canonicalResource));
		return parsed && platform::uri::UriIdentityService::MakeComparisonKey(*parsed.value)
			== platform::uri::UriIdentityService::MakeComparisonKey(*core.resource);
	} catch (...) {
		return false;
	}
}

} // namespace

EditorCoreRecoveredInputAdopter::EditorCoreRecoveredInputAdopter(EditorCoreService& core) noexcept
	: m_core(core)
{
}

bool EditorCoreRecoveredInputAdopter::AdoptInactive(const EditorSessionInputDescriptor& input,
	const EditorDocumentIdentity& identity, std::uint64_t contentVersion)
{
	if (!input.IsValid() || input.inputTypeId != CWorkingCopyPersistenceCodec::kTextInputTypeId
		|| !identity.IsValid() || !IdentityMatches(input.workingCopyIdentity, identity)
		|| contentVersion == 0 || contentVersion > kMaximumWorkingCopyPersistenceGeneration) return false;

	std::string operationId;
	if (!TryNextOperationId(operationId)) return false;

	try {
		const auto result = m_core.OpenResolvedInput({
			.operation = { .operationId = std::move(operationId) },
			.input = { .inputId = input.inputId, .documentIdentity = identity },
			.resolvedDocument = ResolvedEditorDocument{
				.identity = identity,
				.documentRevision = contentVersion,
				.dirty = true,
			},
			.activate = false,
		});
		return result.status == EEditorOperationStatus::Succeeded;
	}
	catch (...) {
		return false;
	}
}

bool EditorCoreRecoveredInputAdopter::RollbackInactive(const EditorSessionInputDescriptor& input,
	const EditorDocumentIdentity& identity, std::uint64_t contentVersion) noexcept
{
	if (!input.IsValid() || input.inputTypeId != CWorkingCopyPersistenceCodec::kTextInputTypeId
		|| !identity.IsValid() || !IdentityMatches(input.workingCopyIdentity, identity)
		|| contentVersion == 0 || contentVersion > kMaximumWorkingCopyPersistenceGeneration) return false;

	try {
		const auto snapshot = m_core.Snapshot();
		if (snapshot.group.activeInputId && *snapshot.group.activeInputId == input.inputId) return false;
		const auto foundInput = std::ranges::find_if(snapshot.group.inputs, [&input](const auto& candidate) {
			return candidate.descriptor.inputId == input.inputId;
		});
		if (foundInput == snapshot.group.inputs.end()) return false;
		const auto foundDocument = std::ranges::find_if(snapshot.documents, [&foundInput](const auto& candidate) {
			return candidate.documentKey == foundInput->documentKey;
		});
		if (foundDocument == snapshot.documents.end() || !foundDocument->dirty
			|| foundDocument->documentRevision != contentVersion
			|| !IdentityMatches(input.workingCopyIdentity, foundDocument->identity)) return false;

		std::string operationId;
		if (!TryNextOperationId(operationId)) return false;
		const auto result = m_core.CloseInput({
			.operation = { .operationId = std::move(operationId), .expectedModelRevision = snapshot.revision },
			.inputId = input.inputId,
		});
		return result.status == EEditorOperationStatus::Succeeded;
	} catch (...) {
		return false;
	}
}

bool EditorCoreRecoveredInputAdopter::TryNextOperationId(std::string& operationId) noexcept
{
	auto sequence = g_nextOperation.load(std::memory_order_relaxed);
	for (;;) {
		if (sequence == 0 || sequence == std::numeric_limits<std::uint64_t>::max()) return false;
		if (g_nextOperation.compare_exchange_weak(sequence, sequence + 1,
			std::memory_order_relaxed, std::memory_order_relaxed)) break;
	}

	std::array<char, std::numeric_limits<std::uint64_t>::digits10 + 1> digits{};
	const auto converted = std::to_chars(digits.data(), digits.data() + digits.size(), sequence);
	if (converted.ec != std::errc{}) return false;
	try {
		operationId.assign(kOperationPrefix);
		operationId.append(digits.data(), converted.ptr);
	}
	catch (...) {
		return false;
	}
	return IsValidEditorExternalId(operationId, kMaxEditorOperationIdLength);
}

} // namespace workbench::editor::persistence
