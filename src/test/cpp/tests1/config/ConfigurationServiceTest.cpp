/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include "config/BuiltinConfigurationDescriptors.h"
#include "config/CConfigurationService.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using config::BuiltinConfigurationDescriptors;
using config::CConfigurationService;
using config::ConfigurationChange;
using config::ConfigurationDescriptor;
using config::ConfigurationEntry;
using config::ConfigurationReplaceSources;
using config::ConfigurationReplaceSource;
using config::ConfigurationResult;
using config::ConfigurationSource;
using config::ConfigurationSourceReplacement;
using config::ConfigurationTarget;
using config::ConfigurationUpdate;
using config::ConfigurationValue;
using config::EConfigurationOutcome;
using config::EConfigurationScope;
using config::RestrictedConfigurationPolicy;

ConfigurationTarget Target(std::wstring profile = L"default", const wchar_t* workspace = nullptr, const wchar_t* folder = nullptr, const wchar_t* language = nullptr)
{
	ConfigurationTarget target;
	target.profileId = std::move(profile);
	if (workspace) {
		auto uri = platform::uri::Uri::Parse(workspace);
		EXPECT_TRUE(uri);
		target.workspaceUri = std::move(*uri.value);
	}
	if (folder) {
		auto uri = platform::uri::Uri::Parse(folder);
		EXPECT_TRUE(uri);
		target.folderUri = std::move(*uri.value);
	}
	if (language) {
		target.languageId = language;
	}
	return target;
}

ConfigurationSource Source(EConfigurationScope scope, const ConfigurationTarget& target = {}, std::string id = "test", std::int32_t priority = 0)
{
	return { scope, target, std::move(id), priority };
}

CConfigurationService Service()
{
	return CConfigurationService({
		{ "editor.tabSize", ConfigurationValue(4), { EConfigurationScope::Application, EConfigurationScope::Profile, EConfigurationScope::Workspace, EConfigurationScope::Folder, EConfigurationScope::LanguageOverride } },
		{ "editor.lineHeight", ConfigurationValue(20), { EConfigurationScope::Application, EConfigurationScope::Profile, EConfigurationScope::Workspace, EConfigurationScope::Folder, EConfigurationScope::LanguageOverride } },
	});
}

std::int64_t Integer(const CConfigurationService& service, const ConfigurationTarget& target, const char* key = "editor.tabSize")
{
	auto result = service.GetValue(key, target);
	EXPECT_EQ(EConfigurationOutcome::Applied, result.outcome);
	EXPECT_TRUE(result.value.has_value());
	return std::get<std::int64_t>(result.value->Value());
}

ConfigurationResult Apply(CConfigurationService& service, EConfigurationScope scope, const ConfigurationTarget& target, int value, const char* operation, std::optional<std::uint64_t> expected = std::nullopt)
{
	return service.Update({ Source(scope, target), "editor.tabSize", ConfigurationValue(value), operation, expected });
}

} // namespace

TEST(ConfigurationService, ReturnsDescriptorDefaultAndDetailedInspection)
{
	auto service = Service();
	const auto target = Target();

	EXPECT_EQ(4, Integer(service, target));
	auto inspected = service.Inspect("editor.tabSize", target);
	ASSERT_EQ(EConfigurationOutcome::Applied, inspected.outcome);
	ASSERT_EQ(1U, inspected.provenance.size());
	EXPECT_EQ(EConfigurationScope::Default, inspected.provenance.front().scope);
	EXPECT_EQ("descriptor", inspected.provenance.front().sourceId);
}

TEST(ConfigurationService, ReadSnapshotPreservesRequestOrderAndUsesEffectivePrecedence)
{
	auto service = Service();
	const auto target = Target(L"default", L"file:///C:/work", L"file:///C:/work/folder", L"cpp");

	ASSERT_EQ(EConfigurationOutcome::Applied,
		service.Update({ Source(EConfigurationScope::Application, {}, "application"), "editor.tabSize", ConfigurationValue(3), "application" }).outcome);
	ASSERT_EQ(EConfigurationOutcome::Applied,
		service.Update({ Source(EConfigurationScope::Profile, Target(), "profile"), "editor.tabSize", ConfigurationValue(5), "profile" }).outcome);
	ASSERT_EQ(EConfigurationOutcome::Applied,
		service.Update({ Source(EConfigurationScope::Workspace, Target(L"default", L"file:///C:/work"), "workspace"), "editor.tabSize", ConfigurationValue(6), "workspace" }).outcome);
	ASSERT_EQ(EConfigurationOutcome::Applied,
		service.Update({ Source(EConfigurationScope::Folder, Target(L"default", L"file:///C:/work", L"file:///C:/work/folder"), "folder"), "editor.tabSize", ConfigurationValue(7), "folder" }).outcome);
	ASSERT_EQ(EConfigurationOutcome::Applied,
		service.Update({ Source(EConfigurationScope::LanguageOverride, target, "language"), "editor.tabSize", ConfigurationValue(8), "language" }).outcome);

	const auto read = service.ReadSnapshot({ "editor.tabSize", "editor.lineHeight", "editor.tabSize" }, target);
	ASSERT_EQ(EConfigurationOutcome::Applied, read.outcome);
	ASSERT_TRUE(read.snapshot.has_value());
	EXPECT_EQ(5U, read.snapshot->revision);
	ASSERT_EQ(3U, read.snapshot->values.size());
	EXPECT_EQ(8, std::get<std::int64_t>(read.snapshot->values[0].Value()));
	EXPECT_EQ(20, std::get<std::int64_t>(read.snapshot->values[1].Value()));
	EXPECT_EQ(8, std::get<std::int64_t>(read.snapshot->values[2].Value()));
}

TEST(ConfigurationService, ReadSnapshotRejectsUnknownKeysAndInvalidTargetsWithoutPartialValues)
{
	auto service = Service();

	const auto unknown = service.ReadSnapshot({ "editor.tabSize", "extension.futureSetting" }, Target());
	EXPECT_EQ(EConfigurationOutcome::InvalidKey, unknown.outcome);
	EXPECT_FALSE(unknown.snapshot.has_value());

	ConfigurationTarget invalid;
	invalid.profileId = L"default";
	auto folder = platform::uri::Uri::Parse(L"file:///C:/work/folder");
	ASSERT_TRUE(folder.value.has_value());
	invalid.folderUri = std::move(*folder.value);
	const auto invalidTarget = service.ReadSnapshot({ "editor.tabSize" }, invalid);
	EXPECT_EQ(EConfigurationOutcome::InvalidScope, invalidTarget.outcome);
	EXPECT_FALSE(invalidTarget.snapshot.has_value());
}

TEST(ConfigurationService, ReadSnapshotRevisionAdvancesOnlyForCommittedStateChanges)
{
	auto service = Service();
	const auto profile = Target();
	const auto language = Target(L"default", nullptr, nullptr, L"cpp");
	const auto readRevision = [&](const ConfigurationTarget& target = Target()) {
		const auto read = service.ReadSnapshot({ "editor.tabSize" }, target);
		EXPECT_EQ(EConfigurationOutcome::Applied, read.outcome);
		EXPECT_TRUE(read.snapshot.has_value());
		return read.snapshot ? read.snapshot->revision : 0U;
	};

	EXPECT_EQ(0U, readRevision());
	const ConfigurationUpdate update { Source(EConfigurationScope::Profile, profile), "editor.tabSize", ConfigurationValue(8), "snapshot-update", 0 };
	EXPECT_EQ(EConfigurationOutcome::Applied, service.Update(update).outcome);
	EXPECT_EQ(1U, readRevision());
	EXPECT_EQ(EConfigurationOutcome::NoChange,
		service.Update({ Source(EConfigurationScope::Profile, profile), "editor.tabSize", ConfigurationValue(8), "snapshot-nochange", 1 }).outcome);
	EXPECT_EQ(1U, readRevision());
	EXPECT_EQ(EConfigurationOutcome::Replayed, service.Update(update).outcome);
	EXPECT_EQ(1U, readRevision());
	EXPECT_EQ(EConfigurationOutcome::Conflict,
		service.Update({ Source(EConfigurationScope::Profile, profile), "editor.tabSize", ConfigurationValue(9), "snapshot-conflict", 0 }).outcome);
	EXPECT_EQ(1U, readRevision());
	const auto replaced = service.ReplaceSource({ Source(EConfigurationScope::Profile, profile),
			{ { "editor.tabSize", ConfigurationValue(8) }, { "editor.lineHeight", ConfigurationValue(25) } },
			"snapshot-replace", 1 });
	EXPECT_EQ(EConfigurationOutcome::Applied, replaced.outcome);
	EXPECT_EQ(2U, replaced.revision);
	EXPECT_EQ(2U, readRevision());
	EXPECT_EQ(EConfigurationOutcome::NoChange,
		service.ReplaceSource({ Source(EConfigurationScope::Profile, profile),
			{ { "editor.tabSize", ConfigurationValue(8) }, { "editor.lineHeight", ConfigurationValue(25) } },
			"snapshot-replace-nochange", 2 }).outcome);
	EXPECT_EQ(2U, readRevision());

	const ConfigurationReplaceSources batch {
		{
			{ Source(EConfigurationScope::Profile, profile), { { "editor.tabSize", ConfigurationValue(6) }, { "editor.lineHeight", ConfigurationValue(30) } }, 2 },
			{ Source(EConfigurationScope::LanguageOverride, language), { { "editor.tabSize", ConfigurationValue(2) } }, 0 },
		},
		"snapshot-batch",
	};
	const auto batchApplied = service.ReplaceSources(batch);
	EXPECT_EQ(EConfigurationOutcome::Applied, batchApplied.outcome);
	ASSERT_EQ(2U, batchApplied.revisions.size());
	EXPECT_EQ(3U, batchApplied.revisions[0].revision);
	EXPECT_EQ(1U, batchApplied.revisions[1].revision);
	EXPECT_EQ(3U, readRevision(language));
	EXPECT_EQ(EConfigurationOutcome::NoChange,
		service.ReplaceSources({
			{
				{ Source(EConfigurationScope::Profile, profile), { { "editor.tabSize", ConfigurationValue(6) }, { "editor.lineHeight", ConfigurationValue(30) } }, 3 },
				{ Source(EConfigurationScope::LanguageOverride, language), { { "editor.tabSize", ConfigurationValue(2) } }, 1 },
			},
			"snapshot-batch-nochange",
		}).outcome);
	EXPECT_EQ(3U, readRevision(language));
	EXPECT_EQ(EConfigurationOutcome::Replayed, service.ReplaceSources(batch).outcome);
	EXPECT_EQ(3U, readRevision(language));
	const auto conflictingBatch = service.ReplaceSources({
		{
			{ Source(EConfigurationScope::Profile, profile), { { "editor.tabSize", ConfigurationValue(7) } }, 2 },
			{ Source(EConfigurationScope::LanguageOverride, language), { { "editor.tabSize", ConfigurationValue(3) } }, 1 },
		},
		"snapshot-batch-conflict",
	});
	EXPECT_EQ(EConfigurationOutcome::Conflict, conflictingBatch.outcome);
	EXPECT_EQ(3U, readRevision(language));
}

TEST(ConfigurationService, ReadSnapshotNeverCombinesValuesFromSeparateReplacementTransactions)
{
	auto service = Service();
	const auto target = Target();
	std::atomic_bool writerStarted = false;
	std::atomic_bool writerFinished = false;
	std::atomic_bool sawMixedSnapshot = false;

	std::thread writer([&] {
		writerStarted.store(true);
		for (int index = 0; index < 256; ++index) {
			const auto value = index % 2 == 0 ? 10 : 20;
			const auto lineHeight = index % 2 == 0 ? 100 : 200;
			const auto result = service.ReplaceSource({ Source(EConfigurationScope::Profile, target),
				{ { "editor.tabSize", ConfigurationValue(value) }, { "editor.lineHeight", ConfigurationValue(lineHeight) } },
				"snapshot-pair-" + std::to_string(index) });
			if (result.outcome != EConfigurationOutcome::Applied) {
				sawMixedSnapshot.store(true);
				break;
			}
		}
		writerFinished.store(true);
	});

	while (!writerStarted.load()) {
		std::this_thread::yield();
	}
	for (int index = 0; index < 4096 && !writerFinished.load(); ++index) {
		const auto read = service.ReadSnapshot({ "editor.tabSize", "editor.lineHeight" }, target);
		if (read.outcome != EConfigurationOutcome::Applied || !read.snapshot || read.snapshot->values.size() != 2U) {
			sawMixedSnapshot.store(true);
			break;
		}
		const auto tabSize = std::get<std::int64_t>(read.snapshot->values[0].Value());
		const auto lineHeight = std::get<std::int64_t>(read.snapshot->values[1].Value());
		if (!((tabSize == 4 && lineHeight == 20)
			|| (tabSize == 10 && lineHeight == 100)
			|| (tabSize == 20 && lineHeight == 200))) {
			sawMixedSnapshot.store(true);
			break;
		}
	}
	writer.join();
	EXPECT_FALSE(sawMixedSnapshot.load());
}

TEST(ConfigurationService, DoesNotRegisterDescriptorWhoseDefaultViolatesItsDeclarativeConstraint)
{
	CConfigurationService service({
		{ "network.invalidDefault", ConfigurationValue(L"outside"), { EConfigurationScope::Profile },
			{ config::EConfigurationValueKind::String, 8, { L"inside" } } },
	});

	EXPECT_EQ(EConfigurationOutcome::InvalidKey,
		service.GetValue("network.invalidDefault", Target()).outcome);
}

TEST(ConfigurationService, ResolvesAllScopesInDocumentedPrecedenceOrder)
{
	auto service = Service();
	const auto application = ConfigurationTarget{};
	const auto profile = Target();
	const auto workspace = Target(L"default", L"file:///C:/work");
	const auto folder = Target(L"default", L"file:///C:/work", L"file:///C:/work/one");
	const auto language = Target(L"default", L"file:///C:/work", L"file:///C:/work/one", L"cpp");

	EXPECT_EQ(EConfigurationOutcome::Applied, Apply(service, EConfigurationScope::Application, application, 1, "application").outcome);
	EXPECT_EQ(EConfigurationOutcome::Applied, Apply(service, EConfigurationScope::Profile, profile, 2, "profile").outcome);
	EXPECT_EQ(EConfigurationOutcome::Applied, Apply(service, EConfigurationScope::Workspace, workspace, 3, "workspace").outcome);
	EXPECT_EQ(EConfigurationOutcome::Applied, Apply(service, EConfigurationScope::Folder, folder, 4, "folder").outcome);
	EXPECT_EQ(EConfigurationOutcome::Applied, Apply(service, EConfigurationScope::LanguageOverride, language, 5, "language").outcome);

	EXPECT_EQ(1, Integer(service, application));
	EXPECT_EQ(2, Integer(service, profile));
	EXPECT_EQ(3, Integer(service, workspace));
	EXPECT_EQ(4, Integer(service, folder));
	EXPECT_EQ(5, Integer(service, language));

	auto inspection = service.Inspect("editor.tabSize", language);
	ASSERT_EQ(6U, inspection.provenance.size());
	EXPECT_EQ(EConfigurationScope::LanguageOverride, inspection.provenance.back().scope);
}

TEST(ConfigurationService, KeepsFolderAndLanguageOverridesIsolatedByUriIdentity)
{
	auto service = Service();
	const auto folderOne = Target(L"default", L"file:///C:/work", L"file:///C:/work/one");
	const auto folderTwo = Target(L"default", L"file:///C:/work", L"file:///C:/work/two");
	const auto cpp = Target(L"default", L"file:///C:/work", L"file:///C:/work/one", L"cpp");
	const auto markdown = Target(L"default", L"file:///C:/work", L"file:///C:/work/one", L"markdown");

	ASSERT_EQ(EConfigurationOutcome::Applied, Apply(service, EConfigurationScope::Folder, folderOne, 8, "folder-one").outcome);
	ASSERT_EQ(EConfigurationOutcome::Applied, Apply(service, EConfigurationScope::LanguageOverride, cpp, 2, "cpp").outcome);
	EXPECT_EQ(8, Integer(service, folderOne));
	EXPECT_EQ(4, Integer(service, folderTwo));
	EXPECT_EQ(2, Integer(service, cpp));
	EXPECT_EQ(8, Integer(service, markdown));
}

TEST(ConfigurationService, RemovesSourceValueAndFallsBackToLowerPrecedence)
{
	auto service = Service();
	const auto target = Target();
	ASSERT_EQ(EConfigurationOutcome::Applied, Apply(service, EConfigurationScope::Profile, target, 9, "set").outcome);
	EXPECT_EQ(9, Integer(service, target));

	auto removed = service.Update({ Source(EConfigurationScope::Profile, target), "editor.tabSize", std::nullopt, "remove", 1 });
	EXPECT_EQ(EConfigurationOutcome::Applied, removed.outcome);
	EXPECT_EQ(2U, removed.revision);
	EXPECT_EQ(4, Integer(service, target));
}

TEST(ConfigurationService, SuppressesEventsForShadowedSourceUpdates)
{
	auto service = Service();
	const auto profile = Target();
	ASSERT_EQ(EConfigurationOutcome::Applied,
		service.Update({ Source(EConfigurationScope::Profile, profile, "high", 2),
			"editor.tabSize", ConfigurationValue(7), "high" }).outcome);
	int callbackCount = 0;
	auto subscription = service.Subscribe([&callbackCount](const auto&) { ++callbackCount; });

	EXPECT_EQ(EConfigurationOutcome::Applied,
		service.Update({ Source(EConfigurationScope::Profile, profile, "low", 1),
			"editor.tabSize", ConfigurationValue(6), "low" }).outcome);
	EXPECT_EQ(0, callbackCount);
	EXPECT_EQ(7, Integer(service, profile));
}

TEST(ConfigurationService, EnforcesSourceRevisionAndReplaysOperationIds)
{
	auto service = Service();
	const auto target = Target();
	auto first = Apply(service, EConfigurationScope::Profile, target, 7, "operation", 0);
	ASSERT_EQ(EConfigurationOutcome::Applied, first.outcome);
	EXPECT_EQ(1U, first.revision);

	auto replay = Apply(service, EConfigurationScope::Profile, target, 7, "operation", 0);
	EXPECT_EQ(EConfigurationOutcome::Replayed, replay.outcome);
	EXPECT_EQ(1U, replay.revision);

	auto conflict = Apply(service, EConfigurationScope::Profile, target, 8, "another-operation", 0);
	EXPECT_EQ(EConfigurationOutcome::Conflict, conflict.outcome);
	EXPECT_EQ(1U, conflict.revision);

	auto replayedConflict = Apply(service, EConfigurationScope::Profile, target, 8, "another-operation", 0);
	EXPECT_EQ(EConfigurationOutcome::Replayed, replayedConflict.outcome);
	EXPECT_EQ(1U, replayedConflict.revision);

	auto noChange = Apply(service, EConfigurationScope::Profile, target, 7, "no-change", 1);
	EXPECT_EQ(EConfigurationOutcome::NoChange, noChange.outcome);
	EXPECT_EQ(1U, noChange.revision);
	auto replayedNoChange = Apply(service, EConfigurationScope::Profile, target, 7, "no-change", 1);
	EXPECT_EQ(EConfigurationOutcome::Replayed, replayedNoChange.outcome);
	EXPECT_EQ(1U, replayedNoChange.revision);

	auto reusedId = Apply(service, EConfigurationScope::Profile, target, 9, "operation", 0);
	EXPECT_EQ(EConfigurationOutcome::OperationIdConflict, reusedId.outcome);
	EXPECT_EQ(1U, reusedId.revision);
}

TEST(ConfigurationService, ReplaceSourceReplayIdentityProtectsAllTerminalResults)
{
	auto service = Service();
	const auto target = Target();
	const auto source = Source(EConfigurationScope::Profile, target);

	ConfigurationReplaceSource empty { source, {}, "empty", 0 };
	auto noChange = service.ReplaceSource(empty);
	EXPECT_EQ(EConfigurationOutcome::NoChange, noChange.outcome);
	EXPECT_EQ(EConfigurationOutcome::Replayed, service.ReplaceSource(empty).outcome);

	ConfigurationReplaceSource different { source, { { "editor.tabSize", ConfigurationValue(6) } }, "empty", 0 };
	EXPECT_EQ(EConfigurationOutcome::OperationIdConflict, service.ReplaceSource(different).outcome);

	ConfigurationReplaceSource set { source, { { "editor.tabSize", ConfigurationValue(6) } }, "set", 0 };
	EXPECT_EQ(EConfigurationOutcome::Applied, service.ReplaceSource(set).outcome);
	ConfigurationReplaceSource stale { source, { { "editor.tabSize", ConfigurationValue(7) } }, "stale", 0 };
	EXPECT_EQ(EConfigurationOutcome::Conflict, service.ReplaceSource(stale).outcome);
	EXPECT_EQ(EConfigurationOutcome::Replayed, service.ReplaceSource(stale).outcome);
}

TEST(ConfigurationService, AtomicallyReplacesBaseAndLanguageOverrideSources)
{
	auto service = Service();
	const auto profile = Target();
	const auto cpp = Target(L"default", nullptr, nullptr, L"cpp");
	const auto profileSource = Source(EConfigurationScope::Profile, profile, "settings-json");
	const auto languageSource = Source(EConfigurationScope::LanguageOverride, cpp, "settings-json");
	int notificationCount = 0;
	std::size_t deliveredChanges = 0;
	auto subscription = service.Subscribe([&](const auto& changes) {
		++notificationCount;
		deliveredChanges = changes.size();
	});

	ConfigurationReplaceSources request {
		{
			ConfigurationSourceReplacement { profileSource, { { "editor.tabSize", ConfigurationValue(8) } }, 0 },
			ConfigurationSourceReplacement { languageSource, { { "editor.tabSize", ConfigurationValue(2) } }, 0 },
		},
		"settings-json-load-1",
	};
	const auto applied = service.ReplaceSources(request);
	ASSERT_EQ(EConfigurationOutcome::Applied, applied.outcome);
	ASSERT_EQ(2U, applied.revisions.size());
	EXPECT_EQ(1U, applied.revisions[0].revision);
	EXPECT_EQ(1U, applied.revisions[1].revision);
	EXPECT_EQ(8, Integer(service, profile));
	EXPECT_EQ(2, Integer(service, cpp));
	EXPECT_EQ(1, notificationCount);
	EXPECT_EQ(2U, deliveredChanges);

	const auto replayed = service.ReplaceSources(request);
	EXPECT_EQ(EConfigurationOutcome::Replayed, replayed.outcome);
	ASSERT_EQ(2U, replayed.revisions.size());
	EXPECT_EQ(1U, replayed.revisions[0].revision);
	EXPECT_EQ(1U, replayed.revisions[1].revision);
	EXPECT_EQ(1, notificationCount);

	request.replacements[0].entries[0].value = ConfigurationValue(9);
	const auto reused = service.ReplaceSources(request);
	EXPECT_EQ(EConfigurationOutcome::OperationIdConflict, reused.outcome);
	EXPECT_EQ(8, Integer(service, profile));
	EXPECT_EQ(2, Integer(service, cpp));
}

TEST(ConfigurationService, RejectsWholeBatchBeforeMutatingAnySource)
{
	CConfigurationService service({
		{ "editor.tabSize", ConfigurationValue(4), { EConfigurationScope::Profile, EConfigurationScope::LanguageOverride } },
		{ "workbench.applicationOnly", ConfigurationValue(false), { EConfigurationScope::Application } },
	});
	const auto profile = Target();
	const auto cpp = Target(L"default", nullptr, nullptr, L"cpp");
	ASSERT_EQ(EConfigurationOutcome::Applied,
		Apply(service, EConfigurationScope::Profile, profile, 6, "initial-profile").outcome);

	ConfigurationReplaceSources invalid {
		{
			ConfigurationSourceReplacement {
				Source(EConfigurationScope::Profile, profile),
				{ { "editor.tabSize", ConfigurationValue(7) } },
				1,
			},
			ConfigurationSourceReplacement {
				Source(EConfigurationScope::LanguageOverride, cpp),
				{ { "workbench.applicationOnly", ConfigurationValue(true) } },
				0,
			},
		},
		"invalid-batch",
	};
	EXPECT_EQ(EConfigurationOutcome::InvalidScope, service.ReplaceSources(invalid).outcome);
	EXPECT_EQ(6, Integer(service, profile));
	EXPECT_EQ(6, Integer(service, cpp));

	invalid.replacements[1].entries = { { "editor.tabSize", ConfigurationValue(2) } };
	invalid.replacements[1].expectedRevision = 1;
	invalid.operationId = "conflicting-batch";
	const auto conflict = service.ReplaceSources(invalid);
	EXPECT_EQ(EConfigurationOutcome::Conflict, conflict.outcome);
	ASSERT_EQ(2U, conflict.revisions.size());
	EXPECT_EQ(1U, conflict.revisions[0].revision);
	EXPECT_EQ(0U, conflict.revisions[1].revision);
	EXPECT_EQ(6, Integer(service, profile));
	EXPECT_EQ(6, Integer(service, cpp));
	EXPECT_EQ(EConfigurationOutcome::Replayed, service.ReplaceSources(invalid).outcome);
}

TEST(ConfigurationService, FileSourceKeepsLatentEntriesButOnlyKnownKeysParticipateInConfiguration)
{
	auto service = Service();
	const auto profile = Target();
	const auto source = Source(EConfigurationScope::Profile, profile, "settings-json");
	int notificationCount = 0;
	std::vector<config::ConfigurationChange> delivered;
	auto subscription = service.Subscribe([&](const auto& changes) {
		++notificationCount;
		delivered = changes;
	});

	const auto applied = service.ReplaceSources({
		{
			{ source, {
				{ "editor.tabSize", ConfigurationValue(8) },
				{ "extension.futureSetting", ConfigurationValue(true) },
			}, 0 },
		},
		"mixed-known-and-latent",
	});
	ASSERT_EQ(EConfigurationOutcome::Applied, applied.outcome);
	ASSERT_EQ(1U, applied.revisions.size());
	EXPECT_EQ(1U, applied.revisions.front().revision);
	EXPECT_EQ(8, Integer(service, profile));
	EXPECT_EQ(EConfigurationOutcome::InvalidKey,
		service.GetValue("extension.futureSetting", profile).outcome);
	EXPECT_EQ(EConfigurationOutcome::InvalidKey,
		service.Inspect("extension.futureSetting", profile).outcome);
	ASSERT_EQ(1, notificationCount);
	ASSERT_EQ(1U, delivered.size());
	EXPECT_EQ("editor.tabSize", delivered.front().key);

	const auto replayed = service.ReplaceSources({
		{
			{ source, {
				{ "editor.tabSize", ConfigurationValue(8) },
				{ "extension.futureSetting", ConfigurationValue(true) },
			}, 0 },
		},
		"mixed-known-and-latent",
	});
	EXPECT_EQ(EConfigurationOutcome::Replayed, replayed.outcome);
	EXPECT_EQ(1, notificationCount);
}

TEST(ConfigurationService, EnforcesDeclarativeConstraintsBeforeChangingStateOrNotifying)
{
	CConfigurationService service({
		{ "network.mode", ConfigurationValue(L"fallback"), { EConfigurationScope::Profile },
			{ config::EConfigurationValueKind::String, 16, { L"off", L"fallback" } } },
		{ "network.timeout", ConfigurationValue(1000), { EConfigurationScope::Profile },
			{ config::EConfigurationValueKind::Integer, std::nullopt, {}, 100, 5000 } },
		{ "network.bypass", ConfigurationValue(ConfigurationValue::Array{}), { EConfigurationScope::Profile },
			{ config::EConfigurationValueKind::Array, std::nullopt, {}, std::nullopt, std::nullopt, 2, true, 8 } },
	});
	const auto profile = Target();
	int callbackCount = 0;
	auto subscription = service.Subscribe([&](const auto&) { ++callbackCount; });

	EXPECT_EQ(EConfigurationOutcome::InvalidValue, service.Update({
		Source(EConfigurationScope::Profile, profile), "network.mode", ConfigurationValue(true), "wrong-kind" }).outcome);
	EXPECT_EQ(EConfigurationOutcome::InvalidValue, service.Update({
		Source(EConfigurationScope::Profile, profile), "network.mode", ConfigurationValue(L"override"), "wrong-enum" }).outcome);
	EXPECT_EQ(EConfigurationOutcome::InvalidValue, service.Update({
		Source(EConfigurationScope::Profile, profile), "network.timeout", ConfigurationValue(99), "wrong-range" }).outcome);
	EXPECT_EQ(EConfigurationOutcome::InvalidValue, service.Update({
		Source(EConfigurationScope::Profile, profile), "network.bypass",
		ConfigurationValue(ConfigurationValue::Array { ConfigurationValue(1) }), "wrong-array" }).outcome);

	EXPECT_EQ(0, callbackCount);
	auto mode = service.GetValue("network.mode", profile);
	ASSERT_TRUE(mode.value.has_value());
	EXPECT_EQ(L"fallback", std::get<std::wstring>(mode.value->Value()));
	auto timeout = service.GetValue("network.timeout", profile);
	ASSERT_TRUE(timeout.value.has_value());
	EXPECT_EQ(1000, std::get<std::int64_t>(timeout.value->Value()));

	const auto applied = service.Update({
		Source(EConfigurationScope::Profile, profile), "network.timeout", ConfigurationValue(2500), "valid" });
	EXPECT_EQ(EConfigurationOutcome::Applied, applied.outcome);
	EXPECT_EQ(1, callbackCount);
}

TEST(ConfigurationService, RejectsInvalidConstrainedReplacementWithoutChangingLastValidSource)
{
	CConfigurationService service({
		{ "network.timeout", ConfigurationValue(1000), { EConfigurationScope::Profile },
			{ config::EConfigurationValueKind::Integer, std::nullopt, {}, 100, 5000 } },
		{ "network.mode", ConfigurationValue(L"fallback"), { EConfigurationScope::Profile },
			{ config::EConfigurationValueKind::String, 16, { L"off", L"fallback" } } },
	});
	const auto profile = Target();
	const auto source = Source(EConfigurationScope::Profile, profile, "settings-json");
	ASSERT_EQ(EConfigurationOutcome::Applied, service.ReplaceSource({ source,
		{ { "network.timeout", ConfigurationValue(2500) }, { "network.mode", ConfigurationValue(L"off") } },
		"initial", 0 }).outcome);
	int callbackCount = 0;
	auto subscription = service.Subscribe([&](const auto&) { ++callbackCount; });

	EXPECT_EQ(EConfigurationOutcome::InvalidValue, service.ReplaceSource({ source,
		{ { "network.timeout", ConfigurationValue(7500) }, { "network.mode", ConfigurationValue(L"fallback") } },
		"invalid", 1 }).outcome);
	EXPECT_EQ(0, callbackCount);
	EXPECT_EQ(2500, Integer(service, profile, "network.timeout"));
	auto mode = service.GetValue("network.mode", profile);
	ASSERT_TRUE(mode.value.has_value());
	EXPECT_EQ(L"off", std::get<std::wstring>(mode.value->Value()));
}

TEST(ConfigurationService, UnknownOnlyFileSourceHasDurableRevisionCasReplayAndClearSemantics)
{
	auto service = Service();
	const auto source = Source(EConfigurationScope::Application, {}, "extensions-json");

	ConfigurationReplaceSource initial {
		source,
		{ { "extension.futureSetting", ConfigurationValue(true) } },
		"latent-initial",
		0,
	};
	const auto first = service.ReplaceSource(initial);
	ASSERT_EQ(EConfigurationOutcome::Applied, first.outcome);
	EXPECT_EQ(1U, first.revision);
	EXPECT_EQ(EConfigurationOutcome::Replayed, service.ReplaceSource(initial).outcome);

	ConfigurationReplaceSource stale {
		source,
		{ { "extension.futureSetting", ConfigurationValue(false) } },
		"latent-stale",
		0,
	};
	EXPECT_EQ(EConfigurationOutcome::Conflict, service.ReplaceSource(stale).outcome);
	EXPECT_EQ(EConfigurationOutcome::Replayed, service.ReplaceSource(stale).outcome);

	ConfigurationReplaceSource clear { source, {}, "latent-clear", 1 };
	const auto cleared = service.ReplaceSource(clear);
	EXPECT_EQ(EConfigurationOutcome::Applied, cleared.outcome);
	EXPECT_EQ(2U, cleared.revision);
	EXPECT_EQ(EConfigurationOutcome::Replayed, service.ReplaceSource(clear).outcome);
	EXPECT_EQ(EConfigurationOutcome::InvalidKey,
		service.GetValue("extension.futureSetting", {}).outcome);
}

TEST(ConfigurationService, UnknownEntriesDoNotBypassKnownScopeValidationOrAtomicity)
{
	CConfigurationService service({
		{ "editor.tabSize", ConfigurationValue(4), { EConfigurationScope::Profile } },
		{ "workbench.applicationOnly", ConfigurationValue(false), { EConfigurationScope::Application } },
	});
	const auto profile = Target();
	const auto source = Source(EConfigurationScope::Profile, profile, "settings-json");
	ASSERT_EQ(EConfigurationOutcome::Applied,
		service.ReplaceSource({ source, { { "editor.tabSize", ConfigurationValue(6) } }, "initial", 0 }).outcome);

	const auto invalid = service.ReplaceSources({
		{
			{ source, {
				{ "editor.tabSize", ConfigurationValue(7) },
				{ "extension.futureSetting", ConfigurationValue(true) },
				{ "workbench.applicationOnly", ConfigurationValue(true) },
			}, 1 },
		},
		"known-scope-error",
	});
	EXPECT_EQ(EConfigurationOutcome::InvalidScope, invalid.outcome);
	EXPECT_EQ(6, Integer(service, profile));

	const auto latentOnly = service.ReplaceSource({
		source,
		{ { "extension.futureSetting", ConfigurationValue(true) } },
		"latent-replaces-known",
		1,
	});
	EXPECT_EQ(EConfigurationOutcome::Applied, latentOnly.outcome);
	EXPECT_EQ(2U, latentOnly.revision);
	EXPECT_EQ(4, Integer(service, profile));
}

TEST(ConfigurationService, BatchCanRemoveLanguageOverrideWithoutChangingBaseRevision)
{
	auto service = Service();
	const auto profile = Target();
	const auto cpp = Target(L"default", nullptr, nullptr, L"cpp");
	const auto profileSource = Source(EConfigurationScope::Profile, profile, "settings-json");
	const auto languageSource = Source(EConfigurationScope::LanguageOverride, cpp, "settings-json");
	ASSERT_EQ(EConfigurationOutcome::Applied,
		service.ReplaceSources({
			{
				{ profileSource, { { "editor.tabSize", ConfigurationValue(8) } }, 0 },
				{ languageSource, { { "editor.tabSize", ConfigurationValue(2) } }, 0 },
			},
			"language-add",
		}).outcome);

	const auto removed = service.ReplaceSources({
		{
			{ profileSource, { { "editor.tabSize", ConfigurationValue(8) } }, 1 },
			{ languageSource, {}, 1 },
		},
		"language-remove",
	});
	ASSERT_EQ(EConfigurationOutcome::Applied, removed.outcome);
	ASSERT_EQ(2U, removed.revisions.size());
	EXPECT_EQ(1U, removed.revisions[0].revision);
	EXPECT_EQ(2U, removed.revisions[1].revision);
	EXPECT_EQ(8, Integer(service, profile));
	EXPECT_EQ(8, Integer(service, cpp));
}

TEST(ConfigurationService, SourcePriorityAndSourceIdDefineSameLayerPrecedence)
{
	auto service = Service();
	const auto target = Target();

	EXPECT_EQ(EConfigurationOutcome::Applied, service.Update({ Source(EConfigurationScope::Profile, target, "low", 1), "editor.tabSize", ConfigurationValue(3), "low-first" }).outcome);
	EXPECT_EQ(EConfigurationOutcome::Applied, service.Update({ Source(EConfigurationScope::Profile, target, "low", 1), "editor.tabSize", ConfigurationValue(4), "low-second" }).outcome);
	EXPECT_EQ(EConfigurationOutcome::Applied, service.Update({ Source(EConfigurationScope::Profile, target, "high", 2), "editor.tabSize", ConfigurationValue(5), "high" }).outcome);
	EXPECT_EQ(5, Integer(service, target));

	EXPECT_EQ(EConfigurationOutcome::Applied, service.Update({ Source(EConfigurationScope::Profile, target, "alpha", 3), "editor.tabSize", ConfigurationValue(6), "alpha" }).outcome);
	EXPECT_EQ(EConfigurationOutcome::Applied, service.Update({ Source(EConfigurationScope::Profile, target, "zulu", 3), "editor.tabSize", ConfigurationValue(7), "zulu" }).outcome);
	EXPECT_EQ(EConfigurationOutcome::Applied, service.Update({ Source(EConfigurationScope::Profile, target, "alpha", 3), "editor.tabSize", ConfigurationValue(8), "alpha-second" }).outcome);
	EXPECT_EQ(7, Integer(service, target));

	auto inspection = service.Inspect("editor.tabSize", target);
	ASSERT_FALSE(inspection.provenance.empty());
	EXPECT_EQ("zulu", inspection.provenance.back().sourceId);
	EXPECT_EQ(3, inspection.provenance.back().priority);
}

TEST(ConfigurationService, ListenerExceptionsDoNotSuppressLaterListenersOrUpdateResult)
{
	auto service = Service();
	const auto target = Target();
	int successfulListenerCalls = 0;
	auto throwingListener = service.Subscribe([](const auto&) { throw std::runtime_error("expected listener failure"); });
	auto successfulListener = service.Subscribe([&successfulListenerCalls](const auto&) { ++successfulListenerCalls; });

	auto result = Apply(service, EConfigurationScope::Profile, target, 8, "listener-isolation");
	EXPECT_EQ(EConfigurationOutcome::Applied, result.outcome);
	EXPECT_EQ(1, successfulListenerCalls);
	EXPECT_EQ(8, Integer(service, target));
}

TEST(ConfigurationService, PermitsListenerReentryAndUnsubscriptionDuringDelivery)
{
	auto service = Service();
	const auto target = Target();
	int firstCount = 0;
	int secondCount = 0;
	config::ConfigurationSubscription second;
	auto first = service.Subscribe([&](const auto&) {
		++firstCount;
		second.Reset();
		if (firstCount == 1) {
			auto result = service.Update({ Source(EConfigurationScope::Profile, target), "editor.lineHeight", ConfigurationValue(30), "nested" });
			EXPECT_EQ(EConfigurationOutcome::Applied, result.outcome);
		}
	});
	second = service.Subscribe([&](const auto&) { ++secondCount; });

	EXPECT_EQ(EConfigurationOutcome::Applied, Apply(service, EConfigurationScope::Profile, target, 8, "outer").outcome);
	EXPECT_EQ(2, firstCount);
	EXPECT_EQ(0, secondCount);
}

TEST(ConfigurationService, RejectsInvalidReplacementWithoutDiscardingLastValidSource)
{
	auto service = Service();
	const auto target = Target();
	ASSERT_EQ(EConfigurationOutcome::Applied, Apply(service, EConfigurationScope::Profile, target, 6, "initial").outcome);

	ConfigurationReplaceSource invalid {
		Source(EConfigurationScope::Profile, target),
		{ { "editor.tabSize", ConfigurationValue(3) }, { "editor.lineHeight", ConfigurationValue(std::numeric_limits<double>::infinity()) } },
		"invalid-replacement",
		1,
	};
	auto result = service.ReplaceSource(invalid);
	EXPECT_EQ(EConfigurationOutcome::InvalidValue, result.outcome);
	EXPECT_EQ(6, Integer(service, target));
}

TEST(ConfigurationService, RejectsIrrelevantTargetFieldsThatWouldAliasAConfigurationLayer)
{
	auto service = Service();
	const auto profile = Target();
	const auto workspace = Target(L"default", L"file:///C:/work");
	const auto folder = Target(L"default", L"file:///C:/work", L"file:///C:/work/one");
	const auto language = Target(L"default", L"file:///C:/work", L"file:///C:/work/one", L"cpp");

	EXPECT_EQ(EConfigurationOutcome::InvalidSource,
		Apply(service, EConfigurationScope::Application, profile, 1, "bad-application").outcome);
	EXPECT_EQ(EConfigurationOutcome::InvalidSource,
		Apply(service, EConfigurationScope::Profile, workspace, 2, "bad-profile").outcome);
	EXPECT_EQ(EConfigurationOutcome::InvalidSource,
		Apply(service, EConfigurationScope::Workspace, folder, 3, "bad-workspace").outcome);
	EXPECT_EQ(EConfigurationOutcome::InvalidSource,
		Apply(service, EConfigurationScope::Folder, language, 4, "bad-folder").outcome);
	EXPECT_EQ(EConfigurationOutcome::Applied,
		Apply(service, EConfigurationScope::LanguageOverride, language, 5, "valid-language").outcome);
}

TEST(ConfigurationService, RejectsConfigurationValuesBeyondTheSemanticDepthLimit)
{
	auto service = Service();
	ConfigurationValue nested(1);
	for (std::size_t depth = 0; depth < 65; ++depth) {
		ConfigurationValue::Array parent;
		parent.emplace_back(std::move(nested));
		nested = ConfigurationValue(std::move(parent));
	}

	auto result = service.Update({ Source(EConfigurationScope::Profile, Target()),
		"editor.tabSize", std::move(nested), "too-deep" });
	EXPECT_EQ(EConfigurationOutcome::InvalidValue, result.outcome);
	EXPECT_EQ(4, Integer(service, Target()));
}

TEST(ConfigurationService, RejectsUnpairedUtf16SurrogatesInValuesAndObjectKeys)
{
	const std::wstring unpairedHigh(1, static_cast<wchar_t>(0xd800));
	const std::wstring unpairedLow(1, static_cast<wchar_t>(0xdc00));
	EXPECT_FALSE(ConfigurationValue(unpairedHigh).IsValid());
	EXPECT_FALSE(ConfigurationValue(unpairedLow).IsValid());
	EXPECT_FALSE(ConfigurationValue(ConfigurationValue::Object {
		{ unpairedHigh, ConfigurationValue(true) },
	}).IsValid());
	EXPECT_TRUE(ConfigurationValue(std::wstring { static_cast<wchar_t>(0xd83d), static_cast<wchar_t>(0xde00) }).IsValid());
}

TEST(ConfigurationService, SubscriptionCleanupContainsUnsubscribeFailures)
{
	int calls = 0;
	config::ConfigurationSubscription subscription([&calls]() {
		++calls;
		throw std::runtime_error("expected cleanup failure");
	});

	EXPECT_NO_THROW(subscription.Reset());
	EXPECT_EQ(1, calls);
	EXPECT_FALSE(subscription.IsActive());
	EXPECT_NO_THROW(subscription.Reset());
	EXPECT_EQ(1, calls);
}

TEST(ConfigurationService, RestrictedWorkspaceAndFolderValuesAreWithheldWhileUntrusted)
{
	auto service = Service();
	const auto profile = Target();
	const auto workspace = Target(L"default", L"file:///C:/work");
	const auto folder = Target(L"default", L"file:///C:/work", L"file:///C:/work/one");

	ASSERT_EQ(EConfigurationOutcome::Applied, Apply(service, EConfigurationScope::Profile, profile, 5, "profile").outcome);
	ASSERT_EQ(EConfigurationOutcome::Applied, Apply(service, EConfigurationScope::Workspace, workspace, 6, "workspace").outcome);
	ASSERT_EQ(EConfigurationOutcome::Applied, Apply(service, EConfigurationScope::Folder, folder, 7, "folder").outcome);
	EXPECT_EQ(7, Integer(service, folder));

	ASSERT_EQ(EConfigurationOutcome::Applied,
		service.ApplyRestrictedConfigurations({ false, { "editor.tabSize" }, workspace }).outcome);

	EXPECT_EQ(5, Integer(service, workspace));
	EXPECT_EQ(5, Integer(service, folder));
}

TEST(ConfigurationService, GrantingAndRevokingTrustTogglesTheRestrictedWorkspaceValue)
{
	auto service = Service();
	const auto profile = Target();
	const auto workspace = Target(L"default", L"file:///C:/work");
	ASSERT_EQ(EConfigurationOutcome::Applied, Apply(service, EConfigurationScope::Profile, profile, 5, "profile").outcome);
	ASSERT_EQ(EConfigurationOutcome::Applied, Apply(service, EConfigurationScope::Workspace, workspace, 6, "workspace").outcome);

	ASSERT_EQ(EConfigurationOutcome::Applied,
		service.ApplyRestrictedConfigurations({ false, { "editor.tabSize" }, workspace }).outcome);
	EXPECT_EQ(5, Integer(service, workspace));

	ASSERT_EQ(EConfigurationOutcome::Applied,
		service.ApplyRestrictedConfigurations({ true, { "editor.tabSize" }, workspace }).outcome);
	EXPECT_EQ(6, Integer(service, workspace));

	ASSERT_EQ(EConfigurationOutcome::Applied,
		service.ApplyRestrictedConfigurations({ false, { "editor.tabSize" }, workspace }).outcome);
	EXPECT_EQ(5, Integer(service, workspace));
}

TEST(ConfigurationService, NonRestrictedKeyIsUnaffectedByAnUntrustedRestrictedPolicy)
{
	auto service = Service();
	const auto workspace = Target(L"default", L"file:///C:/work");
	ASSERT_EQ(EConfigurationOutcome::Applied,
		service.Update({ Source(EConfigurationScope::Workspace, workspace), "editor.lineHeight", ConfigurationValue(24), "workspace-line-height" }).outcome);

	ASSERT_EQ(EConfigurationOutcome::Applied,
		service.ApplyRestrictedConfigurations({ false, { "editor.tabSize" }, workspace }).outcome);

	auto lineHeight = service.GetValue("editor.lineHeight", workspace);
	ASSERT_EQ(EConfigurationOutcome::Applied, lineHeight.outcome);
	ASSERT_TRUE(lineHeight.value.has_value());
	EXPECT_EQ(24, std::get<std::int64_t>(lineHeight.value->Value()));
}

TEST(ConfigurationService, InspectReportsWithheldWorkspaceProvenanceWhileTheEffectiveValueFallsBack)
{
	auto service = Service();
	const auto profile = Target();
	const auto workspace = Target(L"default", L"file:///C:/work");
	ASSERT_EQ(EConfigurationOutcome::Applied, Apply(service, EConfigurationScope::Profile, profile, 5, "profile").outcome);
	ASSERT_EQ(EConfigurationOutcome::Applied, Apply(service, EConfigurationScope::Workspace, workspace, 6, "workspace").outcome);
	ASSERT_EQ(EConfigurationOutcome::Applied,
		service.ApplyRestrictedConfigurations({ false, { "editor.tabSize" }, workspace }).outcome);

	auto inspection = service.Inspect("editor.tabSize", workspace);
	ASSERT_EQ(EConfigurationOutcome::Applied, inspection.outcome);
	ASSERT_TRUE(inspection.effectiveValue.has_value());
	EXPECT_EQ(5, std::get<std::int64_t>(inspection.effectiveValue->Value()));

	const auto workspaceEntry = std::find_if(inspection.provenance.begin(), inspection.provenance.end(),
		[](const auto& entry) { return entry.scope == EConfigurationScope::Workspace; });
	ASSERT_NE(inspection.provenance.end(), workspaceEntry);
	EXPECT_TRUE(workspaceEntry->withheld);
	EXPECT_EQ(6, std::get<std::int64_t>(workspaceEntry->value.Value()));
}

TEST(ConfigurationService, DefaultApplicationAndProfileContributionsAreNeverWithheld)
{
	auto service = Service();
	const auto application = ConfigurationTarget{};
	const auto profile = Target();
	ASSERT_EQ(EConfigurationOutcome::Applied, Apply(service, EConfigurationScope::Application, application, 2, "application").outcome);
	ASSERT_EQ(EConfigurationOutcome::Applied, Apply(service, EConfigurationScope::Profile, profile, 5, "profile").outcome);
	ASSERT_EQ(EConfigurationOutcome::Applied,
		service.ApplyRestrictedConfigurations({ false, { "editor.tabSize" }, profile }).outcome);

	auto inspection = service.Inspect("editor.tabSize", profile);
	ASSERT_EQ(EConfigurationOutcome::Applied, inspection.outcome);
	bool sawDefault = false;
	bool sawApplication = false;
	bool sawProfile = false;
	for (const auto& entry : inspection.provenance) {
		switch (entry.scope) {
		case EConfigurationScope::Default:
			sawDefault = true;
			EXPECT_FALSE(entry.withheld);
			break;
		case EConfigurationScope::Application:
			sawApplication = true;
			EXPECT_FALSE(entry.withheld);
			break;
		case EConfigurationScope::Profile:
			sawProfile = true;
			EXPECT_FALSE(entry.withheld);
			break;
		default:
			break;
		}
	}
	EXPECT_TRUE(sawDefault);
	EXPECT_TRUE(sawApplication);
	EXPECT_TRUE(sawProfile);
	EXPECT_EQ(5, Integer(service, profile));
}

TEST(ConfigurationService, ApplyingAnIdenticalRestrictedPolicyIsNoChangeAndARealChangeBumpsRevisionAndNotifies)
{
	auto service = Service();
	const auto profile = Target();
	const auto workspace = Target(L"default", L"file:///C:/work");
	ASSERT_EQ(EConfigurationOutcome::Applied, Apply(service, EConfigurationScope::Profile, profile, 5, "profile").outcome);
	ASSERT_EQ(EConfigurationOutcome::Applied, Apply(service, EConfigurationScope::Workspace, workspace, 6, "workspace").outcome);

	const RestrictedConfigurationPolicy untrusted { false, { "editor.tabSize" }, workspace };
	const auto first = service.ApplyRestrictedConfigurations(untrusted);
	ASSERT_EQ(EConfigurationOutcome::Applied, first.outcome);
	const auto revisionAfterFirst = first.revision;

	int callbackCount = 0;
	std::vector<ConfigurationChange> lastChanges;
	auto subscription = service.Subscribe([&callbackCount, &lastChanges](const auto& changes) {
		++callbackCount;
		lastChanges = changes;
	});

	const auto noChange = service.ApplyRestrictedConfigurations(untrusted);
	EXPECT_EQ(EConfigurationOutcome::NoChange, noChange.outcome);
	EXPECT_EQ(revisionAfterFirst, noChange.revision);
	EXPECT_EQ(0, callbackCount);

	// ApplyRestrictedConfigurations resolves its before/after comparison, and
	// stamps every emitted ConfigurationChange, against the caller-supplied
	// evaluationTarget rather than a target the service invents on its own.
	// With evaluationTarget == workspace, granting trust genuinely flips
	// editor.tabSize's effective value at that target from the Profile
	// fallback (5) to the now-unwithheld Workspace contribution (6), so the
	// listener must fire exactly once and name that key/target/values.
	const auto trusted = service.ApplyRestrictedConfigurations({ true, { "editor.tabSize" }, workspace });
	EXPECT_EQ(EConfigurationOutcome::Applied, trusted.outcome);
	EXPECT_GT(trusted.revision, revisionAfterFirst);
	EXPECT_EQ(1, callbackCount);
	ASSERT_EQ(1U, lastChanges.size());
	EXPECT_EQ("editor.tabSize", lastChanges.front().key);
	EXPECT_EQ(5, std::get<std::int64_t>(lastChanges.front().previousValue.Value()));
	EXPECT_EQ(6, std::get<std::int64_t>(lastChanges.front().effectiveValue.Value()));
	EXPECT_EQ(6, Integer(service, workspace));
}

TEST(ConfigurationService, ApplyRestrictedConfigurationsRejectsAnInvalidEvaluationTargetAndCommitsNothing)
{
	auto service = Service();
	const auto profile = Target();
	const auto workspace = Target(L"default", L"file:///C:/work");
	ASSERT_EQ(EConfigurationOutcome::Applied, Apply(service, EConfigurationScope::Profile, profile, 5, "profile").outcome);
	ASSERT_EQ(EConfigurationOutcome::Applied, Apply(service, EConfigurationScope::Workspace, workspace, 6, "workspace").outcome);

	const auto baseline = service.ReadSnapshot({ "editor.tabSize" }, workspace);
	ASSERT_TRUE(baseline.snapshot.has_value());
	const auto revisionBefore = baseline.snapshot->revision;

	int callbackCount = 0;
	auto subscription = service.Subscribe([&callbackCount](const auto&) { ++callbackCount; });

	// folderUri without workspaceUri fails IsContextValid, the same predicate
	// ReadSnapshot/Inspect already reject targets with.
	auto parsedFolderUri = platform::uri::Uri::Parse(L"file:///C:/work/one");
	ASSERT_TRUE(parsedFolderUri);
	ConfigurationTarget invalidTarget;
	invalidTarget.profileId = L"default";
	invalidTarget.folderUri = std::move(*parsedFolderUri.value);

	const auto rejected = service.ApplyRestrictedConfigurations({ false, { "editor.tabSize" }, invalidTarget });
	EXPECT_EQ(EConfigurationOutcome::InvalidScope, rejected.outcome);
	EXPECT_EQ(0, callbackCount);

	const auto afterAttempt = service.ReadSnapshot({ "editor.tabSize" }, workspace);
	ASSERT_TRUE(afterAttempt.snapshot.has_value());
	EXPECT_EQ(revisionBefore, afterAttempt.snapshot->revision);
	ASSERT_EQ(1U, afterAttempt.snapshot->values.size());
	EXPECT_EQ(6, std::get<std::int64_t>(afterAttempt.snapshot->values.front().Value()));
}

TEST(ConfigurationService, NonCanonicalRestrictedKeyIsDroppedWhileACanonicalEntryInTheSameListStillApplies)
{
	auto service = Service();
	const auto profile = Target();
	const auto workspace = Target(L"default", L"file:///C:/work");
	ASSERT_EQ(EConfigurationOutcome::Applied, Apply(service, EConfigurationScope::Profile, profile, 5, "profile").outcome);
	ASSERT_EQ(EConfigurationOutcome::Applied, Apply(service, EConfigurationScope::Workspace, workspace, 6, "workspace").outcome);
	ASSERT_EQ(EConfigurationOutcome::Applied,
		service.Update({ Source(EConfigurationScope::Profile, profile), "editor.lineHeight", ConfigurationValue(22), "profile-line-height" }).outcome);
	ASSERT_EQ(EConfigurationOutcome::Applied,
		service.Update({ Source(EConfigurationScope::Workspace, workspace), "editor.lineHeight", ConfigurationValue(28), "workspace-line-height" }).outcome);

	// ".editor.tabSize" fails IsCanonicalKey (leading dot); it must be dropped
	// rather than restricting editor.tabSize by accident, while the genuinely
	// canonical "editor.lineHeight" in the same list still takes effect.
	const auto applied = service.ApplyRestrictedConfigurations({ false, { ".editor.tabSize", "editor.lineHeight" }, workspace });
	ASSERT_EQ(EConfigurationOutcome::Applied, applied.outcome);

	EXPECT_EQ(6, Integer(service, workspace));
	auto lineHeight = service.GetValue("editor.lineHeight", workspace);
	ASSERT_TRUE(lineHeight.value.has_value());
	EXPECT_EQ(22, std::get<std::int64_t>(lineHeight.value->Value()));
}

TEST(ConfigurationService, ReadSnapshotAndGetValueAgreeWithInspectEffectiveValueUnderARestrictedUntrustedPolicy)
{
	auto service = Service();
	const auto profile = Target();
	const auto workspace = Target(L"default", L"file:///C:/work");
	ASSERT_EQ(EConfigurationOutcome::Applied, Apply(service, EConfigurationScope::Profile, profile, 5, "profile").outcome);
	ASSERT_EQ(EConfigurationOutcome::Applied, Apply(service, EConfigurationScope::Workspace, workspace, 6, "workspace").outcome);
	ASSERT_EQ(EConfigurationOutcome::Applied,
		service.ApplyRestrictedConfigurations({ false, { "editor.tabSize" }, workspace }).outcome);

	auto inspection = service.Inspect("editor.tabSize", workspace);
	ASSERT_TRUE(inspection.effectiveValue.has_value());
	auto value = service.GetValue("editor.tabSize", workspace);
	ASSERT_TRUE(value.value.has_value());
	auto snapshot = service.ReadSnapshot({ "editor.tabSize" }, workspace);
	ASSERT_TRUE(snapshot.snapshot.has_value());
	ASSERT_EQ(1U, snapshot.snapshot->values.size());

	EXPECT_EQ(*inspection.effectiveValue, *value.value);
	EXPECT_EQ(*inspection.effectiveValue, snapshot.snapshot->values.front());
	EXPECT_EQ(5, std::get<std::int64_t>(inspection.effectiveValue->Value()));
}

TEST(ConfigurationService, ExtensionsSupportUntrustedWorkspacesIsRegisteredWithAnEmptyObjectDefaultAndRejectedAtWorkspaceScope)
{
	CConfigurationService service(BuiltinConfigurationDescriptors());
	const auto profile = Target();
	const auto workspace = Target(L"default", L"file:///C:/work");

	auto result = service.GetValue("extensions.supportUntrustedWorkspaces", profile);
	ASSERT_EQ(EConfigurationOutcome::Applied, result.outcome);
	ASSERT_TRUE(result.value.has_value());
	const auto* object = std::get_if<ConfigurationValue::Object>(&result.value->Value());
	ASSERT_NE(nullptr, object);
	EXPECT_TRUE(object->empty());

	const auto rejected = service.Update({
		Source(EConfigurationScope::Workspace, workspace), "extensions.supportUntrustedWorkspaces",
		ConfigurationValue(ConfigurationValue::Object{}), "reject-workspace-scope" });
	EXPECT_EQ(EConfigurationOutcome::InvalidScope, rejected.outcome);
}
