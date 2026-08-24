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

} // namespace
