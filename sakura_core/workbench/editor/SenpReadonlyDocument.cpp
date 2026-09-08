/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "workbench/editor/SenpReadonlyDocument.h"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace workbench::editor {
namespace {
constexpr std::size_t kMaximumBlocks = 4096;
bool Title(std::wstring_view text)
{
	return !text.empty() && text.size() <= 256 && std::ranges::none_of(text,
		[](wchar_t value) { return value < 0x20 || value == 0x7f; });
}
void InertText(std::wstring& text)
{
	for (auto& value : text) if ((value < 0x20 && value != L'\n' && value != L'\r' && value != L'\t') || value == 0x7f) value = L'\xfffd';
}
void Deny(markdown::ResourceReference& resource)
{
	resource.disposition = markdown::ResourceDisposition::ExternalBlocked;
	resource.resolvedPath.clear(); resource.allowedRoot.clear();
}
void InertBlock(markdown::Block& block)
{
	InertText(block.text);
	for (auto& span : block.inlineSpans) if (span.resource) Deny(*span.resource);
	for (auto& image : block.images) Deny(image.source);
	for (auto& row : block.tableRows) for (auto& cell : row.cells) {
		InertText(cell.text);
		for (auto& span : cell.inlineSpans) if (span.resource) Deny(*span.resource);
	}
	for (auto& field : block.frontMatterFields) { InertText(field.name); InertText(field.value); }
}
markdown::TableRow Row(const std::vector<std::wstring>& values, bool header)
{
	markdown::TableRow row; row.header = header;
	for (const auto& value : values) row.cells.push_back({ value, {} });
	return row;
}
}

SenpPreparedDocument PrepareSenpReadonlyDocument(const senp::effect::PublishDocument& source,
	std::optional<SenpStructuredSectionRange> range)
{
	if (!senp::effect::ValidateDocument(source) || !Title(source.title)) return {};
	const auto first = range ? range->First() : 0, count = range ? range->Count() : source.sections.size();
	if (first > source.sections.size() || count > source.sections.size() - first) return {};
	for (auto index = first; index < first + count; ++index)
		if (std::holds_alternative<senp::effect::TextResourceSection>(source.sections[index])) return { SenpDocumentResult::Unsupported, {} };
	markdown::Document result;
	result.capabilities.localImageProjection = markdown::CapabilityStatus::Unsupported;
	result.capabilities.secureRemoteImageProjection = markdown::CapabilityStatus::Unsupported;
	result.capabilities.scrollEditorWithPreview = markdown::CapabilityStatus::Unsupported;
	result.capabilities.scrollPreviewWithEditor = markdown::CapabilityStatus::Unsupported;
	markdown::Block heading; heading.kind = markdown::BlockKind::Heading; heading.level = 1; heading.text = source.title;
	result.blocks.push_back(std::move(heading));
	for (auto index = first; index < first + count; ++index) {
		const auto& section = source.sections[index];
		if (const auto* text = std::get_if<senp::effect::MarkdownSection>(&section)) {
			markdown::ParseOptions options;
			options.limits.maximumInputCharacters = 262144;
			options.limits.maximumBlocks = kMaximumBlocks - result.blocks.size();
			options.limits.maximumImages = 64;
			auto parsed = markdown::ParseMarkdown(text->text, options);
			if (parsed.completion != markdown::ParseCompletion::Complete) return {};
			for (auto& block : parsed.blocks) result.blocks.push_back(std::move(block));
		} else if (const auto* metadata = std::get_if<senp::effect::MetadataSection>(&section)) {
			if (metadata->fields.empty()) continue;
			markdown::Block table; table.kind = markdown::BlockKind::Table;
			table.tableAlignments.resize(2, markdown::TableAlignment::Left);
			table.tableRows.push_back(Row({ L"Field", L"Value" }, true));
			for (const auto& field : metadata->fields) table.tableRows.push_back(Row({ field.name, field.value }, false));
			result.blocks.push_back(std::move(table));
		} else if (const auto* data = std::get_if<senp::effect::TableSection>(&section)) {
			markdown::Block table; table.kind = markdown::BlockKind::Table;
			table.tableAlignments.resize(data->columns.size(), markdown::TableAlignment::Left);
			table.tableRows.push_back(Row(data->columns, true));
			for (const auto& row : data->rows) table.tableRows.push_back(Row(row.cells, false));
			result.blocks.push_back(std::move(table));
		}
		if (result.blocks.size() > kMaximumBlocks) return {};
	}
	// Source positions identify the structured output, not an unrelated CEditDoc.
	for (std::size_t index = 0; index < result.blocks.size(); ++index) {
		InertBlock(result.blocks[index]); result.blocks[index].sourceLine = index;
	}
	return { SenpDocumentResult::Accepted, std::move(result) };
}

SenpReadonlyDocument::SenpReadonlyDocument(SenpReadonlyInput input) : m_input(std::move(input))
{
	const auto& scope = m_input.scope;
	const senp::effect::PublishDocument probe{ m_input.resourceId, m_input.title, 0, {} };
	if (m_input.inputId.empty() || scope.extensionId.empty() || scope.ownerGeneration <= 0
		|| scope.workspaceRevision < 0 || scope.accountGeneration < 0 || !Title(m_input.title)
		|| !senp::effect::ValidateDocument(probe)) throw std::invalid_argument("Invalid SENP readonly document input.");
}
bool SenpReadonlyDocument::Matches(const senp::effect::OperationContext& context) const noexcept
{
	return m_pending && context.ownerGeneration == m_pending->ownerGeneration
		&& context.workspaceRevision == m_pending->workspaceRevision && context.accountGeneration == m_pending->accountGeneration
		&& context.requestGeneration == m_pending->requestGeneration;
}
SenpDocumentTransition SenpReadonlyDocument::Finish(SenpDocumentState state, SenpDocumentResult result) noexcept
{
	auto retired = std::exchange(m_pending, std::nullopt);
	if (m_generation == UINT64_MAX) { state = SenpDocumentState::Closed; result = SenpDocumentResult::Closed; }
	else ++m_generation;
	m_state = state;
	if (state == SenpDocumentState::Closed || state == SenpDocumentState::Expired) m_content.reset();
	return { result, std::move(retired) };
}
SenpDocumentTransition SenpReadonlyDocument::Begin(senp::effect::OperationContext context)
{
	if (m_state == SenpDocumentState::Closed || m_state == SenpDocumentState::Expired) return { SenpDocumentResult::Closed, {} };
	const auto& scope = m_input.scope;
	if (context.ownerGeneration != scope.ownerGeneration || context.workspaceRevision != scope.workspaceRevision
		|| context.accountGeneration != scope.accountGeneration || context.requestGeneration <= m_lastRequest)
		return { SenpDocumentResult::Stale, {} };
	if (!senp::effect::Validate({ 2, 1, 1, senp::effect::EventMessage{ context, senp::effect::DocumentRequest{ m_input.resourceId } } })) return {};
	auto transition = Finish(SenpDocumentState::Loading, SenpDocumentResult::Accepted);
	if (transition.result != SenpDocumentResult::Accepted) return transition;
	m_lastRequest = context.requestGeneration; m_pending = std::move(context);
	return transition;
}
SenpDocumentTransition SenpReadonlyDocument::Apply(const senp::effect::OperationContext& context, senp::effect::PublishDocument document)
{
	if (!Matches(context)) return { SenpDocumentResult::Stale, {} };
	if (document.resourceId != m_input.resourceId || !Title(document.title) || !senp::effect::ValidateDocument(document))
		return Finish(SenpDocumentState::Failed, SenpDocumentResult::Invalid);
	if (document.revision < m_lastRevision) return Finish(SenpDocumentState::Failed, SenpDocumentResult::Stale);
	if (document.revision == m_lastRevision && (!m_content || *m_content != document))
		return Finish(SenpDocumentState::Failed, SenpDocumentResult::Invalid);
	auto content = std::make_shared<const senp::effect::PublishDocument>(std::move(document));
	m_lastRevision = content->revision; m_content = std::move(content);
	return Finish(SenpDocumentState::Ready, SenpDocumentResult::Accepted);
}
SenpDocumentTransition SenpReadonlyDocument::Fail(const senp::effect::OperationContext& context)
{
	if (!Matches(context)) return { SenpDocumentResult::Stale, {} };
	return Finish(SenpDocumentState::Failed, SenpDocumentResult::Accepted);
}
SenpDocumentTransition SenpReadonlyDocument::Expire() noexcept
{
	if (m_state == SenpDocumentState::Closed || m_state == SenpDocumentState::Expired) return { SenpDocumentResult::Closed, {} };
	return Finish(SenpDocumentState::Expired, SenpDocumentResult::Accepted);
}
SenpDocumentTransition SenpReadonlyDocument::Close() noexcept
{
	if (m_state == SenpDocumentState::Closed) return { SenpDocumentResult::Closed, {} };
	return Finish(SenpDocumentState::Closed, SenpDocumentResult::Accepted);
}

} // namespace workbench::editor
