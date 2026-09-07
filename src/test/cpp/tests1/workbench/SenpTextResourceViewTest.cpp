/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include "workbench/editor/SenpTextResourceView.h"
#include "workbench/editor/SenpReadonlyWorkbench.h"
#include "workbench/editor/SenpEditorSurfaceSwitcher.h"
#include "theme/CThemeService.h"
#include <Richedit.h>
#include <CommCtrl.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <chrono>

namespace {
using namespace senp;
using namespace workbench::editor;
using Microsoft::WRL::ComPtr;
class SenpTextResourceViewTest : public ::testing::Test {
protected:
	HRESULT com{ ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED) };
	TextResourceScope scope{ "profile-1", "sakura.github-actions", std::string(64, 'a'), "grant:9", 1, 2, 3, 4 };
	SenpTextResourceStore store;
	std::wstring handle{ store.Create(scope).handle }, copied;
	std::size_t producerOffset{};
	bool copySucceeds{ true };
	EditorCoreService core;
	SenpReadonlyWorkbench inputs{ core };
	SenpReadonlyOpenResult opened{ inputs.Open({ scope.extensionId, scope.ownerGeneration, scope.workspaceRevision, scope.accountGeneration }, L"job/7/log", L"Job log") };
	HWND parent{};
	std::unique_ptr<SenpTextResourceView> view;
	std::unique_ptr<SenpEditorSurfaceSwitcher> switcher;
	bool probeDone{};
	unsigned int probeDpi{}, probeTheme{ 3 };
	HBRUSH probeBrush{};
	void CreateNative() {
		ASSERT_TRUE(SUCCEEDED(com));
		parent = ::CreateWindowExW(0, L"STATIC", L"SENP native text verification", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
			40, 40, 680, 430, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
		ASSERT_NE(parent, nullptr);
		view = std::make_unique<SenpTextResourceView>(scope, handle, [this](std::wstring_view text) { copied.assign(text); return copySucceeds; });
		ASSERT_TRUE(view->Create(parent));
		switcher = std::make_unique<SenpEditorSurfaceSwitcher>(inputs, parent);
		ASSERT_TRUE(switcher->Bind(opened.inputId, view->Window(), view->FocusWindow()));
		ASSERT_EQ(switcher->Layout({ 0, 0, 640, 360 }), SenpSurfaceProjection::Empty);
		ASSERT_EQ(switcher->Show(opened.inputId, false), SenpSurfaceProjection::Applied);
		::ShowWindow(parent, SW_SHOWNA); ::UpdateWindow(parent); Pump();
	}
	void Pump() {
		MSG message{}; for (int count = 0; count < 1000 && ::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
			::TranslateMessage(&message); ::DispatchMessageW(&message);
		}
	}
	void Append(std::string_view bytes, bool final = false) {
		if (!bytes.empty()) { ASSERT_EQ(store.Append(scope, handle, producerOffset, bytes), TextResourceResult::Accepted); producerOffset += bytes.size(); }
		if (final) ASSERT_EQ(store.Finish(scope, handle, TextResourceEnd::Complete), TextResourceResult::Accepted);
		ASSERT_EQ(view->Apply(scope, store.Read(scope, handle, view->Viewport().byteOffset, SenpTextResourceStore::kChunkBytes)), SenpTextViewResult::Applied); Pump();
	}
	std::wstring Text() {
		const auto before = view->Viewport(); view->SelectAll(); auto text = view->SelectedText();
		CHARRANGE range{ before.selectionStart, before.selectionEnd }; ::SendMessageW(view->FocusWindow(), EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));
		return text;
	}
	void Select(LONG start, LONG end) { CHARRANGE range{ start, end }; ::SendMessageW(view->FocusWindow(), EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range)); }
	void TearDown() override {
		if (switcher) { switcher->Close(); switcher.reset(); }
		if (view) { view->Close(); view.reset(); }
		if (parent) ::DestroyWindow(parent); store.Close(); Pump();
		if (probeBrush) ::DeleteObject(probeBrush);
		EXPECT_EQ(inputs.Shutdown(), SenpReadonlyStatus::Succeeded);
		if (SUCCEEDED(com)) ::CoUninitialize();
	}
	LRESULT Fingerprint() const {
		const auto state = view->Viewport(); std::uint64_t hash = 2166136261;
		for (const auto value : { state.byteOffset, state.characters, static_cast<std::size_t>(state.scrollY),
			static_cast<std::size_t>(state.selectionStart), static_cast<std::size_t>(state.selectionEnd),
			static_cast<std::size_t>(state.findVisible), static_cast<std::size_t>(::IsWindowVisible(view->Window())) }) hash = (hash ^ value) * 16777619;
		return static_cast<LRESULT>((hash & 0x7fffffffffffffff) | 1);
	}
	static LRESULT CALLBACK ProbeProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR context) {
		auto& self = *reinterpret_cast<SenpTextResourceViewTest*>(context);
		if (message == WM_ERASEBKGND && self.probeBrush) { RECT bounds{}; ::GetClientRect(window, &bounds); ::FillRect(reinterpret_cast<HDC>(wParam), &bounds, self.probeBrush); return 1; }
		if (message == WM_PAINT && self.probeBrush) { PAINTSTRUCT paint{}; const auto dc = ::BeginPaint(window, &paint); ::FillRect(dc, &paint.rcPaint, self.probeBrush); ::EndPaint(window, &paint); return 0; }
		if (message != WM_APP + 0x296) return ::DefSubclassProc(window, message, wParam, lParam);
		try {
			switch (wParam) {
			case 0: return self.view && self.view->Window();
			case 1: {
				const int width = LOWORD(lParam); const unsigned int dpi = HIWORD(lParam) & 0x3ff, themeId = HIWORD(lParam) >> 10;
				if (width < 40 || width > 380 || dpi < 96 || dpi > 192 || themeId > 2) return 0;
				const bool changed = self.probeDpi != dpi || self.probeTheme != themeId;
				if (changed) {
					const auto palette = themeId == 2 ? theme::CThemeService::HighContrastPalette()
						: theme::CThemeService::PaletteFor(themeId == 1 ? theme::ThemeMode::Light : theme::ThemeMode::Dark);
					const auto brush = ::CreateSolidBrush(palette.canvas.ToColorRef()); if (!brush) return 0;
					if (self.probeBrush) ::DeleteObject(self.probeBrush); self.probeBrush = brush;
					self.view->SetStyle(palette, dpi); self.probeDpi = dpi; self.probeTheme = themeId;
				}
				const auto result = self.switcher->Layout({ 0, 0, width + 200, 360 });
				if (changed) ::RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
				return result == SenpSurfaceProjection::Applied;
			}
			case 2: self.switcher->SetVisible(lParam == 0); return 1;
			case 3: ::SendMessageW(self.view->FocusWindow(), WM_VSCROLL, lParam ? SB_BOTTOM : SB_TOP, 0); return 1;
			case 4: self.probeDone = true; return 1;
			case 5: return reinterpret_cast<LRESULT>(self.view->Window());
			case 7: return reinterpret_cast<LRESULT>(self.view->FocusWindow());
			case 8: {
				const auto line = std::string(lParam ? "\nAppend: verification " : "\nAppend: retained selection ") + std::to_string(self.producerOffset) + "\n";
				if (self.store.Append(self.scope, self.handle, self.producerOffset, line) != TextResourceResult::Accepted) return 0;
				self.producerOffset += line.size();
				if (self.view->Apply(self.scope, self.store.Read(self.scope, self.handle, self.view->Viewport().byteOffset, SenpTextResourceStore::kChunkBytes)) != SenpTextViewResult::Applied) return 0;
				::SendMessageW(self.view->FocusWindow(), WM_VSCROLL, SB_BOTTOM, 0); return 1;
			}
			case 9: return self.Fingerprint();
			case 10: self.view->ShowFind(lParam != 0); if (lParam) (void)self.view->Find(L"verification"); ::SetFocus(window); return 1;
			default: return 0;
			}
		} catch (...) { return 0; }
	}
};
TEST_F(SenpTextResourceViewTest, NativePlainTextRetainsSelectionAndScrollDuringIncrementalAppend)
{
	ASSERT_NO_FATAL_FAILURE(CreateNative()); std::string first;
	for (int i = 0; i < 300; ++i) first += "line " + std::to_string(i) + " content\n";
	ASSERT_NO_FATAL_FAILURE(Append(first)); Select(3, 11);
	::SendMessageW(view->FocusWindow(), WM_VSCROLL, SB_BOTTOM, 0); Pump(); const auto before = view->Viewport();
	ASSERT_GT(before.scrollY, 0);
	ASSERT_NO_FATAL_FAILURE(Append("last \xe6\x97\xa5\xf0\x9f\x98\x80\n", true));
	const auto after = view->Viewport(); EXPECT_EQ(after.selectionStart, before.selectionStart); EXPECT_EQ(after.selectionEnd, before.selectionEnd);
	EXPECT_EQ(after.scrollY, before.scrollY); EXPECT_EQ(after.state, TextResourceState::Complete);
	EXPECT_EQ(Text().substr(first.size()), L"last \u65e5\xd83d\xde00\n");
	EXPECT_EQ(::SendMessageW(view->FocusWindow(), EM_GETMODIFY, 0, 0), 0);
	EXPECT_EQ(::SendMessageW(view->FocusWindow(), EM_CANUNDO, 0, 0), 0);
}
TEST_F(SenpTextResourceViewTest, NativeUnicodeSearchMovesForwardBackwardAndWrapsWithoutChangingContent)
{
	ASSERT_NO_FATAL_FAILURE(CreateNative());
	ASSERT_NO_FATAL_FAILURE(Append("Alpha \xe6\x97\xa5 beta ALPHA \xe6\x97\xa5 omega", true));
	EXPECT_EQ(view->Find(L"alpha"), SenpTextFindResult::Found); EXPECT_EQ(view->SelectedText(), L"Alpha");
	EXPECT_EQ(view->Find(L"alpha"), SenpTextFindResult::Found); EXPECT_EQ(view->SelectedText(), L"ALPHA");
	EXPECT_EQ(view->Find(L"alpha"), SenpTextFindResult::Wrapped); EXPECT_EQ(view->SelectedText(), L"Alpha");
	EXPECT_EQ(view->Find(L"alpha", true), SenpTextFindResult::Wrapped); EXPECT_EQ(view->SelectedText(), L"ALPHA");
	EXPECT_EQ(view->Find(L"\u65e5", true), SenpTextFindResult::Found); EXPECT_EQ(view->SelectedText(), L"\u65e5");
	EXPECT_EQ(view->Find(L"alpha", false, true), SenpTextFindResult::NotFound);
	EXPECT_EQ(view->Find(L""), SenpTextFindResult::Invalid); EXPECT_EQ(view->Find(std::wstring(1025, L'x')), SenpTextFindResult::Invalid);
	EXPECT_EQ(Text(), L"Alpha \u65e5 beta ALPHA \u65e5 omega");
}
TEST_F(SenpTextResourceViewTest, NativeCopyUsesTheExactCharacterSelectionAndCurrentLineWhenEmpty)
{
	ASSERT_NO_FATAL_FAILURE(CreateNative());
	ASSERT_NO_FATAL_FAILURE(Append("first\r\n\xe6\x97\xa5 second\nlast", true));
	Select(6, 7); ASSERT_TRUE(view->Copy()); EXPECT_EQ(copied, L"\u65e5");
	Select(8, 8); ASSERT_TRUE(view->Copy()); EXPECT_EQ(copied, L"\u65e5 second\n");
	EXPECT_EQ(view->Viewport().selectionStart, 8); EXPECT_EQ(view->Viewport().selectionEnd, 8);
	Select(15, 15); ASSERT_TRUE(view->Copy()); EXPECT_EQ(copied, L"last\n");
	view->SelectAll(); ASSERT_TRUE(view->Copy()); EXPECT_EQ(copied, L"first\n\u65e5 second\nlast");
	copySucceeds = false; EXPECT_FALSE(view->Copy()); EXPECT_EQ(view->Viewport().state, TextResourceState::Complete);
	view->Fail(TextResourceEnd::Cancelled); EXPECT_EQ(view->Viewport().state, TextResourceState::Complete);
}
TEST_F(SenpTextResourceViewTest, NativeEditPasteCutAndUndoMessagesCannotMutateTheLog)
{
	ASSERT_NO_FATAL_FAILURE(CreateNative());
	ASSERT_NO_FATAL_FAILURE(Append("read only", true)); view->SelectAll();
	for (UINT message : { WM_PASTE, WM_CUT, WM_CLEAR, WM_UNDO, EM_REDO, WM_CHAR }) ::SendMessageW(view->FocusWindow(), message, 'X', 0);
	EXPECT_EQ(Text(), L"read only"); EXPECT_NE(::GetWindowLongPtrW(view->FocusWindow(), GWL_STYLE) & ES_READONLY, 0);
	EXPECT_EQ(::SendMessageW(view->FocusWindow(), EM_GETTEXTMODE, 0, 0) & TM_PLAINTEXT, TM_PLAINTEXT);
	EXPECT_EQ(::SendMessageW(view->FocusWindow(), EM_GETAUTOURLDETECT, 0, 0), 0);
}
TEST_F(SenpTextResourceViewTest, NativeLateForeignAndContradictoryChunksNeverReplaceAcceptedText)
{
	ASSERT_NO_FATAL_FAILURE(CreateNative());
	ASSERT_NO_FATAL_FAILURE(Append("prefix"));
	ASSERT_EQ(store.Append(scope, handle, 6, "next"), TextResourceResult::Accepted);
	const auto chunk = store.Read(scope, handle, 6, 99); auto foreign = scope; ++foreign.accountGeneration;
	EXPECT_EQ(view->Apply(foreign, chunk), SenpTextViewResult::Stale);
	auto stale = chunk; stale.handle += L"-other"; EXPECT_EQ(view->Apply(scope, stale), SenpTextViewResult::Stale);
	stale = chunk; ++stale.revision; EXPECT_EQ(view->Apply(scope, stale), SenpTextViewResult::Stale);
	stale = chunk; stale.offset = 0; EXPECT_EQ(view->Apply(scope, stale), SenpTextViewResult::Stale);
	stale = chunk; stale.state = TextResourceState::Complete; EXPECT_EQ(view->Apply(scope, stale), SenpTextViewResult::Invalid);
	stale = chunk; stale.bytes.clear(); EXPECT_EQ(view->Apply(scope, stale), SenpTextViewResult::Invalid);
	EXPECT_EQ(Text(), L"prefix"); EXPECT_EQ(view->Apply(scope, chunk), SenpTextViewResult::Applied);
	EXPECT_EQ(Text(), L"prefixnext");
}
TEST_F(SenpTextResourceViewTest, NativeSourceTerminalIsFencedWhileItsAcceptedPrefixIsStillDraining)
{
	ASSERT_NO_FATAL_FAILURE(CreateNative()); const std::string bytes(100, 'a');
	ASSERT_EQ(store.Append(scope, handle, 0, bytes), TextResourceResult::Accepted);
	ASSERT_EQ(store.Finish(scope, handle, TextResourceEnd::Failed), TextResourceResult::Accepted);
	ASSERT_EQ(view->Apply(scope, store.Read(scope, handle, 0, 50)), SenpTextViewResult::Applied);
	EXPECT_EQ(view->Viewport().state, TextResourceState::Loading);
	auto remainder = store.Read(scope, handle, 50, 50); auto contradictory = remainder; contradictory.state = TextResourceState::Complete; contradictory.end = TextResourceEnd::Complete;
	EXPECT_EQ(view->Apply(scope, contradictory), SenpTextViewResult::Stale);
	EXPECT_EQ(view->Apply(scope, remainder), SenpTextViewResult::Applied); EXPECT_EQ(view->Viewport().state, TextResourceState::Partial);
	EXPECT_EQ(view->Viewport().end, TextResourceEnd::Failed); EXPECT_EQ(Text(), std::wstring(100, L'a'));
}
TEST_F(SenpTextResourceViewTest, NativeDecoderFailureKeepsEarlierTextAndExpiryErasesTextAndSearch)
{
	ASSERT_NO_FATAL_FAILURE(CreateNative());
	ASSERT_NO_FATAL_FAILURE(Append("private"));
	ASSERT_EQ(store.Append(scope, handle, 7, "\xe6"), TextResourceResult::Accepted);
	ASSERT_EQ(store.Finish(scope, handle, TextResourceEnd::Complete), TextResourceResult::Accepted);
	EXPECT_EQ(view->Apply(scope, store.Read(scope, handle, 7, 99)), SenpTextViewResult::DecodeFailed);
	EXPECT_EQ(view->Viewport().state, TextResourceState::Partial); EXPECT_EQ(Text(), L"private");
	EXPECT_EQ(view->Find(L"private"), SenpTextFindResult::Found);
	wchar_t statusText[512]{};
	::GetWindowTextW(::FindWindowExW(view->Window(), nullptr, L"STATIC", nullptr), statusText, 512);
	EXPECT_NE(std::wstring_view(statusText).find(L"Partial log:"), std::wstring_view::npos);
	EXPECT_NE(std::wstring_view(statusText).find(L"Match"), std::wstring_view::npos);
	copySucceeds = false; EXPECT_FALSE(view->Copy());
	::GetWindowTextW(::FindWindowExW(view->Window(), nullptr, L"STATIC", nullptr), statusText, 512);
	EXPECT_NE(std::wstring_view(statusText).find(L"Partial log:"), std::wstring_view::npos);
	view->ShowFind(true); EXPECT_TRUE(view->Viewport().findVisible);
	ASSERT_EQ(store.Expire(scope, handle), TextResourceResult::Expired);
	EXPECT_EQ(view->Apply(scope, store.Read(scope, handle, 7, 99)), SenpTextViewResult::Applied);
	EXPECT_EQ(view->Viewport().state, TextResourceState::Expired); EXPECT_TRUE(Text().empty()); EXPECT_FALSE(view->Copy());
	EXPECT_EQ(view->Find(L"private"), SenpTextFindResult::Closed);
}
TEST_F(SenpTextResourceViewTest, NativeHiddenAppendDoesNotRevealAnySurfaceAndParentDestructionClosesIt)
{
	ASSERT_NO_FATAL_FAILURE(CreateNative()); const auto root = view->Window(), text = view->FocusWindow();
	::ShowWindow(root, SW_HIDE);
	ASSERT_NO_FATAL_FAILURE(Append("hidden", true));
	EXPECT_FALSE(::IsWindowVisible(root)); EXPECT_FALSE(::IsWindowVisible(text));
	EXPECT_EQ(Text(), L"hidden"); ::DestroyWindow(parent); parent = nullptr;
	EXPECT_EQ(view->Viewport().state, TextResourceState::Closed); EXPECT_EQ(view->Window(), nullptr); EXPECT_FALSE(::IsWindow(text));
	EXPECT_EQ(view->Apply(scope, store.Read(scope, handle, 0, 10)), SenpTextViewResult::Closed);
}
TEST_F(SenpTextResourceViewTest, NativeTextAccessibilityExposesItsReadonlyValueAndSelection)
{
	ASSERT_NO_FATAL_FAILURE(CreateNative());
	ASSERT_NO_FATAL_FAILURE(Append("Accessible text", true)); Select(0, 10);
	ComPtr<IUIAutomation> automation; ASSERT_EQ(S_OK, ::CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)));
	ComPtr<IUIAutomationElement> element; ASSERT_EQ(S_OK, automation->ElementFromHandle(view->FocusWindow(), &element));
	ComPtr<IUIAutomationTextPattern> pattern; ASSERT_EQ(S_OK, element->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&pattern)));
	ComPtr<IUIAutomationTextRangeArray> selections; ASSERT_EQ(S_OK, pattern->GetSelection(&selections));
	ComPtr<IUIAutomationTextRange> selected; ASSERT_EQ(S_OK, selections->GetElement(0, &selected));
	BSTR value{}; ASSERT_EQ(S_OK, selected->GetText(100, &value)); const std::wstring actual(value, ::SysStringLen(value)); ::SysFreeString(value);
	EXPECT_EQ(actual, L"Accessible");
	VARIANT readOnly{}; ASSERT_EQ(S_OK, selected->GetAttributeValue(UIA_IsReadOnlyAttributeId, &readOnly));
	EXPECT_EQ(readOnly.vt, VT_BOOL); EXPECT_EQ(readOnly.boolVal, VARIANT_TRUE); ::VariantClear(&readOnly);
}
TEST_F(SenpTextResourceViewTest, NativeMaximumPayloadUsesOnlyIncrementalAppendsAndHasAnExactFinalLength)
{
	ASSERT_NO_FATAL_FAILURE(CreateNative());
	::ShowWindow(parent, SW_HIDE);
	struct Counts final { std::size_t append{}, replace{}; } counts;
	const auto observer = [](HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR context) -> LRESULT {
		auto& counts = *reinterpret_cast<Counts*>(context);
		if (message == EM_REPLACESEL) ++counts.append;
		if (message == WM_SETTEXT || message == EM_STREAMIN) ++counts.replace;
		return ::DefSubclassProc(window, message, wParam, lParam);
	};
	ASSERT_TRUE(::SetWindowSubclass(view->FocusWindow(), observer, 77, reinterpret_cast<DWORD_PTR>(&counts)));
	struct ObserverGuard final {
		HWND window; SUBCLASSPROC procedure;
		~ObserverGuard() { if (::IsWindow(window)) ::RemoveWindowSubclass(window, procedure, 77); }
	} guard{ view->FocusWindow(), observer };
	std::string page; for (int line = 0; line < 1024; ++line) page += std::string(63, 'x') + '\n';
	const auto started = std::chrono::steady_clock::now(); auto quarter = started;
	for (std::size_t offset = 0; offset < SenpTextResourceStore::kResourceBytes; offset += page.size()) {
		ASSERT_EQ(store.Append(scope, handle, offset, page), TextResourceResult::Accepted);
		ASSERT_EQ(view->Apply(scope, store.Read(scope, handle, offset, page.size())), SenpTextViewResult::Applied);
		if ((offset + page.size()) % (8 * 1024 * 1024) == 0) {
			const auto now = std::chrono::steady_clock::now();
			RecordProperty("append_" + std::to_string((offset + page.size()) / (1024 * 1024)) + "_mib_ms", static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now - quarter).count())); quarter = now;
		}
		ASSERT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(60));
	}
	EXPECT_EQ(store.Finish(scope, handle, TextResourceEnd::Complete), TextResourceResult::Accepted);
	EXPECT_EQ(view->Apply(scope, store.Read(scope, handle, SenpTextResourceStore::kResourceBytes, 1)), SenpTextViewResult::Applied);
	::RemoveWindowSubclass(view->FocusWindow(), observer, 77);
	GETTEXTLENGTHEX length{ GTL_PRECISE | GTL_NUMCHARS, 1200 };
	EXPECT_EQ(::SendMessageW(view->FocusWindow(), EM_GETTEXTLENGTHEX, reinterpret_cast<WPARAM>(&length), 0), static_cast<LRESULT>(SenpTextResourceStore::kResourceBytes));
	const auto actual = Text(); ASSERT_EQ(actual.size(), SenpTextResourceStore::kResourceBytes);
	const std::wstring expectedPage(page.begin(), page.end());
	for (std::size_t offset = 0; offset < actual.size(); offset += page.size())
		ASSERT_TRUE(std::wstring_view(actual).substr(offset, page.size()) == expectedPage) << "Content mismatch at " << offset;
	EXPECT_EQ(counts.append, SenpTextResourceStore::kResourceBytes / page.size()); EXPECT_EQ(counts.replace, 0u);
	EXPECT_EQ(view->Viewport().state, TextResourceState::Complete); EXPECT_EQ(view->Viewport().characters, SenpTextResourceStore::kResourceBytes);
}
TEST_F(SenpTextResourceViewTest, NativeAllocationFailureClosesTheUncertainProjection)
{
	ASSERT_NO_FATAL_FAILURE(CreateNative());
	ASSERT_NO_FATAL_FAILURE(Append("prefix"));
	const auto fail = [](HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) -> LRESULT {
		const auto result = ::DefSubclassProc(window, message, wParam, lParam);
		if (message == EM_REPLACESEL) ::SendMessageW(::GetParent(window), WM_COMMAND, MAKEWPARAM(0, EN_ERRSPACE), reinterpret_cast<LPARAM>(window));
		return result;
	};
	ASSERT_TRUE(::SetWindowSubclass(view->FocusWindow(), fail, 78, 0));
	ASSERT_EQ(store.Append(scope, handle, 6, "uncertain"), TextResourceResult::Accepted);
	EXPECT_EQ(view->Apply(scope, store.Read(scope, handle, 6, 99)), SenpTextViewResult::Failed);
	EXPECT_EQ(view->Viewport().state, TextResourceState::Closed); EXPECT_EQ(view->FocusWindow(), nullptr);
}
TEST_F(SenpTextResourceViewTest, NativeDestructionDuringAppendCannotReopenTheClosedProjection)
{
	ASSERT_NO_FATAL_FAILURE(CreateNative());
	const auto destroy = [](HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) -> LRESULT {
		const auto result = ::DefSubclassProc(window, message, wParam, lParam);
		if (message == EM_REPLACESEL) ::DestroyWindow(::GetParent(window));
		return result;
	};
	ASSERT_TRUE(::SetWindowSubclass(view->FocusWindow(), destroy, 79, 0));
	ASSERT_EQ(store.Append(scope, handle, 0, "closing"), TextResourceResult::Accepted);
	EXPECT_EQ(view->Apply(scope, store.Read(scope, handle, 0, 99)), SenpTextViewResult::Closed);
	EXPECT_EQ(view->Viewport().state, TextResourceState::Closed); EXPECT_EQ(view->Viewport().characters, 0u);
	EXPECT_EQ(view->Apply(scope, store.Read(scope, handle, 0, 99)), SenpTextViewResult::Closed);
}
TEST_F(SenpTextResourceViewTest, DISABLED_VisualCaptureProbe)
{
	wchar_t enabled[2]{};
	if (::GetEnvironmentVariableW(L"SAKURA_SENP_VIEW_PROBE", enabled, 2) != 1 || enabled[0] != L'1')
		GTEST_SKIP() << "Use tools/verify-senp-view-rendering.ps1 -ProbeSet TextResources";
	ASSERT_NO_FATAL_FAILURE(CreateNative());
	std::string content = "Job 7: readonly build log\n\n";
	for (int i = 0; i < 300; ++i) content += "2026-09-08T10:30:00Z verification " + std::to_string(i) + " passed\n";
	ASSERT_NO_FATAL_FAILURE(Append(content));
	ASSERT_TRUE(::SetWindowSubclass(parent, ProbeProcedure, 296, reinterpret_cast<DWORD_PTR>(this)));
	const auto deadline = ::GetTickCount64() + 500000;
	while (!probeDone && ::GetTickCount64() < deadline) {
		(void)::MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE); Pump();
	}
	::RemoveWindowSubclass(parent, ProbeProcedure, 296); EXPECT_TRUE(probeDone);
}
} // namespace
