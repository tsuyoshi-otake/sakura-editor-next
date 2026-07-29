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
using workbench::explorer::ExplorerEntry;
using workbench::explorer::ExplorerPalette;

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

HTREEITEM FindDirectChild(HWND tree, HTREEITEM parent, std::wstring_view text)
{
	for (auto item = TreeView_GetChild(tree, parent); item != nullptr; item = TreeView_GetNextSibling(tree, item)) {
		if (ItemText(tree, item) == text) return item;
	}
	return nullptr;
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

TEST(ExplorerTool, UsesUppercaseWorkspaceFolderNameInsteadOfCanonicalPath)
{
	EXPECT_EQ(L"SAKURACODE", CExplorerTool::WorkspaceDisplayName(L"C:\\Codes\\tsuyoshi-otake\\sakuracode"));
	EXPECT_EQ(L"SAKURACODE", CExplorerTool::WorkspaceDisplayName(L"C:/Codes/tsuyoshi-otake/sakuracode/"));
	EXPECT_EQ(L"SHARE", CExplorerTool::WorkspaceDisplayName(L"\\\\server\\share\\"));
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
	EXPECT_EQ(RGB(0x20, 0x23, 0x2A), palette.panel);
	EXPECT_EQ(RGB(0xE8, 0xEB, 0xF0), palette.text);
	EXPECT_EQ(RGB(0x38, 0x3E, 0x49), palette.border);
	EXPECT_EQ(RGB(0xEB, 0x6A, 0x9A), palette.focus);
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
	const auto rootItem = TreeView_GetRoot(tree);
	ASSERT_NE(nullptr, rootItem);
	EXPECT_EQ(CExplorerTool::WorkspaceDisplayName(root.Path().wstring()), ItemText(tree, rootItem));

	ASSERT_TRUE(PumpMessagesUntil([&] {
		return FindDirectChild(tree, rootItem, L"child") != nullptr &&
			FindDirectChild(tree, rootItem, L"root.txt") != nullptr;
	}, std::chrono::seconds(2)));

	const auto childItem = FindDirectChild(tree, rootItem, L"child");
	ASSERT_NE(nullptr, childItem);
	EXPECT_EQ(nullptr, FindDirectChild(tree, childItem, L"nested.txt"));
	ASSERT_TRUE(TreeView_Expand(tree, childItem, TVE_EXPAND));
	EXPECT_TRUE(PumpMessagesUntil([&] {
		return FindDirectChild(tree, childItem, L"nested.txt") != nullptr;
	}, std::chrono::seconds(2)));

	tool.Close();
	EXPECT_EQ(workbench::explorer::ExplorerWorkerState::Stopped, tool.GetWorkerState());
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
	const auto rootItem = TreeView_GetRoot(tree);
	ASSERT_NE(nullptr, rootItem);
	ASSERT_TRUE(PumpMessagesUntil([&] {
		return FindDirectChild(tree, rootItem, L"junction") != nullptr;
	}, std::chrono::seconds(2)));

	const auto junctionItem = FindDirectChild(tree, rootItem, L"junction");
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

	ASSERT_TRUE(PumpMessagesUntil([&] {
		const auto rootItem = TreeView_GetRoot(tree);
		return rootItem != nullptr && FindDirectChild(tree, rootItem, L"current.txt") != nullptr;
	}, std::chrono::seconds(2)));
	const auto rootItem = TreeView_GetRoot(tree);
	ASSERT_NE(nullptr, rootItem);
	EXPECT_EQ(nullptr, FindDirectChild(tree, rootItem, L"old.txt"));

	// Let the production ReadDirectoryChangesW watcher arm, then verify its
	// 150-ms timer coalesces the refresh instead of applying it immediately.
	ASSERT_TRUE(PumpMessagesUntil([&] {
		return tool.GetWorkerState() == workbench::explorer::ExplorerWorkerState::Idle;
	}, std::chrono::seconds(1)));
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	CreateEmptyFile(secondRoot.Path() / L"later.txt");
	EXPECT_FALSE(PumpMessagesUntil([&] {
		return FindDirectChild(tree, rootItem, L"later.txt") != nullptr;
	}, std::chrono::milliseconds(75)));
	EXPECT_TRUE(PumpMessagesUntil([&] {
		return FindDirectChild(tree, rootItem, L"later.txt") != nullptr;
	}, std::chrono::seconds(2)));

	tool.Close();
	EXPECT_EQ(workbench::explorer::ExplorerWorkerState::Stopped, tool.GetWorkerState());
	::DestroyWindow(parent);
}

} // namespace
