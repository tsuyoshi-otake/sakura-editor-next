/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include "senp/SenpEffectProtocol.h"
#include "markdown/MarkdownParser.h"
#include "workbench/editor/SenpReadonlyWorkbench.h"

namespace workbench::editor {

enum class SenpDocumentState : std::uint8_t { Dormant, Loading, Ready, Failed, Expired, Closed };
enum class SenpDocumentResult : std::uint8_t { Accepted, Stale, Invalid, Unsupported, Closed };
struct SenpDocumentTransition final {
	SenpDocumentResult result{ SenpDocumentResult::Invalid };
	//! Composition must cancel this subscriber; the broker retains physical cleanup.
	std::optional<senp::effect::OperationContext> retired;
};
struct SenpPreparedDocument final {
	SenpDocumentResult result{ SenpDocumentResult::Invalid };
	markdown::Document document;
};

//! I/O-free bounded conversion. Only Markdown sections are parsed. All metadata
//! and table cells stay literal. No local or remote asset is ever admitted.
[[nodiscard]] SenpPreparedDocument PrepareSenpReadonlyDocument(const senp::effect::PublishDocument& document);

//! UI-thread value owner for one already-authorized readonly input. Runtime
//! admission, deadlines, cancellation and core title updates belong to composition.
//! Begin follows successful admission. Every replacement/terminal returns the
//! retired context; no destructor silently owns a live runtime request.
class SenpReadonlyDocument final {
public:
	explicit SenpReadonlyDocument(SenpReadonlyInput input);
	[[nodiscard]] SenpDocumentTransition Begin(senp::effect::OperationContext context);
	[[nodiscard]] SenpDocumentTransition Apply(const senp::effect::OperationContext& context,
		senp::effect::PublishDocument document);
	[[nodiscard]] SenpDocumentTransition Fail(const senp::effect::OperationContext& context);
	[[nodiscard]] SenpDocumentTransition Expire() noexcept;
	[[nodiscard]] SenpDocumentTransition Close() noexcept;
	[[nodiscard]] SenpDocumentState State() const noexcept { return m_state; }
	[[nodiscard]] std::uint64_t Generation() const noexcept { return m_generation; }
	[[nodiscard]] const SenpReadonlyInput& Input() const noexcept { return m_input; }
	[[nodiscard]] std::shared_ptr<const senp::effect::PublishDocument> Content() const noexcept { return m_content; }
private:
	[[nodiscard]] bool Matches(const senp::effect::OperationContext& context) const noexcept;
	[[nodiscard]] SenpDocumentTransition Finish(SenpDocumentState state, SenpDocumentResult result) noexcept;
	SenpReadonlyInput m_input;
	SenpDocumentState m_state{ SenpDocumentState::Dormant };
	std::uint64_t m_generation{};
	std::int64_t m_lastRequest{}, m_lastRevision{ -1 };
	std::optional<senp::effect::OperationContext> m_pending;
	std::shared_ptr<const senp::effect::PublishDocument> m_content;
};

} // namespace workbench::editor
