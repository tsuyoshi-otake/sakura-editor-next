/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include <gtest/gtest.h>
#include "workbench/editor/SenpReadonlyDocumentHost.h"
#include "markdown/CMarkdownPreviewWnd.h"
#include "theme/CThemeService.h"
#include <CommCtrl.h>

namespace workbench::editor::tests {
namespace {
using senp::effect::MarkdownSection;
using senp::effect::MetadataSection;
using senp::effect::OperationContext;
using senp::effect::PublishDocument;
using senp::effect::TableSection;
using senp::effect::TextResourceSection;

SenpReadonlyInput Input() { return { "mixed", { "sample.mixed", 7, 8, 9 }, L"run/42", L"Run 42" }; }
OperationContext Request(std::int64_t request = 1) { return { L"document-" + std::to_wstring(request), 7, 8, 9, request }; }
PublishDocument Mixed(std::int64_t revision = 1) {
	return { L"run/42", L"Run 42", revision, {
		MetadataSection{ { { L"Status", L"Passed" } } },
		MarkdownSection{ L"## Summary\n\nNative details." },
		TextResourceSection{ L"log-42", 11, senp::effect::TextStatus::Complete },
		TableSection{ { L"Check", L"Result" }, { { { L"Build", L"Passed" } } } }
	} };
}
bool PumpUntil(const std::function<bool()>& predicate, DWORD timeout = 5000) {
	const auto deadline = ::GetTickCount64() + timeout;
	while (!predicate() && ::GetTickCount64() < deadline) {
		(void)::MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
		MSG message{};
		while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { ::TranslateMessage(&message); ::DispatchMessageW(&message); }
	}
	return predicate();
}
class Authority final : public ISenpReadonlyTextResources {
public:
	senp::TextResourceScope scope{ "profile", "sample.mixed", std::string(64, 'a'), "grant", 7, 8, 9, 55 };
	bool current{ true }, returnScope{ true }, foreign{};
	int resolveCalls{}, currentCalls{};
	std::optional<senp::TextResourceScope> Resolve(const SenpReadonlyScope&, const TextResourceSection&) const override {
		auto& self = const_cast<Authority&>(*this); ++self.resolveCalls;
		if (!returnScope) return {};
		auto result = scope; if (foreign) result.extensionId = "foreign"; return result;
	}
	bool IsCurrent(const senp::TextResourceScope& value, std::wstring_view handle) const override {
		auto& self = const_cast<Authority&>(*this); ++self.currentCalls;
		return current && value == scope && handle == L"log-42";
	}
};
class SenpReadonlyDocumentHostTest : public testing::Test {
protected:
	HRESULT apartment{ ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED) };
	SenpReadonlyDocument model{ Input() };
	Authority authority;
	HWND parent{};
	std::unique_ptr<SenpReadonlyDocumentHost> host;
	std::wstring copied;
	bool probeDone{};
	bool probeFindVisible{};
	unsigned int probeDpi{}, probeTheme{ 3 };
	HBRUSH probeBrush{};
	void Publish(PublishDocument document = Mixed(), OperationContext request = Request()) {
		ASSERT_EQ(SenpDocumentResult::Accepted, model.Begin(request).result);
		ASSERT_EQ(SenpDocumentResult::Accepted, model.Apply(request, std::move(document)).result);
	}
	void Create() {
		ASSERT_TRUE(SUCCEEDED(apartment) || apartment == RPC_E_CHANGED_MODE);
		INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_STANDARD_CLASSES };
		ASSERT_TRUE(::InitCommonControlsEx(&controls));
		parent = ::CreateWindowExW(0, L"STATIC", L"Mixed SENP document", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
			50, 50, 700, 480, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
		ASSERT_NE(nullptr, parent);
		host = std::make_unique<SenpReadonlyDocumentHost>(model, 880000, &authority,
			[this](std::wstring_view text) { copied.assign(text); return true; });
		ASSERT_TRUE(host->Create(parent));
		LOGFONT font{}; wcscpy_s(font.lfFaceName, L"Consolas");
		host->SetStyle(theme::CThemeService::PaletteFor(theme::ThemeMode::Dark), font, 96);
		host->Layout({ 0, 0, 660, 400 }, 96); host->Show(true);
		::ShowWindow(parent, SW_SHOWNA); ::UpdateWindow(parent);
	}
	void TearDown() override {
		if (host) { host->Close(); host.reset(); }
		if (parent) ::DestroyWindow(parent);
		if (probeBrush) ::DeleteObject(probeBrush);
		EXPECT_TRUE(PumpUntil([] { return markdown::MarkdownPreviewWorkerRetirement::Instance().ReservedOrPendingCount() == 0; }));
		if (SUCCEEDED(apartment)) ::CoUninitialize();
	}
	LRESULT Fingerprint() {
		std::uint64_t hash = 2166136261;
		for (const auto value : { host->Pages().size(), host->ActivePage().value_or(99),
			static_cast<std::size_t>(::IsWindowVisible(host->Window())), static_cast<std::size_t>(probeFindVisible) })
			hash = (hash ^ value) * 16777619;
		for (const auto value : host->SelectedText()) hash = (hash ^ value) * 16777619;
		return static_cast<LRESULT>((hash & 0x7fffffffffffffff) | 1);
	}
	static LRESULT CALLBACK ProbeProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR context) {
		auto& self = *reinterpret_cast<SenpReadonlyDocumentHostTest*>(context);
		if (message == WM_ERASEBKGND && self.probeBrush) {
			RECT rect{}; ::GetClientRect(window, &rect); ::FillRect(reinterpret_cast<HDC>(wParam), &rect, self.probeBrush); return 1;
		}
		if (message == WM_PAINT && self.probeBrush) {
			PAINTSTRUCT paint{}; const auto dc = ::BeginPaint(window, &paint);
			::FillRect(dc, &paint.rcPaint, self.probeBrush); ::EndPaint(window, &paint); return 0;
		}
		if (message != WM_APP + 0x296) return ::DefSubclassProc(window, message, wParam, lParam);
		try {
			switch (wParam) {
			case 0: return self.host && self.host->State() == SenpDocumentHostState::Ready;
			case 1: {
				const int width = LOWORD(lParam); const unsigned int dpi = HIWORD(lParam) & 0x3ff, themeId = HIWORD(lParam) >> 10;
				if (width < 40 || width > 380 || dpi < 96 || dpi > 192 || themeId > 2) return 0;
				const bool styleChanged = self.probeDpi != dpi || self.probeTheme != themeId;
				if (styleChanged) {
					const auto palette = themeId == 2 ? theme::CThemeService::HighContrastPalette()
						: theme::CThemeService::PaletteFor(themeId == 1 ? theme::ThemeMode::Light : theme::ThemeMode::Dark);
					const auto brush = ::CreateSolidBrush(palette.canvas.ToColorRef()); if (!brush) return 0;
					if (self.probeBrush) ::DeleteObject(self.probeBrush); self.probeBrush = brush;
					LOGFONT font{}; wcscpy_s(font.lfFaceName, L"Consolas"); self.host->SetStyle(palette, font, dpi);
					self.probeDpi = dpi; self.probeTheme = themeId;
				}
				self.host->Layout({ 0, 0, width + 200, 360 }, dpi);
				if (styleChanged) ::RedrawWindow(window, nullptr, nullptr,
					RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
				return 1;
			}
			case 2: return self.host->SelectPage(lParam ? 1 : 0, false);
			case 3: self.host->Show(lParam == 0); return 1;
			case 4: self.probeDone = true; return 1;
			case 5: case 7: return reinterpret_cast<LRESULT>(self.host->Window());
			case 9: return self.Fingerprint();
			case 10:
				self.probeFindVisible = lParam != 0;
				self.host->ShowFind(lParam != 0);
				if (lParam) {
					const auto found = self.host->Find(L"Native");
					return found == markdown::PreviewFindResult::Found || found == markdown::PreviewFindResult::Wrapped;
				}
				return 1;
			case 11:
				if (lParam) self.host->SelectAll(); else (void)self.host->Find(L"Native");
				return 1;
			default: return 0;
			}
		} catch (...) { return 0; }
	}
};
}

TEST_F(SenpReadonlyDocumentHostTest, MixedSectionsPreserveOrderAndRetainedPageState)
{
	Publish(); Create();
	ASSERT_EQ(SenpDocumentHostState::Ready, host->State());
	const auto pages = host->Pages(); ASSERT_EQ(3u, pages.size());
	EXPECT_EQ(SenpDocumentPageKind::Structured, pages[0].Kind()); EXPECT_EQ(L"Details 1", pages[0].Label());
	EXPECT_EQ(SenpDocumentPageKind::TextResource, pages[1].Kind()); EXPECT_EQ(L"Text output", pages[1].Label());
	EXPECT_EQ(SenpDocumentPageKind::Structured, pages[2].Kind()); EXPECT_EQ(L"Details 2", pages[2].Label());
	EXPECT_EQ((SenpStructuredSectionRange{ 0, 2 }), pages[0].Sections());
	EXPECT_EQ((SenpStructuredSectionRange{ 2, 1 }), pages[1].Sections());
	EXPECT_EQ((SenpStructuredSectionRange{ 3, 1 }), pages[2].Sections());

	ASSERT_TRUE(host->SelectPage(1, false));
	auto read = host->TakeTextRead(); ASSERT_TRUE(read);
	EXPECT_EQ(0u, read->Offset()); EXPECT_EQ(senp::SenpTextResourceStore::kChunkBytes, read->Count());
	const std::string bytes = "first\nline\n";
	senp::TextResourceChunk chunk{ senp::TextResourceResult::Accepted, senp::TextResourceState::Complete,
		senp::TextResourceEnd::Complete, L"log-42", authority.scope.revision, 0, bytes.size(), bytes };
	ASSERT_EQ(SenpTextViewResult::Applied, host->ApplyText(*read, chunk));
	host->SelectAll(); EXPECT_EQ(L"first\nline\n", host->SelectedText()); EXPECT_TRUE(host->Copy());
	EXPECT_EQ(L"first\nline\n", copied);
	ASSERT_TRUE(host->SelectPage(0, false));
	ASSERT_TRUE(PumpUntil([&] { host->SelectAll(); return host->SelectedText().find(L"Native details") != std::wstring::npos; }));
	ASSERT_TRUE(host->SelectPage(1, false));
	EXPECT_EQ(L"first\nline\n", host->SelectedText());
	EXPECT_FALSE(host->TakeTextRead());
}

TEST_F(SenpReadonlyDocumentHostTest, HiddenPagesDoNotReadAndAuthorityRevocationErasesTheCohort)
{
	Publish(); Create();
	EXPECT_FALSE(host->TakeTextRead());
	ASSERT_TRUE(host->SelectPage(1, false)); host->Show(false);
	EXPECT_FALSE(host->TakeTextRead());
	host->Show(true); ASSERT_TRUE(host->TakeTextRead());
	authority.current = false;
	EXPECT_EQ(SenpDocumentHostState::Denied, host->Sync());
	EXPECT_TRUE(host->Pages().empty()); EXPECT_FALSE(host->ActivePage()); EXPECT_FALSE(host->TakeTextRead());
}

TEST_F(SenpReadonlyDocumentHostTest, ForeignAuthorityAndLateReadResponsesFailClosed)
{
	Publish(); authority.foreign = true; Create();
	EXPECT_EQ(SenpDocumentHostState::Denied, host->State()); EXPECT_TRUE(host->Pages().empty());
	host->Close(); host.reset(); ::DestroyWindow(parent); parent = nullptr;

	authority.foreign = false; Publish(Mixed(2), Request(2)); Create();
	ASSERT_TRUE(host->SelectPage(1, false)); const auto oldRead = host->TakeTextRead(); ASSERT_TRUE(oldRead);
	Publish(Mixed(3), Request(3));
	senp::TextResourceChunk chunk{ senp::TextResourceResult::Accepted, senp::TextResourceState::Complete,
		senp::TextResourceEnd::Complete, L"log-42", authority.scope.revision, 0, 4, "late" };
	EXPECT_EQ(SenpTextViewResult::Stale, host->ApplyText(*oldRead, chunk));
	EXPECT_EQ(SenpDocumentHostState::Ready, host->State()); EXPECT_EQ(0u, *host->ActivePage());
}

TEST_F(SenpReadonlyDocumentHostTest, MissingAuthorityAndParentDestructionReachExplicitTerminals)
{
	Publish();
	ASSERT_TRUE(SUCCEEDED(apartment) || apartment == RPC_E_CHANGED_MODE);
	parent = ::CreateWindowExW(0, L"STATIC", L"Missing text authority", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
		50, 50, 500, 350, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(nullptr, parent);
	host = std::make_unique<SenpReadonlyDocumentHost>(model, 881000);
	ASSERT_TRUE(host->Create(parent)); EXPECT_EQ(SenpDocumentHostState::Unsupported, host->State());
	EXPECT_TRUE(host->Pages().empty());
	host->Close(); host.reset(); ::DestroyWindow(parent); parent = nullptr;

	Create(); ASSERT_EQ(SenpDocumentHostState::Ready, host->State());
	::DestroyWindow(parent); parent = nullptr;
	EXPECT_EQ(SenpDocumentHostState::Closed, host->State()); EXPECT_EQ(nullptr, host->Window());
}

TEST_F(SenpReadonlyDocumentHostTest, DuplicateTextReferencesShareOneRetainedBodyAndRead)
{
	auto document = Mixed();
	document.sections = { TextResourceSection{ L"log-42", 11, senp::effect::TextStatus::Complete },
		MarkdownSection{ L"## Between" }, TextResourceSection{ L"log-42", 11, senp::effect::TextStatus::Complete } };
	Publish(std::move(document)); Create();
	const auto pages = host->Pages(); ASSERT_EQ(3u, pages.size());
	EXPECT_EQ(L"Text output 1", pages[0].Label()); EXPECT_EQ(L"Details", pages[1].Label()); EXPECT_EQ(L"Text output 2", pages[2].Label());
	const auto read = host->TakeTextRead(); ASSERT_TRUE(read);
	const std::string bytes = "shared\n";
	senp::TextResourceChunk chunk{ senp::TextResourceResult::Accepted, senp::TextResourceState::Complete,
		senp::TextResourceEnd::Complete, L"log-42", authority.scope.revision, 0, bytes.size(), bytes };
	ASSERT_EQ(SenpTextViewResult::Applied, host->ApplyText(*read, chunk));
	host->SelectAll(); EXPECT_EQ(L"shared\n", host->SelectedText());
	ASSERT_TRUE(host->SelectPage(2, false)); EXPECT_EQ(L"shared\n", host->SelectedText());
	EXPECT_FALSE(host->TakeTextRead());
}

TEST_F(SenpReadonlyDocumentHostTest, DISABLED_VisualCaptureProbe)
{
	wchar_t enabled[2]{};
	if (::GetEnvironmentVariableW(L"SAKURA_SENP_VIEW_PROBE", enabled, 2) != 1 || enabled[0] != L'1')
		GTEST_SKIP() << "Use tools/verify-senp-view-rendering.ps1 -ProbeSet MixedDocuments";
	Publish(); Create();
	ASSERT_TRUE(host->SelectPage(1, false)); const auto read = host->TakeTextRead(); ASSERT_TRUE(read);
	std::string bytes;
	for (int row = 0; row < 160; ++row) bytes += "2026-09-08 verification " + std::to_string(row) + " passed\n";
	senp::TextResourceChunk chunk{ senp::TextResourceResult::Accepted, senp::TextResourceState::Complete,
		senp::TextResourceEnd::Complete, L"log-42", authority.scope.revision, 0, bytes.size(), bytes };
	ASSERT_EQ(SenpTextViewResult::Applied, host->ApplyText(*read, chunk));
	ASSERT_TRUE(host->SelectPage(0, false));
	ASSERT_TRUE(PumpUntil([&] { host->SelectAll(); return host->SelectedText().find(L"Native details") != std::wstring::npos; }));
	ASSERT_TRUE(::SetWindowSubclass(parent, ProbeProcedure, 296, reinterpret_cast<DWORD_PTR>(this)));
	const auto deadline = ::GetTickCount64() + 300000;
	while (!probeDone && ::GetTickCount64() < deadline) {
		(void)::MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
		MSG message{}; while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { ::TranslateMessage(&message); ::DispatchMessageW(&message); }
	}
	::RemoveWindowSubclass(parent, ProbeProcedure, 296); EXPECT_TRUE(probeDone);
}

} // namespace workbench::editor::tests
