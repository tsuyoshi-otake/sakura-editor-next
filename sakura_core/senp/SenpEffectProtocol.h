/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace senp::effect {

inline constexpr std::size_t kMaximumFrameBytes = 1024U * 1024U;
//! Largest body one tool completion may carry, counted in UTF-8 bytes. A
//! completion carries a whole page of an answer, so it is the one field that is
//! larger than the identifiers and messages beside it. The frame it travels in
//! is still kMaximumFrameBytes, and escaping a page of text doubles it at
//! worst, so a quarter of a frame is what one completion is allowed to be.
inline constexpr std::size_t kMaximumToolDataBytes = 256U * 1024U;
inline constexpr std::size_t kMaximumEffects = 64;
inline constexpr std::size_t kMaximumItems = 256;
inline constexpr std::int64_t kMaximumCounter = INT64_MAX;
inline constexpr std::wstring_view kAbi = L"sakura:senp/extension@3.0.0";

enum class CollapsibleState : std::uint8_t { Leaf, Collapsed, Expanded };
enum class CompletionStatus : std::uint8_t { Succeeded, Cancelled, Failed, TimedOut, HostUnavailable };
enum class PageStatus : std::uint8_t { Complete, Partial, Empty, Failed };
enum class TextStatus : std::uint8_t { Loading, Complete, Partial, Expired, Failed };
enum class StopReason : std::uint8_t { Disabled, Updated, Shutdown, ProtocolError, HostUnavailable };
enum class RejectionCode : std::uint8_t { Busy, Conflict, Stale, InvalidRequest, Unsupported, HostUnavailable };

struct OperationContext final {
	std::wstring operationId{};
	std::int64_t ownerGeneration{};
	std::int64_t workspaceRevision{};
	std::int64_t accountGeneration{};
	std::int64_t requestGeneration{};

private:
	// The defaulted comparison is compiler-synthesized machinery, not a data
	// member; declaring it as a hidden friend after a private section (rather
	// than as the struct's last public line) keeps it found by ADL for `==`
	// while narrowing the type's public member surface to its data fields.
	[[nodiscard]] friend bool operator==(const OperationContext&, const OperationContext&) = default;
};

struct Field final {
	std::wstring name{};
	std::wstring value{};

private:
	// The defaulted comparison is compiler-synthesized machinery, not a data
	// member; declaring it as a hidden friend after a private section (rather
	// than as the struct's last public line) keeps it found by ADL for `==`
	// while narrowing the type's public member surface to its data fields.
	[[nodiscard]] friend bool operator==(const Field&, const Field&) = default;
};

struct Remote final {
	std::wstring name{};
	std::wstring url{};

private:
	// The defaulted comparison is compiler-synthesized machinery, not a data
	// member; declaring it as a hidden friend after a private section (rather
	// than as the struct's last public line) keeps it found by ADL for `==`
	// while narrowing the type's public member surface to its data fields.
	[[nodiscard]] friend bool operator==(const Remote&, const Remote&) = default;
};

struct Repository final {
	std::wstring rootId{};
	std::wstring branch{};
	std::vector<Remote> remotes{};
	// Commits HEAD is ahead of its upstream (WIT u32); last so that aggregate
	// initializers written before it keep their meaning.
	std::int64_t ahead{};

private:
	// The defaulted comparison is compiler-synthesized machinery, not a data
	// member; declaring it as a hidden friend after a private section (rather
	// than as the struct's last public line) keeps it found by ADL for `==`
	// while narrowing the type's public member surface to its data fields.
	[[nodiscard]] friend bool operator==(const Repository&, const Repository&) = default;
};

struct TreeItem final {
	std::wstring id{};
	std::wstring label{};
	std::wstring description{};
	std::wstring tooltip{};
	std::wstring icon{};
	CollapsibleState collapsibleState{};
	std::wstring commandId{};
	std::vector<std::wstring> arguments{};
	// VS Code's TreeItem.contextValue, read by `viewItem` menu conditions.
	// Declared last so aggregate initializers written before it keep their meaning.
	std::wstring contextValue{};

private:
	// The defaulted comparison is compiler-synthesized machinery, not a data
	// member; declaring it as a hidden friend after a private section (rather
	// than as the struct's last public line) keeps it found by ADL for `==`
	// while narrowing the type's public member surface to its data fields.
	[[nodiscard]] friend bool operator==(const TreeItem&, const TreeItem&) = default;
};

struct TreeRequest final {
	std::wstring viewId{};
	std::wstring parentId{};
	std::wstring cursor{};

private:
	// The defaulted comparison is compiler-synthesized machinery, not a data
	// member; declaring it as a hidden friend after a private section (rather
	// than as the struct's last public line) keeps it found by ADL for `==`
	// while narrowing the type's public member surface to its data fields.
	[[nodiscard]] friend bool operator==(const TreeRequest&, const TreeRequest&) = default;
};

struct DocumentRequest final {
	std::wstring resourceId{};

private:
	// The defaulted comparison is compiler-synthesized machinery, not a data
	// member; declaring it as a hidden friend after a private section (rather
	// than as the struct's last public line) keeps it found by ADL for `==`
	// while narrowing the type's public member surface to its data fields.
	[[nodiscard]] friend bool operator==(const DocumentRequest&, const DocumentRequest&) = default;
};

struct CommandInvoked final {
	std::wstring commandId{};
	std::vector<std::wstring> arguments{};

private:
	// The defaulted comparison is compiler-synthesized machinery, not a data
	// member; declaring it as a hidden friend after a private section (rather
	// than as the struct's last public line) keeps it found by ADL for `==`
	// while narrowing the type's public member surface to its data fields.
	[[nodiscard]] friend bool operator==(const CommandInvoked&, const CommandInvoked&) = default;
};

struct ToolCompleted final {
	std::wstring readId{};
	CompletionStatus status{};
	std::wstring data{};
	std::wstring message{};

private:
	// The defaulted comparison is compiler-synthesized machinery, not a data
	// member; declaring it as a hidden friend after a private section (rather
	// than as the struct's last public line) keeps it found by ADL for `==`
	// while narrowing the type's public member surface to its data fields.
	[[nodiscard]] friend bool operator==(const ToolCompleted&, const ToolCompleted&) = default;
};

struct WorkspaceChanged final {
	std::vector<Repository> repositories{};

private:
	// The defaulted comparison is compiler-synthesized machinery, not a data
	// member; declaring it as a hidden friend after a private section (rather
	// than as the struct's last public line) keeps it found by ADL for `==`
	// while narrowing the type's public member surface to its data fields.
	[[nodiscard]] friend bool operator==(const WorkspaceChanged&, const WorkspaceChanged&) = default;
};

struct Cancel final {
	std::wstring operationId{};

private:
	// The defaulted comparison is compiler-synthesized machinery, not a data
	// member; declaring it as a hidden friend after a private section (rather
	// than as the struct's last public line) keeps it found by ADL for `==`
	// while narrowing the type's public member surface to its data fields.
	[[nodiscard]] friend bool operator==(const Cancel&, const Cancel&) = default;
};

struct VisibilityChanged final {
	std::wstring viewId{};
	bool visible{};

private:
	// The defaulted comparison is compiler-synthesized machinery, not a data
	// member; declaring it as a hidden friend after a private section (rather
	// than as the struct's last public line) keeps it found by ADL for `==`
	// while narrowing the type's public member surface to its data fields.
	[[nodiscard]] friend bool operator==(const VisibilityChanged&, const VisibilityChanged&) = default;
};

struct StartToolRead final {
	std::wstring readId{};
	std::wstring toolId{};
	std::wstring operation{};
	std::vector<Field> arguments{};

private:
	// The defaulted comparison is compiler-synthesized machinery, not a data
	// member; declaring it as a hidden friend after a private section (rather
	// than as the struct's last public line) keeps it found by ADL for `==`
	// while narrowing the type's public member surface to its data fields.
	[[nodiscard]] friend bool operator==(const StartToolRead&, const StartToolRead&) = default;
};

struct PublishTreePage final {
	std::wstring viewId{};
	std::wstring parentId{};
	std::vector<TreeItem> items{};
	std::wstring nextCursor{};
	std::int64_t revision{};
	PageStatus status{};
	std::wstring message{};

private:
	// The defaulted comparison is compiler-synthesized machinery, not a data
	// member; declaring it as a hidden friend after a private section (rather
	// than as the struct's last public line) keeps it found by ADL for `==`
	// while narrowing the type's public member surface to its data fields.
	[[nodiscard]] friend bool operator==(const PublishTreePage&, const PublishTreePage&) = default;
};

struct MarkdownSection final {
	std::wstring text{};

private:
	// The defaulted comparison is compiler-synthesized machinery, not a data
	// member; declaring it as a hidden friend after a private section (rather
	// than as the struct's last public line) keeps it found by ADL for `==`
	// while narrowing the type's public member surface to its data fields.
	[[nodiscard]] friend bool operator==(const MarkdownSection&, const MarkdownSection&) = default;
};

struct MetadataSection final {
	std::vector<Field> fields{};

private:
	// The defaulted comparison is compiler-synthesized machinery, not a data
	// member; declaring it as a hidden friend after a private section (rather
	// than as the struct's last public line) keeps it found by ADL for `==`
	// while narrowing the type's public member surface to its data fields.
	[[nodiscard]] friend bool operator==(const MetadataSection&, const MetadataSection&) = default;
};

struct TableRow final {
	std::vector<std::wstring> cells{};

private:
	// The defaulted comparison is compiler-synthesized machinery, not a data
	// member; declaring it as a hidden friend after a private section (rather
	// than as the struct's last public line) keeps it found by ADL for `==`
	// while narrowing the type's public member surface to its data fields.
	[[nodiscard]] friend bool operator==(const TableRow&, const TableRow&) = default;
};

struct TableSection final {
	std::vector<std::wstring> columns{};
	std::vector<TableRow> rows{};

private:
	// The defaulted comparison is compiler-synthesized machinery, not a data
	// member; declaring it as a hidden friend after a private section (rather
	// than as the struct's last public line) keeps it found by ADL for `==`
	// while narrowing the type's public member surface to its data fields.
	[[nodiscard]] friend bool operator==(const TableSection&, const TableSection&) = default;
};

struct TextResourceSection final {
	std::wstring handle{};
	std::int64_t length{};
	TextStatus status{};

private:
	// The defaulted comparison is compiler-synthesized machinery, not a data
	// member; declaring it as a hidden friend after a private section (rather
	// than as the struct's last public line) keeps it found by ADL for `==`
	// while narrowing the type's public member surface to its data fields.
	[[nodiscard]] friend bool operator==(const TextResourceSection&, const TextResourceSection&) = default;
};

struct CompleteCommand final {
	CompletionStatus status{};
	std::wstring message{};

private:
	// The defaulted comparison is compiler-synthesized machinery, not a data
	// member; declaring it as a hidden friend after a private section (rather
	// than as the struct's last public line) keeps it found by ADL for `==`
	// while narrowing the type's public member surface to its data fields.
	[[nodiscard]] friend bool operator==(const CompleteCommand&, const CompleteCommand&) = default;
};

struct InvalidateTree final {
	std::wstring viewId{};

private:
	// The defaulted comparison is compiler-synthesized machinery, not a data
	// member; declaring it as a hidden friend after a private section (rather
	// than as the struct's last public line) keeps it found by ADL for `==`
	// while narrowing the type's public member surface to its data fields.
	[[nodiscard]] friend bool operator==(const InvalidateTree&, const InvalidateTree&) = default;
};

struct OpenDocument final {
	std::wstring resourceId{};

private:
	// The defaulted comparison is compiler-synthesized machinery, not a data
	// member; declaring it as a hidden friend after a private section (rather
	// than as the struct's last public line) keeps it found by ADL for `==`
	// while narrowing the type's public member surface to its data fields.
	[[nodiscard]] friend bool operator==(const OpenDocument&, const OpenDocument&) = default;
};

struct ReleaseResource final {
	std::wstring handle{};

private:
	// The defaulted comparison is compiler-synthesized machinery, not a data
	// member; declaring it as a hidden friend after a private section (rather
	// than as the struct's last public line) keeps it found by ADL for `==`
	// while narrowing the type's public member surface to its data fields.
	[[nodiscard]] friend bool operator==(const ReleaseResource&, const ReleaseResource&) = default;
};

struct Hello final {
	std::wstring abi{};

private:
	// The defaulted comparison is compiler-synthesized machinery, not a data
	// member; declaring it as a hidden friend after a private section (rather
	// than as the struct's last public line) keeps it found by ADL for `==`
	// while narrowing the type's public member surface to its data fields.
	[[nodiscard]] friend bool operator==(const Hello&, const Hello&) = default;
};

struct Activate final {
	OperationContext context{};
	std::wstring extensionId{};

private:
	// The defaulted comparison is compiler-synthesized machinery, not a data
	// member; declaring it as a hidden friend after a private section (rather
	// than as the struct's last public line) keeps it found by ADL for `==`
	// while narrowing the type's public member surface to its data fields.
	[[nodiscard]] friend bool operator==(const Activate&, const Activate&) = default;
};

struct Deactivate final {
	StopReason reason{};

private:
	// The defaulted comparison is compiler-synthesized machinery, not a data
	// member; declaring it as a hidden friend after a private section (rather
	// than as the struct's last public line) keeps it found by ADL for `==`
	// while narrowing the type's public member surface to its data fields.
	[[nodiscard]] friend bool operator==(const Deactivate&, const Deactivate&) = default;
};

struct Ack final {
	std::int64_t ackSequence{};

private:
	// The defaulted comparison is compiler-synthesized machinery, not a data
	// member; declaring it as a hidden friend after a private section (rather
	// than as the struct's last public line) keeps it found by ADL for `==`
	// while narrowing the type's public member surface to its data fields.
	[[nodiscard]] friend bool operator==(const Ack&, const Ack&) = default;
};

struct Rejected final {
	std::int64_t requestSequence{};
	RejectionCode code{};

private:
	// The defaulted comparison is compiler-synthesized machinery, not a data
	// member; declaring it as a hidden friend after a private section (rather
	// than as the struct's last public line) keeps it found by ADL for `==`
	// while narrowing the type's public member surface to its data fields.
	[[nodiscard]] friend bool operator==(const Rejected&, const Rejected&) = default;
};

struct Stopped final {
	StopReason reason{};

private:
	// The defaulted comparison is compiler-synthesized machinery, not a data
	// member; declaring it as a hidden friend after a private section (rather
	// than as the struct's last public line) keeps it found by ADL for `==`
	// while narrowing the type's public member surface to its data fields.
	[[nodiscard]] friend bool operator==(const Stopped&, const Stopped&) = default;
};

using DocumentSection = std::variant<MarkdownSection, MetadataSection, TableSection, TextResourceSection>;

using Event = std::variant<TreeRequest, DocumentRequest, CommandInvoked, ToolCompleted, WorkspaceChanged, Cancel, VisibilityChanged>;

struct PublishDocument final {
	std::wstring resourceId{};
	std::wstring title{};
	std::int64_t revision{};
	std::vector<DocumentSection> sections{};

private:
	// The defaulted comparison is compiler-synthesized machinery, not a data
	// member; declaring it as a hidden friend after a private section (rather
	// than as the struct's last public line) keeps it found by ADL for `==`
	// while narrowing the type's public member surface to its data fields.
	[[nodiscard]] friend bool operator==(const PublishDocument&, const PublishDocument&) = default;
};

struct EventMessage final {
	OperationContext context{};
	Event event{};

private:
	// The defaulted comparison is compiler-synthesized machinery, not a data
	// member; declaring it as a hidden friend after a private section (rather
	// than as the struct's last public line) keeps it found by ADL for `==`
	// while narrowing the type's public member surface to its data fields.
	[[nodiscard]] friend bool operator==(const EventMessage&, const EventMessage&) = default;
};

using Effect = std::variant<StartToolRead, PublishTreePage, PublishDocument, CompleteCommand, InvalidateTree, OpenDocument, ReleaseResource>;

struct EffectsMessage final {
	OperationContext context{};
	std::vector<Effect> effects{};

private:
	// The defaulted comparison is compiler-synthesized machinery, not a data
	// member; declaring it as a hidden friend after a private section (rather
	// than as the struct's last public line) keeps it found by ADL for `==`
	// while narrowing the type's public member surface to its data fields.
	[[nodiscard]] friend bool operator==(const EffectsMessage&, const EffectsMessage&) = default;
};

using Message = std::variant<Hello, Activate, EventMessage, EffectsMessage, Deactivate, Ack, Rejected, Stopped>;

struct Envelope final {
	std::int64_t protocol{};
	std::int64_t sequence{};
	std::int64_t sessionGeneration{};
	Message body{};

private:
	// The defaulted comparison is compiler-synthesized machinery, not a data
	// member; declaring it as a hidden friend after a private section (rather
	// than as the struct's last public line) keeps it found by ADL for `==`
	// while narrowing the type's public member surface to its data fields.
	[[nodiscard]] friend bool operator==(const Envelope&, const Envelope&) = default;
};

//! A failed decode/encode publishes no partially validated envelope.
[[nodiscard]] std::optional<Envelope> Decode(std::string_view json);
[[nodiscard]] std::optional<std::string> Encode(const Envelope& envelope);
[[nodiscard]] bool Validate(const Envelope& envelope);
//! Validate an already-decoded document without copying its effect graph.
[[nodiscard]] bool ValidateDocument(const PublishDocument& document);
//! Validate a workspace payload before anything submits it as an event. The
//! window builds one out of live workspace and Source Control state, and a value
//! the wire would refuse has to be caught where it is built: an admission that
//! fails at the request broker is indistinguishable from the owner going away,
//! so a host-side mistake would read as a runtime fault.
[[nodiscard]] bool ValidateWorkspace(const WorkspaceChanged& workspace);

} // namespace senp::effect
