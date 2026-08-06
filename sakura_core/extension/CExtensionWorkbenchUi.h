/*! @file
	@brief Native workbench models for diagnostics and interactive VS Code extension UI
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "extension/CExtensionDocumentBridge.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

enum class EExtensionDiagnosticSeverity : std::uint8_t {
	Error = 0,
	Warning = 1,
	Information = 2,
	Hint = 3,
};
struct SExtensionDiagnostic {
	SExtensionTextRange range;
	std::wstring message;
	EExtensionDiagnosticSeverity severity = EExtensionDiagnosticSeverity::Error;
	std::wstring source;
	std::wstring code;
};

struct SExtensionProblem {
	std::wstring uri;
	std::wstring extensionId;
	std::wstring collection;
	SExtensionDiagnostic diagnostic;
};

class CExtensionDiagnostics final {
public:
	[[nodiscard]] bool Set(
		std::wstring extensionId,
		std::uint64_t generation,
		std::wstring collection,
		std::wstring uri,
		std::vector<SExtensionDiagnostic> diagnostics);
	[[nodiscard]] bool Delete(
		std::wstring_view extensionId,
		std::uint64_t generation,
		std::wstring_view collection,
		std::wstring_view uri);
	void ClearCollection(std::wstring_view extensionId, std::uint64_t generation, std::wstring_view collection);
	void RemoveOwnedBy(std::wstring_view extensionId, std::uint64_t generation);
	void Clear();
	[[nodiscard]] std::vector<SExtensionDiagnostic> ForUri(std::wstring_view uri) const;
	[[nodiscard]] std::vector<SExtensionProblem> Problems() const;

private:
	struct Key {
		std::wstring extensionId;
		std::uint64_t generation = 0;
		std::wstring collection;
		std::wstring uri;
		auto operator<=>(const Key&) const = default;
	};
	struct KeyHash {
		std::size_t operator()(const Key& key) const noexcept;
	};

	mutable std::mutex m_mutex;
	std::unordered_map<Key, std::vector<SExtensionDiagnostic>, KeyHash> m_entries;
};

class CExtensionProblemsPane final {
public:
	explicit CExtensionProblemsPane(const CExtensionDiagnostics& diagnostics) : m_diagnostics(diagnostics) {}
	[[nodiscard]] std::vector<SExtensionProblem> Snapshot(std::wstring_view uriFilter = {}) const;

private:
	const CExtensionDiagnostics& m_diagnostics;
};

enum class EExtensionQuickInputKind { QuickPick, InputBox };
enum class EExtensionQuickInputState { Pending, Accepted, Cancelled, HostLost, Overloaded };

struct SExtensionQuickPickItem {
	std::size_t sourceIndex = 0;
	std::wstring label;
	std::wstring description;
	std::wstring detail;
	bool picked = false;
};

struct SExtensionQuickInputRequest {
	std::uint64_t id = 0;
	EExtensionQuickInputKind kind = EExtensionQuickInputKind::QuickPick;
	std::wstring extensionId;
	std::uint64_t generation = 0;
	std::wstring title;
	std::wstring placeholder;
	std::wstring value;
	bool canPickMany = false;
	bool password = false;
	std::vector<SExtensionQuickPickItem> items;
};

struct SExtensionQuickInputCompletion {
	std::uint64_t id = 0;
	EExtensionQuickInputState state = EExtensionQuickInputState::Cancelled;
	std::vector<std::size_t> selectedIndices;
	std::optional<std::wstring> value;
};

class CExtensionQuickInput final {
public:
	explicit CExtensionQuickInput(std::size_t maximumPending = 32) : m_maximumPending(maximumPending) {}
	[[nodiscard]] std::optional<std::uint64_t> Show(SExtensionQuickInputRequest request);
	[[nodiscard]] std::vector<SExtensionQuickInputRequest> Pending() const;
	[[nodiscard]] bool Resolve(std::uint64_t id, std::vector<std::size_t> selectedIndices, std::optional<std::wstring> value);
	[[nodiscard]] bool Cancel(std::uint64_t id, EExtensionQuickInputState state = EExtensionQuickInputState::Cancelled);
	[[nodiscard]] std::optional<SExtensionQuickInputCompletion> TakeCompletion(std::uint64_t id);
	void RemoveOwnedBy(std::wstring_view extensionId, std::uint64_t generation, EExtensionQuickInputState state);
	void Clear();

private:
	const std::size_t m_maximumPending;
	mutable std::mutex m_mutex;
	std::uint64_t m_nextId = 1;
	std::unordered_map<std::uint64_t, SExtensionQuickInputRequest> m_pending;
	std::unordered_map<std::uint64_t, SExtensionQuickInputCompletion> m_completions;
};

struct SExtensionOutputChannel {
	std::wstring handle;
	std::wstring extensionId;
	std::uint64_t generation = 0;
	std::wstring name;
	std::wstring languageId;
	std::wstring text;
	std::size_t droppedCharacters = 0;
	bool visible = false;
};

class CExtensionOutputChannel final {
public:
	explicit CExtensionOutputChannel(std::size_t maximumCharactersPerChannel = 2 * 1024 * 1024)
		: m_maximumCharactersPerChannel(maximumCharactersPerChannel) {}
	[[nodiscard]] bool Create(SExtensionOutputChannel channel);
	[[nodiscard]] bool Append(std::wstring_view handle, std::wstring_view extensionId, std::uint64_t generation, std::wstring_view value);
	[[nodiscard]] bool Replace(std::wstring_view handle, std::wstring_view extensionId, std::uint64_t generation, std::wstring value);
	[[nodiscard]] bool Clear(std::wstring_view handle, std::wstring_view extensionId, std::uint64_t generation);
	[[nodiscard]] bool SetVisible(std::wstring_view handle, std::wstring_view extensionId, std::uint64_t generation, bool visible);
	[[nodiscard]] bool Dispose(std::wstring_view handle, std::wstring_view extensionId, std::uint64_t generation);
	void RemoveOwnedBy(std::wstring_view extensionId, std::uint64_t generation);
	void ClearAll();
	[[nodiscard]] std::vector<SExtensionOutputChannel> Snapshot() const;

private:
	[[nodiscard]] SExtensionOutputChannel* FindOwned(
		std::wstring_view handle, std::wstring_view extensionId, std::uint64_t generation);
	void Truncate(SExtensionOutputChannel& channel);

	const std::size_t m_maximumCharactersPerChannel;
	mutable std::mutex m_mutex;
	std::unordered_map<std::wstring, SExtensionOutputChannel> m_channels;
};

struct SExtensionProgress {
	std::wstring handle;
	std::wstring extensionId;
	std::uint64_t generation = 0;
	std::wstring title;
	std::wstring message;
	double increment = 0;
	bool cancellable = false;
	bool cancelRequested = false;
};

class CExtensionProgressCenter final {
public:
	[[nodiscard]] bool Start(SExtensionProgress progress);
	[[nodiscard]] bool Report(
		std::wstring_view handle, std::wstring_view extensionId, std::uint64_t generation,
		std::wstring message, double increment);
	[[nodiscard]] bool End(std::wstring_view handle, std::wstring_view extensionId, std::uint64_t generation);
	[[nodiscard]] bool RequestCancel(std::wstring_view handle);
	void RemoveOwnedBy(std::wstring_view extensionId, std::uint64_t generation);
	void Clear();
	[[nodiscard]] std::vector<SExtensionProgress> Snapshot() const;

private:
	mutable std::mutex m_mutex;
	std::unordered_map<std::wstring, SExtensionProgress> m_items;
};

//! Merged, ready-to-render VS Code `Hover.contents` for one document position.
//! `markdown` is already the joined Markdown text (see
//! `CExtensionService::HandleHoverResponseWorker`); this struct never carries
//! raw provider JSON. A UI-thread consumer hands `markdown` straight to
//! `markdown::ParseMarkdown` -- the same renderer `CExtensionDetailSurface`
//! uses for Marketplace READMEs -- with an empty `documentPath`/`workspaceRoot`
//! so every resource reference resolves as `ResourceDisposition::ExternalBlocked`.
struct SExtensionHoverResult {
	SExtensionDocumentId documentId;
	SExtensionTextPosition position;
	std::wstring markdown;
	//! True when the host answered this exact request with no content at all
	//! (every provider returned nothing, or none matched). Distinct from "no
	//! result published yet" (a request still in flight, or one that was
	//! cancelled/superseded before a response ever arrived) -- that case never
	//! calls Publish at all, so Snapshot() simply returns nullopt for it.
	bool empty = true;
};

/*!
	@brief UI スレッドが最新の Hover 結果を読む窓口

	worker thread が `CExtensionService::RequestHover` の応答を `Publish` で書き、
	マウスが離れた・新しい要求で置き換えられた等の理由で `Clear` する。UI スレッドは
	`MYWM_EXTENSION_WORKBENCH_CHANGED`（`EExtensionWorkbenchChange::Hover`）を受けてから
	`Snapshot()` で読む。「その応答がまだ最新の要求に対するものか」の判定は
	`CExtensionService` 側の generation フェンス（サービス自身が要求ごとに進める
	sequence カウンタ）が Publish を呼ぶ前に行うので、このクラス自身は sequence を
	持たない -- 常に「そのとき最新だった結果、またはまだ何も無い」だけを保持する。
*/
class CExtensionHoverCenter final {
public:
	void Publish(SExtensionHoverResult result);
	void Clear();
	[[nodiscard]] std::optional<SExtensionHoverResult> Snapshot() const;

private:
	mutable std::mutex m_mutex;
	std::optional<SExtensionHoverResult> m_result;
};
