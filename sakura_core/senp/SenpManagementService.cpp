/*! @file */
/*
Copyright (C) 2026, Sakura Editor Organization

SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "senp/SenpManagementService.h"

#include "senp/SenpChildJob.h"

#include <sakura/serialization/JsoncDocument.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <thread>
#include <utility>

namespace senp {
namespace {

using platform::serialization::JsoncValue;

//! One embedded built-in package's resource identity. Immutable once
//! constructed: the table below is the sole source of this mapping.
class BuiltInResource final {
public:
	constexpr BuiltInResource(std::wstring_view id, int packageResource, int hashResource,
		bool installedByDefault) noexcept
		: m_id(id), m_packageResource(packageResource), m_hashResource(hashResource),
		m_installedByDefault(installedByDefault)
	{
	}

	[[nodiscard]] constexpr std::wstring_view Id() const noexcept { return m_id; }
	[[nodiscard]] constexpr int PackageResource() const noexcept { return m_packageResource; }
	[[nodiscard]] constexpr int HashResource() const noexcept { return m_hashResource; }
	[[nodiscard]] constexpr bool InstalledByDefault() const noexcept { return m_installedByDefault; }

private:
	std::wstring_view m_id;
	int m_packageResource;
	int m_hashResource;
	bool m_installedByDefault;
};

// Keep this table as the package-management boundary for built-ins. Adding a
// built-in does not change the installer or runtime service contract.
constexpr std::array kBuiltInResources{
	BuiltInResource{ L"sakura-indent-rainbow", 39000, 39001, true },
	BuiltInResource{ L"sakura-core-language-basics", 39002, 39003, true },
	BuiltInResource{ L"sakura-shell-language-basics", 39004, 39005, true },
	BuiltInResource{ L"sakura-database-language-basics", 39006, 39007, true },
	BuiltInResource{ L"sakura-infrastructure-language-basics", 39008, 39009, true },
	BuiltInResource{ L"sakura-configuration-language-basics", 39010, 39011, true },
	BuiltInResource{ L"sakura-legacy-language-basics", 39012, 39013, true },
	BuiltInResource{ L"sakura-projects", 39014, 39015, true },
	// Installed by default on the owner's decision (#337), so an update brings
	// them into existing profiles too. Without a GitHub connection their Views
	// show the failed read with a Retry row, never placeholder data.
	BuiltInResource{ L"sakura-github-pull-requests", 39016, 39017, true },
	BuiltInResource{ L"sakura-github-actions", 39018, 39019, true },
};
constexpr DWORD kToolTimeoutMilliseconds = 30'000;
constexpr std::size_t kMaximumToolOutputBytes = 4U * 1024U * 1024U;

class ScopedHandle final {
public:
	ScopedHandle() = default;
	explicit ScopedHandle(HANDLE value) noexcept : m_value(value) {}
	~ScopedHandle() { Reset(); }
	ScopedHandle(const ScopedHandle&) = delete;
	ScopedHandle& operator=(const ScopedHandle&) = delete;
	ScopedHandle(ScopedHandle&& other) noexcept : m_value(std::exchange(other.m_value, nullptr)) {}
	ScopedHandle& operator=(ScopedHandle&& other) noexcept
	{
		if (this != &other) Reset(std::exchange(other.m_value, nullptr));
		return *this;
	}
	void Reset(HANDLE value = nullptr) noexcept
	{
		if (m_value != nullptr && m_value != INVALID_HANDLE_VALUE) ::CloseHandle(m_value);
		m_value = value;
	}
	[[nodiscard]] HANDLE Get() const noexcept { return m_value; }
	[[nodiscard]] HANDLE* Put() noexcept { return &m_value; }

private:
	HANDLE m_value = nullptr;
};

std::wstring QuoteArgument(std::wstring_view value)
{
	if (!value.empty() && value.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
		return std::wstring(value);
	}
	std::wstring result(1, L'\"');
	std::size_t slashes = 0;
	for (const wchar_t ch : value) {
		if (ch == L'\\') {
			++slashes;
			continue;
		}
		if (ch == L'\"') {
			result.append(slashes * 2 + 1, L'\\');
			result.push_back(ch);
			slashes = 0;
			continue;
		}
		result.append(slashes, L'\\');
		slashes = 0;
		result.push_back(ch);
	}
	result.append(slashes * 2, L'\\');
	result.push_back(L'\"');
	return result;
}

std::wstring ExecutableDirectory()
{
	std::array<wchar_t, 32768> path{};
	const DWORD length = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
	if (length == 0 || length >= path.size()) return {};
	return std::filesystem::path(std::wstring(path.data(), length)).parent_path().native();
}

using ProcessResult = detail::ToolResult;

ProcessResult RunToolImpl(const std::vector<std::wstring>& arguments)
{
	ProcessResult result;
	const auto directory = ExecutableDirectory();
	if (directory.empty()) return result;
	const auto executable = std::filesystem::path(directory) / L"sakura-senp-tool.exe";
	if (!std::filesystem::is_regular_file(executable)) return result;
	std::wstring commandLine = QuoteArgument(executable.native());
	for (const auto& argument : arguments) {
		commandLine.push_back(L' ');
		commandLine += QuoteArgument(argument);
	}

	// Child-side handles are created non-inheritable; the inherit flag is set
	// only for the minimal window around CreateProcessW below.
	SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, FALSE };
	ScopedHandle outputRead;
	ScopedHandle outputWrite;
	ScopedHandle errorRead;
	ScopedHandle errorWrite;
	ScopedHandle nullInput;
	if (!::CreatePipe(outputRead.Put(), outputWrite.Put(), &security, 0)
		|| !::CreatePipe(errorRead.Put(), errorWrite.Put(), &security, 0)) return result;
	nullInput.Reset(::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
		&security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
	if (nullInput.Get() == nullptr || nullInput.Get() == INVALID_HANDLE_VALUE) return result;

	ScopedHandle job(::CreateJobObjectW(nullptr, nullptr));
	if (job.Get() == nullptr || job.Get() == INVALID_HANDLE_VALUE
		|| !ConfigureChildJob(job.Get(), kPackageToolJobLimits)) return result;

	const auto process = detail::LaunchSenpChildProcess(executable.native(), commandLine,
		{ nullInput.Get(), outputWrite.Get(), errorWrite.Get() },
		CREATE_NO_WINDOW, job.Get());
	outputWrite.Reset();
	errorWrite.Reset();
	nullInput.Reset();
	if (!process) return result;
	result.launched = true;
	ScopedHandle processHandle(process->hProcess);
	ScopedHandle processThread(process->hThread);
	std::thread outputReader(detail::ReadPipeBounded, static_cast<void*>(outputRead.Get()),
		std::ref(result.output), std::ref(result.outputTruncated));
	std::thread errorReader(detail::ReadPipeBounded, static_cast<void*>(errorRead.Get()),
		std::ref(result.error), std::ref(result.errorTruncated));
	const DWORD wait = ::WaitForSingleObject(processHandle.Get(), kToolTimeoutMilliseconds);
	if (wait != WAIT_OBJECT_0) {
		result.timedOut = true;
		if (!::TerminateJobObject(job.Get(), ERROR_TIMEOUT)) {
			(void)::TerminateProcess(processHandle.Get(), ERROR_TIMEOUT);
		}
		(void)::WaitForSingleObject(processHandle.Get(), 1000);
	}
	(void)::GetExitCodeProcess(processHandle.Get(), &result.exitCode);
	if (outputReader.joinable()) outputReader.join();
	if (errorReader.joinable()) errorReader.join();
	outputRead.Reset();
	errorRead.Reset();
	return result;
}

ProcessResult RunTool(const std::vector<std::wstring>& arguments,
	const detail::SenpServiceHooks& hooks)
{
	if (hooks.runTool) return hooks.runTool(arguments);
	return RunToolImpl(arguments);
}

std::optional<std::vector<std::byte>> EmbeddedResourceImpl(int id)
{
	const HMODULE module = ::GetModuleHandleW(nullptr);
	const HRSRC found = ::FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
	if (found == nullptr) return std::nullopt;
	const DWORD size = ::SizeofResource(module, found);
	const HGLOBAL loaded = ::LoadResource(module, found);
	const auto* bytes = static_cast<const std::byte*>(::LockResource(loaded));
	if (size == 0 || bytes == nullptr) return std::nullopt;
	return std::vector<std::byte>(bytes, bytes + size);
}

std::optional<std::vector<std::byte>> EmbeddedResource(int id,
	const detail::SenpServiceHooks& hooks)
{
	if (hooks.embeddedResource) return hooks.embeddedResource(id);
	return EmbeddedResourceImpl(id);
}

bool WriteBytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes)
{
	std::error_code error;
	std::filesystem::create_directories(path.parent_path(), error);
	if (error) return false;
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	if (!output) return false;
	output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	output.flush();
	return output.good();
}

std::wstring Utf8ToWide(std::string_view value)
{
	if (value.empty()) return {};
	const int length = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		value.data(), static_cast<int>(value.size()), nullptr, 0);
	if (length <= 0) return {};
	std::wstring result(static_cast<std::size_t>(length), L'\0');
	if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
		static_cast<int>(value.size()), result.data(), length) != length) return {};
	return result;
}

const std::wstring* StringMember(const JsoncValue::Object& object, std::wstring_view name)
{
	const auto found = object.find(name);
	return found == object.end() ? nullptr : std::get_if<std::wstring>(&found->second.Value());
}

const bool* BoolMember(const JsoncValue::Object& object, std::wstring_view name)
{
	const auto found = object.find(name);
	return found == object.end() ? nullptr : std::get_if<bool>(&found->second.Value());
}

const std::int64_t* IntegerMember(const JsoncValue::Object& object, std::wstring_view name)
{
	const auto found = object.find(name);
	return found == object.end() ? nullptr : std::get_if<std::int64_t>(&found->second.Value());
}

const JsoncValue::Object* ObjectMember(const JsoncValue::Object& object, std::wstring_view name)
{
	const auto found = object.find(name);
	return found == object.end() ? nullptr : std::get_if<JsoncValue::Object>(&found->second.Value());
}

const JsoncValue::Array* ArrayMember(const JsoncValue::Object& object, std::wstring_view name)
{
	const auto found = object.find(name);
	return found == object.end() ? nullptr : std::get_if<JsoncValue::Array>(&found->second.Value());
}

bool ParseStringArray(const JsoncValue::Object& object, std::wstring_view name,
	std::vector<std::wstring>& target)
{
	const auto* values = ArrayMember(object, name);
	if (values == nullptr || values->size() > 256) return false;
	target.reserve(values->size());
	for (const auto& value : *values) {
		const auto* text = std::get_if<std::wstring>(&value.Value());
		if (text == nullptr) return false;
		target.push_back(*text);
	}
	return true;
}

bool ParseLanguageContributions(const JsoncValue::Object& contributes,
	std::vector<LanguageContribution>& target)
{
	const auto* languages = ArrayMember(contributes, L"languages");
	if (languages == nullptr || languages->size() > 64) return false;
	target.reserve(languages->size());
	for (const auto& value : *languages) {
		const auto* object = std::get_if<JsoncValue::Object>(&value.Value());
		if (object == nullptr) return false;
		const auto* id = StringMember(*object, L"id");
		if (id == nullptr) return false;
		LanguageContribution contribution{ .id = *id };
		if (!ParseStringArray(*object, L"aliases", contribution.aliases)
			|| !ParseStringArray(*object, L"extensions", contribution.extensions)
			|| !ParseStringArray(*object, L"filenames", contribution.filenames)
			|| !ParseStringArray(*object, L"filenamePatterns", contribution.filenamePatterns)
			|| !ParseStringArray(*object, L"mimetypes", contribution.mimetypes)) return false;
		if (const auto* firstLine = StringMember(*object, L"firstLine")) contribution.firstLine = *firstLine;
		if (const auto* configuration = StringMember(*object, L"configuration")) {
			contribution.configuration = *configuration;
		}
		target.push_back(std::move(contribution));
	}
	return true;
}

bool ParseGrammarContributions(const JsoncValue::Object& contributes,
	std::vector<GrammarContribution>& target)
{
	const auto* grammars = ArrayMember(contributes, L"grammars");
	if (grammars == nullptr || grammars->size() > 128) return false;
	target.reserve(grammars->size());
	for (const auto& value : *grammars) {
		const auto* object = std::get_if<JsoncValue::Object>(&value.Value());
		if (object == nullptr) return false;
		const auto* scopeName = StringMember(*object, L"scopeName");
		const auto* path = StringMember(*object, L"path");
		if (scopeName == nullptr || path == nullptr) return false;
		GrammarContribution contribution{ .scopeName = *scopeName, .path = *path };
		if (const auto* language = StringMember(*object, L"language")) contribution.language = *language;
		if (!ParseStringArray(*object, L"injectTo", contribution.injectTo)) return false;
		target.push_back(std::move(contribution));
	}
	return true;
}

bool ParseWorkbenchViewContributions(const JsoncValue::Object& contributes,
	std::vector<ViewContainerContribution>& containers,
	std::vector<ViewContribution>& views)
{
	const auto* viewContainers = ObjectMember(contributes, L"viewsContainers");
	const auto* activitybar = viewContainers == nullptr ? nullptr : ArrayMember(*viewContainers, L"activitybar");
	const auto* viewMap = ObjectMember(contributes, L"views");
	if (viewContainers == nullptr || activitybar == nullptr || viewMap == nullptr
		|| activitybar->size() > 32 || viewMap->size() > 32) return false;

	containers.reserve(activitybar->size());
	for (const auto& value : *activitybar) {
		const auto* object = std::get_if<JsoncValue::Object>(&value.Value());
		if (object == nullptr) return false;
		const auto* id = StringMember(*object, L"id");
		const auto* title = StringMember(*object, L"title");
		const auto* icon = StringMember(*object, L"icon");
		const auto* order = IntegerMember(*object, L"order");
		if (id == nullptr || title == nullptr || icon == nullptr || order == nullptr
			|| *order < -10'000 || *order > 10'000) return false;
		containers.push_back({
			.id = *id,
			.title = *title,
			.icon = *icon,
			.order = static_cast<std::int32_t>(*order),
		});
	}

	for (const auto& [containerId, value] : *viewMap) {
		const auto* entries = std::get_if<JsoncValue::Array>(&value.Value());
		if (entries == nullptr || entries->empty() || entries->size() > 32) return false;
		for (const auto& entry : *entries) {
			const auto* object = std::get_if<JsoncValue::Object>(&entry.Value());
			if (object == nullptr) return false;
			const auto* id = StringMember(*object, L"id");
			const auto* title = StringMember(*object, L"name");
			const auto* provider = StringMember(*object, L"provider");
			const auto* order = IntegerMember(*object, L"order");
			if (id == nullptr || title == nullptr || provider == nullptr || order == nullptr
				|| *order < -10'000 || *order > 10'000 || views.size() >= 128) return false;
			views.push_back({
				.id = *id,
				.containerId = containerId,
				.title = *title,
				.provider = *provider,
				.order = static_cast<std::int32_t>(*order),
			});
		}
	}
	return true;
}

bool IsAsciiAlphanumeric(const wchar_t ch) noexcept
{
	return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9');
}

//! The same `$(codicon)` form the package tool accepts for a command icon.
bool ValidThemeIcon(std::wstring_view value) noexcept
{
	return value.size() >= 4 && value.size() <= 132 && value.starts_with(L"$(") && value.ends_with(L')')
		&& std::ranges::all_of(value.substr(2, value.size() - 3),
			[](const wchar_t ch) { return IsAsciiAlphanumeric(ch) || ch == L'-'; });
}

bool ValidEffectIdentifier(std::wstring_view value) noexcept
{
	return !value.empty() && value.size() <= 160 && IsAsciiAlphanumeric(value.front())
		&& IsAsciiAlphanumeric(value.back())
		&& std::ranges::all_of(value, [](const wchar_t ch) {
			return IsAsciiAlphanumeric(ch) || ch == L'.' || ch == L'-' || ch == L'_' || ch == L':';
		});
}

std::wstring_view TrimAsciiSpace(std::wstring_view value) noexcept
{
	const auto space = [](const wchar_t ch) { return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n'; };
	while (!value.empty() && space(value.front())) value.remove_prefix(1);
	while (!value.empty() && space(value.back())) value.remove_suffix(1);
	return value;
}

//! The Views a `view/title` item's `when` names: `view == <id>` clauses joined
//! by `||`, mirroring the package tool's `view_title_when_views`. Anything else
//! is refused rather than evaluated as a partial context-key expression.
std::optional<std::vector<std::wstring>> ViewTitleWhenViews(std::wstring_view when)
{
	if (when.size() > 1024) return std::nullopt;
	std::vector<std::wstring> views;
	for (std::size_t start = 0;;) {
		const auto split = when.find(L"||", start);
		auto clause = TrimAsciiSpace(when.substr(start, split == std::wstring_view::npos ? split : split - start));
		if (!clause.starts_with(L"view")) return std::nullopt;
		clause = TrimAsciiSpace(clause.substr(4));
		if (!clause.starts_with(L"==")) return std::nullopt;
		const auto view = TrimAsciiSpace(clause.substr(2));
		if (!ValidEffectIdentifier(view) || std::ranges::find(views, view) != views.end()) return std::nullopt;
		views.emplace_back(view);
		if (split == std::wstring_view::npos) return views;
		start = split + 2;
	}
}

//! The package tool's `valid_package_image_path`: a bounded relative `.svg`/`.png`
//! path of `[A-Za-z0-9._-]` segments with no empty, `.` or `..` segment.
bool ValidPackageImagePath(std::wstring_view value) noexcept
{
	if (value.size() > 260 || !(value.ends_with(L".svg") || value.ends_with(L".png"))) return false;
	for (std::size_t start = 0;;) {
		const auto split = value.find(L'/', start);
		const auto segment = value.substr(start, split == std::wstring_view::npos ? split : split - start);
		if (segment.empty() || segment == L"." || segment == L".."
			|| !std::ranges::all_of(segment, [](const wchar_t ch) {
				return IsAsciiAlphanumeric(ch) || ch == L'.' || ch == L'_' || ch == L'-';
			})) return false;
		if (split == std::wstring_view::npos) return true;
		start = split + 1;
	}
}

//! A `view/item/context` item's `when`, mirroring the package tool's
//! `view_item_when`: `&&`-joined `viewItem =~ /token/`, `viewItem == token` and
//! at most one `view == <id>`, with at least one `viewItem` clause.
std::optional<ViewItemMenuContribution> ViewItemWhen(std::wstring_view when)
{
	if (when.size() > 1024) return std::nullopt;
	const auto token = [](std::wstring_view value) {
		return !value.empty() && value.size() <= 256 && std::ranges::all_of(value, [](const wchar_t ch) {
			return IsAsciiAlphanumeric(ch) || ch == L'_' || ch == L'-';
		});
	};
	ViewItemMenuContribution parsed;
	for (std::size_t start = 0;;) {
		const auto split = when.find(L"&&", start);
		auto clause = TrimAsciiSpace(when.substr(start, split == std::wstring_view::npos ? split : split - start));
		if (clause.starts_with(L"viewItem")) {
			clause = TrimAsciiSpace(clause.substr(8));
			if (clause.starts_with(L"=~")) {
				auto pattern = TrimAsciiSpace(clause.substr(2));
				if (pattern.size() < 2 || pattern.front() != L'/' || pattern.back() != L'/') return std::nullopt;
				pattern = pattern.substr(1, pattern.size() - 2);
				if (!token(pattern) || std::ranges::find(parsed.Contains(), pattern) != parsed.Contains().end())
					return std::nullopt;
				parsed.AddContains(std::wstring(pattern));
			} else if (clause.starts_with(L"==")) {
				const auto value = TrimAsciiSpace(clause.substr(2));
				if (!token(value) || std::ranges::find(parsed.Equals(), value) != parsed.Equals().end()) return std::nullopt;
				parsed.AddEquals(std::wstring(value));
			} else return std::nullopt;
		} else {
			if (!clause.starts_with(L"view")) return std::nullopt;
			clause = TrimAsciiSpace(clause.substr(4));
			if (!clause.starts_with(L"==")) return std::nullopt;
			const auto view = TrimAsciiSpace(clause.substr(2));
			if (!ValidEffectIdentifier(view) || !parsed.Views().empty()) return std::nullopt;
			parsed.AddView(std::wstring(view));
		}
		if (split == std::wstring_view::npos) break;
		start = split + 2;
	}
	if (parsed.Contains().empty() && parsed.Equals().empty()) return std::nullopt;
	return parsed;
}

bool ParseRuntimeContribution(const JsoncValue::Object& manifest,
	const JsoncValue::Object& contributes, RuntimeContribution& target)
{
	const auto* schema = IntegerMember(manifest, L"schemaVersion");
	if (!schema || (*schema != 1 && *schema != 2)) return false;
	target.schemaVersion = static_cast<std::uint32_t>(*schema);
	if (!ParseStringArray(manifest, L"activationEvents", target.activationEvents)
		|| !ParseStringArray(manifest, L"capabilities", target.capabilities)) return false;
	const auto* runtime = ObjectMember(manifest, L"runtime");
	const auto runtimeMember = manifest.find(L"runtime");
	if (!runtime && runtimeMember != manifest.end()
		&& !std::holds_alternative<std::monostate>(runtimeMember->second.Value())) return false;
	if (runtime) {
		const auto* abi = StringMember(*runtime, L"abi");
		const auto* module = StringMember(*runtime, L"module");
		// Schema 1 has one frozen world. Which schema-2 world this build runs is
		// sakura_senp's decision, delivered as `compatible`; only its shape is checked here.
		constexpr std::wstring_view v1Abi = L"sakura:senp/extension@1.0.0";
		constexpr std::wstring_view prefix = L"sakura:senp/extension@";
		if (!abi || !module || *module != L"module/extension.wasm"
			|| (*schema == 1 && *abi != v1Abi)
			|| (*schema == 2 && (*abi == v1Abi || abi->size() > 64 || !abi->starts_with(prefix)
				|| abi->size() == prefix.size()))) return false;
		target.abi = *abi;
	} else if (*schema == 2 || !target.activationEvents.empty() || !target.capabilities.empty()) return false;
	const auto* commands = ArrayMember(contributes, L"commands");
	// Older v1 package tools predate this default-empty member.
	if (!commands) return *schema == 1 && !contributes.contains(L"commands") && !contributes.contains(L"menus");
	if (commands->size() > 128 || (*schema == 1 && !commands->empty())) return false;
	for (const auto& entry : *commands) {
		const auto* object = std::get_if<JsoncValue::Object>(&entry.Value());
		const auto* icon = object ? StringMember(*object, L"icon") : nullptr;
		if (!object || object->size() != (icon ? 3u : 2u)) return false;
		const auto* command = StringMember(*object, L"command");
		const auto* title = StringMember(*object, L"title");
		if (!command || command->empty() || command->size() > 160 || !title || title->empty() || title->size() > 160
			|| (icon && !ValidThemeIcon(*icon) && !ValidPackageImagePath(*icon))
			|| std::ranges::any_of(target.commands, [&](const auto& prior) { return prior.Command() == *command; })) return false;
		target.commands.push_back({ *command, *title, icon ? *icon : std::wstring{} });
	}
	const auto menusMember = contributes.find(L"menus");
	if (menusMember == contributes.end()) return true;
	const auto* menus = std::get_if<JsoncValue::Object>(&menusMember->second.Value());
	if (!menus || *schema != 2 || menus->empty()
		|| std::ranges::any_of(*menus, [](const auto& member) {
			return member.first != L"view/title" && member.first != L"view/item/context";
		})) return false;
	const auto* viewTitle = ArrayMember(*menus, L"view/title");
	const auto* viewItem = ArrayMember(*menus, L"view/item/context");
	if ((menus->contains(L"view/title") && (!viewTitle || viewTitle->empty() || viewTitle->size() > 64))
		|| (menus->contains(L"view/item/context") && (!viewItem || viewItem->empty() || viewItem->size() > 64)))
		return false;
	if (viewItem) {
		for (const auto& entry : *viewItem) {
			const auto* object = std::get_if<JsoncValue::Object>(&entry.Value());
			if (!object || object->size() != 3) return false;
			const auto* command = StringMember(*object, L"command");
			const auto* when = StringMember(*object, L"when");
			const auto* group = StringMember(*object, L"group");
			const auto declared = command ? std::ranges::find(target.commands, *command, &CommandContribution::Command)
				: target.commands.end();
			if (declared == target.commands.end() || declared->Icon().empty() || !when || !group || *group != L"inline")
				return false;
			auto item = ViewItemWhen(*when);
			if (!item) return false;
			// One command appears once per View; an item without a View clause covers all of them.
			for (const auto& prior : target.ViewItemContext()) {
				if (prior.Command() == *command
					&& (prior.Views().empty() || item->Views().empty() || prior.Views() == item->Views()))
					return false;
			}
			item->SetCommand(*command);
			target.AddViewItemContext(std::move(*item));
		}
	}
	if (!viewTitle) return true;
	for (const auto& entry : *viewTitle) {
		const auto* object = std::get_if<JsoncValue::Object>(&entry.Value());
		if (!object || object->size() != 3) return false;
		const auto* command = StringMember(*object, L"command");
		const auto* when = StringMember(*object, L"when");
		const auto* group = StringMember(*object, L"group");
		const auto declared = command ? std::ranges::find(target.commands, *command, &CommandContribution::Command)
			: target.commands.end();
		// A title button draws a codicon only; a package image path is an inline row action's.
		if (declared == target.commands.end() || !ValidThemeIcon(declared->Icon()) || !when || !group
			|| *group != L"navigation")
			return false;
		auto views = ViewTitleWhenViews(*when);
		if (!views) return false;
		for (const auto& view : *views) {
			for (const auto& prior : target.ViewTitle()) {
				if (prior.Command() == *command && std::ranges::find(prior.Views(), view) != prior.Views().end())
					return false;
			}
		}
		target.AddViewTitle(ViewTitleMenuContribution(*command, std::move(*views)));
	}
	return true;
}

} // namespace

void detail::ReadPipeBounded(void* pipe, std::string& target, bool& overflow) noexcept
{
	std::array<char, 16384> buffer{};
	for (;;) {
		DWORD read = 0;
		if (!::ReadFile(static_cast<HANDLE>(pipe), buffer.data(),
			static_cast<DWORD>(buffer.size()), &read, nullptr) || read == 0) break;
		const auto room = kMaximumToolOutputBytes > target.size()
			? kMaximumToolOutputBytes - target.size() : 0;
		const auto copy = std::min<std::size_t>(room, read);
		if (copy < read) overflow = true;
		target.append(buffer.data(), copy);
	}
}

std::vector<std::wstring_view> EmbeddedBuiltInExtensionIds()
{
	std::vector<std::wstring_view> ids;
	ids.reserve(kBuiltInResources.size());
	for (const auto& builtIn : kBuiltInResources) ids.push_back(builtIn.Id());
	return ids;
}

bool StartupInstallsBuiltIn(std::wstring_view extensionId,
	bool explicitlyUninstalled, bool installedAsBuiltIn) noexcept
{
	if (explicitlyUninstalled) return false;
	const auto builtIn = std::ranges::find(kBuiltInResources, extensionId, &BuiltInResource::Id);
	return builtIn != kBuiltInResources.end()
		&& (builtIn->InstalledByDefault() || installedAsBuiltIn);
}

namespace {

constexpr std::wstring_view kInvalidRowCode = L"invalidRow";
// One line per profile state entry; the producer bounds those at MAX_ENTRIES
// in rust/senp/sakura_senp/src/lib.rs. More lines can only be a corrupt stream.
constexpr std::size_t kMaximumListingLines = 256;
constexpr std::size_t kInvalidFieldBound = 128;
constexpr std::size_t kInvalidMessageBound = 1024;

std::wstring BoundedInvalidText(const std::wstring* value, std::size_t bound)
{
	if (value == nullptr) return {};
	return value->substr(0, bound);
}

//! The descriptor for a `kind:"invalid"` line, or for a line the decoder could
//! not read: it carries no contributions, is not enabled, and owns no runtime.
ExtensionDescriptor InvalidDescriptor(std::wstring id, std::wstring trust,
	std::wstring code, std::wstring message, std::wstring stateName = {})
{
	ExtensionDescriptor row;
	row.id = std::move(id);
	row.displayName = row.id.empty() ? L"invalid package" : row.id;
	row.description = message;
	row.installed = true;
	row.trust = std::move(trust);
	row.invalidCode = std::move(code);
	row.invalidMessage = std::move(message);
	row.stateName = std::move(stateName);
	return row;
}

std::optional<ExtensionDescriptor> DecodeInstalledObject(const JsoncValue::Object& object)
{
	const auto manifestMember = object.find(L"manifest");
	const auto* manifest = manifestMember == object.end()
		? nullptr : std::get_if<JsoncValue::Object>(&manifestMember->second.Value());
	const auto* archive = StringMember(object, L"archiveSha256");
	const auto* enabled = BoolMember(object, L"enabled");
	const auto* signedPackage = BoolMember(object, L"signed");
	const auto* trust = StringMember(object, L"trust");
	const auto* extensionPath = StringMember(object, L"extensionPath");
	const auto* modulePath = StringMember(object, L"modulePath");
	const auto* compatible = BoolMember(object, L"compatible");
	const auto moduleSha256Member = object.find(L"moduleSha256");
	const auto* moduleSha256 = moduleSha256Member == object.end()
		? nullptr : std::get_if<std::wstring>(&moduleSha256Member->second.Value());
	if (manifest == nullptr || archive == nullptr || enabled == nullptr || compatible == nullptr
		|| signedPackage == nullptr || trust == nullptr || extensionPath == nullptr) {
		return std::nullopt;
	}
	const auto isLowerSha256 = [](std::wstring_view value) noexcept {
		return value.size() == 64 && std::ranges::all_of(value, [](wchar_t ch) {
			return (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f');
		});
	};
	if (modulePath != nullptr) {
		if (modulePath->empty() || moduleSha256 == nullptr || !isLowerSha256(*moduleSha256)) {
			return std::nullopt;
		}
	} else if (moduleSha256Member == object.end()
		|| !std::holds_alternative<std::monostate>(moduleSha256Member->second.Value())) {
		return std::nullopt;
	}
	const auto* id = StringMember(*manifest, L"id");
	const auto* displayName = StringMember(*manifest, L"displayName");
	const auto* version = StringMember(*manifest, L"version");
	const auto* publisher = StringMember(*manifest, L"publisher");
	const auto* description = StringMember(*manifest, L"description");
	const auto contributesMember = manifest->find(L"contributes");
	const auto* contributes = contributesMember == manifest->end() ? nullptr
		: std::get_if<JsoncValue::Object>(&contributesMember->second.Value());
	const auto decorationsMember = contributes == nullptr ? JsoncValue::Object::const_iterator{}
		: contributes->find(L"editorDecorations");
	const auto* decorations = contributes == nullptr || decorationsMember == contributes->end()
		? nullptr : std::get_if<JsoncValue::Array>(&decorationsMember->second.Value());
	std::vector<LanguageContribution> languages;
	std::vector<GrammarContribution> grammars;
	std::vector<ViewContainerContribution> viewContainers;
	std::vector<ViewContribution> views;
	RuntimeContribution runtime;
	if (id == nullptr || displayName == nullptr || version == nullptr
		|| publisher == nullptr || description == nullptr || archive->size() != 64
		|| decorations == nullptr || contributes == nullptr
		|| !ParseLanguageContributions(*contributes, languages)
		|| !ParseGrammarContributions(*contributes, grammars)
		|| !ParseWorkbenchViewContributions(*contributes, viewContainers, views)
		|| !ParseRuntimeContribution(*manifest, *contributes, runtime)) return std::nullopt;
	runtime.compatible = *compatible;
	return ExtensionDescriptor{
		.id = *id,
		.displayName = *displayName,
		.version = *version,
		.publisher = *publisher,
		.description = *description,
		.extensionPath = *extensionPath,
		.modulePath = modulePath == nullptr ? std::wstring{} : *modulePath,
		.moduleSha256 = moduleSha256 == nullptr ? std::wstring{} : *moduleSha256,
		.archiveSha256 = *archive,
		.installed = true,
		.builtIn = *trust == L"builtin",
		.enabled = *enabled,
		.signedPackage = *signedPackage,
		.contributesIndentDecorations = !decorations->empty(),
		.languages = std::move(languages),
		.grammars = std::move(grammars),
		.viewContainers = std::move(viewContainers),
		.views = std::move(views),
		.trust = *trust,
		.runtime = std::move(runtime),
	};
}

ExtensionDescriptor DecodeInstalledLine(std::string_view line)
{
	const auto parsed = platform::serialization::CJsoncDocument::Parse(line);
	const auto* object = parsed.Succeeded()
		? std::get_if<JsoncValue::Object>(&parsed.value->Value()) : nullptr;
	if (object == nullptr) {
		return InvalidDescriptor({}, {}, std::wstring(kInvalidRowCode),
			L"listing line is not a JSON object");
	}
	const auto* kind = StringMember(*object, L"kind");
	if (kind != nullptr && *kind == L"invalid") {
		auto code = BoundedInvalidText(StringMember(*object, L"code"), kInvalidFieldBound);
		return InvalidDescriptor(
			BoundedInvalidText(StringMember(*object, L"id"), kInvalidFieldBound),
			BoundedInvalidText(StringMember(*object, L"trust"), kInvalidFieldBound),
			code.empty() ? std::wstring(kInvalidRowCode) : std::move(code),
			BoundedInvalidText(StringMember(*object, L"message"), kInvalidMessageBound),
			BoundedInvalidText(StringMember(*object, L"state"), kInvalidFieldBound));
	}
	// Rows must say what they are: a missing or unknown kind never decodes as
	// installed.
	if (kind == nullptr || *kind != L"installed") {
		return InvalidDescriptor({}, {}, std::wstring(kInvalidRowCode),
			L"listing row kind is missing or unknown");
	}
	auto decoded = DecodeInstalledObject(*object);
	if (!decoded) {
		return InvalidDescriptor({}, {}, std::wstring(kInvalidRowCode),
			L"listing row could not be decoded");
	}
	return std::move(*decoded);
}

} // namespace

std::optional<std::vector<ExtensionDescriptor>> DecodeInstalledExtensions(std::string_view output)
{
	// `list` emits JSON Lines: one complete object per package. Each line is
	// decoded on its own, so one bad row degrades to an invalid descriptor
	// instead of discarding the whole listing.
	std::vector<ExtensionDescriptor> result;
	for (std::size_t offset = 0; offset <= output.size();) {
		const auto newline = output.find('\n', offset);
		auto line = output.substr(offset, newline == std::string_view::npos
			? newline : newline - offset);
		offset = newline == std::string_view::npos ? output.size() + 1 : newline + 1;
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
			line.remove_suffix(1);
		}
		if (!line.empty()) {
			// A stream longer than the producer's own bound is not a listing to
			// truncate and trust: the whole reload fails and keeps its snapshot.
			if (result.size() >= kMaximumListingLines) return std::nullopt;
			result.push_back(DecodeInstalledLine(line));
		}
		if (newline == std::string_view::npos) break;
	}
	return result;
}

namespace {

std::optional<std::vector<std::wstring>> ParseUninstalled(std::string_view json)
{
	const auto parsed = platform::serialization::CJsoncDocument::Parse(json);
	if (!parsed.Succeeded()) return std::nullopt;
	const auto* array = std::get_if<JsoncValue::Array>(&parsed.value->Value());
	if (array == nullptr || array->size() > 256) return std::nullopt;
	std::vector<std::wstring> result;
	result.reserve(array->size());
	for (const auto& value : *array) {
		const auto* id = std::get_if<std::wstring>(&value.Value());
		if (id == nullptr || id->empty() || id->size() > 128
			|| std::ranges::find(result, *id) != result.end()) return std::nullopt;
		result.push_back(*id);
	}
	return result;
}

} // namespace

std::optional<ExtensionDescriptor> DecodeBuiltInExtension(std::string_view json)
{
	const auto parsed = platform::serialization::CJsoncDocument::Parse(json);
	if (!parsed.Succeeded()) return std::nullopt;
	const auto* object = std::get_if<JsoncValue::Object>(&parsed.value->Value());
	if (object == nullptr) return std::nullopt;
	const auto manifestMember = object->find(L"manifest");
	const auto* manifest = manifestMember == object->end()
		? nullptr : std::get_if<JsoncValue::Object>(&manifestMember->second.Value());
	const auto* archive = StringMember(*object, L"archiveSha256");
	const auto* signedPackage = BoolMember(*object, L"signed");
	if (manifest == nullptr || archive == nullptr || archive->size() != 64
		|| signedPackage == nullptr) return std::nullopt;
	const auto* id = StringMember(*manifest, L"id");
	const auto* displayName = StringMember(*manifest, L"displayName");
	const auto* version = StringMember(*manifest, L"version");
	const auto* publisher = StringMember(*manifest, L"publisher");
	const auto* description = StringMember(*manifest, L"description");
	const auto contributesMember = manifest->find(L"contributes");
	const auto* contributes = contributesMember == manifest->end() ? nullptr
		: std::get_if<JsoncValue::Object>(&contributesMember->second.Value());
	const auto decorationsMember = contributes == nullptr ? JsoncValue::Object::const_iterator{}
		: contributes->find(L"editorDecorations");
	const auto* decorations = contributes == nullptr || decorationsMember == contributes->end()
		? nullptr : std::get_if<JsoncValue::Array>(&decorationsMember->second.Value());
	std::vector<LanguageContribution> languages;
	std::vector<GrammarContribution> grammars;
	std::vector<ViewContainerContribution> viewContainers;
	std::vector<ViewContribution> views;
	RuntimeContribution runtime;
	if (id == nullptr || displayName == nullptr || version == nullptr
		|| publisher == nullptr || description == nullptr || decorations == nullptr
		|| contributes == nullptr || !ParseLanguageContributions(*contributes, languages)
		|| !ParseGrammarContributions(*contributes, grammars)
		|| !ParseWorkbenchViewContributions(*contributes, viewContainers, views)
		|| !ParseRuntimeContribution(*manifest, *contributes, runtime)) {
		return std::nullopt;
	}
	return ExtensionDescriptor{
		.id = *id,
		.displayName = *displayName,
		.version = *version,
		.publisher = *publisher,
		.description = *description,
		.archiveSha256 = *archive,
		.installed = false,
		.builtIn = true,
		.enabled = false,
		.signedPackage = *signedPackage,
		.contributesIndentDecorations = !decorations->empty(),
		.languages = std::move(languages),
		.grammars = std::move(grammars),
		.viewContainers = std::move(viewContainers),
		.views = std::move(views),
		.trust = L"builtin",
		.runtime = std::move(runtime),
	};
}

namespace {

struct StagedBuiltInPackage final {
	std::filesystem::path path;
	std::wstring hash;
};

std::optional<StagedBuiltInPackage> StageBuiltInPackage(
	const BuiltInResource& builtIn, std::wstring_view installRoot, std::wstring& diagnostic,
	const detail::SenpServiceHooks& hooks)
{
	const auto package = EmbeddedResource(builtIn.PackageResource(), hooks);
	const auto hashBytes = EmbeddedResource(builtIn.HashResource(), hooks);
	if (!package || !hashBytes) {
		diagnostic = L"Built-in SENP resources are missing: " + std::wstring(builtIn.Id());
		return std::nullopt;
	}
	std::string hash(reinterpret_cast<const char*>(hashBytes->data()), hashBytes->size());
	while (!hash.empty() && (hash.back() == '\r' || hash.back() == '\n')) hash.pop_back();
	if (hash.size() != 64) {
		diagnostic = L"Built-in SENP hash resource is invalid: " + std::wstring(builtIn.Id());
		return std::nullopt;
	}
	const auto staging = std::filesystem::path(installRoot) / L"staging"
		/ (std::wstring(builtIn.Id()) + L"." + std::to_wstring(::GetCurrentProcessId()) + L".senp");
	if (!WriteBytes(staging, *package)) {
		diagnostic = L"Built-in SENP package could not be staged: " + std::wstring(builtIn.Id());
		return std::nullopt;
	}
	return StagedBuiltInPackage{ staging, Utf8ToWide(hash) };
}

std::wstring ProcessDiagnostic(const ProcessResult& result, std::wstring_view fallback)
{
	if (!result.launched) return L"sakura-senp-tool.exe is unavailable";
	if (result.timedOut) return L"SENP package operation timed out";
	const auto error = Utf8ToWide(result.error);
	return error.empty() ? std::wstring(fallback) : error;
}

} // namespace

CWin32SenpManagementService::CWin32SenpManagementService(std::wstring profileRoot,
	detail::SenpServiceHooks hooks)
	: m_profileRoot(std::move(profileRoot))
	, m_hooks(std::move(hooks))
	, m_installRoot((std::filesystem::path(m_profileRoot) / L"senp").native())
{
}

CWin32SenpManagementService::~CWin32SenpManagementService()
{
	Stop();
}

ManagementOperationResult CWin32SenpManagementService::Terminal(
	EManagementOperationStatus status, std::wstring diagnostic)
{
	std::lock_guard lock(m_mutex);
	if (!diagnostic.empty()) m_snapshot.diagnostic = std::move(diagnostic);
	return { status, m_snapshot };
}

bool CWin32SenpManagementService::IsStopped() const noexcept
{
	std::lock_guard lock(m_mutex);
	return m_snapshot.state == EManagementState::Stopped;
}

ManagementOperationResult CWin32SenpManagementService::Start()
{
	std::vector<std::wstring> retry;
	{
		std::lock_guard lock(m_mutex);
		if (m_snapshot.state == EManagementState::Stopped) {
			return { EManagementOperationStatus::Stopped, m_snapshot };
		}
		if (m_snapshot.state == EManagementState::Ready
			|| m_snapshot.state == EManagementState::ReadyWithDiagnostics) {
			// A Start that previously lost built-in installs does not answer
			// AlreadyReady: exactly those ids are attempted again.
			if (m_failedBuiltIns.empty()) return { EManagementOperationStatus::AlreadyReady, m_snapshot };
			retry = m_failedBuiltIns;
		}
	}
	if (retry.empty()) {
		const auto catalog = LoadBuiltInCatalog();
		if (!catalog.Succeeded()) return catalog;
	}
	auto loaded = ReloadInstalled();
	if (!loaded.Succeeded()) return loaded;
	// A built-in tracks the product that bundles it, as VS Code's built-in
	// extensions do. An offered (not installed-by-default) built-in the user
	// installed is refreshed too, never installed on their behalf: a copy left
	// by an earlier build may target a runtime ABI this host cannot run (#299).
	std::vector<std::wstring> failed;
	for (const auto& builtIn : kBuiltInResources) {
		if (!retry.empty()
			&& std::ranges::find(retry, builtIn.Id()) == retry.end()) continue;
		bool current = false;
		bool explicitlyUninstalled = false;
		bool installedAsBuiltIn = false;
		{
			std::lock_guard lock(m_mutex);
			const auto installed = std::ranges::find(m_snapshot.extensions,
				builtIn.Id(), &ExtensionDescriptor::id);
			const auto candidate = std::ranges::find(m_builtInCatalog,
				builtIn.Id(), &ExtensionDescriptor::id);
			installedAsBuiltIn = installed != m_snapshot.extensions.end() && installed->installed
				&& installed->builtIn;
			// `current` requires the built-in trust class too: a same-id
			// developer/publisher copy of identical bytes is a different trust
			// class and startup must replace it with the embedded package.
			current = installed != m_snapshot.extensions.end() && installed->installed
				&& installed->builtIn
				&& candidate != m_builtInCatalog.end()
				&& installed->archiveSha256 == candidate->archiveSha256;
			explicitlyUninstalled = std::ranges::find(m_uninstalledBuiltIns,
				builtIn.Id()) != m_uninstalledBuiltIns.end();
		}
		if (current) continue;
		if (!StartupInstallsBuiltIn(builtIn.Id(), explicitlyUninstalled, installedAsBuiltIn)) continue;
		// One failing built-in never stops the reconciliation: every package is
		// attempted once per Start and the failures are named in the diagnostic.
		loaded = InstallBuiltInPackage(builtIn.Id());
		if (!loaded.Succeeded()) failed.emplace_back(builtIn.Id());
	}
	if (!failed.empty()) {
		std::wstring diagnostic = L"Built-in SENP packages could not be installed:";
		for (const auto& id : failed) {
			diagnostic.push_back(L' ');
			diagnostic += id;
		}
		std::lock_guard lock(m_mutex);
		m_failedBuiltIns = std::move(failed);
		m_snapshot.diagnostic = std::move(diagnostic);
		m_snapshot.state = EManagementState::ReadyWithDiagnostics;
		++m_snapshot.revision;
		return { EManagementOperationStatus::Succeeded, m_snapshot };
	}
	{
		std::lock_guard lock(m_mutex);
		m_failedBuiltIns.clear();
	}
	return loaded;
}

ManagementOperationResult CWin32SenpManagementService::LoadBuiltInCatalog()
{
	std::vector<ExtensionDescriptor> catalog;
	catalog.reserve(kBuiltInResources.size());
	for (const auto& builtIn : kBuiltInResources) {
		std::wstring diagnostic;
		const auto staged = StageBuiltInPackage(builtIn, m_installRoot, diagnostic, m_hooks);
		if (!staged) {
			std::lock_guard lock(m_mutex);
			m_snapshot.state = EManagementState::Failed;
			++m_snapshot.revision;
			m_snapshot.diagnostic = std::move(diagnostic);
			return { EManagementOperationStatus::Unavailable, m_snapshot };
		}
		const auto inspected = RunTool({ L"inspect-builtin", staged->path.native(), staged->hash }, m_hooks);
		std::error_code ignored;
		std::filesystem::remove(staged->path, ignored);
		if (!inspected.launched || inspected.timedOut || inspected.exitCode != 0
			|| inspected.outputTruncated) {
			std::lock_guard lock(m_mutex);
			m_snapshot.state = EManagementState::Failed;
			++m_snapshot.revision;
			m_snapshot.diagnostic = ProcessDiagnostic(inspected, L"Built-in SENP package was rejected");
			return { EManagementOperationStatus::Failed, m_snapshot };
		}
		auto extension = DecodeBuiltInExtension(inspected.output);
		if (!extension || extension->id != builtIn.Id()) {
			std::lock_guard lock(m_mutex);
			m_snapshot.state = EManagementState::Failed;
			++m_snapshot.revision;
			m_snapshot.diagnostic = L"Built-in SENP package metadata is invalid";
			return { EManagementOperationStatus::Failed, m_snapshot };
		}
		catalog.push_back(std::move(*extension));
	}
	std::lock_guard lock(m_mutex);
	m_builtInCatalog = std::move(catalog);
	return { EManagementOperationStatus::Succeeded, m_snapshot };
}

ManagementOperationResult CWin32SenpManagementService::ReloadInstalled()
{
	if (IsStopped()) return Terminal(EManagementOperationStatus::Stopped);
	const auto failedListing = [&](const ProcessResult& result, std::wstring fallback) {
		std::lock_guard lock(m_mutex);
		m_snapshot.state = m_snapshot.extensions.empty()
			? EManagementState::Failed : EManagementState::ReadyWithDiagnostics;
		++m_snapshot.revision;
		m_snapshot.diagnostic = ProcessDiagnostic(result, std::move(fallback));
		return ManagementOperationResult{ EManagementOperationStatus::Failed, m_snapshot };
	};
	const auto listed = RunTool({ L"list", m_installRoot }, m_hooks);
	// A truncated stream is a typed failure, never a partial listing to parse.
	if (!listed.launched || listed.timedOut || listed.exitCode != 0) {
		return failedListing(listed, L"Installed SENP extensions could not be read");
	}
	if (listed.outputTruncated) {
		return failedListing(listed,
			L"Installed SENP extension listing exceeded the 4 MiB output budget");
	}
	const auto listedUninstalled = RunTool({ L"list-uninstalled", m_installRoot }, m_hooks);
	if (!listedUninstalled.launched || listedUninstalled.timedOut
		|| listedUninstalled.exitCode != 0) {
		return failedListing(listedUninstalled,
			L"Uninstalled SENP extension preferences could not be read");
	}
	if (listedUninstalled.outputTruncated) {
		return failedListing(listedUninstalled,
			L"Uninstalled SENP extension preferences exceeded the 4 MiB output budget");
	}
	const auto parsed = DecodeInstalledExtensions(listed.output);
	const auto parsedUninstalled = ParseUninstalled(listedUninstalled.output);
	if (!parsed || !parsedUninstalled) {
		std::lock_guard lock(m_mutex);
		m_snapshot.state = m_snapshot.extensions.empty()
			? EManagementState::Failed : EManagementState::ReadyWithDiagnostics;
		++m_snapshot.revision;
		m_snapshot.diagnostic = L"SENP package-state output is invalid";
		return { EManagementOperationStatus::Failed, m_snapshot };
	}
	std::lock_guard lock(m_mutex);
	auto extensions = std::move(*parsed);
	m_uninstalledBuiltIns = std::move(*parsedUninstalled);
	for (const auto& candidate : m_builtInCatalog) {
		const auto installed = std::ranges::find(extensions, candidate.id, &ExtensionDescriptor::id);
		if (installed == extensions.end()) extensions.push_back(candidate);
	}
	std::ranges::sort(extensions, {}, &ExtensionDescriptor::displayName);
	m_snapshot.extensions = std::move(extensions);
	m_snapshot.diagnostic.clear();
	m_snapshot.state = EManagementState::Ready;
	++m_snapshot.revision;
	return { EManagementOperationStatus::Succeeded, m_snapshot };
}

ManagementOperationResult CWin32SenpManagementService::InstallDeveloperPackage(
	std::wstring_view packagePath, bool enable)
{
	if (packagePath.empty() || packagePath.size() >= 32768 || IsStopped()) {
		return Terminal(IsStopped() ? EManagementOperationStatus::Stopped
			: EManagementOperationStatus::InvalidRequest);
	}
	const std::filesystem::path package(packagePath);
	if (package.extension() != L".senp" || !std::filesystem::is_regular_file(package)) {
		return Terminal(EManagementOperationStatus::InvalidRequest, L"Select a regular .senp package");
	}
	const auto installed = RunTool({ L"install-dev", package.native(), m_installRoot }, m_hooks);
	if (!installed.launched || installed.timedOut || installed.exitCode != 0
		|| installed.outputTruncated) {
		return Terminal(EManagementOperationStatus::Failed,
			ProcessDiagnostic(installed, L"Developer SENP package was rejected"));
	}
	if (enable) {
		const auto parsed = platform::serialization::CJsoncDocument::Parse(installed.output);
		const auto* root = parsed.Succeeded()
			? std::get_if<JsoncValue::Object>(&parsed.value->Value()) : nullptr;
		const auto manifestMember = root == nullptr ? JsoncValue::Object::const_iterator{}
			: root->find(L"manifest");
		const auto* manifest = root == nullptr || manifestMember == root->end()
			? nullptr : std::get_if<JsoncValue::Object>(&manifestMember->second.Value());
		const auto* id = manifest == nullptr ? nullptr : StringMember(*manifest, L"id");
		if (id == nullptr) return Terminal(EManagementOperationStatus::Failed, L"SENP install result is invalid");
		const auto enabled = RunTool({ L"set-enabled", m_installRoot, *id, L"true" }, m_hooks);
		if (!enabled.launched || enabled.timedOut || enabled.exitCode != 0) {
			return Terminal(EManagementOperationStatus::Failed,
				ProcessDiagnostic(enabled, L"Developer SENP extension could not be enabled"));
		}
	}
	return ReloadInstalled();
}

ManagementOperationResult CWin32SenpManagementService::InstallBuiltInPackage(
	std::wstring_view extensionId)
{
	if (extensionId.empty() || IsStopped()) {
		return Terminal(IsStopped() ? EManagementOperationStatus::Stopped
			: EManagementOperationStatus::InvalidRequest);
	}
	const auto builtIn = std::ranges::find(kBuiltInResources, extensionId, &BuiltInResource::Id);
	if (builtIn == kBuiltInResources.end()) {
		return Terminal(EManagementOperationStatus::InvalidRequest,
			L"The built-in SENP extension is unavailable");
	}
	{
		std::lock_guard lock(m_mutex);
		const auto installed = std::ranges::find(m_snapshot.extensions,
			extensionId, &ExtensionDescriptor::id);
		const auto candidate = std::ranges::find(m_builtInCatalog,
			extensionId, &ExtensionDescriptor::id);
		// Same trust class too: a developer/publisher copy of identical bytes
		// is not "already installed": it must be replaced with built-in trust.
		if (installed != m_snapshot.extensions.end() && installed->installed
			&& installed->builtIn
			&& candidate != m_builtInCatalog.end()
			&& installed->archiveSha256 == candidate->archiveSha256) {
			return { EManagementOperationStatus::AlreadyReady, m_snapshot };
		}
	}
	std::wstring diagnostic;
	const auto staged = StageBuiltInPackage(*builtIn, m_installRoot, diagnostic, m_hooks);
	if (!staged) return Terminal(EManagementOperationStatus::Failed, std::move(diagnostic));
	const auto installed = RunTool({
		L"install-builtin", staged->path.native(), m_installRoot, staged->hash,
	}, m_hooks);
	std::error_code ignored;
	std::filesystem::remove(staged->path, ignored);
	if (!installed.launched || installed.timedOut || installed.exitCode != 0 || installed.outputTruncated) {
		return Terminal(EManagementOperationStatus::Failed,
			ProcessDiagnostic(installed, L"Built-in SENP package was rejected"));
	}
	return ReloadInstalled();
}

ManagementOperationResult CWin32SenpManagementService::UninstallBuiltInPackage(
	std::wstring_view extensionId)
{
	if (extensionId.empty() || IsStopped()) {
		return Terminal(IsStopped() ? EManagementOperationStatus::Stopped
			: EManagementOperationStatus::InvalidRequest);
	}
	const auto builtIn = std::ranges::find(kBuiltInResources, extensionId, &BuiltInResource::Id);
	if (builtIn == kBuiltInResources.end()) {
		return Terminal(EManagementOperationStatus::InvalidRequest,
			L"The built-in SENP extension is unavailable");
	}
	std::wstring archiveSha256;
	{
		std::lock_guard lock(m_mutex);
		const auto installed = std::ranges::find(m_snapshot.extensions,
			extensionId, &ExtensionDescriptor::id);
		const auto candidate = std::ranges::find(m_builtInCatalog,
			extensionId, &ExtensionDescriptor::id);
		if (installed == m_snapshot.extensions.end() || !installed->installed) {
			if (std::ranges::find(m_uninstalledBuiltIns, extensionId)
				!= m_uninstalledBuiltIns.end()) {
				return { EManagementOperationStatus::AlreadyReady, m_snapshot };
			}
			return { EManagementOperationStatus::InvalidRequest, m_snapshot };
		}
		if (!installed->builtIn || installed->trust != L"builtin"
			|| candidate == m_builtInCatalog.end()
			|| installed->archiveSha256 != candidate->archiveSha256) {
			return { EManagementOperationStatus::InvalidRequest, m_snapshot };
		}
		archiveSha256 = installed->archiveSha256;
	}
	const auto uninstalled = RunTool({
		L"uninstall-builtin", m_installRoot, std::wstring(extensionId), archiveSha256,
	}, m_hooks);
	if (!uninstalled.launched || uninstalled.timedOut || uninstalled.exitCode != 0) {
		return Terminal(EManagementOperationStatus::Failed,
			ProcessDiagnostic(uninstalled, L"Built-in SENP extension could not be uninstalled"));
	}
	return ReloadInstalled();
}

ManagementOperationResult CWin32SenpManagementService::SetExtensionEnabled(
	std::wstring_view extensionId, bool enabled)
{
	if (extensionId.empty() || extensionId.size() > 128 || IsStopped()) {
		return Terminal(IsStopped() ? EManagementOperationStatus::Stopped
			: EManagementOperationStatus::InvalidRequest);
	}
	{
		std::lock_guard lock(m_mutex);
		const auto installed = std::ranges::find(m_snapshot.extensions,
			extensionId, &ExtensionDescriptor::id);
		if (installed == m_snapshot.extensions.end() || !installed->installed
			|| installed->builtIn || !installed->invalidCode.empty()) {
			return { EManagementOperationStatus::InvalidRequest, m_snapshot };
		}
	}
	const auto result = RunTool({
		L"set-enabled", m_installRoot, std::wstring(extensionId), enabled ? L"true" : L"false",
	}, m_hooks);
	if (!result.launched || result.timedOut || result.exitCode != 0) {
		return Terminal(EManagementOperationStatus::Failed,
			ProcessDiagnostic(result, L"SENP extension could not change enable state"));
	}
	return ReloadInstalled();
}

ManagementOperationResult CWin32SenpManagementService::UninstallPackage(
	std::wstring_view stateName)
{
	if (stateName.empty() || stateName.size() > 256 || IsStopped()) {
		return Terminal(IsStopped() ? EManagementOperationStatus::Stopped
			: EManagementOperationStatus::InvalidRequest);
	}
	const auto uninstalled = RunTool({
		L"uninstall", m_installRoot, std::wstring(stateName),
	}, m_hooks);
	if (!uninstalled.launched || uninstalled.timedOut || uninstalled.exitCode != 0) {
		return Terminal(EManagementOperationStatus::Failed,
			ProcessDiagnostic(uninstalled, L"SENP extension could not be uninstalled"));
	}
	return ReloadInstalled();
}

ManagementOperationResult CWin32SenpManagementService::Refresh()
{
	return ReloadInstalled();
}

void CWin32SenpManagementService::Stop() noexcept
{
	try {
		std::lock_guard lock(m_mutex);
		if (m_snapshot.state == EManagementState::Stopped) return;
		m_snapshot.state = EManagementState::Stopped;
		++m_snapshot.revision;
	} catch (...) {
	}
}

ManagementSnapshot CWin32SenpManagementService::Snapshot() const
{
	std::lock_guard lock(m_mutex);
	return m_snapshot;
}

std::uint64_t CWin32SenpManagementService::Revision() const
{
	std::lock_guard lock(m_mutex);
	return m_snapshot.revision;
}

} // namespace senp
