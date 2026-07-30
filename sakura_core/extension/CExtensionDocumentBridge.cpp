/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "extension/CExtensionDocumentBridge.h"

#include "util/string_ex.h"

#include <algorithm>
#include <limits>
#include <unordered_set>

namespace {

bool IsUriComponentByte(unsigned char value) noexcept
{
	return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
		(value >= '0' && value <= '9') || value == '-' || value == '_' || value == '.' || value == '!' ||
		value == '~' || value == '*' || value == '\'' || value == '(' || value == ')';
}

std::wstring EncodeUriPath(std::wstring_view value)
{
	constexpr char digits[] = "0123456789ABCDEF";
	const auto utf8 = wcstou8s(value);
	std::string encoded;
	encoded.reserve(utf8.size());
	for (const auto raw : utf8) {
		const auto byte = static_cast<unsigned char>(raw);
		if (byte == '/' || IsUriComponentByte(byte)) {
			encoded.push_back(static_cast<char>(byte));
		} else {
			encoded.push_back('%');
			encoded.push_back(digits[byte >> 4]);
			encoded.push_back(digits[byte & 0x0f]);
		}
	}
	return u8stowcs(encoded);
}

int HexValue(wchar_t value) noexcept
{
	if (value >= L'0' && value <= L'9') return value - L'0';
	if (value >= L'a' && value <= L'f') return value - L'a' + 10;
	if (value >= L'A' && value <= L'F') return value - L'A' + 10;
	return -1;
}

std::optional<std::wstring> DecodeUriPath(std::wstring_view value)
{
	std::string bytes;
	for (std::size_t index = 0; index < value.size();) {
		if (value[index] == L'%') {
			if (index + 2 >= value.size()) return std::nullopt;
			const int high = HexValue(value[index + 1]);
			const int low = HexValue(value[index + 2]);
			if (high < 0 || low < 0) return std::nullopt;
			bytes.push_back(static_cast<char>((high << 4) | low));
			index += 3;
			continue;
		}
		const auto utf8 = wcstou8s(value.substr(index, 1));
		bytes.append(utf8);
		++index;
	}
	return u8stowcs(bytes);
}

} // namespace

std::wstring ExtensionFileUriFromPath(const std::filesystem::path& path)
{
	if (path.empty()) return {};
	auto normalized = std::filesystem::absolute(path).lexically_normal().wstring();
	std::ranges::replace(normalized, L'\\', L'/');
	if (normalized.starts_with(L"//")) {
		const auto separator = normalized.find(L'/', 2);
		if (separator == std::wstring::npos) return L"file://" + EncodeUriPath(normalized.substr(2));
		return L"file://" + EncodeUriPath(normalized.substr(2, separator - 2)) +
			EncodeUriPath(normalized.substr(separator));
	}
	if (!normalized.starts_with(L'/')) normalized.insert(normalized.begin(), L'/');
	return L"file://" + EncodeUriPath(normalized);
}

std::optional<std::filesystem::path> ExtensionFilePathFromUri(std::wstring_view uri)
{
	constexpr std::wstring_view prefix = L"file://";
	if (uri.size() < prefix.size() || _wcsnicmp(uri.data(), prefix.data(), prefix.size()) != 0) return std::nullopt;
	auto remainder = uri.substr(prefix.size());
	std::wstring_view authority;
	std::wstring_view encodedPath;
	if (remainder.starts_with(L'/')) {
		encodedPath = remainder;
	} else {
		const auto separator = remainder.find(L'/');
		authority = remainder.substr(0, separator);
		encodedPath = separator == std::wstring_view::npos ? std::wstring_view{} : remainder.substr(separator);
	}
	const auto decoded = DecodeUriPath(encodedPath);
	if (!decoded) return std::nullopt;
	std::wstring native = *decoded;
	std::ranges::replace(native, L'/', L'\\');
	if (!authority.empty()) {
		native = L"\\\\" + std::wstring(authority) + native;
	} else if (native.size() >= 3 && native[0] == L'\\' && native[2] == L':') {
		native.erase(native.begin());
	}
	if (native.empty()) return std::nullopt;
	return std::filesystem::path(std::move(native)).lexically_normal();
}

std::string SExtensionDocumentId::ToString() const
{
	return std::to_string(editorProcessId) + ":" + std::to_string(localDocumentId);
}

bool CExtensionDocumentSync::IsValidSnapshot(const SExtensionDocumentSnapshot& snapshot)
{
	return snapshot.id.IsValid() && !snapshot.uri.empty() && snapshot.version != 0;
}

EExtensionDocumentUpdateResult CExtensionDocumentSync::Open(SExtensionDocumentSnapshot snapshot)
{
	if (!IsValidSnapshot(snapshot)) return EExtensionDocumentUpdateResult::InvalidSnapshot;
	const auto found = m_documents.find(snapshot.id);
	if (found != m_documents.end() && snapshot.version <= found->second.version) {
		return EExtensionDocumentUpdateResult::StaleVersion;
	}
	m_documents.insert_or_assign(snapshot.id, std::move(snapshot));
	return EExtensionDocumentUpdateResult::Applied;
}

EExtensionDocumentUpdateResult CExtensionDocumentSync::Change(SExtensionDocumentSnapshot snapshot)
{
	if (!IsValidSnapshot(snapshot)) return EExtensionDocumentUpdateResult::InvalidSnapshot;
	const auto found = m_documents.find(snapshot.id);
	if (found == m_documents.end()) return EExtensionDocumentUpdateResult::UnknownDocument;
	if (snapshot.version <= found->second.version) return EExtensionDocumentUpdateResult::StaleVersion;
	if (snapshot.version != found->second.version + 1) return EExtensionDocumentUpdateResult::VersionGap;
	found->second = std::move(snapshot);
	return EExtensionDocumentUpdateResult::Applied;
}

EExtensionDocumentUpdateResult CExtensionDocumentSync::Save(SExtensionDocumentSnapshot snapshot)
{
	if (!IsValidSnapshot(snapshot)) return EExtensionDocumentUpdateResult::InvalidSnapshot;
	const auto found = m_documents.find(snapshot.id);
	if (found == m_documents.end()) return EExtensionDocumentUpdateResult::UnknownDocument;
	if (snapshot.version < found->second.version) return EExtensionDocumentUpdateResult::StaleVersion;
	if (snapshot.version > found->second.version + 1) return EExtensionDocumentUpdateResult::VersionGap;
	snapshot.dirty = false;
	found->second = std::move(snapshot);
	return EExtensionDocumentUpdateResult::Applied;
}

bool CExtensionDocumentSync::Close(const SExtensionDocumentId& id)
{
	return m_documents.erase(id) != 0;
}

std::optional<SExtensionDocumentSnapshot> CExtensionDocumentSync::Snapshot(const SExtensionDocumentId& id) const
{
	const auto found = m_documents.find(id);
	return found == m_documents.end() ? std::nullopt : std::optional(found->second);
}

std::vector<SExtensionDocumentSnapshot> CExtensionDocumentSync::Snapshots() const
{
	std::vector<SExtensionDocumentSnapshot> result;
	result.reserve(m_documents.size());
	for (const auto& [id, snapshot] : m_documents) result.push_back(snapshot);
	std::ranges::sort(result, {}, &SExtensionDocumentSnapshot::id);
	return result;
}

void CExtensionDocumentSync::Clear()
{
	m_documents.clear();
}

CExtensionEventAggregator::CExtensionEventAggregator(
	std::size_t maximumChanges,
	std::size_t maximumBytes,
	std::chrono::milliseconds minimumDelay,
	std::chrono::milliseconds maximumDelay)
	: m_maximumChanges((std::max)(std::size_t(1), maximumChanges))
	, m_maximumBytes((std::max)(std::size_t(1), maximumBytes))
	, m_minimumDelay((std::max)(std::chrono::milliseconds(1), minimumDelay))
	, m_maximumDelay((std::max)(m_minimumDelay, maximumDelay))
{
}

void CExtensionEventAggregator::Enqueue(SExtensionDocumentSnapshot snapshot, Clock::time_point now)
{
	const auto bytes = snapshot.text.size() * sizeof(wchar_t);
	const auto found = m_pending.find(snapshot.id);
	if (found == m_pending.end()) {
		m_pending.emplace(snapshot.id, Pending{
			.snapshot = std::move(snapshot),
			.first = now,
			.ready = now + m_minimumDelay,
			.deadline = now + m_maximumDelay,
			.changes = 1,
			.bytes = bytes,
			.snapshotOnly = bytes > m_maximumBytes,
		});
		return;
	}
	auto& pending = found->second;
	pending.snapshot = std::move(snapshot);
	pending.changes++;
	pending.bytes = pending.bytes > (std::numeric_limits<std::size_t>::max)() - bytes
		? (std::numeric_limits<std::size_t>::max)() : pending.bytes + bytes;
	pending.snapshotOnly = pending.snapshotOnly || pending.changes > m_maximumChanges || pending.bytes > m_maximumBytes;
	// The ready time may move, but never beyond the original sixteen-millisecond deadline.
	pending.ready = (std::min)(now + m_minimumDelay, pending.deadline);
}

std::vector<SExtensionAggregatedDocumentEvent> CExtensionEventAggregator::DrainReady(Clock::time_point now)
{
	std::vector<SExtensionAggregatedDocumentEvent> result;
	for (auto iterator = m_pending.begin(); iterator != m_pending.end();) {
		if (now < iterator->second.ready && now < iterator->second.deadline) {
			++iterator;
			continue;
		}
		auto& pending = iterator->second;
		result.push_back({ std::move(pending.snapshot), pending.snapshotOnly, pending.changes });
		iterator = m_pending.erase(iterator);
	}
	std::ranges::sort(result, {}, [](const auto& event) { return event.snapshot.id; });
	return result;
}

std::optional<CExtensionEventAggregator::Clock::time_point> CExtensionEventAggregator::NextReadyTime() const noexcept
{
	std::optional<Clock::time_point> result;
	for (const auto& [id, pending] : m_pending) {
		(void)id;
		const auto ready = (std::min)(pending.ready, pending.deadline);
		if (!result || ready < *result) result = ready;
	}
	return result;
}

void CExtensionEventAggregator::Clear()
{
	m_pending.clear();
}

CExtensionUiDispatcher::PostResult CExtensionUiDispatcher::Post(Task task)
{
	if (!task) return PostResult::Overloaded;
	std::lock_guard lock(m_mutex);
	if (m_stopped) return PostResult::Stopped;
	if (m_tasks.size() >= m_capacity) return PostResult::Overloaded;
	m_tasks.emplace_back(std::move(task));
	return PostResult::Queued;
}

std::size_t CExtensionUiDispatcher::Drain(std::size_t maximumTasks)
{
	std::size_t completed = 0;
	while (completed < maximumTasks) {
		Task task;
		{
			std::lock_guard lock(m_mutex);
			if (m_tasks.empty()) break;
			task = std::move(m_tasks.front());
			m_tasks.pop_front();
		}
		task();
		++completed;
	}
	return completed;
}

void CExtensionUiDispatcher::Stop() noexcept
{
	std::lock_guard lock(m_mutex);
	m_stopped = true;
	m_tasks.clear();
}

std::size_t CExtensionUiDispatcher::Size() const
{
	std::lock_guard lock(m_mutex);
	return m_tasks.size();
}

namespace {

std::optional<std::size_t> OffsetAt(std::wstring_view text, const SExtensionTextPosition& position)
{
	std::size_t offset = 0;
	std::uint32_t line = 0;
	while (line < position.line) {
		const auto newline = text.find(L'\n', offset);
		if (newline == std::wstring_view::npos) return std::nullopt;
		offset = newline + 1;
		++line;
	}
	const auto lineEnd = text.find(L'\n', offset);
	const auto contentEnd = lineEnd == std::wstring_view::npos ? text.size()
		: (lineEnd > offset && text[lineEnd - 1] == L'\r' ? lineEnd - 1 : lineEnd);
	if (position.character > contentEnd - offset) return std::nullopt;
	return offset + position.character;
}

} // namespace

EExtensionApplyEditStatus CExtensionApplyEdit::Prepare(
	const SExtensionDocumentSnapshot& snapshot,
	const SExtensionDocumentEdit& requested,
	PreparedEdit& prepared)
{
	struct OffsetEdit { std::size_t start; std::size_t end; const std::wstring* text; };
	std::vector<OffsetEdit> edits;
	edits.reserve(requested.edits.size());
	for (const auto& edit : requested.edits) {
		const auto start = OffsetAt(snapshot.text, edit.range.start);
		const auto end = OffsetAt(snapshot.text, edit.range.end);
		if (!start || !end || *start > *end) return EExtensionApplyEditStatus::InvalidRange;
		edits.push_back({ *start, *end, &edit.newText });
	}
	std::ranges::sort(edits, {}, &OffsetEdit::start);
	for (std::size_t index = 1; index < edits.size(); ++index) {
		if (edits[index].start < edits[index - 1].end) return EExtensionApplyEditStatus::OverlappingEdits;
	}
	prepared.id = snapshot.id;
	prepared.text = snapshot.text;
	for (auto iterator = edits.rbegin(); iterator != edits.rend(); ++iterator) {
		prepared.text.replace(iterator->start, iterator->end - iterator->start, *iterator->text);
	}
	if (requested.crlf) {
		std::wstring normalized;
		normalized.reserve(prepared.text.size());
		for (std::size_t index = 0; index < prepared.text.size(); ++index) {
			if (prepared.text[index] == L'\r') {
				if (index + 1 < prepared.text.size() && prepared.text[index + 1] == L'\n') ++index;
				normalized.append(*requested.crlf ? L"\r\n" : L"\n");
			} else if (prepared.text[index] == L'\n') {
				normalized.append(*requested.crlf ? L"\r\n" : L"\n");
			} else {
				normalized.push_back(prepared.text[index]);
			}
		}
		prepared.text = std::move(normalized);
	}
	return EExtensionApplyEditStatus::Applied;
}

SExtensionApplyEditResult CExtensionApplyEdit::Apply(
	const std::vector<SExtensionDocumentEdit>& requestedDocuments,
	bool commandDispatchActive)
{
	if (commandDispatchActive) return { EExtensionApplyEditStatus::CommandReentry };
	std::unordered_set<SExtensionDocumentId> seen;
	std::vector<PreparedEdit> prepared;
	UndoUnit undo{ .id = m_nextUndoUnit };
	prepared.reserve(requestedDocuments.size());
	undo.before.reserve(requestedDocuments.size());
	for (const auto& requested : requestedDocuments) {
		if (!seen.emplace(requested.documentId).second) return { EExtensionApplyEditStatus::InvalidRange };
		const auto found = m_documents.m_documents.find(requested.documentId);
		if (found == m_documents.m_documents.end()) return { EExtensionApplyEditStatus::UnknownDocument };
		if (requested.expectedVersion != found->second.version) return { EExtensionApplyEditStatus::VersionMismatch };
		PreparedEdit value;
		const auto status = Prepare(found->second, requested, value);
		if (status != EExtensionApplyEditStatus::Applied) return { status };
		undo.before.push_back(found->second);
		prepared.push_back(std::move(value));
	}
	for (auto& value : prepared) {
		auto& snapshot = m_documents.m_documents.at(value.id);
		snapshot.text = std::move(value.text);
		++snapshot.version;
		snapshot.dirty = true;
	}
	m_undoUnits.push_back(std::move(undo));
	return { EExtensionApplyEditStatus::Applied, m_nextUndoUnit++ };
}
