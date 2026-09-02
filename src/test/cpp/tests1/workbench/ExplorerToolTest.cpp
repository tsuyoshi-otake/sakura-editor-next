/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include <CommCtrl.h>
#include <winioctl.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

#include "workbench/explorer/CExplorerTool.h"

namespace {

using workbench::explorer::CExplorerTool;
using workbench::explorer::ExplorerEditorActivationAction;
using workbench::explorer::ExplorerEntry;
using workbench::explorer::ExplorerFileActivationKind;
using workbench::explorer::ExplorerPalette;
using workbench::explorer::ExplorerWelcomeAction;
using workbench::explorer::ExplorerWelcomeBlockKind;
using workbench::explorer::ExplorerWelcomeState;
using workbench::explorer::BuildExplorerWelcomeBlocks;
using workbench::explorer::PlanExplorerEditorActivation;

struct JunctionReparseData {
	DWORD reparseTag;
	USHORT reparseDataLength;
	USHORT reserved;
	USHORT substituteNameOffset;
	USHORT substituteNameLength;
	USHORT printNameOffset;
	USHORT printNameLength;
	wchar_t pathBuffer[MAXIMUM_REPARSE_DATA_BUFFER_SIZE / sizeof(wchar_t)];
};

static_assert(offsetof(JunctionReparseData, pathBuffer) == 16);

class TemporaryDirectory final {
public:
	TemporaryDirectory()
	{
		wchar_t tempPath[MAX_PATH]{};
		wchar_t candidate[MAX_PATH]{};
		EXPECT_NE(0u, ::GetTempPathW(static_cast<DWORD>(std::size(tempPath)), tempPath));
		EXPECT_NE(0u, ::GetTempFileNameW(tempPath, L"sce", 0, candidate));
		(void)::DeleteFileW(candidate);
		EXPECT_TRUE(::CreateDirectoryW(candidate, nullptr));
		m_path = candidate;
	}

	~TemporaryDirectory()
	{
		std::error_code ignored;
		std::filesystem::remove_all(m_path, ignored);
	}

	const std::filesystem::path& Path() const noexcept { return m_path; }

private:
	std::filesystem::path m_path;
};

void CreateEmptyFile(const std::filesystem::path& path)
{
	const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	ASSERT_NE(INVALID_HANDLE_VALUE, file);
	::CloseHandle(file);
}

bool CreateDirectoryJunction(const std::filesystem::path& junction, const std::filesystem::path& target)
{
	if (!::CreateDirectoryW(junction.c_str(), nullptr)) return false;
	const HANDLE directory = ::CreateFileW(junction.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (directory == INVALID_HANDLE_VALUE) return false;

	const std::wstring substituteName = L"\\??\\" + target.wstring();
	const std::wstring printName = target.wstring();
	JunctionReparseData data{};
	data.reparseTag = IO_REPARSE_TAG_MOUNT_POINT;
	data.substituteNameOffset = 0;
	data.substituteNameLength = static_cast<USHORT>(substituteName.size() * sizeof(wchar_t));
	data.printNameOffset = data.substituteNameLength + sizeof(wchar_t);
	data.printNameLength = static_cast<USHORT>(printName.size() * sizeof(wchar_t));
	data.reparseDataLength = static_cast<USHORT>(data.substituteNameLength + data.printNameLength + 12);
	std::memcpy(data.pathBuffer, substituteName.c_str(),
		(substituteName.size() + 1) * sizeof(wchar_t));
	std::memcpy(reinterpret_cast<std::byte*>(data.pathBuffer) + data.printNameOffset,
		printName.c_str(), (printName.size() + 1) * sizeof(wchar_t));

	DWORD ignored{};
	const bool created = ::DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, &data,
		static_cast<DWORD>(sizeof(DWORD) + sizeof(USHORT) * 2 + data.reparseDataLength), nullptr, 0, &ignored, nullptr) != FALSE;
	::CloseHandle(directory);
	return created;
}

bool PumpMessagesUntil(const std::function<bool()>& predicate, std::chrono::milliseconds timeout)
{
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	do {
		MSG message{};
		while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
			::TranslateMessage(&message);
			::DispatchMessageW(&message);
		}
		if (predicate()) return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	} while (std::chrono::steady_clock::now() < deadline);
	return predicate();
}

std::wstring ItemText(HWND tree, HTREEITEM item)
{
	wchar_t text[1024]{};
	TVITEMW value{};
	value.mask = TVIF_TEXT;
	value.hItem = item;
	value.pszText = text;
	value.cchTextMax = static_cast<int>(std::size(text));
	return TreeView_GetItem(tree, &value) ? std::wstring(text) : std::wstring{};
}

// The open folder has no row of its own: VS Code draws a root row only in a
// multi-root workspace, so with one folder the tree's top level *is* that
// folder's contents. Passing nullptr as the parent addresses that level.
constexpr HTREEITEM kWorkspaceLevel = nullptr;

HTREEITEM FindDirectChild(HWND tree, HTREEITEM parent, std::wstring_view text)
{
	for (auto item = TreeView_GetChild(tree, parent); item != nullptr; item = TreeView_GetNextSibling(tree, item)) {
		if (ItemText(tree, item) == text) return item;
	}
	return nullptr;
}

bool IsExpanded(HWND tree, HTREEITEM item)
{
	return item != nullptr &&
		(TreeView_GetItemState(tree, item, TVIS_EXPANDED) & TVIS_EXPANDED) != 0;
}

void SendTreeMouseClickAt(HWND tree, int x, int y)
{
	(void)::SendMessageW(tree, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
	NMHDR notification{};
	notification.hwndFrom = tree;
	notification.idFrom = static_cast<UINT_PTR>(::GetDlgCtrlID(tree));
	notification.code = NM_CLICK;
	(void)::SendMessageW(::GetParent(tree), WM_NOTIFY, notification.idFrom,
		reinterpret_cast<LPARAM>(&notification));
}

void SendTreeMouseClick(HWND tree, HTREEITEM item)
{
	RECT label{};
	ASSERT_TRUE(TreeView_GetItemRect(tree, item, &label, TRUE));
	const int x = label.left + std::max(1L, (label.right - label.left) / 2);
	const int y = label.top + std::max(1L, (label.bottom - label.top) / 2);
	SendTreeMouseClickAt(tree, x, y);
}

void SendTreeMouseDoubleClick(HWND tree, HTREEITEM item)
{
	RECT label{};
	ASSERT_TRUE(TreeView_GetItemRect(tree, item, &label, TRUE));
	const int x = label.left + std::max(1L, (label.right - label.left) / 2);
	const int y = label.top + std::max(1L, (label.bottom - label.top) / 2);
	(void)::SendMessageW(tree, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
	(void)::SendMessageW(tree, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(x, y));
	(void)::SendMessageW(tree, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
}

HWND CreateHiddenParentWindow()
{
	return ::CreateWindowExW(0, L"STATIC", L"Explorer test parent", WS_OVERLAPPED,
		CW_USEDEFAULT, CW_USEDEFAULT, 800, 600, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
}

TEST(ExplorerTool, SortsDirectoriesFirstThenNamesCaseInsensitively)
{
	std::vector<ExplorerEntry> entries{
		{ L"zebra.txt", L"C:\\root\\zebra.txt", false, false },
		{ L"beta", L"C:\\root\\beta", true, false },
		{ L"Alpha", L"C:\\root\\Alpha", true, false },
		{ L"apple.txt", L"C:\\root\\apple.txt", false, false },
	};

	const auto sorted = CExplorerTool::SortEntries(std::move(entries));
	ASSERT_EQ(4u, sorted.size());
	EXPECT_EQ(L"Alpha", sorted[0].name);
	EXPECT_EQ(L"beta", sorted[1].name);
	EXPECT_EQ(L"apple.txt", sorted[2].name);
	EXPECT_EQ(L"zebra.txt", sorted[3].name);
}

TEST(ExplorerTool, PreservesWorkspaceFolderDisplayCasingInsteadOfCanonicalPath)
{
	EXPECT_EQ(L"sakuracode", CExplorerTool::WorkspaceDisplayName(L"C:\\Codes\\tsuyoshi-otake\\sakuracode"));
	EXPECT_EQ(L"sakuraCode", CExplorerTool::WorkspaceDisplayName(L"C:/Codes/tsuyoshi-otake/sakuraCode/"));
	EXPECT_EQ(L"share", CExplorerTool::WorkspaceDisplayName(L"\\\\server\\share\\"));
}

TEST(ExplorerTool, TreatsReparseDirectoriesAsLeaves)
{
	const ExplorerEntry normalDirectory{ L"source", L"C:\\root\\source", true, false };
	const ExplorerEntry junction{ L"linked", L"C:\\root\\linked", true, true };

	EXPECT_TRUE(CExplorerTool::CanExpand(normalDirectory));
	EXPECT_FALSE(CExplorerTool::CanExpand(junction));
	EXPECT_TRUE(CExplorerTool::IsReparsePoint(FILE_ATTRIBUTE_REPARSE_POINT));
	EXPECT_FALSE(CExplorerTool::IsReparsePoint(FILE_ATTRIBUTE_DIRECTORY));
}

TEST(ExplorerTool, DiscardsResultsFromStaleOrCancelledGenerations)
{
	EXPECT_TRUE(CExplorerTool::IsCurrentGeneration(7, 7));
	EXPECT_FALSE(CExplorerTool::IsCurrentGeneration(7, 6));
	EXPECT_FALSE(CExplorerTool::IsCurrentGeneration(7, 0));
	EXPECT_FALSE(CExplorerTool::IsCurrentGeneration(0, 0));
}

TEST(ExplorerTool, ExposesThePlannedPaletteAndKeepsRootWindowLocal)
{
	CExplorerTool first;
	CExplorerTool second;
	first.SetRoot(L"C:\\first");
	second.SetRoot(L"C:\\second");

	EXPECT_EQ(L"C:\\first", first.GetRoot());
	EXPECT_EQ(L"C:\\second", second.GetRoot());
	const ExplorerPalette palette = first.GetPalette();
	EXPECT_EQ(RGB(0x20, 0x23, 0x2A), palette.background);
	EXPECT_EQ(RGB(0xE8, 0xEB, 0xF0), palette.text);
	EXPECT_EQ(RGB(0x38, 0x3E, 0x49), palette.border);
	EXPECT_EQ(RGB(0xEB, 0x6A, 0x9A), palette.focus);
}

TEST(ExplorerTool, ProjectRevealSurvivesTheProjectsActivationMouseUp)
{
	TemporaryDirectory root;
	CreateEmptyFile(root.Path() / L"visible.txt");
	const HWND parent = CreateHiddenParentWindow();
	ASSERT_NE(nullptr, parent);
	CExplorerTool tool;
	tool.SetRoot(root.Path().wstring());
	ASSERT_TRUE(tool.Create(parent));
	tool.Layout(RECT{ 0, 0, 300, 240 }, 96);
	const HWND tree = ::FindWindowExW(tool.GetHwnd(), nullptr, WC_TREEVIEWW, nullptr);
	ASSERT_NE(nullptr, tree);
	EXPECT_TRUE(tool.IsFilesPaneExpanded());
	EXPECT_NE(0, ::GetWindowLongPtrW(tree, GWL_STYLE) & WS_VISIBLE);

	(void)::SendMessageW(tool.GetHwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(5, 5));
	(void)::SendMessageW(tool.GetHwnd(), WM_LBUTTONUP, 0, MAKELPARAM(5, 5));
	EXPECT_FALSE(tool.IsFilesPaneExpanded());
	EXPECT_EQ(0, ::GetWindowLongPtrW(tree, GWL_STYLE) & WS_VISIBLE);

	tool.SetFilesPaneExpanded(true);
	EXPECT_TRUE(tool.IsFilesPaneExpanded());
	EXPECT_NE(0, ::GetWindowLongPtrW(tree, GWL_STYLE) & WS_VISIBLE);
	// Activating a Project runs on the list's double-click notification.  Its
	// trailing button-up can land over the newly revealed Explorer header, but
	// it did not start a click on this surface and must not collapse the pane.
	(void)::SendMessageW(tool.GetHwnd(), WM_LBUTTONUP, 0, MAKELPARAM(5, 5));
	EXPECT_TRUE(tool.IsFilesPaneExpanded());
	EXPECT_NE(0, ::GetWindowLongPtrW(tree, GWL_STYLE) & WS_VISIBLE);
	// A cancelled header press must disarm before a later release arrives.
	(void)::SendMessageW(tool.GetHwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(5, 5));
	(void)::SendMessageW(tool.GetHwnd(), WM_CANCELMODE, 0, 0);
	(void)::SendMessageW(tool.GetHwnd(), WM_LBUTTONUP, 0, MAKELPARAM(5, 5));
	EXPECT_TRUE(tool.IsFilesPaneExpanded());
	EXPECT_NE(0, ::GetWindowLongPtrW(tree, GWL_STYLE) & WS_VISIBLE);
	// Reapplying the successful Project projection is an idempotent no-op.
	tool.SetFilesPaneExpanded(true);
	EXPECT_NE(0, ::GetWindowLongPtrW(tree, GWL_STYLE) & WS_VISIBLE);

	tool.Close();
	::DestroyWindow(parent);
}

TEST(ExplorerTool, TracksEachUpstreamEmptyExplorerWelcomeVariant)
{
	CExplorerTool explorer;

	EXPECT_EQ(ExplorerWelcomeState::NoFolder, explorer.GetWelcomeState());
	explorer.SetWelcomeState(ExplorerWelcomeState::NoFolderWithEditors);
	EXPECT_EQ(ExplorerWelcomeState::NoFolderWithEditors, explorer.GetWelcomeState());
	explorer.SetWelcomeState(ExplorerWelcomeState::EmptyWorkspace);
	EXPECT_EQ(ExplorerWelcomeState::EmptyWorkspace, explorer.GetWelcomeState());
	explorer.SetWelcomeState(ExplorerWelcomeState::WorkspaceWithFoldersUnsupported);
	EXPECT_EQ(ExplorerWelcomeState::WorkspaceWithFoldersUnsupported, explorer.GetWelcomeState());
}

TEST(ExplorerTool, NoFolderWelcomeUsesOrderedParagraphsAndRealActions)
{
	const auto blocks = BuildExplorerWelcomeBlocks(ExplorerWelcomeState::NoFolder);
	ASSERT_EQ(4u, blocks.size());
	EXPECT_EQ(ExplorerWelcomeBlockKind::Paragraph, blocks[0].kind);
	EXPECT_EQ(ExplorerWelcomeBlockKind::Action, blocks[1].kind);
	EXPECT_EQ(ExplorerWelcomeAction::OpenFolder, blocks[1].action);
	EXPECT_EQ(ExplorerWelcomeBlockKind::Paragraph, blocks[2].kind);
	EXPECT_EQ(ExplorerWelcomeBlockKind::Action, blocks[3].kind);
	EXPECT_EQ(ExplorerWelcomeAction::CloneRepository, blocks[3].action);
}

TEST(ExplorerTool, ExistingWelcomeVariantsKeepTheirActionSets)
{
	const auto withEditors = BuildExplorerWelcomeBlocks(ExplorerWelcomeState::NoFolderWithEditors);
	ASSERT_EQ(3u, withEditors.size());
	EXPECT_EQ(ExplorerWelcomeAction::OpenFolder, withEditors[1].action);
	EXPECT_EQ(ExplorerWelcomeAction::AddFolder, withEditors[2].action);

	const auto emptyWorkspace = BuildExplorerWelcomeBlocks(ExplorerWelcomeState::EmptyWorkspace);
	ASSERT_EQ(2u, emptyWorkspace.size());
	EXPECT_EQ(ExplorerWelcomeAction::AddFolder, emptyWorkspace[1].action);
}

TEST(ExplorerTool, ProductionWorkerEnumeratesOnlyExpandedDirectoriesAndStopsOnClose)
{
	TemporaryDirectory root;
	const auto childDirectory = root.Path() / L"child";
	ASSERT_TRUE(std::filesystem::create_directory(childDirectory));
	CreateEmptyFile(childDirectory / L"nested.txt");
	CreateEmptyFile(root.Path() / L"root.txt");

	const HWND parent = CreateHiddenParentWindow();
	ASSERT_NE(nullptr, parent);
	CExplorerTool tool;
	tool.SetRoot(root.Path().wstring());
	ASSERT_TRUE(tool.Create(parent));
	const HWND tree = ::FindWindowExW(tool.GetHwnd(), nullptr, WC_TREEVIEWW, nullptr);
	ASSERT_NE(nullptr, tree);
	EXPECT_EQ(0, ::GetWindowLongPtrW(tree, GWL_STYLE) & WS_BORDER);
	EXPECT_EQ(0, ::GetWindowLongPtrW(tree, GWL_EXSTYLE) &
		(WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE));
	EXPECT_NE(0, ::GetWindowLongPtrW(tree, GWL_STYLE) & TVS_NOHSCROLL);
	EXPECT_EQ(0, ::GetWindowLongPtrW(tree, GWL_STYLE) & WS_HSCROLL);
	ASSERT_TRUE(PumpMessagesUntil([&] {
		return FindDirectChild(tree, kWorkspaceLevel, L"child") != nullptr &&
			FindDirectChild(tree, kWorkspaceLevel, L"root.txt") != nullptr;
	}, std::chrono::seconds(2)));

	const auto childItem = FindDirectChild(tree, kWorkspaceLevel, L"child");
	ASSERT_NE(nullptr, childItem);
	EXPECT_EQ(nullptr, FindDirectChild(tree, childItem, L"nested.txt"));
	ASSERT_TRUE(TreeView_Expand(tree, childItem, TVE_EXPAND));
	EXPECT_TRUE(PumpMessagesUntil([&] {
		return FindDirectChild(tree, childItem, L"nested.txt") != nullptr;
	}, std::chrono::seconds(2)));
	EXPECT_TRUE(IsExpanded(tree, childItem));

	tool.CollapseAllFolders();
	EXPECT_FALSE(IsExpanded(tree, childItem));

	tool.Close();
	EXPECT_EQ(workbench::explorer::ExplorerWorkerState::Stopped, tool.GetWorkerState());
	::DestroyWindow(parent);
}

TEST(ExplorerTool, PreviewReusesOneEditorUntilDoubleClickOrEditPinsIt)
{
	auto first = PlanExplorerEditorActivation(
		false, false, ExplorerFileActivationKind::Preview);
	EXPECT_EQ(ExplorerEditorActivationAction::OpenNewEditor, first.action);
	ASSERT_TRUE(first.nextEditorIsPreview);

	auto second = PlanExplorerEditorActivation(
		first.nextEditorIsPreview, false, ExplorerFileActivationKind::Preview);
	EXPECT_EQ(ExplorerEditorActivationAction::ReplaceCurrentPreview, second.action);
	ASSERT_TRUE(second.nextEditorIsPreview);

	auto pinned = PlanExplorerEditorActivation(
		second.nextEditorIsPreview, true, ExplorerFileActivationKind::Pinned);
	EXPECT_EQ(ExplorerEditorActivationAction::ActivateCurrent, pinned.action);
	ASSERT_FALSE(pinned.nextEditorIsPreview);

	const auto afterEdit = PlanExplorerEditorActivation(
		false, false, ExplorerFileActivationKind::Preview);
	EXPECT_EQ(ExplorerEditorActivationAction::OpenNewEditor, afterEdit.action);
	EXPECT_TRUE(afterEdit.nextEditorIsPreview);
}

TEST(ExplorerTool, UsesOverlayVerticalScrollbarWithoutHorizontalScrollbar)
{
	TemporaryDirectory root;
	for (int index = 0; index < 40; ++index) {
		CreateEmptyFile(root.Path() / (L"file-with-a-long-name-" + std::to_wstring(index) + L".txt"));
	}

	const HWND parent = CreateHiddenParentWindow();
	ASSERT_NE(nullptr, parent);
	CExplorerTool tool;
	tool.SetRoot(root.Path().wstring());
	ASSERT_TRUE(tool.Create(parent));
	tool.Layout(RECT{ 0, 0, 180, 100 }, 96);
	const HWND tree = ::FindWindowExW(tool.GetHwnd(), nullptr, WC_TREEVIEWW, nullptr);
	ASSERT_NE(nullptr, tree);
	ASSERT_TRUE(PumpMessagesUntil([&] { return TreeView_GetCount(tree) >= 40; }, std::chrono::seconds(2)));
	const HWND overlay = ::FindWindowExW(
		tool.GetHwnd(), nullptr, L"SakuraWorkbenchOverlayScrollbar", nullptr);
	ASSERT_NE(nullptr, overlay);
	EXPECT_NE(0, ::GetWindowLongPtrW(overlay, GWL_STYLE) & WS_VISIBLE);
	EXPECT_EQ(0, ::GetWindowLongPtrW(tree, GWL_STYLE) & (WS_HSCROLL | WS_VSCROLL));

	tool.Close();
	::DestroyWindow(parent);
}

TEST(ExplorerTool, SingleClickActivatesHitFileFromNativeClickNotification)
{
	TemporaryDirectory root;
	ASSERT_TRUE(std::filesystem::create_directory(root.Path() / L"child"));
	const auto firstFilePath = root.Path() / L"first.txt";
	const auto secondFilePath = root.Path() / L"second.txt";
	CreateEmptyFile(firstFilePath);
	CreateEmptyFile(secondFilePath);

	const HWND parent = CreateHiddenParentWindow();
	ASSERT_NE(nullptr, parent);
	CExplorerTool tool;
	std::wstring activatedPath;
	ExplorerFileActivationKind activatedKind = ExplorerFileActivationKind::Pinned;
	int activationCount = 0;
	tool.SetFileActivationCallback([&](std::wstring_view path, ExplorerFileActivationKind kind) {
		activatedPath = path;
		activatedKind = kind;
		++activationCount;
	});
	tool.SetRoot(root.Path().wstring());
	ASSERT_TRUE(tool.Create(parent));
	tool.Layout(RECT{ 0, 0, 400, 300 }, 96);
	::ShowWindow(parent, SW_SHOWNOACTIVATE);
	::UpdateWindow(parent);
	const HWND tree = ::FindWindowExW(tool.GetHwnd(), nullptr, WC_TREEVIEWW, nullptr);
	ASSERT_NE(nullptr, tree);
	ASSERT_TRUE(PumpMessagesUntil([&] {
		return FindDirectChild(tree, kWorkspaceLevel, L"child") != nullptr &&
			FindDirectChild(tree, kWorkspaceLevel, L"first.txt") != nullptr &&
			FindDirectChild(tree, kWorkspaceLevel, L"second.txt") != nullptr;
	}, std::chrono::seconds(2)));

	// Activating a file must not rebuild the tree, so the first top-level row
	// has to stay the same item afterwards.
	const auto firstTopLevelItem = TreeView_GetRoot(tree);
	ASSERT_NE(nullptr, firstTopLevelItem);
	const auto firstFileItem = FindDirectChild(tree, kWorkspaceLevel, L"first.txt");
	ASSERT_NE(nullptr, firstFileItem);
	const auto secondFileItem = FindDirectChild(tree, kWorkspaceLevel, L"second.txt");
	ASSERT_NE(nullptr, secondFileItem);
	const auto childItem = FindDirectChild(tree, kWorkspaceLevel, L"child");
	ASSERT_NE(nullptr, childItem);
	ASSERT_TRUE(TreeView_SelectItem(tree, childItem));
	SendTreeMouseClick(tree, firstFileItem);
	ASSERT_TRUE(PumpMessagesUntil([&] { return activationCount == 1; }, std::chrono::seconds(1)));
	EXPECT_EQ(firstFilePath.wstring(), activatedPath);
	EXPECT_EQ(ExplorerFileActivationKind::Preview, activatedKind);
	EXPECT_EQ(root.Path().wstring(), tool.GetRoot());
	EXPECT_EQ(firstTopLevelItem, TreeView_GetRoot(tree));

	SendTreeMouseClick(tree, secondFileItem);
	ASSERT_TRUE(PumpMessagesUntil([&] { return activationCount == 2; }, std::chrono::seconds(1)));
	EXPECT_EQ(secondFilePath.wstring(), activatedPath);
	EXPECT_EQ(ExplorerFileActivationKind::Preview, activatedKind);
	EXPECT_EQ(root.Path().wstring(), tool.GetRoot());
	EXPECT_EQ(firstTopLevelItem, TreeView_GetRoot(tree));
	EXPECT_NE(nullptr, FindDirectChild(tree, kWorkspaceLevel, L"first.txt"));
	EXPECT_NE(nullptr, FindDirectChild(tree, kWorkspaceLevel, L"second.txt"));

	SendTreeMouseDoubleClick(tree, secondFileItem);
	ASSERT_TRUE(PumpMessagesUntil([&] { return activationCount == 3; }, std::chrono::seconds(1)));
	EXPECT_EQ(secondFilePath.wstring(), activatedPath);
	EXPECT_EQ(ExplorerFileActivationKind::Pinned, activatedKind);

	SendTreeMouseClick(tree, childItem);
	ASSERT_TRUE(PumpMessagesUntil([] { return true; }, std::chrono::milliseconds(0)));
	EXPECT_EQ(3, activationCount);

	RECT client{};
	ASSERT_TRUE(::GetClientRect(tree, &client));
	SendTreeMouseClickAt(tree, client.right - 20, client.bottom - 20);
	ASSERT_TRUE(PumpMessagesUntil([] { return true; }, std::chrono::milliseconds(0)));
	EXPECT_EQ(3, activationCount);

	tool.Close();
	::DestroyWindow(parent);
}

TEST(ExplorerTool, MouseWheelScrollsTheTreeWithHiddenNativeScrollbars)
{
	TemporaryDirectory root;
	for (int index = 0; index < 40; ++index) {
		CreateEmptyFile(root.Path() / (L"file-" + std::to_wstring(index) + L".txt"));
	}

	const HWND parent = CreateHiddenParentWindow();
	ASSERT_NE(nullptr, parent);
	CExplorerTool tool;
	tool.SetRoot(root.Path().wstring());
	ASSERT_TRUE(tool.Create(parent));
	tool.Layout(RECT{ 0, 0, 240, 100 }, 96);
	::ShowWindow(parent, SW_SHOWNOACTIVATE);
	::UpdateWindow(parent);
	const HWND tree = ::FindWindowExW(tool.GetHwnd(), nullptr, WC_TREEVIEWW, nullptr);
	ASSERT_NE(nullptr, tree);
	ASSERT_TRUE(PumpMessagesUntil([&] { return TreeView_GetCount(tree) >= 40; }, std::chrono::seconds(2)));
	const auto firstBefore = TreeView_GetFirstVisible(tree);
	ASSERT_NE(nullptr, firstBefore);

	(void)::SendMessageW(tree, WM_MOUSEWHEEL,
		MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA / 2)), 0);
	EXPECT_EQ(firstBefore, TreeView_GetFirstVisible(tree));
	(void)::SendMessageW(tree, WM_MOUSEWHEEL,
		MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA / 2)), 0);
	EXPECT_NE(firstBefore, TreeView_GetFirstVisible(tree));

	tool.Close();
	::DestroyWindow(parent);
}

TEST(ExplorerTool, RefreshRestoresExpandedDescendantsByFilesystemPath)
{
	TemporaryDirectory root;
	const auto childDirectory = root.Path() / L"child";
	const auto grandchildDirectory = childDirectory / L"grandchild";
	ASSERT_TRUE(std::filesystem::create_directories(grandchildDirectory));
	CreateEmptyFile(grandchildDirectory / L"nested.txt");

	const HWND parent = CreateHiddenParentWindow();
	ASSERT_NE(nullptr, parent);
	CExplorerTool tool;
	tool.SetRoot(root.Path().wstring());
	ASSERT_TRUE(tool.Create(parent));
	const HWND tree = ::FindWindowExW(tool.GetHwnd(), nullptr, WC_TREEVIEWW, nullptr);
	ASSERT_NE(nullptr, tree);
	ASSERT_TRUE(PumpMessagesUntil([&] {
		return FindDirectChild(tree, kWorkspaceLevel, L"child") != nullptr;
	}, std::chrono::seconds(2)));

	auto childItem = FindDirectChild(tree, kWorkspaceLevel, L"child");
	ASSERT_NE(nullptr, childItem);
	ASSERT_TRUE(TreeView_Expand(tree, childItem, TVE_EXPAND));
	ASSERT_TRUE(PumpMessagesUntil([&] {
		return FindDirectChild(tree, childItem, L"grandchild") != nullptr;
	}, std::chrono::seconds(2)));
	auto grandchildItem = FindDirectChild(tree, childItem, L"grandchild");
	ASSERT_NE(nullptr, grandchildItem);
	const auto originalChildItem = childItem;
	const auto originalGrandchildItem = grandchildItem;
	ASSERT_TRUE(TreeView_Expand(tree, grandchildItem, TVE_EXPAND));
	ASSERT_TRUE(PumpMessagesUntil([&] {
		return FindDirectChild(tree, grandchildItem, L"nested.txt") != nullptr;
	}, std::chrono::seconds(2)));

	ASSERT_TRUE(PumpMessagesUntil([&] {
		return tool.GetWorkerState() == workbench::explorer::ExplorerWorkerState::Idle;
	}, std::chrono::seconds(15)));
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	CreateEmptyFile(root.Path() / L"later.txt");
	ASSERT_TRUE(PumpMessagesUntil([&] {
		childItem = FindDirectChild(tree, kWorkspaceLevel, L"child");
		if (!IsExpanded(tree, childItem)) return false;
		grandchildItem = FindDirectChild(tree, childItem, L"grandchild");
		return IsExpanded(tree, grandchildItem) &&
			FindDirectChild(tree, grandchildItem, L"nested.txt") != nullptr &&
			FindDirectChild(tree, kWorkspaceLevel, L"later.txt") != nullptr;
	}, std::chrono::seconds(3)));
	EXPECT_EQ(originalChildItem, childItem);
	EXPECT_EQ(originalGrandchildItem, grandchildItem);

	// Reapplying the same workspace root must not rebuild the TreeView or lose
	// path-owned expansion state during ordinary workbench synchronization.
	tool.SetRoot(root.Path().wstring());
	EXPECT_TRUE(IsExpanded(tree, childItem));
	EXPECT_TRUE(IsExpanded(tree, grandchildItem));

	tool.Close();
	::DestroyWindow(parent);
}

TEST(ExplorerTool, CreatesVisibleContainerAndTreeForVisibleParent)
{
	const HWND parent = ::CreateWindowExW(0, L"STATIC", L"Visible Explorer test parent",
		WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
		nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(nullptr, parent);

	CExplorerTool tool;
	tool.SetRoot(L"C:\\");
	ASSERT_TRUE(tool.Create(parent));
	const HWND tree = ::FindWindowExW(tool.GetHwnd(), nullptr, WC_TREEVIEWW, nullptr);
	ASSERT_NE(nullptr, tree);
	EXPECT_TRUE(::IsWindowVisible(tool.GetHwnd()));
	EXPECT_TRUE(::IsWindowVisible(tree));

	tool.Close();
	::DestroyWindow(parent);
}

TEST(ExplorerTool, ProductionWorkerDisplaysJunctionsAsLeaves)
{
	TemporaryDirectory root;
	const auto targetDirectory = root.Path() / L"target";
	ASSERT_TRUE(std::filesystem::create_directory(targetDirectory));
	CreateEmptyFile(targetDirectory / L"nested.txt");
	const auto junction = root.Path() / L"junction";
	ASSERT_TRUE(CreateDirectoryJunction(junction, targetDirectory)) << ::GetLastError();

	const HWND parent = CreateHiddenParentWindow();
	ASSERT_NE(nullptr, parent);
	CExplorerTool tool;
	tool.SetRoot(root.Path().wstring());
	ASSERT_TRUE(tool.Create(parent));
	const HWND tree = ::FindWindowExW(tool.GetHwnd(), nullptr, WC_TREEVIEWW, nullptr);
	ASSERT_NE(nullptr, tree);
	ASSERT_TRUE(PumpMessagesUntil([&] {
		return FindDirectChild(tree, kWorkspaceLevel, L"junction") != nullptr;
	}, std::chrono::seconds(2)));

	const auto junctionItem = FindDirectChild(tree, kWorkspaceLevel, L"junction");
	ASSERT_NE(nullptr, junctionItem);
	EXPECT_EQ(nullptr, TreeView_GetChild(tree, junctionItem));

	tool.Close();
	EXPECT_EQ(workbench::explorer::ExplorerWorkerState::Stopped, tool.GetWorkerState());
	::DestroyWindow(parent);
}

TEST(ExplorerTool, ProductionWorkerRejectsOldRootsAndDebouncesDirectoryChanges)
{
	TemporaryDirectory firstRoot;
	TemporaryDirectory secondRoot;
	CreateEmptyFile(firstRoot.Path() / L"old.txt");
	CreateEmptyFile(secondRoot.Path() / L"current.txt");

	const HWND parent = CreateHiddenParentWindow();
	ASSERT_NE(nullptr, parent);
	CExplorerTool tool;
	ASSERT_TRUE(tool.Create(parent));
	const HWND tree = ::FindWindowExW(tool.GetHwnd(), nullptr, WC_TREEVIEWW, nullptr);
	ASSERT_NE(nullptr, tree);
	tool.SetRoot(firstRoot.Path().wstring());
	tool.SetRoot(secondRoot.Path().wstring());

	// The deadlines below only bound how long a correct result may take to
	// arrive; the debounce assertion is the negative wait further down, which
	// keeps its narrow window.  A hosted runner opening a cold directory can be
	// far slower than the ~250 ms this takes on a developer machine.
	ASSERT_TRUE(PumpMessagesUntil([&] {
		return FindDirectChild(tree, kWorkspaceLevel, L"current.txt") != nullptr;
	}, std::chrono::seconds(15)));
	EXPECT_EQ(nullptr, FindDirectChild(tree, kWorkspaceLevel, L"old.txt"));

	// Let the production ReadDirectoryChangesW watcher arm, then verify its
	// 150-ms timer coalesces the refresh instead of applying it immediately.
	ASSERT_TRUE(PumpMessagesUntil([&] {
		return tool.GetWorkerState() == workbench::explorer::ExplorerWorkerState::Idle;
	}, std::chrono::seconds(1)));
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	CreateEmptyFile(secondRoot.Path() / L"later.txt");
	EXPECT_FALSE(PumpMessagesUntil([&] {
		return FindDirectChild(tree, kWorkspaceLevel, L"later.txt") != nullptr;
	}, std::chrono::milliseconds(75)));
	EXPECT_TRUE(PumpMessagesUntil([&] {
		return FindDirectChild(tree, kWorkspaceLevel, L"later.txt") != nullptr;
	}, std::chrono::seconds(15)));

	tool.Close();
	EXPECT_EQ(workbench::explorer::ExplorerWorkerState::Stopped, tool.GetWorkerState());
	::DestroyWindow(parent);
}

} // namespace
