/*! @file */
/*
Copyright (C) 2026, Sakura Editor Organization

SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include "senp/SenpLanguageService.h"
#include "senp/SenpManagementService.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace {

class FakeManagementService final : public senp::ISenpManagementService {
public:
	senp::ManagementOperationResult Start() override
	{
		return { senp::EManagementOperationStatus::Succeeded, snapshot };
	}
	senp::ManagementOperationResult InstallDeveloperPackage(std::wstring_view, bool) override
	{
		return { senp::EManagementOperationStatus::InvalidRequest, snapshot };
	}
	senp::ManagementOperationResult InstallBuiltInPackage(std::wstring_view) override
	{
		return { senp::EManagementOperationStatus::InvalidRequest, snapshot };
	}
	senp::ManagementOperationResult UninstallBuiltInPackage(std::wstring_view) override
	{
		return { senp::EManagementOperationStatus::InvalidRequest, snapshot };
	}
	senp::ManagementOperationResult SetExtensionEnabled(std::wstring_view, bool) override
	{
		return { senp::EManagementOperationStatus::InvalidRequest, snapshot };
	}
	senp::ManagementOperationResult UninstallPackage(std::wstring_view) override
	{
		return { senp::EManagementOperationStatus::InvalidRequest, snapshot };
	}
	senp::ManagementOperationResult Refresh() override
	{
		return { senp::EManagementOperationStatus::Succeeded, snapshot };
	}
	void Stop() noexcept override {}
	senp::ManagementSnapshot Snapshot() const override { return snapshot; }

	senp::ManagementSnapshot snapshot;
};

class TemporaryGrammarDirectory final {
public:
	TemporaryGrammarDirectory()
	{
		static std::atomic_uint64_t next{ 0 };
		m_path = std::filesystem::temp_directory_path()
			/ ("sakura-senp-language-test-" + std::to_string(::GetCurrentProcessId())
				+ "-" + std::to_string(++next));
		std::filesystem::create_directories(m_path / "assets" / "syntaxes");
	}
	~TemporaryGrammarDirectory()
	{
		std::error_code error;
		std::filesystem::remove_all(m_path, error);
	}

	void Write(std::string_view filename, std::string_view source) const
	{
		std::ofstream output(m_path / "assets" / "syntaxes" / filename, std::ios::binary);
		ASSERT_TRUE(output.good());
		output.write(source.data(), static_cast<std::streamsize>(source.size()));
		ASSERT_TRUE(output.good());
	}

	const std::filesystem::path& Path() const noexcept { return m_path; }

private:
	std::filesystem::path m_path;
};

senp::ExtensionDescriptor CreateExtension(const TemporaryGrammarDirectory& directory)
{
	senp::ExtensionDescriptor extension;
	extension.id = L"test.infrastructure-languages";
	extension.extensionPath = directory.Path().wstring();
	extension.installed = true;
	extension.enabled = true;
	extension.languages = {
		{ L"terraform", {}, { L".tf", L".tfvars" }, {}, {}, {}, {}, {} },
		{ L"yaml", {}, { L".yaml", L".yml" }, {}, {}, {}, {}, {} },
		{ L"cloudformation", {}, {}, { L"template.yaml", L"template.yml" }, {}, {}, {}, {} },
	};
	extension.grammars = {
		{ L"terraform", L"source.hcl.terraform", L"assets/syntaxes/terraform.tmLanguage.json", {} },
		{ L"yaml", L"source.yaml", L"assets/syntaxes/yaml.tmLanguage.json", {} },
		{ L"cloudformation", L"source.cloudformation", L"assets/syntaxes/cloudformation.tmLanguage.json", {} },
	};
	return extension;
}

void WriteStubGrammar(const TemporaryGrammarDirectory& directory, std::wstring_view languageId)
{
	std::string narrow;
	for (const wchar_t ch : languageId) narrow.push_back(static_cast<char>(ch));
	directory.Write(narrow + ".tmLanguage.json",
		"{ \"scopeName\": \"source." + narrow + "\", \"patterns\": [] }");
}

senp::ExtensionDescriptor CreatePatternExtension(const TemporaryGrammarDirectory& directory,
	std::initializer_list<senp::LanguageContribution> languages)
{
	senp::ExtensionDescriptor extension;
	extension.id = L"test.pattern-languages";
	extension.extensionPath = directory.Path().wstring();
	extension.installed = true;
	extension.enabled = true;
	extension.languages.assign(languages);
	for (const auto& language : extension.languages) {
		extension.grammars.push_back({ language.id, L"source." + language.id,
			L"assets/syntaxes/" + language.id + L".tmLanguage.json", {} });
		WriteStubGrammar(directory, language.id);
	}
	return extension;
}

std::unique_ptr<senp::ISenpLanguageService> StartService(FakeManagementService& management,
	senp::ExtensionDescriptor extension)
{
	management.snapshot.state = senp::EManagementState::Ready;
	management.snapshot.extensions.push_back(std::move(extension));
	auto service = senp::CreateSenpLanguageService(management);
	EXPECT_TRUE(service->Start());
	return service;
}

TEST(SenpLanguageServiceTest, SelectsTerraformByExtensionAndTokenizesWithContributedGrammar)
{
	TemporaryGrammarDirectory directory;
	directory.Write("terraform.tmLanguage.json", R"JSON({
		"scopeName": "source.hcl.terraform",
		"patterns": [{ "match": "\\b(resource|variable)\\b", "name": "keyword.declaration.terraform" }]
	})JSON");
	directory.Write("yaml.tmLanguage.json", R"JSON({ "scopeName": "source.yaml", "patterns": [] })JSON");
	directory.Write("cloudformation.tmLanguage.json", R"JSON({ "scopeName": "source.cloudformation", "patterns": [] })JSON");

	FakeManagementService management;
	management.snapshot.state = senp::EManagementState::Ready;
	management.snapshot.extensions.push_back(CreateExtension(directory));
	auto service = senp::CreateSenpLanguageService(management);
	ASSERT_TRUE(service->Start());
	auto session = service->CreateSession(L"main.tf", L"resource \"aws_s3_bucket\" \"site\" {");
	ASSERT_NE(nullptr, session);
	EXPECT_EQ(L"terraform", session->LanguageId());
	EXPECT_EQ(L"source.hcl.terraform", session->ScopeName());

	textmate::RuleStackHandle next;
	const auto tokens = session->TokenizeLine(L"resource \"aws_s3_bucket\" \"site\" {", session->InitialState(), next);
	ASSERT_FALSE(tokens.tokens.empty());
	ASSERT_FALSE(tokens.tokens.front().scopes.empty());
	EXPECT_EQ(L"keyword.declaration.terraform", tokens.tokens.front().scopes.back());
}

TEST(SenpLanguageServiceTest, ExactCloudFormationFilenameWinsOverGenericYamlExtension)
{
	TemporaryGrammarDirectory directory;
	directory.Write("terraform.tmLanguage.json", R"JSON({ "scopeName": "source.hcl.terraform", "patterns": [] })JSON");
	directory.Write("yaml.tmLanguage.json", R"JSON({ "scopeName": "source.yaml", "patterns": [] })JSON");
	directory.Write("cloudformation.tmLanguage.json", R"JSON({ "scopeName": "source.cloudformation", "patterns": [] })JSON");

	FakeManagementService management;
	management.snapshot.state = senp::EManagementState::Ready;
	management.snapshot.extensions.push_back(CreateExtension(directory));
	auto service = senp::CreateSenpLanguageService(management);
	ASSERT_TRUE(service->Start());
	auto session = service->CreateSession(L"template.yaml", L"Resources:");
	ASSERT_NE(nullptr, session);
	EXPECT_EQ(L"cloudformation", session->LanguageId());
	EXPECT_EQ(L"source.cloudformation", session->ScopeName());
}

TEST(SenpLanguageServiceTest, AnInvalidListingRowContributesNoLanguage)
{
	TemporaryGrammarDirectory directory;
	directory.Write("yaml.tmLanguage.json", R"JSON({ "scopeName": "source.yaml", "patterns": [] })JSON");
	FakeManagementService management;
	management.snapshot.state = senp::EManagementState::Ready;
	// The descriptor carries a grammar path but is flagged invalid: a row the
	// package tool could not read is never an enabled contribution.
	auto broken = CreateExtension(directory);
	broken.invalidCode = L"invalidManifest";
	broken.enabled = false;
	management.snapshot.extensions.push_back(std::move(broken));
	auto service = senp::CreateSenpLanguageService(management);
	ASSERT_TRUE(service->Start());
	EXPECT_EQ(nullptr, service->CreateSession(L"config.yaml", L""));
}

TEST(SenpLanguageServiceTest, FilenamePatternWinsOverPlainExtension)
{
	TemporaryGrammarDirectory directory;
	FakeManagementService management;
	auto service = StartService(management, CreatePatternExtension(directory, {
		{ L"yaml", {}, { L".yaml", L".yml" }, {}, {}, {}, {}, {} },
		{ L"infrastructure-yaml", {}, {}, {}, { L"docker-compose.*.yml" }, {}, {}, {} },
	}));
	auto session = service->CreateSession(L"docker-compose.dev.yml", L"");
	ASSERT_NE(nullptr, session);
	EXPECT_EQ(L"infrastructure-yaml", session->LanguageId());
}

TEST(SenpLanguageServiceTest, JsonFilenamePatternWinsOverPlainExtension)
{
	TemporaryGrammarDirectory directory;
	FakeManagementService management;
	auto service = StartService(management, CreatePatternExtension(directory, {
		{ L"json", {}, { L".json" }, {}, {}, {}, {}, {} },
		{ L"template-json", {}, {}, {}, { L"*.template.json" }, {}, {}, {} },
	}));
	auto session = service->CreateSession(L"x.template.json", L"");
	ASSERT_NE(nullptr, session);
	EXPECT_EQ(L"template-json", session->LanguageId());
}

TEST(SenpLanguageServiceTest, PatternWithoutSlashMatchesBasenameOnly)
{
	TemporaryGrammarDirectory directory;
	FakeManagementService management;
	auto service = StartService(management, CreatePatternExtension(directory, {
		{ L"workflows", {}, {}, {}, { L"*workflows*" }, {}, {}, {} },
	}));
	// The pattern has no '/', so it sees only the basename a.yml and must not
	// match a directory name elsewhere in the path.
	EXPECT_EQ(nullptr, service->CreateSession(L"C:\\repo\\workflows\\a.yml", L""));
}

TEST(SenpLanguageServiceTest, SlashPatternMatchesFullPathSegments)
{
	TemporaryGrammarDirectory directory;
	FakeManagementService management;
	auto service = StartService(management, CreatePatternExtension(directory, {
		{ L"actions", {}, {}, {}, { L"**/.github/workflows/*.yml" }, {}, {}, {} },
	}));
	auto session = service->CreateSession(L"C:\\repo\\.github\\workflows\\ci.yml", L"");
	ASSERT_NE(nullptr, session);
	EXPECT_EQ(L"actions", session->LanguageId());
	// A single '*' never crosses a segment boundary.
	EXPECT_EQ(nullptr, service->CreateSession(L"C:\\repo\\.github\\workflows\\sub\\ci.yml", L""));
	// The literal '.github' segment is required.
	EXPECT_EQ(nullptr, service->CreateSession(L"C:\\repo\\github\\workflows\\ci.yml", L""));
}

TEST(SenpLanguageServiceTest, LeadingDoubleStarMatchesZeroSegments)
{
	TemporaryGrammarDirectory directory;
	FakeManagementService management;
	auto service = StartService(management, CreatePatternExtension(directory, {
		{ L"marker", {}, {}, {}, { L"**/marker.yml" }, {}, {}, {} },
	}));
	// '**/' consumes zero segments, so the bare filename matches too.
	auto session = service->CreateSession(L"marker.yml", L"");
	ASSERT_NE(nullptr, session);
	EXPECT_EQ(L"marker", session->LanguageId());
}

TEST(SenpLanguageServiceTest, FilenameBeatsFilenamePattern)
{
	TemporaryGrammarDirectory directory;
	FakeManagementService management;
	auto service = StartService(management, CreatePatternExtension(directory, {
		{ L"compose-exact", {}, {}, { L"compose.yml" }, {}, {}, {}, {} },
		{ L"compose-pattern", {}, {}, {}, { L"*compose*.yml" }, {}, {}, {} },
	}));
	auto session = service->CreateSession(L"compose.yml", L"");
	ASSERT_NE(nullptr, session);
	EXPECT_EQ(L"compose-exact", session->LanguageId());
}

TEST(SenpLanguageServiceTest, LongestFilenamePatternWins)
{
	TemporaryGrammarDirectory directory;
	FakeManagementService management;
	auto service = StartService(management, CreatePatternExtension(directory, {
		{ L"short", {}, {}, {}, { L"*compose*.yml" }, {}, {}, {} },
		{ L"long", {}, {}, {}, { L"docker-compose.*.yml" }, {}, {}, {} },
	}));
	auto session = service->CreateSession(L"docker-compose.dev.yml", L"");
	ASSERT_NE(nullptr, session);
	EXPECT_EQ(L"long", session->LanguageId());
}

TEST(SenpLanguageServiceTest, FirstLineLosesToAnyPathMatch)
{
	TemporaryGrammarDirectory directory;
	FakeManagementService management;
	auto service = StartService(management, CreatePatternExtension(directory, {
		{ L"shellscript", {}, {}, {}, {}, {}, L"^#!", {} },
		{ L"yaml", {}, { L".yml" }, {}, {}, {}, {}, {} },
	}));
	auto session = service->CreateSession(L"a.yml", L"#!/bin/sh");
	ASSERT_NE(nullptr, session);
	EXPECT_EQ(L"yaml", session->LanguageId());
}

TEST(SenpLanguageServiceTest, FirstLineAppliesWithoutPathMatch)
{
	TemporaryGrammarDirectory directory;
	FakeManagementService management;
	auto service = StartService(management, CreatePatternExtension(directory, {
		{ L"shellscript", {}, {}, {}, {}, {}, L"^#!", {} },
		{ L"yaml", {}, { L".yml" }, {}, {}, {}, {}, {} },
	}));
	auto session = service->CreateSession(L"a.zzz", L"#!/bin/sh");
	ASSERT_NE(nullptr, session);
	EXPECT_EQ(L"shellscript", session->LanguageId());
}

TEST(SenpLanguageServiceTest, QuestionMarkNeverMatchesASeparator)
{
	TemporaryGrammarDirectory directory;
	FakeManagementService management;
	auto service = StartService(management, CreatePatternExtension(directory, {
		{ L"qmark", {}, {}, {}, { L"**/a?b/*.yml" }, {}, {}, {} },
	}));
	auto session = service->CreateSession(L"C:\\repo\\axb\\c.yml", L"");
	ASSERT_NE(nullptr, session);
	EXPECT_EQ(L"qmark", session->LanguageId());
	EXPECT_EQ(nullptr, service->CreateSession(L"C:\\repo\\a\\b\\c.yml", L""));
}

TEST(SenpLanguageServiceTest, DoubleStarSlashClosesOnlyAtASegmentBoundary)
{
	TemporaryGrammarDirectory directory;
	FakeManagementService management;
	auto service = StartService(management, CreatePatternExtension(directory, {
		{ L"actions", {}, {}, {}, { L"**/.github/workflows/*.yml" }, {}, {}, {} },
	}));
	// '**/' may not stop in the middle of the 'x.github' segment.
	EXPECT_EQ(nullptr, service->CreateSession(L"C:\\repo\\x.github\\workflows\\ci.yml", L""));
}

TEST(SenpLanguageServiceTest, InnerDoubleStarMatchesZeroOrMoreSegments)
{
	TemporaryGrammarDirectory directory;
	FakeManagementService management;
	auto service = StartService(management, CreatePatternExtension(directory, {
		{ L"actions", {}, {}, {}, { L"**/.github/**/*.yml" }, {}, {}, {} },
	}));
	auto direct = service->CreateSession(L"C:\\repo\\.github\\ci.yml", L"");
	ASSERT_NE(nullptr, direct);
	EXPECT_EQ(L"actions", direct->LanguageId());
	auto nested = service->CreateSession(L"C:\\repo\\.github\\a\\b\\ci.yml", L"");
	ASSERT_NE(nullptr, nested);
	EXPECT_EQ(L"actions", nested->LanguageId());
}

TEST(SenpLanguageServiceTest, TrailingDoubleStarMatchesTheFolderAndEverythingBelowIt)
{
	TemporaryGrammarDirectory directory;
	FakeManagementService management;
	auto service = StartService(management, CreatePatternExtension(directory, {
		{ L"dotgithub", {}, {}, {}, { L"**/.github/**" }, {}, {}, {} },
	}));
	auto bare = service->CreateSession(L"C:\\repo\\.github", L"");
	ASSERT_NE(nullptr, bare);
	EXPECT_EQ(L"dotgithub", bare->LanguageId());
	auto inside = service->CreateSession(L"C:\\repo\\.github\\workflows\\ci.yml", L"");
	ASSERT_NE(nullptr, inside);
	EXPECT_EQ(L"dotgithub", inside->LanguageId());
	// '.githubx' is a different segment; '/**' may not consume past it.
	EXPECT_EQ(nullptr, service->CreateSession(L"C:\\repo\\.githubx\\a.yml", L""));
}

TEST(SenpLanguageServiceTest, GrammarOverTheLimitIsRefused)
{
	TemporaryGrammarDirectory directory;
	auto extension = CreatePatternExtension(directory, {
		{ L"yaml", {}, { L".yml" }, {}, {}, {}, {}, {} },
		{ L"json", {}, { L".json" }, {}, {}, {}, {}, {} },
	});
	// Content is irrelevant: the file is refused before parsing. Grammars are
	// read at Start, so the oversized file must replace the stub beforehand.
	directory.Write("yaml.tmLanguage.json", std::string(32U * 1024U * 1024U + 1U, 'x'));
	FakeManagementService management;
	auto service = StartService(management, std::move(extension));
	EXPECT_EQ(nullptr, service->CreateSession(L"a.yml", L""));
	// A sibling language whose grammar is under the limit still loads.
	auto session = service->CreateSession(L"a.json", L"");
	ASSERT_NE(nullptr, session);
	EXPECT_EQ(L"json", session->LanguageId());
}

} // namespace
