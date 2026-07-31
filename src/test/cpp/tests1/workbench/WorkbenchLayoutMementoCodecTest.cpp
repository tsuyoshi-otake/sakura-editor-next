/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "platform/storage/StorageTypes.h"
#include "workbench/layout/WorkbenchLayoutMementoCodec.h"

#include <algorithm>
#include <string>

namespace {

using workbench::layout::CWorkbenchLayoutMementoCodec;
using workbench::layout::EWorkbenchLayoutMementoCodecStatus;
using workbench::layout::EWorkbenchPanelAlignment;
using workbench::layout::EWorkbenchPartPosition;
using workbench::layout::EWorkbenchViewContainerLocation;
using workbench::layout::WorkbenchLayoutStateSnapshot;
using workbench::layout::WorkbenchPartState;
using workbench::layout::WorkbenchViewContainerState;
using workbench::layout::WorkbenchViewState;

WorkbenchLayoutStateSnapshot Sample()
{
	WorkbenchLayoutStateSnapshot snapshot;
	snapshot.generation = 77;
	snapshot.revision = 88;
	snapshot.panelAlignment = EWorkbenchPanelAlignment::Justify;
	snapshot.parts = {
		{ "workbench.parts.panel", true, EWorkbenchPartPosition::Bottom, 240 },
		{ "workbench.parts.auxiliarybar", false, EWorkbenchPartPosition::Right, 321 },
		{ "workbench.parts.sidebar", true, EWorkbenchPartPosition::Left, 280 },
		{ "workbench.parts.editor", true, EWorkbenchPartPosition::Center, std::nullopt },
	};
	snapshot.containers = {
		{ "terminal", EWorkbenchViewContainerLocation::Panel, 30, true, "terminal" },
		{ "workbench.view.explorer", EWorkbenchViewContainerLocation::SideBar, -10, true,
			"workbench.explorer.fileView" },
		{ "sakura.extensionViews", EWorkbenchViewContainerLocation::AuxiliaryBar, 10, false, std::nullopt },
	};
	snapshot.views = {
		{ "terminal", "terminal", 10, true },
		{ "workbench.explorer.fileView", "workbench.view.explorer", 10, true },
		{ "outline", "workbench.view.explorer", 20, true },
	};
	snapshot.activeContainers.sideBar = "workbench.view.explorer";
	snapshot.activeContainers.panel = "terminal";
	snapshot.activeContainers.auxiliaryBar = "sakura.extensionViews";
	snapshot.focus.partId = "workbench.parts.sidebar";
	snapshot.focus.containerId = "workbench.view.explorer";
	snapshot.focus.viewId = "outline";
	return snapshot;
}

} // namespace

TEST(WorkbenchLayoutMementoCodec, RoundTripIsDeterministicAndOmitsRuntimeCoordinates)
{
	auto first = Sample();
	auto second = first;
	std::ranges::reverse(second.parts);
	std::ranges::reverse(second.containers);
	std::ranges::reverse(second.views);
	second.generation = 999;
	second.revision = 1000;

	const auto encodedFirst = CWorkbenchLayoutMementoCodec::Encode(first);
	const auto encodedSecond = CWorkbenchLayoutMementoCodec::Encode(second);
	ASSERT_TRUE(encodedFirst.Succeeded()) << encodedFirst.diagnostic;
	ASSERT_TRUE(encodedSecond.Succeeded()) << encodedSecond.diagnostic;
	EXPECT_EQ(encodedFirst.payload, encodedSecond.payload);
	EXPECT_EQ(std::string::npos, encodedFirst.payload.find("generation"));
	EXPECT_EQ(std::string::npos, encodedFirst.payload.find("revision"));
	EXPECT_EQ(std::string::npos, encodedFirst.payload.find("hwnd"));
	EXPECT_EQ(std::string::npos, encodedFirst.payload.find("maximized"));
	EXPECT_EQ(std::string::npos, encodedFirst.payload.find("drag"));

	const auto decoded = CWorkbenchLayoutMementoCodec::Decode(encodedFirst.payload);
	ASSERT_TRUE(decoded.Succeeded()) << decoded.diagnostic;
	EXPECT_EQ(0U, decoded.snapshot->generation);
	EXPECT_EQ(0U, decoded.snapshot->revision);
	EXPECT_EQ(EWorkbenchPanelAlignment::Justify, decoded.snapshot->panelAlignment);
	EXPECT_EQ("workbench.view.explorer", *decoded.snapshot->activeContainers.sideBar);
	EXPECT_EQ("terminal", *decoded.snapshot->activeContainers.panel);
	EXPECT_EQ("sakura.extensionViews", *decoded.snapshot->activeContainers.auxiliaryBar);
	const auto explorer = std::ranges::find(decoded.snapshot->containers,
		"workbench.view.explorer", &WorkbenchViewContainerState::containerId);
	ASSERT_NE(decoded.snapshot->containers.end(), explorer);
	EXPECT_EQ(-10, explorer->order);
	ASSERT_EQ(4U, decoded.snapshot->parts.size());
	const auto panel = std::ranges::find(decoded.snapshot->parts, "workbench.parts.panel", &WorkbenchPartState::partId);
	ASSERT_NE(decoded.snapshot->parts.end(), panel);
	ASSERT_TRUE(panel->committedExtentDip.has_value());
	EXPECT_EQ(240U, *panel->committedExtentDip);
	EXPECT_EQ(EWorkbenchPartPosition::Bottom, panel->position);

	const auto reencoded = CWorkbenchLayoutMementoCodec::Encode(*decoded.snapshot);
	ASSERT_TRUE(reencoded.Succeeded());
	EXPECT_EQ(encodedFirst.payload, reencoded.payload);
}

TEST(WorkbenchLayoutMementoCodec, UnknownStableIdsRoundTripWithoutRegistryMaterialization)
{
	auto snapshot = Sample();
	snapshot.containers.push_back({ "publisher.future.container", EWorkbenchViewContainerLocation::AuxiliaryBar,
		901, false, "publisher.future.view" });
	snapshot.views.push_back({ "publisher.future.view", "publisher.future.container", 902, false });

	const auto encoded = CWorkbenchLayoutMementoCodec::Encode(snapshot);
	ASSERT_TRUE(encoded.Succeeded());
	const auto decoded = CWorkbenchLayoutMementoCodec::Decode(encoded.payload);
	ASSERT_TRUE(decoded.Succeeded());
	EXPECT_NE(decoded.snapshot->containers.end(), std::ranges::find(
		decoded.snapshot->containers, "publisher.future.container", &WorkbenchViewContainerState::containerId));
	EXPECT_NE(decoded.snapshot->views.end(), std::ranges::find(
		decoded.snapshot->views, "publisher.future.view", &WorkbenchViewState::viewId));
}

TEST(WorkbenchLayoutMementoCodec, DuplicateSemanticIdsAreRejectedAtomically)
{
	const std::string payload = R"json({
		"formatVersion":1,"panelAlignment":"center","focus":{},
		"parts":[
			{"id":"workbench.parts.editor","position":"center","visible":true},
			{"id":"workbench.parts.editor","position":"center","visible":false}
		],"containers":[],"views":[]
	})json";
	const auto decoded = CWorkbenchLayoutMementoCodec::Decode(payload);
	EXPECT_EQ(EWorkbenchLayoutMementoCodecStatus::CorruptPayload, decoded.status);
	EXPECT_FALSE(decoded.snapshot.has_value());
}

TEST(WorkbenchLayoutMementoCodec, UnsupportedSchemaIsDistinctFromCorruptPayload)
{
	const auto unsupported = CWorkbenchLayoutMementoCodec::Decode(
		R"json({"formatVersion":3,"activeContainers":{},"panelAlignment":"center","focus":{},"parts":[],"containers":[],"views":[]})json");
	EXPECT_EQ(EWorkbenchLayoutMementoCodecStatus::UnsupportedSchema, unsupported.status);
	EXPECT_FALSE(unsupported.snapshot.has_value());

	const auto corrupt = CWorkbenchLayoutMementoCodec::Decode(
		R"json({"formatVersion":"one","panelAlignment":"center","focus":{},"parts":[],"containers":[],"views":[]})json");
	EXPECT_EQ(EWorkbenchLayoutMementoCodecStatus::CorruptPayload, corrupt.status);
	EXPECT_FALSE(corrupt.snapshot.has_value());
}

TEST(WorkbenchLayoutMementoCodec, LegacyV1DecodesWithoutActiveIntentAndReencodesAsCanonicalV2)
{
	const auto legacy = CWorkbenchLayoutMementoCodec::Decode(R"json({
		"formatVersion":1,"panelAlignment":"center","focus":{},
		"parts":[],"containers":[],"views":[]
	})json");
	ASSERT_TRUE(legacy.Succeeded()) << legacy.diagnostic;
	EXPECT_FALSE(legacy.snapshot->activeContainers.sideBar);
	EXPECT_FALSE(legacy.snapshot->activeContainers.panel);
	EXPECT_FALSE(legacy.snapshot->activeContainers.auxiliaryBar);

	const auto upgraded = CWorkbenchLayoutMementoCodec::Encode(*legacy.snapshot);
	ASSERT_TRUE(upgraded.Succeeded()) << upgraded.diagnostic;
	EXPECT_NE(std::string::npos, upgraded.payload.find(R"json("formatVersion":2)json"));
	EXPECT_NE(std::string::npos, upgraded.payload.find(R"json("activeContainers":{})json"));
}

TEST(WorkbenchLayoutMementoCodec, V2RequiresBoundedUniqueActiveContainerIds)
{
	const auto missing = CWorkbenchLayoutMementoCodec::Decode(
		R"json({"formatVersion":2,"panelAlignment":"center","focus":{},"parts":[],"containers":[],"views":[]})json");
	EXPECT_EQ(EWorkbenchLayoutMementoCodecStatus::CorruptPayload, missing.status);

	const auto malformed = CWorkbenchLayoutMementoCodec::Decode(
		R"json({"formatVersion":2,"activeContainers":[],"panelAlignment":"center","focus":{},"parts":[],"containers":[],"views":[]})json");
	EXPECT_EQ(EWorkbenchLayoutMementoCodecStatus::CorruptPayload, malformed.status);

	const auto invalidId = CWorkbenchLayoutMementoCodec::Decode(
		R"json({"formatVersion":2,"activeContainers":{"sideBar":""},"panelAlignment":"center","focus":{},"parts":[],"containers":[],"views":[]})json");
	EXPECT_EQ(EWorkbenchLayoutMementoCodecStatus::CorruptPayload, invalidId.status);

	const auto duplicate = CWorkbenchLayoutMementoCodec::Decode(
		R"json({"formatVersion":2,"activeContainers":{"sideBar":"same.container","panel":"same.container"},"panelAlignment":"center","focus":{},"parts":[],"containers":[],"views":[]})json");
	EXPECT_EQ(EWorkbenchLayoutMementoCodecStatus::CorruptPayload, duplicate.status);
}

TEST(WorkbenchLayoutMementoCodec, ParserRejectsTrailingInputAndExcessiveNesting)
{
	const auto trailing = CWorkbenchLayoutMementoCodec::Decode(
		R"json({"formatVersion":1,"panelAlignment":"center","focus":{},"parts":[],"containers":[],"views":[]} false)json");
	EXPECT_EQ(EWorkbenchLayoutMementoCodecStatus::CorruptPayload, trailing.status);

	std::string nested = R"json({"formatVersion":1,"panelAlignment":"center","focus":{},"parts":[],"containers":[],"views":[],"future":)json";
	nested.append(workbench::layout::kMaximumWorkbenchLayoutMementoDepth + 1, '[');
	nested += "0";
	nested.append(workbench::layout::kMaximumWorkbenchLayoutMementoDepth + 1, ']');
	nested += "}";
	const auto deep = CWorkbenchLayoutMementoCodec::Decode(nested);
	EXPECT_EQ(EWorkbenchLayoutMementoCodecStatus::CorruptPayload, deep.status);
}

TEST(WorkbenchLayoutMementoCodec, InvalidUtf8AndNumericFieldsFailClosed)
{
	std::string invalidUtf8 = R"json({"formatVersion":1,"panelAlignment":"center","focus":{},"parts":[],"containers":[],"views":[],"x":")json";
	invalidUtf8.push_back(static_cast<char>(0xc0));
	invalidUtf8 += R"json("})json";
	const auto badUtf8 = CWorkbenchLayoutMementoCodec::Decode(invalidUtf8);
	EXPECT_EQ(EWorkbenchLayoutMementoCodecStatus::CorruptPayload, badUtf8.status);

	const auto fractionalOrder = CWorkbenchLayoutMementoCodec::Decode(R"json({
		"formatVersion":1,"panelAlignment":"center","focus":{},"parts":[],
		"containers":[{"id":"terminal","location":"panel","order":1.5,"visible":true}],"views":[]
	})json");
	EXPECT_EQ(EWorkbenchLayoutMementoCodecStatus::CorruptPayload, fractionalOrder.status);

	const auto zeroExtent = CWorkbenchLayoutMementoCodec::Decode(R"json({
		"formatVersion":1,"panelAlignment":"center","focus":{},
		"parts":[{"id":"workbench.parts.panel","position":"bottom","visible":true,"extentDip":0}],
		"containers":[],"views":[]
	})json");
	EXPECT_EQ(EWorkbenchLayoutMementoCodecStatus::CorruptPayload, zeroExtent.status);
}

TEST(WorkbenchLayoutMementoCodec, PayloadLimitsAreTerminal)
{
	const std::string oversized(platform::storage::kMaximumStorageStringBytes + 1, ' ');
	const auto decoded = CWorkbenchLayoutMementoCodec::Decode(oversized);
	EXPECT_EQ(EWorkbenchLayoutMementoCodecStatus::PayloadTooLarge, decoded.status);

	auto snapshot = Sample();
	for (std::size_t index = 0; index < 600; ++index) {
		std::string id = "publisher." + std::to_string(index) + ".";
		id.append(130, 'x');
		snapshot.views.push_back({ std::move(id), "terminal", static_cast<std::int32_t>(index), true });
	}
	const auto encoded = CWorkbenchLayoutMementoCodec::Encode(snapshot);
	EXPECT_EQ(EWorkbenchLayoutMementoCodecStatus::PayloadTooLarge, encoded.status);
	EXPECT_TRUE(encoded.payload.empty());
}

TEST(WorkbenchLayoutMementoCodec, InvalidSnapshotDoesNotProduceAPartialPayload)
{
	auto snapshot = Sample();
	snapshot.parts.push_back(snapshot.parts.front());
	const auto encoded = CWorkbenchLayoutMementoCodec::Encode(snapshot);
	EXPECT_EQ(EWorkbenchLayoutMementoCodecStatus::InvalidSnapshot, encoded.status);
	EXPECT_TRUE(encoded.payload.empty());
}
