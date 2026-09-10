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
inline constexpr std::wstring_view kAbi = L"sakura:senp/extension@2.0.0";

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
	bool operator==(const OperationContext&) const = default;
};

struct Field final {
	std::wstring name{};
	std::wstring value{};
	bool operator==(const Field&) const = default;
};

struct Remote final {
	std::wstring name{};
	std::wstring url{};
	bool operator==(const Remote&) const = default;
};

struct Repository final {
	std::wstring rootId{};
	std::wstring branch{};
	std::vector<Remote> remotes{};
	// Commits HEAD is ahead of its upstream (WIT u32); last so that aggregate
	// initializers written before it keep their meaning.
	std::int64_t ahead{};
	bool operator==(const Repository&) const = default;
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
	bool operator==(const TreeItem&) const = default;
};

struct TreeRequest final {
	std::wstring viewId{};
	std::wstring parentId{};
	std::wstring cursor{};
	bool operator==(const TreeRequest&) const = default;
};

struct DocumentRequest final {
	std::wstring resourceId{};
	bool operator==(const DocumentRequest&) const = default;
};

struct CommandInvoked final {
	std::wstring commandId{};
	std::vector<std::wstring> arguments{};
	bool operator==(const CommandInvoked&) const = default;
};

struct ToolCompleted final {
	std::wstring readId{};
	CompletionStatus status{};
	std::wstring data{};
	std::wstring message{};
	bool operator==(const ToolCompleted&) const = default;
};

struct WorkspaceChanged final {
	std::vector<Repository> repositories{};
	bool operator==(const WorkspaceChanged&) const = default;
};

struct Cancel final {
	std::wstring operationId{};
	bool operator==(const Cancel&) const = default;
};

struct VisibilityChanged final {
	std::wstring viewId{};
	bool visible{};
	bool operator==(const VisibilityChanged&) const = default;
};

struct StartToolRead final {
	std::wstring readId{};
	std::wstring toolId{};
	std::wstring operation{};
	std::vector<Field> arguments{};
	bool operator==(const StartToolRead&) const = default;
};

struct PublishTreePage final {
	std::wstring viewId{};
	std::wstring parentId{};
	std::vector<TreeItem> items{};
	std::wstring nextCursor{};
	std::int64_t revision{};
	PageStatus status{};
	std::wstring message{};
	bool operator==(const PublishTreePage&) const = default;
};

struct MarkdownSection final {
	std::wstring text{};
	bool operator==(const MarkdownSection&) const = default;
};

struct MetadataSection final {
	std::vector<Field> fields{};
	bool operator==(const MetadataSection&) const = default;
};

struct TableRow final {
	std::vector<std::wstring> cells{};
	bool operator==(const TableRow&) const = default;
};

struct TableSection final {
	std::vector<std::wstring> columns{};
	std::vector<TableRow> rows{};
	bool operator==(const TableSection&) const = default;
};

struct TextResourceSection final {
	std::wstring handle{};
	std::int64_t length{};
	TextStatus status{};
	bool operator==(const TextResourceSection&) const = default;
};

struct CompleteCommand final {
	CompletionStatus status{};
	std::wstring message{};
	bool operator==(const CompleteCommand&) const = default;
};

struct InvalidateTree final {
	std::wstring viewId{};
	bool operator==(const InvalidateTree&) const = default;
};

struct OpenDocument final {
	std::wstring resourceId{};
	bool operator==(const OpenDocument&) const = default;
};

struct ReleaseResource final {
	std::wstring handle{};
	bool operator==(const ReleaseResource&) const = default;
};

struct Hello final {
	std::wstring abi{};
	bool operator==(const Hello&) const = default;
};

struct Activate final {
	OperationContext context{};
	std::wstring extensionId{};
	bool operator==(const Activate&) const = default;
};

struct Deactivate final {
	StopReason reason{};
	bool operator==(const Deactivate&) const = default;
};

struct Ack final {
	std::int64_t ackSequence{};
	bool operator==(const Ack&) const = default;
};

struct Rejected final {
	std::int64_t requestSequence{};
	RejectionCode code{};
	bool operator==(const Rejected&) const = default;
};

struct Stopped final {
	StopReason reason{};
	bool operator==(const Stopped&) const = default;
};

using DocumentSection = std::variant<MarkdownSection, MetadataSection, TableSection, TextResourceSection>;

using Event = std::variant<TreeRequest, DocumentRequest, CommandInvoked, ToolCompleted, WorkspaceChanged, Cancel, VisibilityChanged>;

struct PublishDocument final {
	std::wstring resourceId{};
	std::wstring title{};
	std::int64_t revision{};
	std::vector<DocumentSection> sections{};
	bool operator==(const PublishDocument&) const = default;
};

struct EventMessage final {
	OperationContext context{};
	Event event{};
	bool operator==(const EventMessage&) const = default;
};

using Effect = std::variant<StartToolRead, PublishTreePage, PublishDocument, CompleteCommand, InvalidateTree, OpenDocument, ReleaseResource>;

struct EffectsMessage final {
	OperationContext context{};
	std::vector<Effect> effects{};
	bool operator==(const EffectsMessage&) const = default;
};

using Message = std::variant<Hello, Activate, EventMessage, EffectsMessage, Deactivate, Ack, Rejected, Stopped>;

struct Envelope final {
	std::int64_t protocol{};
	std::int64_t sequence{};
	std::int64_t sessionGeneration{};
	Message body{};
	bool operator==(const Envelope&) const = default;
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
