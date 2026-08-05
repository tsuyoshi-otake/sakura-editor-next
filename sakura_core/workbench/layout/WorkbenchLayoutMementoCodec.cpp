/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/layout/WorkbenchLayoutMementoCodec.h"

#include <sakura/storage/StorageTypes.h>

#include <picojson/picojson.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <utility>

namespace workbench::layout {
namespace {

using JsonArray = picojson::array;
using JsonObject = picojson::object;
using JsonValue = picojson::value;

WorkbenchLayoutMementoEncodeResult EncodeFailure(
	EWorkbenchLayoutMementoCodecStatus status, std::string diagnostic)
{
	return { status, {}, std::move(diagnostic) };
}

WorkbenchLayoutMementoDecodeResult DecodeFailure(
	EWorkbenchLayoutMementoCodecStatus status, std::string diagnostic)
{
	return { status, std::nullopt, std::move(diagnostic) };
}

bool IsValidStableId(std::string_view value) noexcept
{
	if (!IsValidWorkbenchLayoutId(value)
		|| !platform::storage::IsValidStorageUtf8(value, false)) {
		return false;
	}
	return std::none_of(value.begin(), value.end(), [](char character) {
		const auto byte = static_cast<unsigned char>(character);
		return byte < 0x20 || byte == 0x7f;
	});
}

bool IsValidPosition(EWorkbenchPartPosition value) noexcept
{
	return value == EWorkbenchPartPosition::Top || value == EWorkbenchPartPosition::Left
		|| value == EWorkbenchPartPosition::Center || value == EWorkbenchPartPosition::Right
		|| value == EWorkbenchPartPosition::Bottom;
}

bool IsValidLocation(EWorkbenchViewContainerLocation value) noexcept
{
	return value == EWorkbenchViewContainerLocation::SideBar
		|| value == EWorkbenchViewContainerLocation::Panel
		|| value == EWorkbenchViewContainerLocation::AuxiliaryBar;
}

bool IsValidPanelAlignment(EWorkbenchPanelAlignment value) noexcept
{
	return value == EWorkbenchPanelAlignment::Left || value == EWorkbenchPanelAlignment::Center
		|| value == EWorkbenchPanelAlignment::Right || value == EWorkbenchPanelAlignment::Justify;
}

template <class Item, class IdSelector>
bool HasUniqueValidIds(const std::vector<Item>& items, IdSelector select)
{
	std::set<std::string, std::less<>> ids;
	for (const auto& item : items) {
		const auto& id = select(item);
		if (!IsValidStableId(id) || !ids.emplace(id).second) return false;
	}
	return true;
}

bool IsValidSnapshot(const WorkbenchLayoutStateSnapshot& snapshot)
{
	if (snapshot.schemaVersion != kWorkbenchLayoutStateSchemaVersion
		|| snapshot.parts.size() > kMaximumWorkbenchLayoutMementoParts
		|| snapshot.containers.size() > kMaximumWorkbenchLayoutMementoContainers
		|| snapshot.views.size() > kMaximumWorkbenchLayoutMementoViews
		|| !IsValidPanelAlignment(snapshot.panelAlignment)
		|| !HasUniqueValidIds(snapshot.parts, [](const WorkbenchPartState& item) -> const std::string& { return item.partId; })
		|| !HasUniqueValidIds(snapshot.containers, [](const WorkbenchViewContainerState& item) -> const std::string& { return item.containerId; })
		|| !HasUniqueValidIds(snapshot.views, [](const WorkbenchViewState& item) -> const std::string& { return item.viewId; })) {
		return false;
	}
	for (const auto& part : snapshot.parts) {
		if (!IsValidPosition(part.position)
			|| (part.committedExtentDip && (*part.committedExtentDip == 0
				|| *part.committedExtentDip > kMaximumWorkbenchLayoutMementoExtentDip))) {
			return false;
		}
	}
	for (const auto& container : snapshot.containers) {
		if (!IsValidLocation(container.location)
			|| (container.activeViewId && !IsValidStableId(*container.activeViewId))) {
			return false;
		}
	}
	for (const auto& view : snapshot.views) {
		if (!IsValidStableId(view.containerId)) return false;
	}
	std::set<std::string, std::less<>> activeContainerIds;
	const std::array activeContainers{
		&snapshot.activeContainers.sideBar,
		&snapshot.activeContainers.panel,
		&snapshot.activeContainers.auxiliaryBar,
	};
	for (const auto* active : activeContainers) {
		if (*active && (!IsValidStableId(**active)
			|| !activeContainerIds.emplace(**active).second)) return false;
	}
	return (!snapshot.focus.partId || IsValidStableId(*snapshot.focus.partId))
		&& (!snapshot.focus.containerId || IsValidStableId(*snapshot.focus.containerId))
		&& (!snapshot.focus.viewId || IsValidStableId(*snapshot.focus.viewId));
}

const char* PositionText(EWorkbenchPartPosition value) noexcept
{
	switch (value) {
	case EWorkbenchPartPosition::Top: return "top";
	case EWorkbenchPartPosition::Left: return "left";
	case EWorkbenchPartPosition::Center: return "center";
	case EWorkbenchPartPosition::Right: return "right";
	case EWorkbenchPartPosition::Bottom: return "bottom";
	}
	return "";
}

const char* LocationText(EWorkbenchViewContainerLocation value) noexcept
{
	switch (value) {
	case EWorkbenchViewContainerLocation::SideBar: return "sidebar";
	case EWorkbenchViewContainerLocation::Panel: return "panel";
	case EWorkbenchViewContainerLocation::AuxiliaryBar: return "auxiliarybar";
	}
	return "";
}

const char* AlignmentText(EWorkbenchPanelAlignment value) noexcept
{
	switch (value) {
	case EWorkbenchPanelAlignment::Left: return "left";
	case EWorkbenchPanelAlignment::Center: return "center";
	case EWorkbenchPanelAlignment::Right: return "right";
	case EWorkbenchPanelAlignment::Justify: return "justify";
	}
	return "";
}

std::optional<EWorkbenchPartPosition> ParsePosition(std::string_view value) noexcept
{
	if (value == "top") return EWorkbenchPartPosition::Top;
	if (value == "left") return EWorkbenchPartPosition::Left;
	if (value == "center") return EWorkbenchPartPosition::Center;
	if (value == "right") return EWorkbenchPartPosition::Right;
	if (value == "bottom") return EWorkbenchPartPosition::Bottom;
	return std::nullopt;
}

std::optional<EWorkbenchViewContainerLocation> ParseLocation(std::string_view value) noexcept
{
	if (value == "sidebar") return EWorkbenchViewContainerLocation::SideBar;
	if (value == "panel") return EWorkbenchViewContainerLocation::Panel;
	if (value == "auxiliarybar") return EWorkbenchViewContainerLocation::AuxiliaryBar;
	return std::nullopt;
}

std::optional<EWorkbenchPanelAlignment> ParseAlignment(std::string_view value) noexcept
{
	if (value == "left") return EWorkbenchPanelAlignment::Left;
	if (value == "center") return EWorkbenchPanelAlignment::Center;
	if (value == "right") return EWorkbenchPanelAlignment::Right;
	if (value == "justify") return EWorkbenchPanelAlignment::Justify;
	return std::nullopt;
}

const JsonValue* Find(const JsonObject& object, std::string_view key) noexcept
{
	const auto found = object.find(std::string(key));
	return found == object.end() ? nullptr : &found->second;
}

bool ReadRequiredString(const JsonObject& object, std::string_view key, std::string& value)
{
	const auto* field = Find(object, key);
	if (field == nullptr || !field->is<std::string>()) return false;
	value = field->get<std::string>();
	return IsValidStableId(value);
}

bool ReadRequiredBoolean(const JsonObject& object, std::string_view key, bool& value) noexcept
{
	const auto* field = Find(object, key);
	if (field == nullptr || !field->is<bool>()) return false;
	value = field->get<bool>();
	return true;
}

bool ReadUnsigned(const JsonValue& field, std::uint32_t maximum, std::uint32_t& value) noexcept
{
	if (!field.is<double>()) return false;
	const auto number = field.get<double>();
	if (!std::isfinite(number) || number < 0.0 || number > static_cast<double>(maximum)
		|| std::floor(number) != number) {
		return false;
	}
	value = static_cast<std::uint32_t>(number);
	return true;
}

bool ReadRequiredUnsigned(const JsonObject& object, std::string_view key,
	std::uint32_t maximum, std::uint32_t& value) noexcept
{
	const auto* field = Find(object, key);
	return field != nullptr && ReadUnsigned(*field, maximum, value);
}

bool ReadSigned(const JsonValue& field, std::int32_t& value) noexcept
{
	if (!field.is<double>()) return false;
	const auto number = field.get<double>();
	if (!std::isfinite(number)
		|| number < static_cast<double>(kMinimumWorkbenchLayoutMementoOrder)
		|| number > static_cast<double>(kMaximumWorkbenchLayoutMementoOrder)
		|| std::floor(number) != number) {
		return false;
	}
	value = static_cast<std::int32_t>(number);
	return true;
}

bool ReadRequiredSigned(const JsonObject& object, std::string_view key, std::int32_t& value) noexcept
{
	const auto* field = Find(object, key);
	return field != nullptr && ReadSigned(*field, value);
}

bool ReadOptionalString(const JsonObject& object, std::string_view key, std::optional<std::string>& value)
{
	const auto* field = Find(object, key);
	if (field == nullptr || field->is<picojson::null>()) {
		value.reset();
		return true;
	}
	if (!field->is<std::string>() || !IsValidStableId(field->get<std::string>())) return false;
	value = field->get<std::string>();
	return true;
}

bool ReadOptionalExtent(const JsonObject& object, std::optional<std::uint32_t>& value) noexcept
{
	const auto* field = Find(object, "extentDip");
	if (field == nullptr || field->is<picojson::null>()) {
		value.reset();
		return true;
	}
	std::uint32_t extent = 0;
	if (!ReadUnsigned(*field, kMaximumWorkbenchLayoutMementoExtentDip, extent) || extent == 0) return false;
	value = extent;
	return true;
}

bool HasBoundedJsonNesting(std::string_view payload) noexcept
{
	std::size_t depth = 0;
	bool inString = false;
	bool escaped = false;
	for (const char character : payload) {
		if (inString) {
			if (escaped) {
				escaped = false;
			} else if (character == '\\') {
				escaped = true;
			} else if (character == '"') {
				inString = false;
			}
			continue;
		}
		if (character == '"') {
			inString = true;
		} else if (character == '{' || character == '[') {
			if (++depth > kMaximumWorkbenchLayoutMementoDepth) return false;
		} else if (character == '}' || character == ']') {
			if (depth == 0) return false;
			--depth;
		}
	}
	return !inString && !escaped && depth == 0;
}

bool IsJsonWhitespace(char character) noexcept
{
	return character == ' ' || character == '\t' || character == '\r' || character == '\n';
}

JsonObject EncodePart(const WorkbenchPartState& part)
{
	JsonObject result;
	if (part.committedExtentDip) result["extentDip"] = JsonValue(static_cast<double>(*part.committedExtentDip));
	result["id"] = JsonValue(part.partId);
	result["position"] = JsonValue(PositionText(part.position));
	result["visible"] = JsonValue(part.visible);
	return result;
}

JsonObject EncodeContainer(const WorkbenchViewContainerState& container)
{
	JsonObject result;
	if (container.activeViewId) result["activeViewId"] = JsonValue(*container.activeViewId);
	result["id"] = JsonValue(container.containerId);
	result["location"] = JsonValue(LocationText(container.location));
	result["order"] = JsonValue(static_cast<double>(container.order));
	result["visible"] = JsonValue(container.visible);
	return result;
}

JsonObject EncodeView(const WorkbenchViewState& view)
{
	JsonObject result;
	result["containerId"] = JsonValue(view.containerId);
	result["id"] = JsonValue(view.viewId);
	result["order"] = JsonValue(static_cast<double>(view.order));
	result["visible"] = JsonValue(view.visible);
	return result;
}

template <class Item, class IdSelector>
void SortByStableId(std::vector<Item>& items, IdSelector select)
{
	std::ranges::sort(items, {}, select);
}

} // namespace

WorkbenchLayoutMementoEncodeResult CWorkbenchLayoutMementoCodec::Encode(
	const WorkbenchLayoutStateSnapshot& snapshot) noexcept
{
	try {
		if (!IsValidSnapshot(snapshot)) {
			return EncodeFailure(EWorkbenchLayoutMementoCodecStatus::InvalidSnapshot,
				"layout snapshot failed memento validation");
		}

		auto parts = snapshot.parts;
		auto containers = snapshot.containers;
		auto views = snapshot.views;
		SortByStableId(parts, &WorkbenchPartState::partId);
		SortByStableId(containers, &WorkbenchViewContainerState::containerId);
		SortByStableId(views, &WorkbenchViewState::viewId);

		JsonArray encodedParts;
		encodedParts.reserve(parts.size());
		for (const auto& part : parts) encodedParts.emplace_back(EncodePart(part));
		JsonArray encodedContainers;
		encodedContainers.reserve(containers.size());
		for (const auto& container : containers) encodedContainers.emplace_back(EncodeContainer(container));
		JsonArray encodedViews;
		encodedViews.reserve(views.size());
		for (const auto& view : views) encodedViews.emplace_back(EncodeView(view));

		JsonObject focus;
		if (snapshot.focus.partId) focus["partId"] = JsonValue(*snapshot.focus.partId);
		if (snapshot.focus.containerId) focus["containerId"] = JsonValue(*snapshot.focus.containerId);
		if (snapshot.focus.viewId) focus["viewId"] = JsonValue(*snapshot.focus.viewId);
		JsonObject activeContainers;
		if (snapshot.activeContainers.sideBar)
			activeContainers["sideBar"] = JsonValue(*snapshot.activeContainers.sideBar);
		if (snapshot.activeContainers.panel)
			activeContainers["panel"] = JsonValue(*snapshot.activeContainers.panel);
		if (snapshot.activeContainers.auxiliaryBar)
			activeContainers["auxiliaryBar"] = JsonValue(*snapshot.activeContainers.auxiliaryBar);

		JsonObject root;
		root["activeContainers"] = JsonValue(std::move(activeContainers));
		root["containers"] = JsonValue(std::move(encodedContainers));
		root["focus"] = JsonValue(std::move(focus));
		root["formatVersion"] = JsonValue(static_cast<double>(kWorkbenchLayoutMementoFormatVersion));
		root["panelAlignment"] = JsonValue(AlignmentText(snapshot.panelAlignment));
		root["parts"] = JsonValue(std::move(encodedParts));
		root["views"] = JsonValue(std::move(encodedViews));
		auto payload = JsonValue(std::move(root)).serialize(false);
		if (payload.size() > platform::storage::kMaximumStorageStringBytes) {
			return EncodeFailure(EWorkbenchLayoutMementoCodecStatus::PayloadTooLarge,
				"layout memento exceeds the storage value limit");
		}
		return { EWorkbenchLayoutMementoCodecStatus::Succeeded, std::move(payload), {} };
	} catch (...) {
		return EncodeFailure(EWorkbenchLayoutMementoCodecStatus::InvalidSnapshot,
			"layout memento encoding failed");
	}
}

WorkbenchLayoutMementoDecodeResult CWorkbenchLayoutMementoCodec::Decode(std::string_view payload) noexcept
{
	try {
		if (payload.size() > platform::storage::kMaximumStorageStringBytes) {
			return DecodeFailure(EWorkbenchLayoutMementoCodecStatus::PayloadTooLarge,
				"layout memento exceeds the storage value limit");
		}
		if (!platform::storage::IsValidStorageUtf8(payload, false) || !HasBoundedJsonNesting(payload)) {
			return DecodeFailure(EWorkbenchLayoutMementoCodecStatus::CorruptPayload,
				"layout memento is not bounded valid UTF-8 JSON");
		}

		JsonValue rootValue;
		std::string parseError;
		auto position = picojson::parse(rootValue, payload.begin(), payload.end(), &parseError);
		while (position != payload.end() && IsJsonWhitespace(*position)) ++position;
		if (!parseError.empty() || position != payload.end() || !rootValue.is<JsonObject>()) {
			return DecodeFailure(EWorkbenchLayoutMementoCodecStatus::CorruptPayload,
				"layout memento JSON is malformed");
		}
		const auto& root = rootValue.get<JsonObject>();
		std::uint32_t formatVersion = 0;
		if (!ReadRequiredUnsigned(root, "formatVersion", UINT32_MAX, formatVersion)) {
			return DecodeFailure(EWorkbenchLayoutMementoCodecStatus::CorruptPayload,
				"layout memento formatVersion is invalid");
		}
		if (formatVersion != kWorkbenchLayoutMementoLegacyFormatVersion
			&& formatVersion != kWorkbenchLayoutMementoFormatVersion) {
			return DecodeFailure(EWorkbenchLayoutMementoCodecStatus::UnsupportedSchema,
				"layout memento formatVersion is unsupported");
		}

		const auto* partsValue = Find(root, "parts");
		const auto* containersValue = Find(root, "containers");
		const auto* viewsValue = Find(root, "views");
		const auto* alignmentValue = Find(root, "panelAlignment");
		const auto* focusValue = Find(root, "focus");
		const auto* activeContainersValue = Find(root, "activeContainers");
		if (partsValue == nullptr || !partsValue->is<JsonArray>()
			|| containersValue == nullptr || !containersValue->is<JsonArray>()
			|| viewsValue == nullptr || !viewsValue->is<JsonArray>()
			|| alignmentValue == nullptr || !alignmentValue->is<std::string>()
			|| focusValue == nullptr || !focusValue->is<JsonObject>()
			|| (formatVersion == kWorkbenchLayoutMementoFormatVersion
				&& (activeContainersValue == nullptr || !activeContainersValue->is<JsonObject>()))) {
			return DecodeFailure(EWorkbenchLayoutMementoCodecStatus::CorruptPayload,
				"layout memento root fields are invalid");
		}
		if (partsValue->get<JsonArray>().size() > kMaximumWorkbenchLayoutMementoParts
			|| containersValue->get<JsonArray>().size() > kMaximumWorkbenchLayoutMementoContainers
			|| viewsValue->get<JsonArray>().size() > kMaximumWorkbenchLayoutMementoViews) {
			return DecodeFailure(EWorkbenchLayoutMementoCodecStatus::CorruptPayload,
				"layout memento contribution count is invalid");
		}

		WorkbenchLayoutStateSnapshot snapshot;
		snapshot.schemaVersion = kWorkbenchLayoutStateSchemaVersion;
		snapshot.generation = 0;
		snapshot.revision = 0;
		const auto alignment = ParseAlignment(alignmentValue->get<std::string>());
		if (!alignment) {
			return DecodeFailure(EWorkbenchLayoutMementoCodecStatus::CorruptPayload,
				"layout memento panel alignment is invalid");
		}
		snapshot.panelAlignment = *alignment;
		if (formatVersion == kWorkbenchLayoutMementoFormatVersion) {
			const auto& activeContainers = activeContainersValue->get<JsonObject>();
			if (!ReadOptionalString(activeContainers, "sideBar", snapshot.activeContainers.sideBar)
				|| !ReadOptionalString(activeContainers, "panel", snapshot.activeContainers.panel)
				|| !ReadOptionalString(activeContainers, "auxiliaryBar",
					snapshot.activeContainers.auxiliaryBar)) {
				return DecodeFailure(EWorkbenchLayoutMementoCodecStatus::CorruptPayload,
					"layout memento active container fields are invalid");
			}
		}

		for (const auto& value : partsValue->get<JsonArray>()) {
			if (!value.is<JsonObject>()) return DecodeFailure(
				EWorkbenchLayoutMementoCodecStatus::CorruptPayload, "layout memento part is invalid");
			const auto& object = value.get<JsonObject>();
			WorkbenchPartState part;
			std::string positionText;
			if (!ReadRequiredString(object, "id", part.partId)
				|| !ReadRequiredString(object, "position", positionText)
				|| !ReadRequiredBoolean(object, "visible", part.visible)
				|| !ReadOptionalExtent(object, part.committedExtentDip)) {
				return DecodeFailure(EWorkbenchLayoutMementoCodecStatus::CorruptPayload,
					"layout memento part fields are invalid");
			}
			const auto positionValue = ParsePosition(positionText);
			if (!positionValue) return DecodeFailure(EWorkbenchLayoutMementoCodecStatus::CorruptPayload,
				"layout memento part position is invalid");
			part.position = *positionValue;
			snapshot.parts.emplace_back(std::move(part));
		}

		for (const auto& value : containersValue->get<JsonArray>()) {
			if (!value.is<JsonObject>()) return DecodeFailure(
				EWorkbenchLayoutMementoCodecStatus::CorruptPayload, "layout memento container is invalid");
			const auto& object = value.get<JsonObject>();
			WorkbenchViewContainerState container;
			std::string locationText;
			if (!ReadRequiredString(object, "id", container.containerId)
				|| !ReadRequiredString(object, "location", locationText)
				|| !ReadRequiredSigned(object, "order", container.order)
				|| !ReadRequiredBoolean(object, "visible", container.visible)
				|| !ReadOptionalString(object, "activeViewId", container.activeViewId)) {
				return DecodeFailure(EWorkbenchLayoutMementoCodecStatus::CorruptPayload,
					"layout memento container fields are invalid");
			}
			const auto location = ParseLocation(locationText);
			if (!location) return DecodeFailure(EWorkbenchLayoutMementoCodecStatus::CorruptPayload,
				"layout memento container location is invalid");
			container.location = *location;
			snapshot.containers.emplace_back(std::move(container));
		}

		for (const auto& value : viewsValue->get<JsonArray>()) {
			if (!value.is<JsonObject>()) return DecodeFailure(
				EWorkbenchLayoutMementoCodecStatus::CorruptPayload, "layout memento view is invalid");
			const auto& object = value.get<JsonObject>();
			WorkbenchViewState view;
			if (!ReadRequiredString(object, "id", view.viewId)
				|| !ReadRequiredString(object, "containerId", view.containerId)
				|| !ReadRequiredSigned(object, "order", view.order)
				|| !ReadRequiredBoolean(object, "visible", view.visible)) {
				return DecodeFailure(EWorkbenchLayoutMementoCodecStatus::CorruptPayload,
					"layout memento view fields are invalid");
			}
			snapshot.views.emplace_back(std::move(view));
		}

		const auto& focus = focusValue->get<JsonObject>();
		if (!ReadOptionalString(focus, "partId", snapshot.focus.partId)
			|| !ReadOptionalString(focus, "containerId", snapshot.focus.containerId)
			|| !ReadOptionalString(focus, "viewId", snapshot.focus.viewId)) {
			return DecodeFailure(EWorkbenchLayoutMementoCodecStatus::CorruptPayload,
				"layout memento focus fields are invalid");
		}
		if (!IsValidSnapshot(snapshot)) {
			return DecodeFailure(EWorkbenchLayoutMementoCodecStatus::CorruptPayload,
				"layout memento contains invalid or duplicate stable state");
		}
		SortByStableId(snapshot.parts, &WorkbenchPartState::partId);
		SortByStableId(snapshot.containers, &WorkbenchViewContainerState::containerId);
		SortByStableId(snapshot.views, &WorkbenchViewState::viewId);
		return { EWorkbenchLayoutMementoCodecStatus::Succeeded, std::move(snapshot), {} };
	} catch (...) {
		return DecodeFailure(EWorkbenchLayoutMementoCodecStatus::CorruptPayload,
			"layout memento decoding failed");
	}
}

} // namespace workbench::layout
