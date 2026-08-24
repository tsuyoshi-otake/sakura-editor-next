/*! @file */
/*
Copyright (C) 2026, Sakura Editor Organization

SPDX-License-Identifier: Zlib
*/
#pragma once

#include "textmate/TextMateTokenizer.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace senp {

class ISenpManagementService;

//! One document-local tokenizer. The session owns TextMate's mutable regex
//! cache and carries no package-management or editor-window responsibility.
class ISenpTextMateSession {
public:
	virtual ~ISenpTextMateSession() = default;
	[[nodiscard]] virtual std::wstring_view LanguageId() const noexcept = 0;
	[[nodiscard]] virtual std::wstring_view ScopeName() const noexcept = 0;
	[[nodiscard]] virtual textmate::RuleStackHandle InitialState() const = 0;
	[[nodiscard]] virtual textmate::TextMateLineTokenizeResult TokenizeLine(
		std::wstring_view line,
		const textmate::RuleStackHandle& previousState,
		textmate::RuleStackHandle& nextState) const = 0;
};

//! Resolves enabled declarative SENP language/grammar contributions. It never
//! launches extension code and never sends document text outside this process.
class ISenpLanguageService {
public:
	virtual ~ISenpLanguageService() = default;
	[[nodiscard]] virtual bool Start() = 0;
	virtual void Stop() noexcept = 0;
	virtual void NotifyExtensionsChanged() noexcept = 0;
	//! Monotonic projection generation. Editor views use it to discard document
	//! tokenization state after an install, enable, disable, or uninstall.
	[[nodiscard]] virtual std::uint64_t Revision() const noexcept = 0;
	[[nodiscard]] virtual std::unique_ptr<ISenpTextMateSession> CreateSession(
		std::wstring_view resourcePath,
		std::wstring_view firstLine,
		std::wstring_view preferredLanguageId = {}) = 0;
};

[[nodiscard]] std::unique_ptr<ISenpLanguageService> CreateSenpLanguageService(
	ISenpManagementService& management);

} // namespace senp
