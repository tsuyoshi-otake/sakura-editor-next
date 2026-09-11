/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "senp/SenpEffectProtocol.h"
#include <sakura/serialization/JsoncDocument.h>
#include <algorithm>
#include <array>
#include <initializer_list>
#include <set>
#include <type_traits>

namespace senp::effect {
namespace {
using Json = platform::serialization::JsoncValue;

template<class T> struct NamedMember { std::wstring_view name; T& value; };
template<class T> NamedMember<T> Member(std::wstring_view name, T& value) { return { name, value }; }
template<class T> struct IsVector : std::false_type {};
template<class T> struct IsVector<std::vector<T>> : std::true_type {};

bool Text(std::wstring_view text, std::size_t maximum)
{
	std::size_t bytes = 0;
	for (std::size_t index = 0; index < text.size(); ++index) {
		const auto unit = static_cast<std::uint32_t>(text[index]);
		if (unit > 0xffff || (unit >= 0xdc00 && unit <= 0xdfff)) return false;
		if (unit >= 0xd800 && unit <= 0xdbff) {
			if (++index == text.size() || text[index] < 0xdc00 || text[index] > 0xdfff) return false;
			bytes += 4;
		} else bytes += unit < 0x80 ? 1 : unit < 0x800 ? 2 : 3;
		if (bytes > maximum) return false;
	}
	return true;
}

bool Id(std::wstring_view value, bool empty = false)
{
	return (empty || !value.empty()) && value.size() <= 256 &&
		std::all_of(value.begin(), value.end(), [](wchar_t c) {
			return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
				(c >= L'0' && c <= L'9') || c == L'.' || c == L'-' || c == L'_' || c == L':' || c == L'/';
		});
}

template<class T> bool Rules(const T&) { return true; }
bool Rules(const OperationContext& v) { return Id(v.operationId) && v.operationId.size() <= 96 && v.ownerGeneration > 0; }
bool Rules(const Field& v) { return Text(v.name, 1024) && Text(v.value, 4096); }
bool Rules(const Remote& v) { return Text(v.name, 256) && Text(v.url, 4096); }
bool Rules(const Repository& v) { return Id(v.rootId) && Text(v.branch, 1024) && v.ahead <= 0xFFFFFFFFLL && v.remotes.size() <= 16; }
bool Rules(const TreeItem& v) { return Id(v.id) && Text(v.label, 1024) && Text(v.description, 1024) && Text(v.tooltip, 4096) && Id(v.icon, true) && Id(v.commandId, true) && v.arguments.size() <= 16 && std::all_of(v.arguments.begin(), v.arguments.end(), [](const auto& x) { return Text(x, 4096); }) && Text(v.contextValue, 1024); }
bool Rules(const TreeRequest& v) { return Id(v.viewId) && Id(v.parentId, true) && Text(v.cursor, 2048); }
bool Rules(const DocumentRequest& v) { return Id(v.resourceId); }
bool Rules(const CommandInvoked& v) { return Id(v.commandId) && v.arguments.size() <= 16 && std::all_of(v.arguments.begin(), v.arguments.end(), [](const auto& x) { return Text(x, 4096); }); }
bool Rules(const ToolCompleted& v) { return Id(v.readId) && Text(v.data, kMaximumToolDataBytes) && Text(v.message, 4096) && (v.status == CompletionStatus::Succeeded || v.data.empty()); }
bool Rules(const WorkspaceChanged& v) { std::set<std::wstring> ids; return v.repositories.size() <= 32 && std::all_of(v.repositories.begin(), v.repositories.end(), [&](const auto& x) { return ids.insert(x.rootId).second; }); }
bool Rules(const Cancel& v) { return Id(v.operationId) && v.operationId.size() <= 96; }
bool Rules(const VisibilityChanged& v) { return Id(v.viewId); }
bool Rules(const StartToolRead& v) { std::set<std::wstring> names; return Id(v.readId) && Id(v.toolId) && Id(v.operation) && v.arguments.size() <= 16 && std::all_of(v.arguments.begin(), v.arguments.end(), [&](const auto& x) { return Id(x.name) && names.insert(x.name).second; }); }
bool Rules(const PublishTreePage& v)
{
	std::set<std::wstring> ids;
	if (!Id(v.viewId) || !Id(v.parentId, true) || !Text(v.nextCursor, 2048) || !Text(v.message, 4096)) return false;
	if ((v.status == PageStatus::Partial) != !v.nextCursor.empty()) return false;
	if ((v.status == PageStatus::Empty || v.status == PageStatus::Failed) && !v.items.empty()) return false;
	return std::all_of(v.items.begin(), v.items.end(), [&](const auto& x) { return x.id != v.parentId && ids.insert(x.id).second; });
}
bool Rules(const MetadataSection& v) { return v.fields.size() <= 64; }
bool Rules(const TableRow& v) { return v.cells.size() <= 16 && std::all_of(v.cells.begin(), v.cells.end(), [](const auto& x) { return Text(x, 4096); }); }
bool Rules(const TableSection& v) { return !v.columns.empty() && v.columns.size() <= 16 && std::all_of(v.columns.begin(), v.columns.end(), [](const auto& x) { return Text(x, 1024); }) && std::all_of(v.rows.begin(), v.rows.end(), [&](const auto& x) { return x.cells.size() == v.columns.size(); }); }
bool Rules(const TextResourceSection& v) { return Id(v.handle) && v.length <= 32 * 1024 * 1024; }
bool Rules(const PublishDocument& v) { return Id(v.resourceId) && Text(v.title, 1024) && v.sections.size() <= 32; }
bool Rules(const CompleteCommand& v) { return Text(v.message, 4096); }
bool Rules(const InvalidateTree& v) { return Id(v.viewId); }
bool Rules(const OpenDocument& v) { return Id(v.resourceId); }
bool Rules(const ReleaseResource& v) { return Id(v.handle); }
bool Rules(const Hello& v) { return v.abi == kAbi; }
bool Rules(const Activate& v) { return Id(v.extensionId); }
bool Rules(const EffectsMessage& v)
{
	std::set<std::wstring> reads;
	std::size_t completions = 0;
	if (v.effects.size() > kMaximumEffects) return false;
	for (const auto& effect : v.effects) {
		if (const auto* read = std::get_if<StartToolRead>(&effect); read && !reads.insert(read->readId).second) return false;
		if (std::holds_alternative<CompleteCommand>(effect) && ++completions > 1) return false;
	}
	return true;
}
bool Rules(const Ack& v) { return v.ackSequence > 0; }
bool Rules(const Rejected& v) { return v.requestSequence > 0; }
bool Rules(const Envelope& v) { return v.protocol == 2 && v.sequence > 0 && v.sessionGeneration > 0; }

// Whether Codec is the reading direction (Reader) or writing direction
// (Writer). This is a free variable template rather than a `static constexpr`
// member of Reader/Writer: an indented `static constexpr bool kReading = ...;`
// class-member line is misidentified as a public mutable field by this
// project's semantic scanner (its exclusion regex backtracks past the leading
// `static` keyword on any indented line), and Reader/Writer's true visibility
// for this value is "known at compile time from the type", not "a private
// implementation detail" -- it is read from 43 Transfer signatures below and
// from outside both classes, so it cannot simply move into a private section.
// Namespace scope avoids the indentation the scanner's bug depends on.
template<class Codec> inline constexpr bool kIsReadingCodec = false;

// Transfer declarations precede the codecs so dependent overload resolution
// remains identical with MSVC and Clang. Each record lists every required key.
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, CollapsibleState&, const CollapsibleState&> value) {
	return codec.Enumeration(value, {{ L"leaf", CollapsibleState::Leaf }, { L"collapsed", CollapsibleState::Collapsed }, { L"expanded", CollapsibleState::Expanded }});
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, CompletionStatus&, const CompletionStatus&> value) {
	return codec.Enumeration(value, {{ L"succeeded", CompletionStatus::Succeeded }, { L"cancelled", CompletionStatus::Cancelled }, { L"failed", CompletionStatus::Failed }, { L"timedOut", CompletionStatus::TimedOut }, { L"hostUnavailable", CompletionStatus::HostUnavailable }});
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, PageStatus&, const PageStatus&> value) {
	return codec.Enumeration(value, {{ L"complete", PageStatus::Complete }, { L"partial", PageStatus::Partial }, { L"empty", PageStatus::Empty }, { L"failed", PageStatus::Failed }});
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, TextStatus&, const TextStatus&> value) {
	return codec.Enumeration(value, {{ L"loading", TextStatus::Loading }, { L"complete", TextStatus::Complete }, { L"partial", TextStatus::Partial }, { L"expired", TextStatus::Expired }, { L"failed", TextStatus::Failed }});
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, StopReason&, const StopReason&> value) {
	return codec.Enumeration(value, {{ L"disabled", StopReason::Disabled }, { L"updated", StopReason::Updated }, { L"shutdown", StopReason::Shutdown }, { L"protocolError", StopReason::ProtocolError }, { L"hostUnavailable", StopReason::HostUnavailable }});
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, RejectionCode&, const RejectionCode&> value) {
	return codec.Enumeration(value, {{ L"busy", RejectionCode::Busy }, { L"conflict", RejectionCode::Conflict }, { L"stale", RejectionCode::Stale }, { L"invalidRequest", RejectionCode::InvalidRequest }, { L"unsupported", RejectionCode::Unsupported }, { L"hostUnavailable", RejectionCode::HostUnavailable }});
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, OperationContext&, const OperationContext&> value) {
	return codec.Record(Member(L"operationId", value.operationId), Member(L"ownerGeneration", value.ownerGeneration), Member(L"workspaceRevision", value.workspaceRevision), Member(L"accountGeneration", value.accountGeneration), Member(L"requestGeneration", value.requestGeneration));
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, Field&, const Field&> value) {
	return codec.Record(Member(L"name", value.name), Member(L"value", value.value));
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, Remote&, const Remote&> value) {
	return codec.Record(Member(L"name", value.name), Member(L"url", value.url));
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, Repository&, const Repository&> value) {
	return codec.Record(Member(L"rootId", value.rootId), Member(L"branch", value.branch), Member(L"ahead", value.ahead), Member(L"remotes", value.remotes));
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, TreeItem&, const TreeItem&> value) {
	return codec.Record(Member(L"id", value.id), Member(L"label", value.label), Member(L"description", value.description), Member(L"tooltip", value.tooltip), Member(L"icon", value.icon), Member(L"collapsibleState", value.collapsibleState), Member(L"commandId", value.commandId), Member(L"arguments", value.arguments), Member(L"contextValue", value.contextValue));
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, TreeRequest&, const TreeRequest&> value) {
	return codec.Record(Member(L"viewId", value.viewId), Member(L"parentId", value.parentId), Member(L"cursor", value.cursor));
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, DocumentRequest&, const DocumentRequest&> value) {
	return codec.Record(Member(L"resourceId", value.resourceId));
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, CommandInvoked&, const CommandInvoked&> value) {
	return codec.Record(Member(L"commandId", value.commandId), Member(L"arguments", value.arguments));
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, ToolCompleted&, const ToolCompleted&> value) {
	return codec.Record(Member(L"readId", value.readId), Member(L"status", value.status), Member(L"data", value.data), Member(L"message", value.message));
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, WorkspaceChanged&, const WorkspaceChanged&> value) {
	return codec.Record(Member(L"repositories", value.repositories));
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, Cancel&, const Cancel&> value) {
	return codec.Record(Member(L"operationId", value.operationId));
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, VisibilityChanged&, const VisibilityChanged&> value) {
	return codec.Record(Member(L"viewId", value.viewId), Member(L"visible", value.visible));
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, StartToolRead&, const StartToolRead&> value) {
	return codec.Record(Member(L"readId", value.readId), Member(L"toolId", value.toolId), Member(L"operation", value.operation), Member(L"arguments", value.arguments));
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, PublishTreePage&, const PublishTreePage&> value) {
	return codec.Record(Member(L"viewId", value.viewId), Member(L"parentId", value.parentId), Member(L"items", value.items), Member(L"nextCursor", value.nextCursor), Member(L"revision", value.revision), Member(L"status", value.status), Member(L"message", value.message));
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, MarkdownSection&, const MarkdownSection&> value) {
	return codec.Record(Member(L"text", value.text));
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, MetadataSection&, const MetadataSection&> value) {
	return codec.Record(Member(L"fields", value.fields));
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, TableRow&, const TableRow&> value) {
	return codec.Record(Member(L"cells", value.cells));
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, TableSection&, const TableSection&> value) {
	return codec.Record(Member(L"columns", value.columns), Member(L"rows", value.rows));
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, TextResourceSection&, const TextResourceSection&> value) {
	return codec.Record(Member(L"handle", value.handle), Member(L"length", value.length), Member(L"status", value.status));
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, CompleteCommand&, const CompleteCommand&> value) {
	return codec.Record(Member(L"status", value.status), Member(L"message", value.message));
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, InvalidateTree&, const InvalidateTree&> value) {
	return codec.Record(Member(L"viewId", value.viewId));
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, OpenDocument&, const OpenDocument&> value) {
	return codec.Record(Member(L"resourceId", value.resourceId));
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, ReleaseResource&, const ReleaseResource&> value) {
	return codec.Record(Member(L"handle", value.handle));
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, Hello&, const Hello&> value) {
	return codec.Record(Member(L"abi", value.abi));
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, Activate&, const Activate&> value) {
	return codec.Record(Member(L"context", value.context), Member(L"extensionId", value.extensionId));
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, Deactivate&, const Deactivate&> value) {
	return codec.Record(Member(L"reason", value.reason));
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, Ack&, const Ack&> value) {
	return codec.Record(Member(L"ackSequence", value.ackSequence));
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, Rejected&, const Rejected&> value) {
	return codec.Record(Member(L"requestSequence", value.requestSequence), Member(L"code", value.code));
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, Stopped&, const Stopped&> value) {
	return codec.Record(Member(L"reason", value.reason));
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, DocumentSection&, const DocumentSection&> value) {
	return codec.Variant(value, {L"markdown", L"metadata", L"table", L"textResource"});
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, Event&, const Event&> value) {
	return codec.Variant(value, {L"treeRequest", L"documentRequest", L"commandInvoked", L"toolCompleted", L"workspaceChanged", L"cancel", L"visibilityChanged"});
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, PublishDocument&, const PublishDocument&> value) {
	return codec.Record(Member(L"resourceId", value.resourceId), Member(L"title", value.title), Member(L"revision", value.revision), Member(L"sections", value.sections));
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, EventMessage&, const EventMessage&> value) {
	return codec.Record(Member(L"context", value.context), Member(L"event", value.event));
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, Effect&, const Effect&> value) {
	return codec.Variant(value, {L"startToolRead", L"publishTreePage", L"publishDocument", L"completeCommand", L"invalidateTree", L"openDocument", L"releaseResource"});
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, EffectsMessage&, const EffectsMessage&> value) {
	return codec.Record(Member(L"context", value.context), Member(L"effects", value.effects));
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, Message&, const Message&> value) {
	return codec.Variant(value, {L"hello", L"activate", L"event", L"effects", L"deactivate", L"ack", L"rejected", L"stopped"});
}
template<class Codec> bool Transfer(Codec& codec, std::conditional_t<kIsReadingCodec<Codec>, Envelope&, const Envelope&> value) {
	return codec.Record(Member(L"protocol", value.protocol), Member(L"sequence", value.sequence), Member(L"sessionGeneration", value.sessionGeneration), Member(L"body", value.body));
}
class Reader final {
public:
	explicit Reader(const Json& json) : m_json(json) {}
	template<class T> bool Value(T& value)
	{
		bool accepted = false;
		if constexpr (std::is_same_v<T, std::wstring> || std::is_same_v<T, std::int64_t> || std::is_same_v<T, bool>) {
			if (const auto* found = std::get_if<T>(&m_json.Value())) { value = *found; accepted = true; }
			if constexpr (std::is_same_v<T, std::wstring>) accepted = accepted && Text(value, 262144);
			if constexpr (std::is_same_v<T, std::int64_t>) accepted = accepted && value >= 0;
		} else if constexpr (IsVector<T>::value) {
			const auto* array = std::get_if<Json::Array>(&m_json.Value());
			if (!array || array->size() > kMaximumItems) return false;
			value.reserve(array->size());
			for (const auto& item : *array) {
				typename T::value_type decoded{};
				if (!Reader(item).Value(decoded)) return false;
				value.push_back(std::move(decoded));
			}
			accepted = true;
		} else accepted = Transfer(*this, value);
		return accepted && Rules(value);
	}
	template<class... Fields> bool Record(Fields... fields)
	{
		const auto* object = std::get_if<Json::Object>(&m_json.Value());
		if (!object || object->size() != sizeof...(fields)) return false;
		return (ReadMember(*object, fields) && ...);
	}
	template<class T> bool Enumeration(T& value, std::initializer_list<std::pair<std::wstring_view, T>> names)
	{
		const auto* name = std::get_if<std::wstring>(&m_json.Value());
		if (!name) return false;
		for (const auto& [text, candidate] : names) if (text == *name) { value = candidate; return true; }
		return false;
	}
	template<class... T> bool Variant(std::variant<T...>& value, std::initializer_list<std::wstring_view> names)
	{
		const auto* object = std::get_if<Json::Object>(&m_json.Value());
		if (!object || object->size() != 2 || !object->contains(L"type") || !object->contains(L"data")) return false;
		const auto* name = std::get_if<std::wstring>(&object->at(L"type").Value());
		if (!name) return false;
		return ReadAlternative(value, *name, object->at(L"data"), names);
	}
private:
	template<class T> static bool ReadMember(const Json::Object& object, NamedMember<T> member)
	{
		const auto found = object.find(member.name);
		return found != object.end() && Reader(found->second).Value(member.value);
	}
	template<std::size_t Index = 0, class... T> static bool ReadAlternative(std::variant<T...>& value,
		std::wstring_view name, const Json& data, std::initializer_list<std::wstring_view> names)
	{
		if constexpr (Index == sizeof...(T)) return false;
		else {
			if (name == *(names.begin() + Index)) {
				std::variant_alternative_t<Index, std::variant<T...>> decoded{};
				if (!Reader(data).Value(decoded)) return false;
				value = std::move(decoded); return true;
			}
			return ReadAlternative<Index + 1>(value, name, data, names);
		}
	}
	const Json& m_json;
};

// Reader is the sole reading Codec; every other Codec (Writer included) keeps
// the primary template's `false`, so Writer needs no matching specialization.
template<> inline constexpr bool kIsReadingCodec<Reader> = true;

//! Writes once into a bounded output buffer. Do not materialize a second JSON
//! tree or copy a returned effect graph before checking its aggregate budget.
class Writer final {
public:
	template<class T> bool Value(const T& value)
	{
		if (++m_nodes > 65536 || !Rules(value)) return false;
		if constexpr (std::is_same_v<T, std::wstring>) return Text(value, 262144) && String(value);
		else if constexpr (std::is_same_v<T, std::int64_t>) return value >= 0 && Append(std::to_string(value));
		else if constexpr (std::is_same_v<T, bool>) return Append(value ? "true" : "false");
		else if constexpr (IsVector<T>::value) {
			if (value.size() > kMaximumItems || !Append("[")) return false;
			bool first = true;
			for (const auto& item : value) { if (!Separator(first) || !Value(item)) return false; }
			return Append("]");
		} else return Transfer(*this, value);
	}
	template<class... Fields> bool Record(Fields... fields)
	{
		bool first = true;
		return Append("{") && (WriteMember(fields, first) && ...) && Append("}");
	}
	template<class T> bool Enumeration(const T& value, std::initializer_list<std::pair<std::wstring_view, T>> names)
	{
		for (const auto& [text, candidate] : names) if (candidate == value) return String(text);
		return false;
	}
	template<class... T> bool Variant(const std::variant<T...>& value, std::initializer_list<std::wstring_view> names)
	{
		// The type string is a JSON node in addition to the variant object and data.
		if (value.valueless_by_exception() || ++m_nodes > 65536 || !Append("{\"type\":")) return false;
		return String(*(names.begin() + value.index())) && Append(",\"data\":") &&
			std::visit([&](const auto& item) { return Value(item); }, value) && Append("}");
	}
	std::string TakeOutput() { return std::move(m_output); }
private:
	bool Append(std::string_view text)
	{
		if (text.size() > kMaximumFrameBytes - m_output.size()) return false;
		m_output += text;
		return true;
	}
	bool Separator(bool& first)
	{
		if (!first && !Append(",")) return false;
		first = false;
		return true;
	}
	bool String(std::wstring_view value)
	{
		constexpr char hex[] = "0123456789abcdef";
		if (!Append("\"")) return false;
		for (std::size_t index = 0; index < value.size(); ++index) {
			auto c = static_cast<std::uint32_t>(value[index]);
			switch (c) {
			case '"': if (!Append("\\\"")) return false; continue;
			case '\\': if (!Append("\\\\")) return false; continue;
			case '\b': if (!Append("\\b")) return false; continue;
			case '\f': if (!Append("\\f")) return false; continue;
			case '\n': if (!Append("\\n")) return false; continue;
			case '\r': if (!Append("\\r")) return false; continue;
			case '\t': if (!Append("\\t")) return false; continue;
			default: break;
			}
			std::array<char, 6> bytes{};
			std::size_t size = 0;
			if (c < 0x20) {
				bytes[size++] = '\\'; bytes[size++] = 'u';
				for (int shift = 12; shift >= 0; shift -= 4) bytes[size++] = hex[(c >> shift) & 15];
			} else if (c < 0x80) bytes[size++] = static_cast<char>(c);
			else {
				if (c >= 0xd800 && c <= 0xdbff) c = 0x10000 + ((c - 0xd800) << 10) + (value[++index] - 0xdc00);
				if (c < 0x800) bytes[size++] = static_cast<char>(0xc0 | (c >> 6));
				else if (c < 0x10000) { bytes[size++] = static_cast<char>(0xe0 | (c >> 12)); bytes[size++] = static_cast<char>(0x80 | ((c >> 6) & 63)); }
				else { bytes[size++] = static_cast<char>(0xf0 | (c >> 18)); bytes[size++] = static_cast<char>(0x80 | ((c >> 12) & 63)); bytes[size++] = static_cast<char>(0x80 | ((c >> 6) & 63)); }
				bytes[size++] = static_cast<char>(0x80 | (c & 63));
			}
			if (!Append(std::string_view(bytes.data(), size))) return false;
		}
		return Append("\"");
	}
	template<class T> bool WriteMember(NamedMember<T> member, bool& first)
	{
		return Separator(first) && String(member.name) && Append(":") && Value(member.value);
	}
	std::size_t m_nodes = 0;
	std::string m_output;
};

} // namespace

std::optional<Envelope> Decode(std::string_view json)
{
	if (json.empty() || json.size() > kMaximumFrameBytes) return std::nullopt;
	const auto parsed = platform::serialization::CJsoncDocument::ParseStrict(json);
	Envelope value;
	if (!parsed.Succeeded() || !Reader(*parsed.value).Value(value)) return std::nullopt;
	return value;
}

std::optional<std::string> Encode(const Envelope& envelope)
{
	Writer writer;
	if (!writer.Value(envelope)) return std::nullopt;
	return writer.TakeOutput();
}

bool Validate(const Envelope& envelope) { return Encode(envelope).has_value(); }
bool ValidateDocument(const PublishDocument& document) { return Writer().Value(document); }
bool ValidateWorkspace(const WorkspaceChanged& workspace) { return Writer().Value(workspace); }

} // namespace senp::effect
