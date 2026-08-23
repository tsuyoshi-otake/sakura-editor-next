/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace markdown {

//! The block types rendered by the lightweight native Markdown preview.
enum class BlockKind {
	Heading,
	Paragraph,
	BulletListItem,
	OrderedListItem,
	BlockQuote,
	CodeBlock,
	HorizontalRule,
	Image,
	Table,
	FrontMatter,
	Math,
	MermaidDiagram,
};

//! Inline decoration kinds projected by the native renderer.
enum class InlineKind {
	Emphasis,
	Strong,
	Code,
	Link,
	Image,
	Strikethrough,
	Autolink,
	Math,
};

enum class TaskListState {
	NotTask,
	Unchecked,
	Checked,
};

//! Why native preview is showing inert source instead of pretending to execute
//! a capability that is not available in the process.
enum class NativeFallbackKind {
	None,
	LiteralSource,
	UnsupportedSyntax,
	LimitExceeded,
};

enum class FrontMatterMode {
	Hide,
	Table,
	CodeBlock,
};

struct FrontMatterField {
	std::wstring name;
	std::wstring value;
};

enum class TableAlignment {
	Default,
	Left,
	Center,
	Right,
};

enum class ResourceUse {
	Link,
	Image,
};

//! The result of resolving a Markdown resource without performing I/O.
enum class ResourceDisposition {
	ResolvedLocal,
	Fragment,
	ExternalBlocked,
	UnsafeSchemeBlocked,
	OutsideAllowedRoots,
	LimitExceeded,
	Invalid,
};

struct ResourceReference {
	ResourceUse use = ResourceUse::Link;
	ResourceDisposition disposition = ResourceDisposition::Invalid;
	std::wstring original;
	//! Absolute local path only when disposition is ResolvedLocal.
	std::wstring resolvedPath;
	//! The root the native image loader must verify again after following reparse points.
	std::wstring allowedRoot;
};

struct InlineSpan {
	InlineKind kind = InlineKind::Link;
	std::size_t start = 0;
	std::size_t length = 0;
	std::optional<ResourceReference> resource;
};

struct TableCell {
	std::wstring text;
	std::vector<InlineSpan> inlineSpans;
};

struct TableRow {
	bool header = false;
	std::vector<TableCell> cells;
};

struct ImageNode {
	std::wstring altText;
	ResourceReference source;
};

struct Block {
	BlockKind kind = BlockKind::Paragraph;
	//! Heading depth for headings, indentation depth for list items.
	int level = 0;
	//! A rendered list marker, empty for non-list blocks.
	std::wstring marker;
	//! Plain text after inline Markdown has been normalized for display.
	std::wstring text;
	//! Ranges in text that receive an inline visual treatment.
	std::vector<InlineSpan> inlineSpans;
	std::vector<TableAlignment> tableAlignments;
	std::vector<TableRow> tableRows;
	std::optional<ImageNode> image;
	//! Normalized first word of a fenced-code info string.
	std::wstring language;
	TaskListState taskListState = TaskListState::NotTask;
	NativeFallbackKind fallbackKind = NativeFallbackKind::None;
	FrontMatterMode frontMatterMode = FrontMatterMode::Table;
	std::vector<FrontMatterField> frontMatterFields;
	std::size_t sourceLine = 0;
};

enum class CapabilityStatus {
	Supported,
	Unsupported,
};

//! Explicit native-preview boundaries. Unsupported features must never be inferred
//! from visual similarity with a full semantic renderer.
struct PreviewCapabilities {
	CapabilityStatus localImageProjection = CapabilityStatus::Supported;
	CapabilityStatus linkActivation = CapabilityStatus::Unsupported;
	//! Scroll sync is two independent directions upstream, named after the
	//! settings that gate them: markdown.preview.scrollPreviewWithEditor moves
	//! the preview when the editor scrolls, and
	//! markdown.preview.scrollEditorWithPreview moves the editor when the
	//! preview scrolls. One combined flag could not state that one direction
	//! works while the other does not, so the boundary is declared per axis.
	CapabilityStatus scrollPreviewWithEditor = CapabilityStatus::Supported;
	CapabilityStatus scrollEditorWithPreview = CapabilityStatus::Supported;
	CapabilityStatus rawHtmlExecution = CapabilityStatus::Unsupported;
	CapabilityStatus mathTypesetting = CapabilityStatus::Unsupported;
	/*!
		@brief Native flowchart drawing

		Split from the other diagram families because they have genuinely
		different outcomes: a `graph`/`flowchart` block inside the supported
		subset is laid out and drawn, while every other family stays literal. A
		single flag could not say that.
	*/
	CapabilityStatus mermaidFlowchartRendering = CapabilityStatus::Supported;
	//! `sequenceDiagram`, `classDiagram`, `stateDiagram`, `gantt`, and the rest.
	CapabilityStatus mermaidNonFlowchartRendering = CapabilityStatus::Unsupported;
};

enum class ParseCompletion {
	Complete,
	InputLimitReached,
	BlockLimitReached,
};

struct Document {
	std::vector<Block> blocks;
	PreviewCapabilities capabilities;
	ParseCompletion completion = ParseCompletion::Complete;
};

struct ParseLimits {
	std::size_t maximumInputCharacters = 16U * 1024U * 1024U;
	std::size_t maximumBlocks = 1U * 1024U * 1024U;
	std::size_t maximumHtmlDepth = 64;
	std::size_t maximumInlineDepth = 32;
	std::size_t maximumImages = 1024;
	std::size_t maximumFrontMatterLines = 1024;
	std::size_t maximumFrontMatterFields = 512;
};

struct ParseOptions {
	//! Absolute path of the Markdown document, when one exists.
	std::wstring documentPath;
	//! Semantic workspace folder. Root-relative resources resolve here; an empty
	//! value makes the document directory the only local-resource root.
	std::wstring workspaceRoot;
	FrontMatterMode frontMatterMode = FrontMatterMode::Table;
	ParseLimits limits;
};

//! Parses Markdown into a safe, window-independent render model. Raw HTML is
//! interpreted only as a bounded set of structural and semantic wrappers and
//! is never executed.
[[nodiscard]] Document ParseMarkdown(std::wstring_view source, const ParseOptions& options = {});

enum class LiveUpdateAction {
	None,
	AwaitStableRevision,
	Render,
};

//! Pure debounce contract used by the existing edit timer. A changed revision
//! must be observed unchanged once before it is rendered.
class PreviewLiveUpdateModel final {
public:
	[[nodiscard]] LiveUpdateAction Observe(int revision) noexcept;
	void Commit(int revision) noexcept;
	void Reset() noexcept;

	[[nodiscard]] int RenderedRevision() const noexcept { return m_renderedRevision; }
	[[nodiscard]] bool HasPendingUpdate() const noexcept { return m_pending; }

private:
	int m_renderedRevision = -1;
	int m_observedRevision = -1;
	bool m_pending = false;
};

} // namespace markdown
