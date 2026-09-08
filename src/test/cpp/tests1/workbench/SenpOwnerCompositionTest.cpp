/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include <gtest/gtest.h>

#include "env/ShareDataTestSuite.hpp"
#include "outline/CDlgFuncList.h"
#include "workbench/SenpOwnerComposition.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <deque>
#include <map>

namespace workbench {
namespace {
using Clock = std::chrono::steady_clock;

class CompositionTargetState final {
public:
	void Begin(std::wstring_view resource, const senp::effect::OperationContext& context)
	{
		m_resource.assign(resource); m_context = context; ++m_begins;
	}
	[[nodiscard]] bool Publish(const senp::effect::OperationContext& context,
		senp::effect::PublishDocument document)
	{
		if (context != m_context || document.resourceId != m_resource) return false;
		m_document = std::move(document); ++m_publishes; return true;
	}
	void Complete(senp::effect::CompleteCommand completion)
	{
		m_completion = std::move(completion); ++m_completions;
	}
	bool StartToolRead(const senp::effect::OperationContext& context,
		senp::effect::StartToolRead read)
	{
		if (read.toolId != L"github" || read.operation != L"repositoryRead"
			|| m_toolTerminal || m_toolResponses.empty()) return false;
		m_lastRead = read;
		m_toolTerminal = SenpToolReadTerminal{ context, {
			read.readId, senp::effect::CompletionStatus::Succeeded,
			std::move(m_toolResponses.front()), L"" } };
		m_toolResponses.pop_front();
		++m_toolReads;
		return true;
	}
	std::optional<SenpToolReadTerminal> TakeToolRead()
	{
		auto terminal = std::move(m_toolTerminal);
		m_toolTerminal.reset();
		return terminal;
	}
	void CancelToolReads(const senp::effect::OperationContext& context)
	{
		if (m_toolTerminal && m_toolTerminal->Context().requestGeneration == context.requestGeneration)
			m_toolTerminal.reset();
	}
	void SetToolResponse(std::wstring value)
	{
		m_toolResponses.clear();
		m_toolResponses.push_back(std::move(value));
	}
	void EnqueueToolResponse(std::wstring value) { m_toolResponses.push_back(std::move(value)); }
	void Revoke() noexcept { ++m_revokes; }
	[[nodiscard]] int Begins() const noexcept { return m_begins; }
	[[nodiscard]] int Publishes() const noexcept { return m_publishes; }
	[[nodiscard]] int Completions() const noexcept { return m_completions; }
	[[nodiscard]] int Revokes() const noexcept { return m_revokes; }
	[[nodiscard]] int ToolReads() const noexcept { return m_toolReads; }
	[[nodiscard]] const senp::effect::StartToolRead& LastRead() const noexcept { return m_lastRead; }
	[[nodiscard]] const senp::effect::PublishDocument& Document() const noexcept { return m_document; }
private:
	std::wstring m_resource;
	senp::effect::OperationContext m_context;
	senp::effect::PublishDocument m_document;
	senp::effect::CompleteCommand m_completion;
	senp::effect::StartToolRead m_lastRead;
	std::optional<SenpToolReadTerminal> m_toolTerminal;
	std::deque<std::wstring> m_toolResponses;
	int m_begins{}, m_publishes{}, m_completions{}, m_revokes{}, m_toolReads{};
};

class CompositionTarget final : public ISenpOwnerProjectionTarget {
public:
	explicit CompositionTarget(std::shared_ptr<CompositionTargetState> state) noexcept
		: m_state(std::move(state)) {}
	bool BeginDocument(std::wstring_view resourceId,
		const senp::effect::OperationContext& context) noexcept override
	{
		m_state->Begin(resourceId, context); return true;
	}
	bool PublishDocument(const senp::effect::OperationContext& context,
		senp::effect::PublishDocument document) noexcept override
	{
		return m_state->Publish(context, std::move(document));
	}
	bool FailDocument(const senp::effect::OperationContext&, senp::InvocationStatus) noexcept override
	{
		return false;
	}
	bool CompleteCommand(const senp::effect::OperationContext&,
		senp::effect::CompleteCommand completion) noexcept override
	{
		m_state->Complete(std::move(completion)); return true;
	}
	bool ReleaseResource(std::wstring_view) noexcept override { return true; }
	bool StartToolRead(const senp::effect::OperationContext& context,
		senp::effect::StartToolRead read) noexcept override
	{
		return m_state->StartToolRead(context, std::move(read));
	}
	std::optional<SenpToolReadTerminal> TakeToolRead() noexcept override
	{
		return m_state->TakeToolRead();
	}
	void CancelToolReads(const senp::effect::OperationContext& context) noexcept override
	{
		m_state->CancelToolReads(context);
	}
	void Revoke() noexcept override { m_state->Revoke(); }
private:
	std::shared_ptr<CompositionTargetState> m_state;
};

class CompositionBody final : public viewcontainer::ISenpViewBody {
public:
	explicit CompositionBody(viewcontainer::SenpViewBodyHost host) : m_host(std::move(host))
	{
		m_window = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD, 0, 0, 1, 1,
			m_host.parent, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	}
	~CompositionBody() override { Close(); }
	HWND Window() const noexcept override { return m_window; }
	void Layout(const RECT& bounds, unsigned int) noexcept override
	{
		if (m_window) (void)::SetWindowPos(m_window, nullptr, bounds.left, bounds.top,
			bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_NOACTIVATE | SWP_NOZORDER);
	}
	void SetVisible(const bool visible) noexcept override
	{
		if (m_window) ::ShowWindow(m_window, visible ? SW_SHOWNA : SW_HIDE);
	}
	void SetPalette(const theme::ThemePalette&, layout::EViewContainerLocation) noexcept override {}
	bool Focus() noexcept override
	{
		if (!m_window) return false;
		::SetFocus(m_window);
		return ::GetFocus() == m_window;
	}
	bool PreTranslate(MSG&) noexcept override { return false; }
	void Close() noexcept override
	{
		m_host = {};
		if (m_window) { ::DestroyWindow(m_window); m_window = nullptr; }
	}
private:
	viewcontainer::SenpViewBodyHost m_host;
	HWND m_window{};
};

class SenpOwnerComposition : public testing::Test, public env::ShareDataTestSuite {
protected:
	static void SetUpTestSuite() { SetUpShareData(); }
	static void TearDownTestSuite() { TearDownShareData(); }
	void SetUp() override
	{
		m_owner = ::CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 640, 480,
			nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
		ASSERT_NE(nullptr, m_owner);
	}
	void TearDown() override { if (m_owner) ::DestroyWindow(m_owner); }

	template<class Predicate>
	bool Await(CSenpOwnerComposition& composition, Predicate predicate) const
	{
		const auto deadline = Clock::now() + std::chrono::seconds(12);
		while (Clock::now() < deadline) {
			if (!composition.Poll(Clock::now())) return false;
			if (predicate()) return true;
			::Sleep(5);
		}
		return false;
	}

	HWND m_owner{};
};

TEST_F(SenpOwnerComposition, RealSamplePublishesTwoTreesAndStructuredDocument)
{
	const auto fixtureEnvironment = _wgetenv(L"SAKURA_SENP_RUNTIME_FIXTURES");
	if (!fixtureEnvironment || !*fixtureEnvironment) GTEST_SKIP() << "SENP runtime fixtures are not configured";
	const std::filesystem::path fixtures(fixtureEnvironment);
	const auto host = fixtures / L"sakura-senp-host.exe";
	const auto component = fixtures / L"sample-extension.wasm";
	std::ifstream digestFile(fixtures / L"sample-extension.sha256");
	std::string digest;
	digestFile >> digest;
	ASSERT_TRUE(std::filesystem::is_regular_file(host));
	ASSERT_TRUE(std::filesystem::is_regular_file(component));
	ASSERT_EQ(64U, digest.size());

	layout::WorkbenchContributionRegistry catalog;
	CDlgFuncList dialog;
	viewcontainer::CViewContainerPages pages(dialog);
	ASSERT_TRUE(pages.Create(m_owner));
	CSenpOwnerComposition composition(catalog, pages);
	auto target = std::make_shared<CompositionTargetState>();
	std::map<std::wstring, std::shared_ptr<tree::SenpTreeProvider>, std::less<>> providers;
	layout::WorkbenchViewContainerDescriptor container{
		"sample.senp", "SENP Sample", layout::EViewContainerLocation::Sidebar, 90,
		"$(beaker)", false, { layout::EViewContainerLocation::Sidebar },
	};
	std::vector<SenpOwnerTreeContribution> trees;
	trees.emplace_back(layout::WorkbenchViewDescriptor{
		"sample.projects", "sample.senp", "Projects", 10, true, true, "senp.tree" },
		std::vector<std::string>{ "sample.openDetails" });
	trees.emplace_back(layout::WorkbenchViewDescriptor{
		"sample.states", "sample.senp", "States", 20, true, true, "senp.tree" },
		std::vector<std::string>{});
	SenpOwnerPublicationOptions publication(
		m_owner, { std::move(container) }, std::move(trees),
		std::make_unique<CompositionTarget>(target), [](std::string_view) { return true; },
		[&providers](viewcontainer::SenpViewBodyHost host,
			std::shared_ptr<tree::SenpTreeProvider> provider, std::wstring) {
			providers.emplace(std::wstring(provider->ViewId()), provider);
			auto body = std::make_unique<CompositionBody>(std::move(host));
			return body->Window() ? std::unique_ptr<viewcontainer::ISenpViewBody>(std::move(body)) : nullptr;
		});
	const auto now = Clock::now();
	const auto accepted = composition.Activate({
		.hostExecutable = host.native(), .modulePath = component.native(),
		.moduleSha256 = std::wstring(digest.begin(), digest.end()),
		.extensionId = L"sakura-senp-sample",
		.context = { .workspaceRevision = 3, .accountGeneration = 0 },
	}, std::wstring(digest.begin(), digest.end()), std::move(publication), now);
	ASSERT_EQ(senp::OwnerChangeStatus::Accepted, accepted.status);
	std::optional<senp::OwnerChangeResult> transition;
	ASSERT_TRUE(Await(composition, [&] {
		transition = composition.TakeTransition();
		return transition.has_value();
	}));
	ASSERT_EQ(senp::OwnerChangeStatus::Activated, transition->status);
	ASSERT_EQ(2U, providers.size());
	ASSERT_TRUE(pages.Contains("sample.senp"));

	for (auto& [id, provider] : providers) provider->SetVisible(true, Clock::now());
	ASSERT_TRUE(Await(composition, [&] {
		return providers.at(L"sample.projects")->Model().ItemCount() == 2
			&& providers.at(L"sample.states")->Model().ItemCount() == 3;
	}));
	auto projects = providers.at(L"sample.projects");
	ASSERT_TRUE(projects->Select(L"project:alpha"));
	ASSERT_TRUE(projects->Execute(L"project:alpha"));
	ASSERT_TRUE(Await(composition, [&] { return target->Publishes() == 1; }));
	EXPECT_EQ(1, target->Begins());
	EXPECT_EQ(1, target->Completions());
	EXPECT_EQ(L"Alpha details", target->Document().title);
	ASSERT_EQ(4U, target->Document().sections.size());
	EXPECT_TRUE(std::holds_alternative<senp::effect::TextResourceSection>(
		target->Document().sections.back()));

	EXPECT_TRUE(composition.Close());
	EXPECT_EQ(1, target->Revokes());
	EXPECT_TRUE(catalog.Snapshot().owners.empty());
	pages.Close();
}

TEST_F(SenpOwnerComposition, RealGithubIssuesAndPullRequestsReachNativeProviders)
{
	const auto fixtureEnvironment = _wgetenv(L"SAKURA_SENP_RUNTIME_FIXTURES");
	if (!fixtureEnvironment || !*fixtureEnvironment) GTEST_SKIP() << "SENP runtime fixtures are not configured";
	const std::filesystem::path fixtures(fixtureEnvironment);
	const auto host = fixtures / L"sakura-senp-host.exe";
	const auto component = fixtures / L"github-pull-requests-extension.wasm";
	std::ifstream digestFile(fixtures / L"github-pull-requests-extension.sha256");
	std::string digest;
	digestFile >> digest;
	ASSERT_TRUE(std::filesystem::is_regular_file(host));
	ASSERT_TRUE(std::filesystem::is_regular_file(component));
	ASSERT_EQ(64U, digest.size());

	layout::WorkbenchContributionRegistry catalog;
	CDlgFuncList dialog;
	viewcontainer::CViewContainerPages pages(dialog);
	ASSERT_TRUE(pages.Create(m_owner));
	CSenpOwnerComposition composition(catalog, pages);
	auto target = std::make_shared<CompositionTargetState>();
	target->SetToolResponse(LR"({"body":[{"id":11,"number":7,"title":"Visible issue","state":"open","user":{"login":"octocat"},"labels":[{"name":"bug"}],"html_url":"https://github.com/o/r/issues/7","comments":2},{"id":12,"number":8,"title":"Filtered PR","state":"open","user":{"login":"hubot"},"labels":[],"html_url":"https://github.com/o/r/pull/8","comments":0,"pull_request":{}}],"nextPage":2})");
	target->EnqueueToolResponse(LR"({"id":11,"number":7,"title":"Visible issue","state":"open","user":{"login":"octocat"},"labels":[{"name":"bug"}],"html_url":"https://github.com/o/r/issues/7","comments":2,"body":"Issue body","created_at":"2026-09-01T00:00:00Z","updated_at":"2026-09-02T00:00:00Z"})");
	target->EnqueueToolResponse(LR"({"body":[{"id":91,"user":{"login":"hubot"},"body":"Comment body","html_url":"https://github.com/o/r/issues/7#issuecomment-91","created_at":"2026-09-02T01:00:00Z","updated_at":"2026-09-02T01:00:00Z"}],"nextPage":2})");
	target->EnqueueToolResponse(LR"({"id":91,"user":{"login":"hubot"},"body":"Comment body","html_url":"https://github.com/o/r/issues/7#issuecomment-91","created_at":"2026-09-02T01:00:00Z","updated_at":"2026-09-02T01:00:00Z"})");
	target->EnqueueToolResponse(LR"({"body":[{"id":51,"number":8,"title":"Cross-fork change","state":"open","user":{"login":"contributor"},"labels":[{"name":"ready"}],"html_url":"https://github.com/base/project/pull/8","comments":0,"draft":false,"merged_at":null,"base":{"ref":"main","sha":"bbbb","repo":{"full_name":"base/project"}},"head":{"ref":"feature","sha":"hhhh","repo":{"full_name":"fork/project"}}}]})");
	target->EnqueueToolResponse(LR"({"id":51,"number":8,"title":"Cross-fork change","state":"closed","user":{"login":"contributor"},"labels":[{"name":"ready"}],"html_url":"https://github.com/base/project/pull/8","comments":0,"draft":false,"merged_at":"2026-09-03T00:00:00Z","base":{"ref":"main","sha":"bbbb","repo":{"full_name":"base/project"}},"head":{"ref":"feature","sha":"hhhh","repo":{"full_name":"fork/project"}},"body":"Pull request body","created_at":"2026-09-01T00:00:00Z","updated_at":"2026-09-03T00:00:00Z"})");
	std::map<std::wstring, std::shared_ptr<tree::SenpTreeProvider>, std::less<>> providers;
	layout::WorkbenchViewContainerDescriptor container{
		"github-pull-requests", "GitHub", layout::EViewContainerLocation::Sidebar, 6,
		"$(github)", false, { layout::EViewContainerLocation::Sidebar },
	};
	std::vector<SenpOwnerTreeContribution> trees;
	trees.emplace_back(layout::WorkbenchViewDescriptor{
		"pr:github", "github-pull-requests", "Pull Requests", 10, true, true, "senp.tree" },
		std::vector<std::string>{ "github.openPullRequest" });
	trees.emplace_back(layout::WorkbenchViewDescriptor{
		"issues:github", "github-pull-requests", "Issues", 20, true, true, "senp.tree" },
		std::vector<std::string>{ "github.openIssue", "github.openIssueComment" });
	SenpOwnerPublicationOptions publication(
		m_owner, { std::move(container) }, std::move(trees),
		std::make_unique<CompositionTarget>(target), [](std::string_view) { return true; },
		[&providers](viewcontainer::SenpViewBodyHost host,
			std::shared_ptr<tree::SenpTreeProvider> provider, std::wstring) {
			providers.emplace(std::wstring(provider->ViewId()), provider);
			auto body = std::make_unique<CompositionBody>(std::move(host));
			return body->Window() ? std::unique_ptr<viewcontainer::ISenpViewBody>(std::move(body)) : nullptr;
		});
	const auto now = Clock::now();
	const auto accepted = composition.Activate({
		.hostExecutable = host.native(), .modulePath = component.native(),
		.moduleSha256 = std::wstring(digest.begin(), digest.end()),
		.extensionId = L"sakura-github-pull-requests",
		.context = { .workspaceRevision = 3, .accountGeneration = 4 },
	}, std::wstring(digest.begin(), digest.end()), std::move(publication), now);
	ASSERT_EQ(senp::OwnerChangeStatus::Accepted, accepted.status);
	std::optional<senp::OwnerChangeResult> transition;
	ASSERT_TRUE(Await(composition, [&] {
		transition = composition.TakeTransition();
		return transition.has_value();
	}));
	ASSERT_EQ(senp::OwnerChangeStatus::Activated, transition->status);
	ASSERT_EQ(2U, providers.size());

	providers.at(L"issues:github")->SetVisible(true, Clock::now());
	ASSERT_TRUE(Await(composition, [&] {
		return providers.at(L"issues:github")->Model().ItemCount() == 1;
	}));
	EXPECT_EQ(1, target->ToolReads());
	EXPECT_EQ(L"issues:open:1", target->LastRead().readId);
	const auto& model = providers.at(L"issues:github")->Model();
	const auto issue = model.Node(L"issue:11:7");
	ASSERT_TRUE(issue);
	EXPECT_EQ(L"#7 Visible issue", issue->item.label);
	const auto root = model.Node(L"");
	ASSERT_TRUE(root);
	EXPECT_EQ(L"issues:open:2", root->nextCursor);

	ASSERT_TRUE(providers.at(L"issues:github")->Select(L"issue:11:7"));
	ASSERT_TRUE(providers.at(L"issues:github")->Execute(L"issue:11:7"));
	ASSERT_TRUE(Await(composition, [&] { return target->Publishes() == 1; }));
	EXPECT_EQ(L"#7 Visible issue", target->Document().title);
	ASSERT_EQ(2U, target->Document().sections.size());
	ASSERT_TRUE(std::holds_alternative<senp::effect::MarkdownSection>(
		target->Document().sections[1]));
	EXPECT_EQ(L"Issue body", std::get<senp::effect::MarkdownSection>(
		target->Document().sections[1]).text);

	ASSERT_EQ(tree::TreeResult::Applied, providers.at(L"issues:github")->SetExpanded(
		L"issue:11:7", true, Clock::now()));
	ASSERT_TRUE(Await(composition, [&] {
		return providers.at(L"issues:github")->Model().Node(L"comment:91").has_value();
	}));
	const auto comment = providers.at(L"issues:github")->Model().Node(L"comment:91");
	ASSERT_TRUE(comment);
	EXPECT_EQ(L"comments:2", providers.at(L"issues:github")->Model().Node(
		L"issue:11:7")->nextCursor);
	ASSERT_TRUE(providers.at(L"issues:github")->Select(L"comment:91"));
	ASSERT_TRUE(providers.at(L"issues:github")->Execute(L"comment:91"));
	ASSERT_TRUE(Await(composition, [&] { return target->Publishes() == 2; }));
	EXPECT_EQ(L"Comment by @hubot", target->Document().title);
	ASSERT_TRUE(std::holds_alternative<senp::effect::MarkdownSection>(
		target->Document().sections[1]));
	EXPECT_EQ(L"Comment body", std::get<senp::effect::MarkdownSection>(
		target->Document().sections[1]).text);
	EXPECT_EQ(4, target->ToolReads());

	providers.at(L"pr:github")->SetVisible(true, Clock::now());
	ASSERT_TRUE(Await(composition, [&] {
		return providers.at(L"pr:github")->Model().ItemCount() == 1;
	}));
	const auto pull = providers.at(L"pr:github")->Model().Node(L"pull:51:8");
	ASSERT_TRUE(pull);
	EXPECT_EQ(L"#8 Cross-fork change", pull->item.label);
	ASSERT_TRUE(providers.at(L"pr:github")->Select(L"pull:51:8"));
	ASSERT_TRUE(providers.at(L"pr:github")->Execute(L"pull:51:8"));
	ASSERT_TRUE(Await(composition, [&] { return target->Publishes() == 3; }));
	EXPECT_EQ(L"#8 Cross-fork change", target->Document().title);
	ASSERT_TRUE(std::holds_alternative<senp::effect::MetadataSection>(
		target->Document().sections[0]));
	const auto& fields = std::get<senp::effect::MetadataSection>(
		target->Document().sections[0]).fields;
	EXPECT_TRUE(std::ranges::any_of(fields, [](const auto& field) {
		return field.name == L"State" && field.value == L"merged";
	}));
	EXPECT_TRUE(std::ranges::any_of(fields, [](const auto& field) {
		return field.name == L"Base" && field.value.find(L"base/project:main") != std::wstring::npos;
	}));
	EXPECT_TRUE(std::ranges::any_of(fields, [](const auto& field) {
		return field.name == L"Head" && field.value.find(L"fork/project:feature") != std::wstring::npos;
	}));
	EXPECT_EQ(L"pulls/8", target->LastRead().arguments.front().value);
	EXPECT_EQ(6, target->ToolReads());
	EXPECT_TRUE(composition.Close());
	EXPECT_EQ(1, target->Revokes());
	pages.Close();
}

TEST_F(SenpOwnerComposition, RealGithubActionsReachNativeProviders)
{
	const auto fixtureEnvironment = _wgetenv(L"SAKURA_SENP_RUNTIME_FIXTURES");
	if (!fixtureEnvironment || !*fixtureEnvironment) GTEST_SKIP() << "SENP runtime fixtures are not configured";
	const std::filesystem::path fixtures(fixtureEnvironment);
	const auto host = fixtures / L"sakura-senp-host.exe";
	const auto component = fixtures / L"github-actions-extension.wasm";
	std::ifstream digestFile(fixtures / L"github-actions-extension.sha256");
	std::string digest;
	digestFile >> digest;
	ASSERT_TRUE(std::filesystem::is_regular_file(host));
	ASSERT_TRUE(std::filesystem::is_regular_file(component));
	ASSERT_EQ(64U, digest.size());

	layout::WorkbenchContributionRegistry catalog;
	CDlgFuncList dialog;
	viewcontainer::CViewContainerPages pages(dialog);
	ASSERT_TRUE(pages.Create(m_owner));
	CSenpOwnerComposition composition(catalog, pages);
	auto target = std::make_shared<CompositionTargetState>();
	target->EnqueueToolResponse(LR"({"body":{"total_count":1,"workflows":[{"id":31,"name":"Build","path":".github/workflows/build.yml","state":"active","html_url":"https://github.com/o/r/actions/workflows/build.yml"}]}})");
	target->EnqueueToolResponse(LR"({"body":{"total_count":1,"workflow_runs":[{"id":51,"workflow_id":31,"run_number":8,"run_attempt":2,"name":"Build","display_title":"Build changes","event":"push","head_branch":"main","head_sha":"abcd","status":"in_progress","conclusion":null,"created_at":"2026-09-01T00:00:00Z","updated_at":"2026-09-02T00:00:00Z","run_started_at":null,"html_url":"https://github.com/o/r/actions/runs/51"}]}})");
	target->EnqueueToolResponse(LR"({"id":51,"workflow_id":31,"run_number":8,"run_attempt":2,"name":"Build","display_title":"Build changes","event":"push","head_branch":"main","head_sha":"abcd","status":"in_progress","conclusion":null,"created_at":"2026-09-01T00:00:00Z","updated_at":"2026-09-02T00:00:00Z","run_started_at":null,"html_url":"https://github.com/o/r/actions/runs/51"})");
	target->EnqueueToolResponse(LR"({"id":51,"workflow_id":31,"run_number":8,"run_attempt":1,"name":"Build","display_title":"Build changes","event":"push","head_branch":"main","head_sha":"abcd","status":"in_progress","conclusion":null,"created_at":"2026-09-01T00:00:00Z","updated_at":"2026-09-02T00:00:00Z","run_started_at":null,"html_url":"https://github.com/o/r/actions/runs/51"})");
	std::map<std::wstring, std::shared_ptr<tree::SenpTreeProvider>, std::less<>> providers;
	layout::WorkbenchViewContainerDescriptor container{
		"github-actions", "GitHub Actions", layout::EViewContainerLocation::Sidebar, 6,
		"$(play-circle)", false, { layout::EViewContainerLocation::Sidebar },
	};
	std::vector<SenpOwnerTreeContribution> trees;
	trees.emplace_back(layout::WorkbenchViewDescriptor{
        "github-actions.workflows", "github-actions", "Workflows", 10, true, true, "senp.tree" },
        std::vector<std::string>{ "github-actions.workflow.run.open" });
    trees.emplace_back(layout::WorkbenchViewDescriptor{
        "github-actions.current-branch", "github-actions", "Current Branch", 20, true, true, "senp.tree" },
        std::vector<std::string>{ "github-actions.workflow.run.open" });
	SenpOwnerPublicationOptions publication(
		m_owner, { std::move(container) }, std::move(trees),
		std::make_unique<CompositionTarget>(target), [](std::string_view) { return true; },
		[&providers](viewcontainer::SenpViewBodyHost host,
			std::shared_ptr<tree::SenpTreeProvider> provider, std::wstring) {
			providers.emplace(std::wstring(provider->ViewId()), provider);
			auto body = std::make_unique<CompositionBody>(std::move(host));
			return body->Window() ? std::unique_ptr<viewcontainer::ISenpViewBody>(std::move(body)) : nullptr;
		});
	const auto now = Clock::now();
	const auto accepted = composition.Activate({
		.hostExecutable = host.native(), .modulePath = component.native(),
		.moduleSha256 = std::wstring(digest.begin(), digest.end()),
		.extensionId = L"sakura-github-actions",
		.context = { .workspaceRevision = 3, .accountGeneration = 4 },
	}, std::wstring(digest.begin(), digest.end()), std::move(publication), now);
	ASSERT_EQ(senp::OwnerChangeStatus::Accepted, accepted.status);
	std::optional<senp::OwnerChangeResult> transition;
	ASSERT_TRUE(Await(composition, [&] {
		transition = composition.TakeTransition();
		return transition.has_value();
	}));
	ASSERT_EQ(senp::OwnerChangeStatus::Activated, transition->status);
	ASSERT_EQ(2U, providers.size());


    const auto provider = providers.at(L"github-actions.workflows");
    provider->SetVisible(true, Clock::now());
    ASSERT_TRUE(Await(composition, [&] { return provider->Model().Node(L"workflow:31").has_value(); }));
    EXPECT_EQ(L"actions/workflows", target->LastRead().arguments.front().value);
    ASSERT_EQ(tree::TreeResult::Applied, provider->SetExpanded(L"workflow:31", true, Clock::now()));
    ASSERT_TRUE(Await(composition, [&] { return provider->Model().Node(L"run:51").has_value(); }));
    EXPECT_EQ(L"actions/workflows/31/runs", target->LastRead().arguments.front().value);
    ASSERT_EQ(tree::TreeResult::Applied, provider->SetExpanded(L"run:51", true, Clock::now()));
    ASSERT_TRUE(Await(composition, [&] { return provider->Model().Node(L"attempt:51:1").has_value(); }));
    EXPECT_TRUE(provider->Model().Node(L"attempt:51:2").has_value());
    EXPECT_EQ(L"actions/runs/51", target->LastRead().arguments.front().value);
    ASSERT_TRUE(provider->Select(L"attempt:51:1"));
    ASSERT_TRUE(provider->Execute(L"attempt:51:1"));
    ASSERT_TRUE(Await(composition, [&] { return target->Publishes() == 1; }));
    EXPECT_EQ(L"github-actions-run:51:1", target->Document().resourceId);
    ASSERT_TRUE(std::holds_alternative<senp::effect::MetadataSection>(target->Document().sections[0]));
    const auto& fields = std::get<senp::effect::MetadataSection>(target->Document().sections[0]).fields;
    EXPECT_TRUE(std::ranges::any_of(fields, [](const auto& field) {
        return field.name == L"Attempt" && field.value == L"1";
    }));
    EXPECT_TRUE(std::ranges::any_of(fields, [](const auto& field) {
        return field.name == L"State" && field.value == L"in_progress";
    }));
    EXPECT_EQ(L"actions/runs/51/attempts/1", target->LastRead().arguments.front().value);
    EXPECT_EQ(4, target->ToolReads());
    EXPECT_TRUE(composition.Close());
    EXPECT_EQ(1, target->Revokes());
    pages.Close();
}

} // namespace
} // namespace workbench
