/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/editor/EditorGroupModel.h"

#include <algorithm>
#include <type_traits>
#include <utility>

namespace workbench::editor {

bool EditorGroupModel::Contains(std::string_view inputId) const noexcept
{
	return std::any_of(m_inputs.begin(), m_inputs.end(), [inputId](const Input& input) {
		return input.descriptor.inputId == inputId;
	});
}

bool EditorGroupModel::Open(EditorInputDescriptor input, std::wstring documentKey, bool activate)
{
	if (!input.IsValid() || documentKey.empty() || Contains(input.inputId)) return false;

	// Complete every potentially allocating step before either model collection changes.
	// Once capacity is reserved, the moves below are non-throwing for these value types,
	// so callers can safely undo a preceding document-reference acquisition on failure.
	Input pending{ .descriptor = std::move(input), .documentKey = std::move(documentKey) };
	std::optional<std::string> pendingActiveInputId;
	if (activate) pendingActiveInputId = pending.descriptor.inputId;
	m_inputs.reserve(m_inputs.size() + 1);
	m_inputs.push_back(std::move(pending));
	if (pendingActiveInputId) m_activeInputId = std::move(pendingActiveInputId);
	return true;
}

bool EditorGroupModel::Show(std::string_view inputId)
{
	if (!Contains(inputId) || (m_activeInputId && *m_activeInputId == inputId)) return false;
	std::string pendingActiveInputId(inputId);
	m_activeInputId.emplace(std::move(pendingActiveInputId));
	return true;
}

bool EditorGroupModel::ReplaceDocument(std::string_view inputId, EditorDocumentIdentity documentIdentity, std::wstring documentKey)
{
	const auto found = std::find_if(m_inputs.begin(), m_inputs.end(), [inputId](const Input& input) {
		return input.descriptor.inputId == inputId;
	});
	if (found == m_inputs.end() || !documentIdentity.IsValid() || documentKey.empty()) return false;

	// Copy and construct the complete replacement before mutating the visible group. Input's
	// components are all no-throw movable, so swapping after the preparation is atomic here.
	Input replacement = *found;
	replacement.descriptor.documentIdentity = std::move(documentIdentity);
	replacement.documentKey = std::move(documentKey);
	static_assert(std::is_nothrow_move_constructible_v<Input> && std::is_nothrow_move_assignable_v<Input>);
	using std::swap;
	swap(*found, replacement);
	return true;
}

std::optional<EditorInputSnapshot> EditorGroupModel::Close(std::string_view inputId)
{
	const auto found = std::find_if(m_inputs.begin(), m_inputs.end(), [inputId](const Input& input) {
		return input.descriptor.inputId == inputId;
	});
	if (found == m_inputs.end()) return std::nullopt;

	const auto index = static_cast<std::size_t>(std::distance(m_inputs.begin(), found));
	const bool wasActive = m_activeInputId && *m_activeInputId == inputId;
	EditorInputSnapshot removed{ .descriptor = found->descriptor, .documentKey = found->documentKey };
	std::optional<std::string> nextActiveInputId = m_activeInputId;
	if (wasActive) {
		if (m_inputs.size() == 1) {
			nextActiveInputId.reset();
		} else {
			const auto neighborBeforeErase = index + 1 < m_inputs.size() ? index + 1 : index - 1;
			nextActiveInputId = m_inputs[neighborBeforeErase].descriptor.inputId;
		}
	}
	m_inputs.erase(found);

	if (wasActive) {
		m_activeInputId = std::move(nextActiveInputId);
	}
	return removed;
}

std::optional<std::wstring> EditorGroupModel::DocumentKeyFor(std::string_view inputId) const
{
	const auto found = std::find_if(m_inputs.begin(), m_inputs.end(), [inputId](const Input& input) {
		return input.descriptor.inputId == inputId;
	});
	return found == m_inputs.end() ? std::nullopt : std::optional<std::wstring>{ found->documentKey };
}

EditorGroupSnapshot EditorGroupModel::Snapshot() const
{
	EditorGroupSnapshot snapshot;
	snapshot.activeInputId = m_activeInputId;
	snapshot.inputs.reserve(m_inputs.size());
	for (const auto& input : m_inputs) {
		snapshot.inputs.push_back({ .descriptor = input.descriptor, .documentKey = input.documentKey });
	}
	return snapshot;
}

} // namespace workbench::editor
