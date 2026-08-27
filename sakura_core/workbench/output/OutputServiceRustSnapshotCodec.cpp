/*! @file
 * @brief Decoder for the copied Rust Output provider snapshot ABI.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"
#include "workbench/output/OutputServiceRustSnapshotCodec.h"

#include "workbench/output/OutputServiceRustProviderAbi.h"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace workbench::output {
namespace {

constexpr std::size_t kMaximumLabelBytes = 512;
constexpr std::size_t kMaximumMetadataBytes = 512;

bool IsValidUtf8(const std::string_view value, const bool permitControls) noexcept
{
	for (std::size_t index = 0; index < value.size();) {
		const auto first = static_cast<unsigned char>(value[index]);
		if (first < 0x80) {
			if ((!permitControls && (first < 0x20 || first == 0x7f)) || first == 0) return false;
			++index;
			continue;
		}
		std::size_t continuationCount{};
		std::uint32_t codePoint{};
		if (first >= 0xc2 && first <= 0xdf) { continuationCount = 1; codePoint = first & 0x1f; }
		else if (first >= 0xe0 && first <= 0xef) { continuationCount = 2; codePoint = first & 0x0f; }
		else if (first >= 0xf0 && first <= 0xf4) { continuationCount = 3; codePoint = first & 0x07; }
		else return false;
		if (index + continuationCount >= value.size()) return false;
		for (std::size_t continuation = 1; continuation <= continuationCount; ++continuation) {
			const auto next = static_cast<unsigned char>(value[index + continuation]);
			if ((next & 0xc0) != 0x80) return false;
			codePoint = (codePoint << 6) | (next & 0x3f);
		}
		const auto minimum = continuationCount == 1 ? 0x80U : continuationCount == 2 ? 0x800U : 0x10000U;
		if (codePoint < minimum || codePoint > 0x10ffff || (codePoint >= 0xd800 && codePoint <= 0xdfff)) return false;
		if (!permitControls && (codePoint >= 0x80 && codePoint <= 0x9f)) return false;
		index += continuationCount + 1;
	}
	return true;
}

bool IsValidMetadataValue(const std::optional<std::string>& value) noexcept
{
	return !value || (!value->empty() && value->size() <= kMaximumMetadataBytes
		&& IsValidUtf8(*value, false));
}

struct SnapshotReader final {
	const std::vector<std::uint8_t>& bytes;
	std::size_t offset{};

	[[nodiscard]] std::size_t Remaining() const noexcept
	{
		return offset <= bytes.size() ? bytes.size() - offset : 0;
	}

	[[nodiscard]] bool ReadByte(std::uint8_t& value) noexcept
	{
		if (Remaining() < 1) return false;
		value = bytes[offset++];
		return true;
	}

	[[nodiscard]] bool ReadU32(std::uint32_t& value) noexcept
	{
		if (Remaining() < sizeof(value)) return false;
		value = static_cast<std::uint32_t>(bytes[offset])
			| (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
			| (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
			| (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
		offset += sizeof(value);
		return true;
	}

	[[nodiscard]] bool ReadU64(std::uint64_t& value) noexcept
	{
		if (Remaining() < sizeof(value)) return false;
		value = 0;
		for (std::size_t index = 0; index < sizeof(value); ++index) {
			value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8);
		}
		offset += sizeof(value);
		return true;
	}

	[[nodiscard]] bool ReadBytes(std::string& value)
	{
		std::uint64_t length{};
		if (!ReadU64(length) || length > Remaining()
			|| length > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) return false;
		const auto size = static_cast<std::size_t>(length);
		value.assign(reinterpret_cast<const char*>(bytes.data() + offset), size);
		offset += size;
		return IsValidUtf8(value, true);
	}

	[[nodiscard]] bool ReadOptionalBytes(std::optional<std::string>& value)
	{
		std::uint8_t present{};
		if (!ReadByte(present) || present > 1) return false;
		if (present == 0) { value.reset(); return true; }
		std::string copied;
		if (!ReadBytes(copied)) return false;
		value = std::move(copied);
		return true;
	}
};

} // namespace

std::optional<OutputServiceSnapshot> DecodeOutputServiceRustSnapshotV1(
	const std::vector<std::uint8_t>& bytes)
{
	SnapshotReader reader{ bytes };
	if (reader.Remaining() < kOutputServiceRustSnapshotMagicV1.size()
		|| !std::equal(kOutputServiceRustSnapshotMagicV1.begin(),
			kOutputServiceRustSnapshotMagicV1.end(), bytes.begin())) return std::nullopt;
	reader.offset = kOutputServiceRustSnapshotMagicV1.size();

	OutputServiceSnapshot snapshot;
	if (!reader.ReadU64(snapshot.revision)) return std::nullopt;
	std::uint8_t stopped{};
	if (!reader.ReadByte(stopped) || stopped > 1) return std::nullopt;
	snapshot.stopped = stopped != 0;
	if (!reader.ReadU64(snapshot.droppedNotificationCount)) return std::nullopt;

	std::uint8_t activePresent{};
	if (!reader.ReadByte(activePresent) || activePresent > 1) return std::nullopt;
	if (activePresent != 0) {
		std::string active;
		if (!reader.ReadBytes(active) || !IsValidOutputStableId(active)) return std::nullopt;
		snapshot.activeChannelId = std::move(active);
	}

	std::uint64_t channelCount{};
	if (!reader.ReadU64(channelCount) || channelCount > reader.Remaining()
		|| channelCount > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) return std::nullopt;
	snapshot.channels.reserve(static_cast<std::size_t>(channelCount));
	for (std::uint64_t index = 0; index < channelCount; ++index) {
		OutputChannelSnapshot channel;
		if (!reader.ReadBytes(channel.channelId) || !reader.ReadBytes(channel.label)
			|| !reader.ReadBytes(channel.owner.ownerId) || !reader.ReadU64(channel.owner.generation)) return std::nullopt;
		if (!IsValidOutputStableId(channel.channelId) || !IsValidOutputStableId(channel.owner.ownerId)
			|| channel.owner.generation == 0 || channel.label.empty()
			|| channel.label.size() > kMaximumLabelBytes || !IsValidUtf8(channel.label, false)) return std::nullopt;
		std::uint8_t kind{};
		if (!reader.ReadByte(kind) || kind > SAKURA_OUTPUT_PROVIDER_CHANNEL_LOG) return std::nullopt;
		channel.kind = static_cast<EOutputChannelKind>(kind);
		if (!reader.ReadOptionalBytes(channel.metadata.languageId)
			|| !reader.ReadOptionalBytes(channel.metadata.source)
			|| !IsValidMetadataValue(channel.metadata.languageId)
			|| !IsValidMetadataValue(channel.metadata.source)) return std::nullopt;
		std::uint8_t visible{};
		std::uint8_t preservedFocus{};
		if (!reader.ReadByte(visible) || visible > 1 || !reader.ReadByte(preservedFocus) || preservedFocus > 1
			|| !reader.ReadU64(channel.droppedCharacterCount) || !reader.ReadBytes(channel.text)) return std::nullopt;
		channel.visible = visible != 0;
		channel.lastShowPreservedFocus = preservedFocus != 0;

		std::uint64_t logCount{};
		if (!reader.ReadU64(logCount) || logCount > reader.Remaining()
			|| logCount > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) return std::nullopt;
		channel.logEntries.reserve(static_cast<std::size_t>(logCount));
		for (std::uint64_t logIndex = 0; logIndex < logCount; ++logIndex) {
			std::uint32_t level{};
			OutputLogEntry entry;
			if (!reader.ReadU32(level) || level > static_cast<std::uint32_t>(EOutputLogLevel::Error)
				|| !reader.ReadBytes(entry.message) || entry.message.empty()
				|| !IsValidUtf8(entry.message, true) || !reader.ReadOptionalBytes(entry.source)
				|| !IsValidMetadataValue(entry.source)) return std::nullopt;
			entry.level = static_cast<EOutputLogLevel>(level);
			channel.logEntries.push_back(std::move(entry));
		}
		if (!reader.ReadBytes(channel.projectedText)) return std::nullopt;
		snapshot.channels.push_back(std::move(channel));
	}
	if (reader.Remaining() != 0) return std::nullopt;
	return snapshot;
}

} // namespace workbench::output
