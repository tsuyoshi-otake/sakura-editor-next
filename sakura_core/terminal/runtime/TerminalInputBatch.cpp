/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "terminal/runtime/TerminalInputBatch.h"

#include "terminal/input/SakuraTerminalInputAdapter.h"
#include "terminal/window/TerminalInput.h"

#include <limits>
#include <string>
#include <utility>
#include <windows.h>

namespace terminal {
namespace {

bool IsValidUtf16(const std::u16string& value) noexcept
{
	for (std::size_t index = 0; index < value.size(); ++index) {
		const auto codeUnit = static_cast<std::uint16_t>(value[index]);
		if (codeUnit >= 0xd800 && codeUnit <= 0xdbff) {
			if (index + 1 >= value.size()) return false;
			const auto low = static_cast<std::uint16_t>(value[index + 1]);
			if (low < 0xdc00 || low > 0xdfff) return false;
			++index;
		} else if (codeUnit >= 0xdc00 && codeUnit <= 0xdfff) {
			return false;
		}
	}
	return true;
}

std::wstring ToWide(const std::u16string& value)
{
	static_assert(sizeof(wchar_t) == sizeof(char16_t));
	return std::wstring(value.begin(), value.end());
}

bool AppendBounded(std::vector<std::uint8_t>& destination, const std::string& source,
	std::size_t maximumBytes)
{
	if (destination.size() > maximumBytes) return false;
	if (source.size() > maximumBytes - destination.size()) return false;
	destination.insert(destination.end(), source.begin(), source.end());
	return true;
}

std::optional<TerminalKeyEvent> MakeKeyEvent(TerminalNamedKey key) noexcept
{
	TerminalKeyEvent event;
	switch (key) {
	case TerminalNamedKey::Enter: event.virtualKey = VK_RETURN; break;
	case TerminalNamedKey::Escape: event.virtualKey = VK_ESCAPE; break;
	case TerminalNamedKey::Tab: event.virtualKey = VK_TAB; break;
	case TerminalNamedKey::BSpace: event.virtualKey = VK_BACK; break;
	case TerminalNamedKey::Space:
		event.virtualKey = VK_SPACE;
		event.character = L' ';
		break;
	case TerminalNamedKey::Up: event.virtualKey = VK_UP; event.enhanced = true; break;
	case TerminalNamedKey::Down: event.virtualKey = VK_DOWN; event.enhanced = true; break;
	case TerminalNamedKey::Left: event.virtualKey = VK_LEFT; event.enhanced = true; break;
	case TerminalNamedKey::Right: event.virtualKey = VK_RIGHT; event.enhanced = true; break;
	case TerminalNamedKey::Home: event.virtualKey = VK_HOME; event.enhanced = true; break;
	case TerminalNamedKey::End: event.virtualKey = VK_END; event.enhanced = true; break;
	case TerminalNamedKey::PageUp: event.virtualKey = VK_PRIOR; event.enhanced = true; break;
	case TerminalNamedKey::PageDown: event.virtualKey = VK_NEXT; event.enhanced = true; break;
	case TerminalNamedKey::Insert: event.virtualKey = VK_INSERT; event.enhanced = true; break;
	case TerminalNamedKey::Delete: event.virtualKey = VK_DELETE; event.enhanced = true; break;
	case TerminalNamedKey::F1: event.virtualKey = VK_F1; break;
	case TerminalNamedKey::F2: event.virtualKey = VK_F2; break;
	case TerminalNamedKey::F3: event.virtualKey = VK_F3; break;
	case TerminalNamedKey::F4: event.virtualKey = VK_F4; break;
	case TerminalNamedKey::F5: event.virtualKey = VK_F5; break;
	case TerminalNamedKey::F6: event.virtualKey = VK_F6; break;
	case TerminalNamedKey::F7: event.virtualKey = VK_F7; break;
	case TerminalNamedKey::F8: event.virtualKey = VK_F8; break;
	case TerminalNamedKey::F9: event.virtualKey = VK_F9; break;
	case TerminalNamedKey::F10: event.virtualKey = VK_F10; break;
	case TerminalNamedKey::F11: event.virtualKey = VK_F11; break;
	case TerminalNamedKey::F12: event.virtualKey = VK_F12; break;
	default: return std::nullopt;
	}
	return event;
}

TerminalInputResultCode MapEncodeCode(ETerminalInputBatchEncodeCode code) noexcept
{
	switch (code) {
	case ETerminalInputBatchEncodeCode::InvalidInput: return TerminalInputResultCode::InvalidInput;
	case ETerminalInputBatchEncodeCode::UnsupportedKey: return TerminalInputResultCode::UnsupportedKey;
	case ETerminalInputBatchEncodeCode::ResourceExhausted: return TerminalInputResultCode::InvalidInput;
	case ETerminalInputBatchEncodeCode::Succeeded: break;
	}
	return TerminalInputResultCode::InvalidInput;
}

} // namespace

TerminalInputBatchEncoder::TerminalInputBatchEncoder(TerminalInputBatchLimits limits) noexcept
	: m_limits(limits)
{
}

TerminalInputBatchEncodingResult TerminalInputBatchEncoder::Encode(
	const TerminalInputBatch& batch,
	SakuraTerminalInputAdapter& adapter,
	bool bracketedPaste) const
{
	TerminalInputBatchEncodingResult result;
	if (!batch.operationId.IsValid() || !batch.target.instanceId.IsValid()
		|| !batch.target.runtimeGeneration.IsValid() || batch.actions.empty()
		|| batch.actions.size() > m_limits.maximumActions || batch.repeatCount == 0) {
		return result;
	}
	if (batch.repeatCount > m_limits.maximumRepeatCount) {
		result.code = ETerminalInputBatchEncodeCode::ResourceExhausted;
		return result;
	}

	std::size_t codeUnits{};
	for (const auto& action : batch.actions) {
		if (action.text.size() > m_limits.maximumUtf16CodeUnits - codeUnits) {
			result.code = ETerminalInputBatchEncodeCode::ResourceExhausted;
			return result;
		}
		codeUnits += action.text.size();
		if (!IsValidUtf16(action.text)) return result;
		switch (action.kind) {
		case TerminalInputActionKind::LiteralText:
		case TerminalInputActionKind::PasteText:
			break;
		case TerminalInputActionKind::NamedKey:
			if (!action.text.empty()) return result;
			if (!MakeKeyEvent(action.key)) {
				result.code = ETerminalInputBatchEncodeCode::UnsupportedKey;
				return result;
			}
			break;
		default:
			return result;
		}
	}

	for (std::uint16_t repetition = 0; repetition < batch.repeatCount; ++repetition) {
		for (const auto& action : batch.actions) {
			std::string encoded;
			switch (action.kind) {
			case TerminalInputActionKind::LiteralText:
				encoded = EncodeTerminalText(ToWide(action.text));
				break;
			case TerminalInputActionKind::PasteText:
				encoded = EncodeTerminalPaste(ToWide(action.text), bracketedPaste);
				break;
			case TerminalInputActionKind::NamedKey: {
				if (!action.text.empty()) return result;
				const auto event = MakeKeyEvent(action.key);
				if (!event) {
					result.code = ETerminalInputBatchEncodeCode::UnsupportedKey;
					return result;
				}
				const auto adapterResult = adapter.EncodeKey(*event);
				if (!adapterResult || adapterResult->empty()) {
					result.code = ETerminalInputBatchEncodeCode::UnsupportedKey;
					return result;
				}
				encoded = *adapterResult;
				break;
			}
			default:
				return result;
			}
			if (!AppendBounded(result.bytes, encoded, m_limits.maximumEncodedBytes)) {
				result.bytes.clear();
				result.code = ETerminalInputBatchEncodeCode::ResourceExhausted;
				return result;
			}
		}
	}
	result.code = ETerminalInputBatchEncodeCode::Succeeded;
	return result;
}

TerminalInputBatchCommitter::TerminalInputBatchCommitter(TerminalInputBatchLimits limits) noexcept
	: m_encoder(limits)
{
}

TerminalInputResult TerminalInputBatchCommitter::EncodeAndCommit(
	const TerminalInputBatch& batch,
	SakuraTerminalInputAdapter& adapter,
	bool bracketedPaste,
	std::chrono::steady_clock::time_point now,
	const TerminalInputCommitSink& commit) const
{
	TerminalInputResult result;
	if (batch.deadline != std::chrono::steady_clock::time_point{} && now >= batch.deadline) {
		result.code = TerminalInputResultCode::DeadlineExceeded;
		return result;
	}
	if (!commit) {
		result.code = TerminalInputResultCode::NotRunning;
		return result;
	}
	const auto encoded = m_encoder.Encode(batch, adapter, bracketedPaste);
	if (!encoded.Succeeded()) {
		result.code = MapEncodeCode(encoded.code);
		return result;
	}
	switch (commit(encoded.bytes)) {
	case TerminalQueueInputResult::Accepted: result.code = TerminalInputResultCode::Accepted; break;
	case TerminalQueueInputResult::QueueFull: result.code = TerminalInputResultCode::QueueFull; break;
	case TerminalQueueInputResult::NotRunning: result.code = TerminalInputResultCode::NotRunning; break;
	}
	return result;
}

} // namespace terminal
