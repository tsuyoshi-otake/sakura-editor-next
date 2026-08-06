/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "extension/CExtensionWorkbenchUi.h"

#include <algorithm>
#include <cmath>
#include <tuple>

std::size_t CExtensionDiagnostics::KeyHash::operator()(const Key& key) const noexcept
{
	auto value = std::hash<std::wstring>{}(key.extensionId);
	const auto combine = [&](std::size_t next) {
		value ^= next + static_cast<std::size_t>(0x9e3779b9U) + (value << 6) + (value >> 2);
	};
	combine(std::hash<std::uint64_t>{}(key.generation));
	combine(std::hash<std::wstring>{}(key.collection));
	combine(std::hash<std::wstring>{}(key.uri));
	return value;
}
bool CExtensionDiagnostics::Set(
	std::wstring extensionId,
	std::uint64_t generation,
	std::wstring collection,
	std::wstring uri,
	std::vector<SExtensionDiagnostic> diagnostics)
{
	if (extensionId.empty() || generation == 0 || collection.empty() || uri.empty()) return false;
	for (const auto& diagnostic : diagnostics) {
		if (diagnostic.message.empty() || diagnostic.range.end < diagnostic.range.start) return false;
	}
	std::lock_guard lock(m_mutex);
	m_entries.insert_or_assign(
		Key{ std::move(extensionId), generation, std::move(collection), std::move(uri) },
		std::move(diagnostics));
	return true;
}

bool CExtensionDiagnostics::Delete(
	std::wstring_view extensionId,
	std::uint64_t generation,
	std::wstring_view collection,
	std::wstring_view uri)
{
	std::lock_guard lock(m_mutex);
	return m_entries.erase(Key{ std::wstring(extensionId), generation, std::wstring(collection), std::wstring(uri) }) != 0;
}

void CExtensionDiagnostics::ClearCollection(
	std::wstring_view extensionId,
	std::uint64_t generation,
	std::wstring_view collection)
{
	std::lock_guard lock(m_mutex);
	std::erase_if(m_entries, [&](const auto& entry) {
		return entry.first.extensionId == extensionId && entry.first.generation == generation && entry.first.collection == collection;
	});
}

void CExtensionDiagnostics::RemoveOwnedBy(std::wstring_view extensionId, std::uint64_t generation)
{
	std::lock_guard lock(m_mutex);
	std::erase_if(m_entries, [&](const auto& entry) {
		return entry.first.extensionId == extensionId && entry.first.generation == generation;
	});
}

void CExtensionDiagnostics::Clear()
{
	std::lock_guard lock(m_mutex);
	m_entries.clear();
}

std::vector<SExtensionDiagnostic> CExtensionDiagnostics::ForUri(std::wstring_view uri) const
{
	std::lock_guard lock(m_mutex);
	std::vector<SExtensionDiagnostic> result;
	for (const auto& [key, diagnostics] : m_entries) {
		if (key.uri == uri) result.insert(result.end(), diagnostics.begin(), diagnostics.end());
	}
	std::ranges::sort(result, [](const auto& left, const auto& right) {
		return std::tie(left.range.start, left.severity, left.message) < std::tie(right.range.start, right.severity, right.message);
	});
	return result;
}

std::vector<SExtensionProblem> CExtensionDiagnostics::Problems() const
{
	std::lock_guard lock(m_mutex);
	std::vector<SExtensionProblem> result;
	for (const auto& [key, diagnostics] : m_entries) {
		for (const auto& diagnostic : diagnostics) {
			result.push_back({ key.uri, key.extensionId, key.collection, diagnostic });
		}
	}
	std::ranges::sort(result, [](const auto& left, const auto& right) {
		return std::tie(left.uri, left.diagnostic.range.start, left.diagnostic.severity, left.diagnostic.message) <
			std::tie(right.uri, right.diagnostic.range.start, right.diagnostic.severity, right.diagnostic.message);
	});
	return result;
}

std::vector<SExtensionProblem> CExtensionProblemsPane::Snapshot(std::wstring_view uriFilter) const
{
	auto result = m_diagnostics.Problems();
	if (!uriFilter.empty()) std::erase_if(result, [&](const auto& problem) { return problem.uri != uriFilter; });
	return result;
}

std::optional<std::uint64_t> CExtensionQuickInput::Show(SExtensionQuickInputRequest request)
{
	if (request.extensionId.empty() || request.generation == 0) return std::nullopt;
	std::lock_guard lock(m_mutex);
	if (m_pending.size() >= m_maximumPending) return std::nullopt;
	request.id = m_nextId++;
	const auto id = request.id;
	m_pending.emplace(id, std::move(request));
	return id;
}

std::vector<SExtensionQuickInputRequest> CExtensionQuickInput::Pending() const
{
	std::lock_guard lock(m_mutex);
	std::vector<SExtensionQuickInputRequest> result;
	result.reserve(m_pending.size());
	for (const auto& [id, request] : m_pending) result.push_back(request);
	std::ranges::sort(result, {}, &SExtensionQuickInputRequest::id);
	return result;
}

bool CExtensionQuickInput::Resolve(
	std::uint64_t id,
	std::vector<std::size_t> selectedIndices,
	std::optional<std::wstring> value)
{
	std::lock_guard lock(m_mutex);
	const auto found = m_pending.find(id);
	if (found == m_pending.end()) return false;
	if (found->second.kind == EExtensionQuickInputKind::QuickPick) {
		for (const auto index : selectedIndices) {
			if (index >= found->second.items.size()) return false;
		}
		if (!found->second.canPickMany && selectedIndices.size() > 1) return false;
	} else if (!value) {
		return false;
	}
	m_completions.emplace(id, SExtensionQuickInputCompletion{
		.id = id, .state = EExtensionQuickInputState::Accepted,
		.selectedIndices = std::move(selectedIndices), .value = std::move(value),
	});
	m_pending.erase(found);
	return true;
}

bool CExtensionQuickInput::Cancel(std::uint64_t id, EExtensionQuickInputState state)
{
	if (state == EExtensionQuickInputState::Pending || state == EExtensionQuickInputState::Accepted) return false;
	std::lock_guard lock(m_mutex);
	const auto found = m_pending.find(id);
	if (found == m_pending.end()) return false;
	m_completions.emplace(id, SExtensionQuickInputCompletion{ .id = id, .state = state });
	m_pending.erase(found);
	return true;
}

std::optional<SExtensionQuickInputCompletion> CExtensionQuickInput::TakeCompletion(std::uint64_t id)
{
	std::lock_guard lock(m_mutex);
	const auto found = m_completions.find(id);
	if (found == m_completions.end()) return std::nullopt;
	auto completion = std::move(found->second);
	m_completions.erase(found);
	return completion;
}

void CExtensionQuickInput::RemoveOwnedBy(
	std::wstring_view extensionId,
	std::uint64_t generation,
	EExtensionQuickInputState state)
{
	std::lock_guard lock(m_mutex);
	for (auto iterator = m_pending.begin(); iterator != m_pending.end();) {
		if (iterator->second.extensionId == extensionId && iterator->second.generation == generation) {
			m_completions.emplace(iterator->first, SExtensionQuickInputCompletion{ .id = iterator->first, .state = state });
			iterator = m_pending.erase(iterator);
		} else {
			++iterator;
		}
	}
}

void CExtensionQuickInput::Clear()
{
	std::lock_guard lock(m_mutex);
	for (const auto& [id, request] : m_pending) {
		m_completions.emplace(id, SExtensionQuickInputCompletion{ .id = id, .state = EExtensionQuickInputState::HostLost });
	}
	m_pending.clear();
}

SExtensionOutputChannel* CExtensionOutputChannel::FindOwned(
	std::wstring_view handle,
	std::wstring_view extensionId,
	std::uint64_t generation)
{
	const auto found = m_channels.find(std::wstring(handle));
	if (found == m_channels.end() || found->second.extensionId != extensionId || found->second.generation != generation) return nullptr;
	return &found->second;
}

void CExtensionOutputChannel::Truncate(SExtensionOutputChannel& channel)
{
	if (channel.text.size() <= m_maximumCharactersPerChannel) return;
	const auto count = channel.text.size() - m_maximumCharactersPerChannel;
	channel.text.erase(0, count);
	channel.droppedCharacters += count;
}

bool CExtensionOutputChannel::Create(SExtensionOutputChannel channel)
{
	if (channel.handle.empty() || channel.extensionId.empty() || channel.generation == 0 || channel.name.empty()) return false;
	std::lock_guard lock(m_mutex);
	const auto found = m_channels.find(channel.handle);
	if (found != m_channels.end()) return found->second.extensionId == channel.extensionId && found->second.generation == channel.generation;
	Truncate(channel);
	m_channels.emplace(channel.handle, std::move(channel));
	return true;
}

bool CExtensionOutputChannel::Append(
	std::wstring_view handle,
	std::wstring_view extensionId,
	std::uint64_t generation,
	std::wstring_view value)
{
	std::lock_guard lock(m_mutex);
	auto* channel = FindOwned(handle, extensionId, generation);
	if (!channel) return false;
	channel->text.append(value);
	Truncate(*channel);
	return true;
}

bool CExtensionOutputChannel::Replace(
	std::wstring_view handle,
	std::wstring_view extensionId,
	std::uint64_t generation,
	std::wstring value)
{
	std::lock_guard lock(m_mutex);
	auto* channel = FindOwned(handle, extensionId, generation);
	if (!channel) return false;
	channel->text = std::move(value);
	channel->droppedCharacters = 0;
	Truncate(*channel);
	return true;
}

bool CExtensionOutputChannel::Clear(
	std::wstring_view handle,
	std::wstring_view extensionId,
	std::uint64_t generation)
{
	return Replace(handle, extensionId, generation, {});
}

bool CExtensionOutputChannel::SetVisible(
	std::wstring_view handle,
	std::wstring_view extensionId,
	std::uint64_t generation,
	bool visible)
{
	std::lock_guard lock(m_mutex);
	auto* channel = FindOwned(handle, extensionId, generation);
	if (!channel) return false;
	channel->visible = visible;
	return true;
}

bool CExtensionOutputChannel::Dispose(
	std::wstring_view handle,
	std::wstring_view extensionId,
	std::uint64_t generation)
{
	std::lock_guard lock(m_mutex);
	if (!FindOwned(handle, extensionId, generation)) return false;
	m_channels.erase(std::wstring(handle));
	return true;
}

void CExtensionOutputChannel::RemoveOwnedBy(std::wstring_view extensionId, std::uint64_t generation)
{
	std::lock_guard lock(m_mutex);
	std::erase_if(m_channels, [&](const auto& entry) {
		return entry.second.extensionId == extensionId && entry.second.generation == generation;
	});
}

void CExtensionOutputChannel::ClearAll()
{
	std::lock_guard lock(m_mutex);
	m_channels.clear();
}

std::vector<SExtensionOutputChannel> CExtensionOutputChannel::Snapshot() const
{
	std::lock_guard lock(m_mutex);
	std::vector<SExtensionOutputChannel> result;
	result.reserve(m_channels.size());
	for (const auto& [handle, channel] : m_channels) result.push_back(channel);
	std::ranges::sort(result, [](const auto& left, const auto& right) {
		return std::tie(left.name, left.handle) < std::tie(right.name, right.handle);
	});
	return result;
}

bool CExtensionProgressCenter::Start(SExtensionProgress progress)
{
	if (progress.handle.empty() || progress.extensionId.empty() || progress.generation == 0) return false;
	std::lock_guard lock(m_mutex);
	return m_items.emplace(progress.handle, std::move(progress)).second;
}

bool CExtensionProgressCenter::Report(
	std::wstring_view handle,
	std::wstring_view extensionId,
	std::uint64_t generation,
	std::wstring message,
	double increment)
{
	std::lock_guard lock(m_mutex);
	const auto found = m_items.find(std::wstring(handle));
	if (found == m_items.end() || found->second.extensionId != extensionId || found->second.generation != generation) return false;
	if (!message.empty()) found->second.message = std::move(message);
	if (std::isfinite(increment)) found->second.increment = (std::clamp)(found->second.increment + increment, 0.0, 100.0);
	return true;
}

bool CExtensionProgressCenter::End(
	std::wstring_view handle,
	std::wstring_view extensionId,
	std::uint64_t generation)
{
	std::lock_guard lock(m_mutex);
	const auto found = m_items.find(std::wstring(handle));
	if (found == m_items.end() || found->second.extensionId != extensionId || found->second.generation != generation) return false;
	m_items.erase(found);
	return true;
}

bool CExtensionProgressCenter::RequestCancel(std::wstring_view handle)
{
	std::lock_guard lock(m_mutex);
	const auto found = m_items.find(std::wstring(handle));
	if (found == m_items.end() || !found->second.cancellable) return false;
	found->second.cancelRequested = true;
	return true;
}

void CExtensionProgressCenter::RemoveOwnedBy(std::wstring_view extensionId, std::uint64_t generation)
{
	std::lock_guard lock(m_mutex);
	std::erase_if(m_items, [&](const auto& entry) {
		return entry.second.extensionId == extensionId && entry.second.generation == generation;
	});
}

void CExtensionProgressCenter::Clear()
{
	std::lock_guard lock(m_mutex);
	m_items.clear();
}

std::vector<SExtensionProgress> CExtensionProgressCenter::Snapshot() const
{
	std::lock_guard lock(m_mutex);
	std::vector<SExtensionProgress> result;
	result.reserve(m_items.size());
	for (const auto& [handle, progress] : m_items) result.push_back(progress);
	std::ranges::sort(result, {}, &SExtensionProgress::handle);
	return result;
}

void CExtensionHoverCenter::Publish(SExtensionHoverResult result)
{
	std::lock_guard lock(m_mutex);
	m_result = std::move(result);
}

void CExtensionHoverCenter::Clear()
{
	std::lock_guard lock(m_mutex);
	m_result.reset();
}

std::optional<SExtensionHoverResult> CExtensionHoverCenter::Snapshot() const
{
	std::lock_guard lock(m_mutex);
	return m_result;
}
