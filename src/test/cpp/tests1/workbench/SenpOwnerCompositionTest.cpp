/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include <gtest/gtest.h>

#include "env/ShareDataTestSuite.hpp"
#include "outline/CDlgFuncList.h"
#include "workbench/SenpOwnerComposition.h"

#include <filesystem>
#include <fstream>
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
	void Revoke() noexcept { ++m_revokes; }
	[[nodiscard]] int Begins() const noexcept { return m_begins; }
	[[nodiscard]] int Publishes() const noexcept { return m_publishes; }
	[[nodiscard]] int Completions() const noexcept { return m_completions; }
	[[nodiscard]] int Revokes() const noexcept { return m_revokes; }
	[[nodiscard]] const senp::effect::PublishDocument& Document() const noexcept { return m_document; }
private:
	std::wstring m_resource;
	senp::effect::OperationContext m_context;
	senp::effect::PublishDocument m_document;
	senp::effect::CompleteCommand m_completion;
	int m_begins{}, m_publishes{}, m_completions{}, m_revokes{};
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

} // namespace
} // namespace workbench
