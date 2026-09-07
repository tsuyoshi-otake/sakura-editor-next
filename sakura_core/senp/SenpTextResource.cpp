/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "senp/SenpTextResource.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>

namespace senp {
namespace {
bool Identifier(std::string_view value, bool extension = false) noexcept
{
	return !value.empty() && value.size() <= 128 && std::all_of(value.begin(), value.end(), [extension](unsigned char c) {
		return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_'
			|| (!extension && ((c >= 'A' && c <= 'Z') || c == ':'));
	});
}
bool Valid(const TextResourceScope& scope) noexcept
{
	return Identifier(scope.profileId) && Identifier(scope.extensionId, true) && Identifier(scope.grantId)
		&& scope.packageDigest.size() == 64 && std::all_of(scope.packageDigest.begin(), scope.packageDigest.end(), [](char c) {
			return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
		}) && scope.ownerGeneration > 0 && scope.workspaceRevision >= 0 && scope.accountGeneration >= 0 && scope.revision >= 0;
}
std::uint64_t NextStoreId()
{
	static std::atomic<std::uint64_t> next{ 1 };
	auto value = next.load();
	while (value != (std::numeric_limits<std::uint64_t>::max)()) {
		if (next.compare_exchange_weak(value, value + 1)) return value;
	}
	throw std::overflow_error("Text resource store identities exhausted.");
}
}
struct SenpTextResourceStore::Impl {
	using Page = std::array<char, kChunkBytes>;
	struct Entry final {
		TextResourceScope scope;
		std::vector<std::unique_ptr<Page>> pages;
		std::size_t size{};
		TextResourceState state{ TextResourceState::Loading };
		TextResourceEnd end{ TextResourceEnd::None };
	};
	std::map<std::wstring, Entry, std::less<>> entries;
	const std::uint64_t instance{ NextStoreId() };
	std::uint64_t next{ 1 };
	std::size_t bytes{}, allocated{};
	bool closed{};
	auto Find(const TextResourceScope& scope, std::wstring_view handle) noexcept {
		auto entry = entries.find(handle);
		return entry != entries.end() && entry->second.scope == scope ? entry : entries.end();
	}
	static void Finish(Entry& entry, TextResourceEnd end) noexcept {
		entry.end = end;
		entry.state = end == TextResourceEnd::Complete ? TextResourceState::Complete
			: entry.size ? TextResourceState::Partial : TextResourceState::Failed;
	}
	void Clear(Entry& entry) noexcept {
		bytes -= entry.size; allocated -= entry.pages.size() * kChunkBytes;
		entry.pages.clear(); entry.size = 0;
	}
};

SenpTextResourceStore::SenpTextResourceStore() : m_impl(std::make_unique<Impl>()) {}
SenpTextResourceStore::~SenpTextResourceStore() = default;
TextResourceCreated SenpTextResourceStore::Create(TextResourceScope scope)
{
	auto& self = *m_impl;
	if (self.closed) return { TextResourceResult::Closed, {} };
	if (!Valid(scope)) return {};
	if (self.entries.size() == kResources || self.next == (std::numeric_limits<std::uint64_t>::max)())
		return { TextResourceResult::LimitReached, {} };
	TextResourceCreated result{ TextResourceResult::Accepted, L"text:" + std::to_wstring(self.instance) + L":" + std::to_wstring(self.next++) };
	self.entries.emplace(result.handle, Impl::Entry{ std::move(scope) });
	return result;
}
TextResourceResult SenpTextResourceStore::Append(const TextResourceScope& scope, std::wstring_view handle,
	std::size_t offset, std::string_view bytes)
{
	auto& self = *m_impl;
	if (self.closed) return TextResourceResult::Closed;
	auto found = self.Find(scope, handle);
	if (found == self.entries.end()) return TextResourceResult::NotFound;
	auto& entry = found->second;
	if (entry.state != TextResourceState::Loading) return TextResourceResult::Terminal;
	if (offset != entry.size) return TextResourceResult::Stale;
	if (bytes.empty() || bytes.size() > kChunkBytes) return TextResourceResult::Invalid;
	const auto pages = (entry.size + bytes.size() + kChunkBytes - 1) / kChunkBytes;
	const auto additional = pages - entry.pages.size();
	if (bytes.size() > kResourceBytes - entry.size || additional * kChunkBytes > kTotalBytes - self.allocated) {
		Impl::Finish(entry, TextResourceEnd::LimitExceeded); return TextResourceResult::LimitReached;
	}
	// Prepare all allocations before changing bytes or ownership counters.
	std::vector<std::unique_ptr<Impl::Page>> prepared;
	prepared.reserve(additional);
	for (std::size_t i = 0; i < additional; ++i) prepared.push_back(std::make_unique<Impl::Page>());
	if (pages > entry.pages.capacity()) entry.pages.reserve((std::min)(kResourceBytes / kChunkBytes,
		(std::max)(pages, entry.pages.capacity() * 2)));
	for (auto& page : prepared) entry.pages.push_back(std::move(page));
	std::size_t consumed{};
	while (consumed < bytes.size()) {
		const auto position = entry.size + consumed;
		const auto count = (std::min)(bytes.size() - consumed, kChunkBytes - position % kChunkBytes);
		std::copy_n(bytes.data() + consumed, count, entry.pages[position / kChunkBytes]->data() + position % kChunkBytes);
		consumed += count;
	}
	entry.size += bytes.size(); self.bytes += bytes.size(); self.allocated += additional * kChunkBytes;
	return TextResourceResult::Accepted;
}
TextResourceResult SenpTextResourceStore::Finish(const TextResourceScope& scope, std::wstring_view handle, TextResourceEnd end)
{
	auto& self = *m_impl;
	if (self.closed) return TextResourceResult::Closed;
	auto found = self.Find(scope, handle);
	if (found == self.entries.end()) return TextResourceResult::NotFound;
	if (found->second.state != TextResourceState::Loading) return TextResourceResult::Terminal;
	if (end != TextResourceEnd::Complete && end != TextResourceEnd::Failed
		&& end != TextResourceEnd::Cancelled && end != TextResourceEnd::LimitExceeded) return TextResourceResult::Invalid;
	Impl::Finish(found->second, end); return TextResourceResult::Accepted;
}
TextResourceChunk SenpTextResourceStore::Read(const TextResourceScope& scope, std::wstring_view handle,
	std::size_t offset, std::size_t count) const
{
	const auto& self = *m_impl;
	TextResourceChunk result;
	if (self.closed) { result.result = TextResourceResult::Closed; result.state = TextResourceState::Closed; return result; }
	const auto found = self.entries.find(handle);
	if (found == self.entries.end() || found->second.scope != scope) { result.result = TextResourceResult::NotFound; return result; }
	const auto& entry = found->second;
	result.handle = found->first; result.revision = entry.scope.revision;
	result.state = entry.state; result.end = entry.end; result.length = entry.size; result.offset = offset;
	if (entry.state == TextResourceState::Expired) { result.result = TextResourceResult::Expired; return result; }
	if (!count || count > kChunkBytes || offset > entry.size) return result;
	result.result = TextResourceResult::Accepted;
	result.bytes.resize((std::min)(count, entry.size - offset));
	std::size_t copied{};
	while (copied < result.bytes.size()) {
		const auto position = offset + copied;
		const auto size = (std::min)(result.bytes.size() - copied, kChunkBytes - position % kChunkBytes);
		std::copy_n(entry.pages[position / kChunkBytes]->data() + position % kChunkBytes, size, result.bytes.data() + copied);
		copied += size;
	}
	return result;
}
TextResourceResult SenpTextResourceStore::Expire(const TextResourceScope& scope, std::wstring_view handle) noexcept
{
	auto& self = *m_impl;
	if (self.closed) return TextResourceResult::Closed;
	auto found = self.Find(scope, handle);
	if (found == self.entries.end()) return TextResourceResult::NotFound;
	self.Clear(found->second); found->second.state = TextResourceState::Expired; found->second.end = TextResourceEnd::Revoked;
	return TextResourceResult::Expired;
}
TextResourceResult SenpTextResourceStore::Release(const TextResourceScope& scope, std::wstring_view handle) noexcept
{
	auto& self = *m_impl;
	if (self.closed) return TextResourceResult::Closed;
	auto found = self.Find(scope, handle);
	if (found == self.entries.end()) return TextResourceResult::NotFound;
	self.Clear(found->second); self.entries.erase(found); return TextResourceResult::Accepted;
}
void SenpTextResourceStore::Close() noexcept
{
	auto& self = *m_impl; self.closed = true; self.entries.clear(); self.bytes = 0; self.allocated = 0;
}
TextResourceUsage SenpTextResourceStore::Usage() const noexcept
{
	const auto& self = *m_impl; return { self.entries.size(), self.bytes, self.allocated, self.closed };
}

TextDecodedChunk SenpTextResourceDecoder::Decode(std::size_t offset, std::string_view bytes, bool final)
{
	if (m_closed) return { TextDecodeResult::Closed, {} };
	if (offset != m_offset) return { TextDecodeResult::Stale, {} };
	if (bytes.size() > SenpTextResourceStore::kChunkBytes) return {};
	if (bytes.size() > SenpTextResourceStore::kResourceBytes - m_offset) { Close(); return { TextDecodeResult::LimitReached, {} }; }
	std::string input = m_pending; input.append(bytes);
	TextDecodedChunk result{ TextDecodeResult::Accepted, {} }; result.text.reserve(input.size());
	bool previousCr = m_previousCr;
	std::size_t cursor{};
	while (cursor < input.size()) {
		const auto lead = static_cast<unsigned char>(input[cursor]);
		const std::size_t length = lead < 0x80 ? 1 : lead >= 0xc2 && lead <= 0xdf ? 2
			: lead >= 0xe0 && lead <= 0xef ? 3 : lead >= 0xf0 && lead <= 0xf4 ? 4 : 0;
		bool invalid = !length;
		const auto available = (std::min)(length, input.size() - cursor);
		for (std::size_t i = 1; i < available; ++i) {
			const auto unit = static_cast<unsigned char>(input[cursor + i]);
			invalid |= unit < 0x80 || unit > 0xbf || (i == 1 && ((lead == 0xe0 && unit < 0xa0)
				|| (lead == 0xed && unit > 0x9f) || (lead == 0xf0 && unit < 0x90) || (lead == 0xf4 && unit > 0x8f)));
		}
		if (invalid || (available < length && final)) { Close(); return { TextDecodeResult::InvalidUtf8, {} }; }
		if (available < length) break;
		std::uint32_t point = length == 1 ? lead : lead & (0x7f >> length);
		for (std::size_t i = 1; i < length; ++i) point = (point << 6) | (static_cast<unsigned char>(input[cursor + i]) & 0x3f);
		cursor += length;
		if (point == '\n' && previousCr) { previousCr = false; continue; }
		previousCr = point == '\r';
		if (point == '\r') point = '\n';
		if (point < 0x20 && point != '\n' && point != '\t') point += 0x2400;
		else if (point == 0x7f) point = 0x2421;
		else if ((point >= 0x80 && point <= 0x9f) || point == 0x200e || point == 0x200f
			|| (point >= 0x202a && point <= 0x202e) || (point >= 0x2066 && point <= 0x2069)) point = 0xfffd;
		if (point <= 0xffff) result.text.push_back(static_cast<wchar_t>(point));
		else { point -= 0x10000; result.text.push_back(static_cast<wchar_t>(0xd800 + (point >> 10)));
			result.text.push_back(static_cast<wchar_t>(0xdc00 + (point & 0x3ff))); }
	}
	m_pending = input.substr(cursor); m_previousCr = previousCr; m_offset += bytes.size();
	if (final) Close();
	return result;
}
void SenpTextResourceDecoder::Close() noexcept { m_closed = true; m_pending.clear(); }

} // namespace senp
