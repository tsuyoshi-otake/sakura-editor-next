/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace senp {

//! Native broker authorization context, never reconstructed from a Wasm label.
//! Broker must additionally prove the grant is currently live before each call.
struct TextResourceScope final {
	std::string profileId, extensionId, packageDigest, grantId;
	std::int64_t ownerGeneration{}, workspaceRevision{}, accountGeneration{}, revision{};
	bool operator==(const TextResourceScope&) const = default;
};
enum class TextResourceState : std::uint8_t { Loading, Complete, Partial, Failed, Expired, Closed };
enum class TextResourceEnd : std::uint8_t { None, Complete, Failed, Cancelled, LimitExceeded, Revoked };
enum class TextResourceResult : std::uint8_t { Accepted, Invalid, NotFound, Stale, Terminal, LimitReached, Expired, Closed };
struct TextResourceCreated final {
	TextResourceResult result{ TextResourceResult::Invalid };
	std::wstring handle;
};
struct TextResourceChunk final {
	TextResourceResult result{ TextResourceResult::Invalid };
	TextResourceState state{ TextResourceState::Failed };
	TextResourceEnd end{ TextResourceEnd::None };
	std::wstring handle;
	std::int64_t revision{};
	std::size_t offset{}, length{};
	std::string bytes;
};
struct TextResourceUsage final {
	std::size_t resources{}, bytes{}, allocatedBytes{};
	bool closed{};
};

//! Single broker-thread store. Calls never perform I/O or invoke callbacks.
//! Physical producers belong to the broker: every limit/failure/cancel/expiry
//! must stop their work there. Handles are opaque identities, not bearer tokens.
class SenpTextResourceStore final {
public:
	static constexpr std::size_t kChunkBytes = 64 * 1024;
	static constexpr std::size_t kResourceBytes = 32 * 1024 * 1024;
	static constexpr std::size_t kTotalBytes = 64 * 1024 * 1024;
	static constexpr std::size_t kResources = 64;
	SenpTextResourceStore();
	~SenpTextResourceStore();
	SenpTextResourceStore(const SenpTextResourceStore&) = delete;
	SenpTextResourceStore& operator=(const SenpTextResourceStore&) = delete;
	[[nodiscard]] TextResourceCreated Create(TextResourceScope scope);
	[[nodiscard]] TextResourceResult Append(const TextResourceScope& scope, std::wstring_view handle,
		std::size_t offset, std::string_view bytes);
	[[nodiscard]] TextResourceResult Finish(const TextResourceScope& scope, std::wstring_view handle, TextResourceEnd end);
	[[nodiscard]] TextResourceChunk Read(const TextResourceScope& scope, std::wstring_view handle,
		std::size_t offset, std::size_t count) const;
	[[nodiscard]] TextResourceResult Expire(const TextResourceScope& scope, std::wstring_view handle) noexcept;
	[[nodiscard]] TextResourceResult Release(const TextResourceScope& scope, std::wstring_view handle) noexcept;
	void Close() noexcept;
	[[nodiscard]] TextResourceUsage Usage() const noexcept;
private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

enum class TextDecodeResult : std::uint8_t { Accepted, Invalid, Stale, InvalidUtf8, LimitReached, Closed };
struct TextDecodedChunk final {
	TextDecodeResult result{ TextDecodeResult::Invalid };
	std::wstring text;
};
//! Incremental strict UTF-8 decoder. Output uses LF, visible control pictures
//! and no ANSI/OSC actions. The consumer owns indexed text; this retains only
//! up to three UTF-8 bytes, never a duplicate of the document or a line index.
class SenpTextResourceDecoder final {
public:
	[[nodiscard]] TextDecodedChunk Decode(std::size_t offset, std::string_view bytes, bool final);
	void Close() noexcept;
	[[nodiscard]] std::size_t Offset() const noexcept { return m_offset; }
	[[nodiscard]] std::size_t PendingBytes() const noexcept { return m_pending.size(); }
	[[nodiscard]] bool IsClosed() const noexcept { return m_closed; }
private:
	std::string m_pending;
	std::size_t m_offset{};
	bool m_previousCr{}, m_closed{};
};

} // namespace senp
