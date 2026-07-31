/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include "config/CConfigurationService.h"
#include "config/JsoncConfigurationSource.h"

#include <string>
#include <utility>
#include <vector>

namespace {

using config::CConfigurationService;
using config::CJsoncConfigurationSource;
using config::ConfigurationBatchResult;
using config::ConfigurationDescriptor;
using config::ConfigurationLookupResult;
using config::ConfigurationReplaceSources;
using config::ConfigurationSource;
using config::ConfigurationSubscription;
using config::ConfigurationTarget;
using config::ConfigurationValue;
using config::EConfigurationOutcome;
using config::EConfigurationScope;
using config::EJsoncConfigurationDiagnosticCode;
using config::IConfigurationService;
using config::JsoncConfigurationSourceRevisions;

ConfigurationTarget Target(const wchar_t* language = nullptr)
{
	ConfigurationTarget target;
	target.profileId = L"default";
	if (language) {
		target.languageId = language;
	}
	return target;
}

ConfigurationSource Source()
{
	return { EConfigurationScope::Profile, Target(), "settings.json", 0 };
}

JsoncConfigurationSourceRevisions Revisions(std::uint64_t baseRevision)
{
	return { baseRevision, {} };
}

CConfigurationService Service()
{
	return CConfigurationService({
		{ "editor.tabSize", ConfigurationValue(4), { EConfigurationScope::Profile, EConfigurationScope::LanguageOverride } },
		{ "editor.guides", ConfigurationValue(ConfigurationValue::Array {}), { EConfigurationScope::Profile, EConfigurationScope::LanguageOverride } },
		{ "editor.layout", ConfigurationValue(ConfigurationValue::Object {}), { EConfigurationScope::Profile, EConfigurationScope::LanguageOverride } },
	});
}

std::int64_t Integer(const CConfigurationService& service, const ConfigurationTarget& target)
{
	auto value = service.GetValue("editor.tabSize", target);
	EXPECT_EQ(EConfigurationOutcome::Applied, value.outcome);
	EXPECT_TRUE(value.value.has_value());
	return std::get<std::int64_t>(value.value->Value());
}

class CountingConfigurationService final : public IConfigurationService {
public:
	ConfigurationLookupResult GetValue(const std::string&, const ConfigurationTarget&) const override { return {}; }
	config::ConfigurationReadSnapshotResult ReadSnapshot(const std::vector<std::string>&, const ConfigurationTarget&) const override { return {}; }
	config::ConfigurationInspection Inspect(const std::string&, const ConfigurationTarget&) const override { return {}; }
	config::ConfigurationResult Update(const config::ConfigurationUpdate&) override { return {}; }
	config::ConfigurationResult ReplaceSource(const config::ConfigurationReplaceSource&) override { return {}; }
	ConfigurationBatchResult ReplaceSources(const ConfigurationReplaceSources& request) override
	{
		++replaceSourcesCalls;
		lastRequest = request;
		return { EConfigurationOutcome::Applied, {}, {} };
	}
	ConfigurationSubscription Subscribe(config::ConfigurationListener) override { return {}; }

	int replaceSourcesCalls = 0;
	ConfigurationReplaceSources lastRequest;
};

const ConfigurationValue& FindEntry(const std::vector<config::ConfigurationEntry>& entries, const char* key)
{
	for (const auto& entry : entries) {
		if (entry.key == key) {
			return entry.value;
		}
	}
	ADD_FAILURE() << "missing configuration entry: " << key;
	return entries.front().value;
}

} // namespace

TEST(JsoncConfigurationSource, ParsesCommentsTrailingCommasUnicodeAndNestedValues)
{
	const auto parsed = CJsoncConfigurationSource::Parse(R"json(
// Unicode is represented as JSON escapes, then decoded to UTF-16.
{
  "editor.tabSize": 2,
  "editor.guides": [80, { "label": "\u65e5\u672c\u8a9e", },],
  "editor.layout": { "enabled": true, "ratio": 1.5, },
}
)json", Source(), "parse");

	ASSERT_TRUE(parsed.Succeeded());
	ASSERT_EQ(1U, parsed.replacements.size());
	EXPECT_EQ("parse", parsed.operationId);
	const auto& entries = parsed.replacements.front().entries;
	ASSERT_EQ(3U, entries.size());
	EXPECT_EQ(2, std::get<std::int64_t>(FindEntry(entries, "editor.tabSize").Value()));
	const auto& guides = std::get<ConfigurationValue::Array>(FindEntry(entries, "editor.guides").Value());
	ASSERT_EQ(2U, guides.size());
	const auto& label = std::get<ConfigurationValue::Object>(guides[1].Value()).at(L"label");
	EXPECT_EQ(L"\u65e5\u672c\u8a9e", std::get<std::wstring>(label.Value()));
	const auto& layout = std::get<ConfigurationValue::Object>(FindEntry(entries, "editor.layout").Value());
	EXPECT_TRUE(std::get<bool>(layout.at(L"enabled").Value()));
	EXPECT_DOUBLE_EQ(1.5, std::get<double>(layout.at(L"ratio").Value()));

	const auto rawUtf8 = CJsoncConfigurationSource::Parse(
		std::string("{\"editor.layout\":{\"label\":\"") + "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e" + "\"}}",
		Source(), "raw-utf8");
	ASSERT_TRUE(rawUtf8.Succeeded());
	const auto& rawLayout = std::get<ConfigurationValue::Object>(rawUtf8.replacements.front().entries.front().value.Value());
	EXPECT_EQ(L"\u65e5\u672c\u8a9e", std::get<std::wstring>(rawLayout.at(L"label").Value()));
}

TEST(JsoncConfigurationSource, MapsLanguageBlocksAndCombinedSelectorsToDistinctLanguageSources)
{
	const auto parsed = CJsoncConfigurationSource::Parse(R"json({
  "editor.tabSize": 3,
  "[cpp]": { "editor.tabSize": 2, },
  "[javascript][typescript]": { "editor.guides": [72], },
})json", Source(), "language-map");

	ASSERT_TRUE(parsed.Succeeded());
	ASSERT_EQ(4U, parsed.replacements.size());
	EXPECT_EQ(EConfigurationScope::Profile, parsed.replacements[0].source.scope);
	ASSERT_TRUE(parsed.replacements[1].source.target.languageId.has_value());
	EXPECT_EQ(L"cpp", *parsed.replacements[1].source.target.languageId);
	EXPECT_EQ("settings.json:language:3:cpp", parsed.replacements[1].source.sourceId);
	EXPECT_EQ(L"javascript", *parsed.replacements[2].source.target.languageId);
	EXPECT_EQ(L"typescript", *parsed.replacements[3].source.target.languageId);
}

TEST(JsoncConfigurationSource, RejectsOverlappingLanguageContributions)
{
	const auto parsed = CJsoncConfigurationSource::Parse(
		R"json({ "[javascript][typescript]": { "editor.tabSize": 2 }, "[typescript]": { "editor.guides": [80] } })json",
		Source(), "overlap");
	ASSERT_FALSE(parsed.Succeeded());
	ASSERT_TRUE(parsed.diagnostic.has_value());
	EXPECT_EQ(EJsoncConfigurationDiagnosticCode::DuplicateKey, parsed.diagnostic->code);
}

TEST(JsoncConfigurationSource, AcceptsBomAndRejectsInvalidUtf8InCommentsBeforeServiceCall)
{
	const std::string bomDocument = std::string("\xef\xbb\xbf// UTF-8 BOM is accepted\n{ \"editor.tabSize\": 2 }");
	EXPECT_TRUE(CJsoncConfigurationSource::Parse(bomDocument, Source(), "bom").Succeeded());

	CountingConfigurationService service;
	const std::string invalidComment = std::string("// ") + "\xc3\x28" + "\n{ \"editor.tabSize\": 2 }";
	auto invalid = CJsoncConfigurationSource::Apply(service, invalidComment, Source(), "bad-comment");
	ASSERT_FALSE(invalid.Parsed());
	ASSERT_TRUE(invalid.diagnostic.has_value());
	EXPECT_EQ(EJsoncConfigurationDiagnosticCode::InvalidUtf8, invalid.diagnostic->code);
	EXPECT_EQ(0, service.replaceSourcesCalls);
}

TEST(JsoncConfigurationSource, CallsBatchServiceExactlyOnceForValidDocument)
{
	CountingConfigurationService service;
	auto applied = CJsoncConfigurationSource::Apply(service,
		R"json({ "editor.tabSize": 2, "[cpp]": { "editor.tabSize": 3 } })json",
		Source(), "once", Revisions(0));

	EXPECT_TRUE(applied.Parsed());
	EXPECT_EQ(EConfigurationOutcome::Applied, applied.result.outcome);
	ASSERT_EQ(1, service.replaceSourcesCalls);
	EXPECT_EQ("once", service.lastRequest.operationId);
	ASSERT_EQ(2U, service.lastRequest.replacements.size());
	ASSERT_TRUE(service.lastRequest.replacements[0].expectedRevision.has_value());
	EXPECT_EQ(0U, *service.lastRequest.replacements[0].expectedRevision);
	EXPECT_EQ(0U, *service.lastRequest.replacements[1].expectedRevision);
}

TEST(JsoncConfigurationSource, RejectsSyntaxDuplicateOversizeAndRootFailuresWithoutServiceCall)
{
	CountingConfigurationService service;
	const std::string oversized(CJsoncConfigurationSource::kMaximumInputBytes + 1U, ' ');
	const std::vector<std::pair<std::string, EJsoncConfigurationDiagnosticCode>> invalid {
		{ "{", EJsoncConfigurationDiagnosticCode::UnexpectedEndOfInput },
		{ R"json({"editor.tabSize": 2, "editor.tabSize": 3})json", EJsoncConfigurationDiagnosticCode::DuplicateKey },
		{ "[]", EJsoncConfigurationDiagnosticCode::RootMustBeObject },
		{ oversized, EJsoncConfigurationDiagnosticCode::InputTooLarge },
	};
	for (const auto& [input, expected] : invalid) {
		auto applied = CJsoncConfigurationSource::Apply(service, input, Source(), "invalid-" + std::to_string(service.replaceSourcesCalls));
		ASSERT_FALSE(applied.Parsed());
		ASSERT_TRUE(applied.diagnostic.has_value());
		EXPECT_EQ(expected, applied.diagnostic->code);
	}
	EXPECT_EQ(0, service.replaceSourcesCalls);
}

TEST(JsoncConfigurationSource, PreservesLastEffectiveConfigurationForInvalidDocument)
{
	auto service = Service();
	auto initial = CJsoncConfigurationSource::Apply(service, R"json({ "editor.tabSize": 7 })json", Source(), "initial", Revisions(0));
	ASSERT_EQ(EConfigurationOutcome::Applied, initial.result.outcome);
	EXPECT_EQ(7, Integer(service, Target()));

	auto invalid = CJsoncConfigurationSource::Apply(service,
		R"json({ "editor.tabSize": 8, "editor.tabSize": 9 })json", Source(), "invalid", Revisions(1));
	ASSERT_FALSE(invalid.Parsed());
	EXPECT_EQ(EJsoncConfigurationDiagnosticCode::DuplicateKey, invalid.diagnostic->code);
	EXPECT_EQ(7, Integer(service, Target()));
}

TEST(JsoncConfigurationSource, UsesPerSourceBatchCasAndOperationReplay)
{
	auto service = Service();
	const auto document = R"json({ "editor.tabSize": 6, "[cpp]": { "editor.tabSize": 2 } })json";
	auto applied = CJsoncConfigurationSource::Apply(service, document, Source(), "batch", Revisions(0));
	ASSERT_EQ(EConfigurationOutcome::Applied, applied.result.outcome);
	EXPECT_EQ(6, Integer(service, Target()));
	EXPECT_EQ(2, Integer(service, Target(L"cpp")));

	auto replay = CJsoncConfigurationSource::Apply(service, document, Source(), "batch", Revisions(0));
	EXPECT_EQ(EConfigurationOutcome::Replayed, replay.result.outcome);

	JsoncConfigurationSourceRevisions stale;
	stale.baseRevision = 1;
	stale.languageRevisions.emplace(L"cpp", 0); // The language source is already revision 1.
	auto conflict = CJsoncConfigurationSource::Apply(service, document, Source(), "stale", std::move(stale));
	EXPECT_EQ(EConfigurationOutcome::Conflict, conflict.result.outcome);
	EXPECT_EQ(6, Integer(service, Target()));
	EXPECT_EQ(2, Integer(service, Target(L"cpp")));
}

TEST(JsoncConfigurationSource, ClearsTrackedLanguageOverridesMissingFromNextDocument)
{
	auto service = Service();
	auto first = CJsoncConfigurationSource::Apply(service,
		R"json({ "editor.tabSize": 6, "[cpp]": { "editor.tabSize": 2 } })json",
		Source(), "first", Revisions(0));
	ASSERT_EQ(EConfigurationOutcome::Applied, first.result.outcome);

	JsoncConfigurationSourceRevisions secondRevisions;
	secondRevisions.baseRevision = 1;
	secondRevisions.languageRevisions.emplace(L"cpp", 1);
	auto second = CJsoncConfigurationSource::Apply(service,
		R"json({ "editor.tabSize": 8 })json", Source(), "remove-cpp", std::move(secondRevisions));
	ASSERT_TRUE(second.Parsed());
	ASSERT_EQ(EConfigurationOutcome::Applied, second.result.outcome);
	ASSERT_EQ(2U, second.result.revisions.size());
	EXPECT_EQ(8, Integer(service, Target()));
	EXPECT_EQ(8, Integer(service, Target(L"cpp")));
}
