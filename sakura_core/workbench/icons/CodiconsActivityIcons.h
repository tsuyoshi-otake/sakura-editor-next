/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib AND CC-BY-4.0
*/
#pragma once

// The Sakura GDI wrapper is Zlib; imported Codicons geometry is CC-BY-4.0.
// Imported from microsoft/vscode-codicons at c20fbe9efb8ff7cc77182c5b43c44025544ff843.
// See CODICONS-ATTRIBUTION.md beside this file for the source path data, license,
// attribution, and the local coordinate transformation.

#include "workbench/IconMetrics.h"

#include <Windows.h>

#include <cmath>
#include <string_view>

namespace workbench::icons::codicons {

//! Codicons used by the native workbench chrome outside the Activity Bar.
enum class Icon {
	Layout,
	LayoutSidebarLeft,
	LayoutPanel,
	LayoutSidebarRight,
	Account,
	Gear,
	ChromeMinimize,
	ChromeMaximize,
	ChromeRestore,
	ChromeClose,
	GitBranch,
	Target,
	Newline,
	Code,
	FileBinary,
	RecordSmall,
	Insert,
	ZoomIn,
	File,
	OpenPreview,
	ChevronDown,
	Close,
	CloseAll,
	Loading,
};

namespace detail {

constexpr int kSvgUnits = 24;
constexpr int kCoordinateScale = 2000;
constexpr int kLogicalExtent = kSvgUnits * kCoordinateScale;

struct SvgIconData {
	int viewBox = 16;
	bool evenOdd = false;
	std::string_view path;
};

//! Returns the largest square viewport contained by a caller-owned icon box.
//! Rendering SVG coordinates into this viewport preserves the source aspect ratio.
[[nodiscard]] constexpr IconRect SvgIconLetterboxBounds(IconRect box) noexcept
{
	const int side = std::min(std::max(0, box.Width()), std::max(0, box.Height()));
	const int left = box.left + (box.Width() - side) / 2;
	const int top = box.top + (box.Height() - side) / 2;
	return { left, top, left + side, top + side };
}

[[nodiscard]] constexpr SvgIconData DataFor(Icon icon) noexcept;

inline void SkipSeparators(const char*& cursor, const char* end) noexcept
{
	while (cursor != end && (*cursor == ' ' || *cursor == '\t' || *cursor == '\r'
		|| *cursor == '\n' || *cursor == ',')) ++cursor;
}

[[nodiscard]] inline bool IsNumberStart(char value) noexcept
{
	return value == '-' || value == '+' || value == '.' || (value >= '0' && value <= '9');
}

[[nodiscard]] inline bool ParseNumber(const char*& cursor, const char* end, double& value) noexcept
{
	SkipSeparators(cursor, end);
	if (cursor == end || !IsNumberStart(*cursor)) return false;
	double sign = 1.0;
	if (*cursor == '-' || *cursor == '+') {
		if (*cursor == '-') sign = -1.0;
		++cursor;
	}
	double integral = 0.0;
	bool hasDigits = false;
	while (cursor != end && *cursor >= '0' && *cursor <= '9') {
		hasDigits = true;
		integral = integral * 10.0 + static_cast<double>(*cursor - '0');
		++cursor;
	}
	double fraction = 0.0;
	double divisor = 1.0;
	if (cursor != end && *cursor == '.') {
		++cursor;
		while (cursor != end && *cursor >= '0' && *cursor <= '9') {
			hasDigits = true;
			fraction = fraction * 10.0 + static_cast<double>(*cursor - '0');
			divisor *= 10.0;
			++cursor;
		}
	}
	if (!hasDigits) return false;
	value = sign * (integral + fraction / divisor);
	return true;
}

[[nodiscard]] inline int LogicalCoordinate(double value) noexcept
{
	return static_cast<int>(std::lround(value * static_cast<double>(kCoordinateScale)));
}

[[nodiscard]] inline bool AppendSvgPath(HDC dc, std::string_view path) noexcept
{
	const char* cursor = path.data();
	const char* const end = cursor + path.size();
	char command = 0;
	double currentX = 0.0;
	double currentY = 0.0;
	double startX = 0.0;
	double startY = 0.0;
	while (true) {
		SkipSeparators(cursor, end);
		if (cursor == end) return true;
		if ((*cursor >= 'A' && *cursor <= 'Z') || (*cursor >= 'a' && *cursor <= 'z')) {
			command = *cursor++;
		}
		if (command == 'Z' || command == 'z') {
			::CloseFigure(dc);
			currentX = startX;
			currentY = startY;
			command = 0;
			continue;
		}
		if (command == 0 || (command >= 'a' && command <= 'z')) return false;
		double values[6]{};
		int count = 0;
		switch (command) {
		case 'M': case 'L': count = 2; break;
		case 'H': case 'V': count = 1; break;
		case 'C': count = 6; break;
		default: return false;
		}
		for (int index = 0; index < count; ++index) {
			if (!ParseNumber(cursor, end, values[index])) return false;
		}
		switch (command) {
		case 'M':
			currentX = values[0]; currentY = values[1];
			startX = currentX; startY = currentY;
			::MoveToEx(dc, LogicalCoordinate(currentX), LogicalCoordinate(currentY), nullptr);
			command = 'L';
			break;
		case 'L':
			currentX = values[0]; currentY = values[1];
			::LineTo(dc, LogicalCoordinate(currentX), LogicalCoordinate(currentY));
			break;
		case 'H':
			currentX = values[0];
			::LineTo(dc, LogicalCoordinate(currentX), LogicalCoordinate(currentY));
			break;
		case 'V':
			currentY = values[0];
			::LineTo(dc, LogicalCoordinate(currentX), LogicalCoordinate(currentY));
			break;
		case 'C': {
			const POINT points[] = {
				{ LogicalCoordinate(values[0]), LogicalCoordinate(values[1]) },
				{ LogicalCoordinate(values[2]), LogicalCoordinate(values[3]) },
				{ LogicalCoordinate(values[4]), LogicalCoordinate(values[5]) },
			};
			::PolyBezierTo(dc, points, 3);
			currentX = values[4]; currentY = values[5];
			break;
		}
		default: return false;
		}
	}
}

[[nodiscard]] constexpr SvgIconData DataFor(Icon icon) noexcept
{
	switch (icon) {
	case Icon::Layout:
		return { 16, true, R"svg(M5.5 1C6.327 1 7 1.673 7 2.5V13.5C7 14.327 6.327 15 5.5 15H2.5C1.673 15 1 14.327 1 13.5V2.5C1 1.673 1.673 1 2.5 1H5.5ZM2.5 2C2.225 2 2 2.225 2 2.5V13.5C2 13.775 2.225 14 2.5 14H5.5C5.775 14 6 13.775 6 13.5V2.5C6 2.225 5.775 2 5.5 2H2.5ZM13.5 9C14.327 9 15 9.673 15 10.5V13.5C15 14.327 14.327 15 13.5 15H10.5C9.673 15 9 14.327 9 13.5V10.5C9 9.673 9.673 9 10.5 9H13.5ZM10.5 10C10.225 10 10 10.225 10 10.5V13.5C10 13.775 10.225 14 10.5 14H13.5C13.775 14 14 13.775 14 13.5V10.5C14 10.225 13.775 10 13.5 10H10.5ZM13.5 1C14.327 1 15 1.673 15 2.5V5.5C15 6.327 14.327 7 13.5 7H10.5C9.673 7 9 6.327 9 5.5V2.5C9 1.673 9.673 1 10.5 1H13.5ZM10.5 2C10.225 2 10 2.225 10 2.5V5.5C10 5.775 10.225 6 10.5 6H13.5C13.775 6 14 5.775 14 5.5V2.5C14 2.225 13.775 2 13.5 2H10.5Z)svg" };
	case Icon::LayoutSidebarLeft:
		return { 16, false, R"svg(M12.5 1C13.881 1 15 2.119 15 3.5V12.5C15 13.881 13.881 15 12.5 15H3.5C2.119 15 1 13.881 1 12.5V3.5C1 2.119 2.119 1 3.5 1H12.5ZM12.5 14C13.328 14 14 13.328 14 12.5V3.5C14 2.672 13.328 2 12.5 2H7V14H12.5Z)svg" };
	case Icon::LayoutPanel:
		return { 16, false, R"svg(M15 12.5C15 13.881 13.881 15 12.5 15H3.5C2.119 15 1 13.881 1 12.5V3.5C1 2.119 2.119 1 3.5 1H12.5C13.881 1 15 2.119 15 3.5V12.5ZM2 10H14V3.5C14 2.672 13.328 2 12.5 2H3.5C2.672 2 2 2.672 2 3.5V10Z)svg" };
	case Icon::LayoutSidebarRight:
		return { 16, false, R"svg(M12.5 1C13.881 1 15 2.119 15 3.5V12.5C15 13.881 13.881 15 12.5 15H3.5C2.119 15 1 13.881 1 12.5V3.5C1 2.119 2.119 1 3.5 1H12.5ZM9 14V2H3.5C2.672 2 2 2.672 2 3.5V12.5C2 13.328 2.672 14 3.5 14H9Z)svg" };
	case Icon::Account:
		return { 16, false, R"svg(M6 5C6 3.89543 6.89543 3 8 3C9.10457 3 10 3.89543 10 5C10 6.10457 9.10457 7 8 7C6.89543 7 6 6.10457 6 5ZM5.49998 8L10.5 8C11.3284 8 12 8.67157 12 9.5C12 10.6161 11.541 11.5103 10.7879 12.1148C10.0466 12.7098 9.05308 13 8 13C6.94692 13 5.95342 12.7098 5.21215 12.1148C4.45897 11.5103 4 10.6161 4 9.5C4 8.67161 4.67156 8 5.49998 8ZM8 0C3.58172 0 0 3.58172 0 8C0 12.4183 3.58172 16 8 16C12.4183 16 16 12.4183 16 8C16 3.58172 12.4183 0 8 0ZM1 8C1 4.13401 4.13401 1 8 1C11.866 1 15 4.13401 15 8C15 11.866 11.866 15 8 15C4.13401 15 1 11.866 1 8Z)svg" };
	case Icon::Gear:
		return { 16, false, R"svg(M7.99997 6C6.89497 6 5.99997 6.895 5.99997 8C5.99997 9.105 6.89497 10 7.99997 10C9.10497 10 9.99997 9.105 9.99997 8C9.99997 6.895 9.10497 6 7.99997 6ZM7.99997 9C7.44797 9 6.99997 8.552 6.99997 8C6.99997 7.448 7.44797 7 7.99997 7C8.55197 7 8.99997 7.448 8.99997 8C8.99997 8.552 8.55197 9 7.99997 9ZM14.565 9.715L13.279 8.628C13.245 8.599 13.213 8.567 13.184 8.533C12.888 8.186 12.931 7.667 13.279 7.372L14.565 6.285C14.693 6.177 14.742 6.003 14.691 5.844C14.386 4.903 13.882 4.04 13.219 3.308C13.139 3.22 13.027 3.172 12.912 3.172C12.865 3.172 12.818 3.18 12.773 3.196L11.186 3.761C11.144 3.776 11.1 3.788 11.056 3.796C11.006 3.805 10.956 3.81 10.907 3.81C10.515 3.81 10.167 3.532 10.094 3.134L9.79097 1.482C9.76097 1.318 9.63397 1.188 9.46997 1.153C8.98997 1.051 8.49897 1 8.00097 1C7.50297 1 7.01097 1.052 6.53097 1.153C6.36697 1.188 6.23997 1.318 6.20997 1.482L5.90797 3.134C5.89997 3.178 5.88797 3.221 5.87297 3.263C5.75197 3.6 5.43397 3.81 5.09397 3.81C5.00197 3.81 4.90797 3.794 4.81597 3.762L3.22897 3.197C3.18397 3.181 3.13597 3.173 3.08997 3.173C2.97497 3.173 2.86297 3.221 2.78297 3.309C2.11897 4.041 1.61597 4.904 1.30997 5.845C1.25797 6.004 1.30797 6.178 1.43597 6.286L2.72197 7.373C2.75597 7.402 2.78797 7.434 2.81697 7.468C3.11297 7.815 3.06997 8.334 2.72197 8.629L1.43597 9.716C1.30797 9.824 1.25897 9.998 1.30997 10.157C1.61497 11.098 2.11897 11.961 2.78297 12.693C2.86297 12.781 2.97497 12.829 3.08997 12.829C3.13697 12.829 3.18397 12.821 3.22897 12.805L4.81597 12.24C4.85797 12.225 4.90197 12.213 4.94597 12.205C4.99597 12.196 5.04597 12.192 5.09497 12.192C5.48697 12.192 5.83497 12.47 5.90797 12.868L6.20997 14.52C6.23997 14.684 6.36697 14.814 6.53097 14.849C7.01097 14.951 7.50297 15.002 8.00097 15.002C8.49897 15.002 8.99097 14.95 9.46997 14.849C9.63397 14.814 9.76097 14.684 9.79097 14.52L10.094 12.868C10.102 12.824 10.114 12.781 10.129 12.739C10.25 12.402 10.568 12.192 10.908 12.192C11 12.192 11.094 12.208 11.186 12.24L12.772 12.805C12.818 12.821 12.865 12.829 12.911 12.829C13.026 12.829 13.138 12.781 13.218 12.693C13.882 11.961 14.385 11.098 14.69 10.157C14.742 9.998 14.692 9.824 14.564 9.716L14.565 9.715ZM12.728 11.726L11.521 11.296C11.323 11.226 11.117 11.19 10.908 11.19C10.139 11.19 9.44697 11.676 9.18797 12.399C9.15397 12.492 9.12897 12.588 9.11097 12.686L8.88097 13.937C8.59097 13.979 8.29597 14 8.00097 14C7.70597 14 7.41097 13.979 7.11997 13.936L6.89097 12.685C6.73197 11.818 5.97697 11.189 5.09497 11.189C4.98697 11.189 4.87697 11.199 4.76597 11.219C4.66897 11.237 4.57397 11.262 4.47997 11.295L3.27297 11.725C2.90497 11.264 2.61097 10.759 2.39397 10.214L3.36797 9.391C3.74097 9.076 3.96797 8.634 4.00797 8.148C4.04797 7.662 3.89497 7.19 3.57797 6.818C3.51397 6.743 3.44297 6.672 3.36797 6.608L2.39397 5.785C2.61097 5.24 2.90497 4.734 3.27297 4.274L4.47997 4.704C4.67797 4.774 4.88397 4.81 5.09397 4.81C5.86297 4.81 6.55497 4.324 6.81397 3.601C6.84797 3.507 6.87297 3.411 6.89097 3.314L7.11997 2.063C7.41097 2.021 7.70597 1.999 8.00097 1.999C8.29597 1.999 8.59097 2.02 8.88097 2.062L9.10997 3.313C9.26897 4.18 10.024 4.809 10.906 4.809C11.014 4.809 11.124 4.799 11.234 4.779C11.331 4.761 11.427 4.736 11.521 4.703L12.728 4.273C13.096 4.733 13.39 5.239 13.607 5.784L12.634 6.607C12.261 6.922 12.033 7.364 11.994 7.85C11.954 8.336 12.107 8.809 12.424 9.18C12.489 9.256 12.559 9.326 12.635 9.39L13.609 10.213C13.392 10.758 13.098 11.264 12.73 11.724L12.728 11.726Z)svg" };
	case Icon::ChromeMinimize:
		return { 16, false, R"svg(M3 7.5C3 7.22386 3.22386 7 3.5 7H12.5C12.7761 7 13 7.22386 13 7.5C13 7.77614 12.7761 8 12.5 8H3.5C3.22386 8 3 7.77614 3 7.5Z)svg" };
	case Icon::ChromeMaximize:
		return { 16, false, R"svg(M2 4.5C2 3.11929 3.11929 2 4.5 2H11.5C12.8807 2 14 3.11929 14 4.5V11.5C14 12.8807 12.8807 14 11.5 14H4.5C3.11929 14 2 12.8807 2 11.5V4.5ZM4.5 3C3.67157 3 3 3.67157 3 4.5V11.5C3 12.3284 3.67157 13 4.5 13H11.5C12.3284 13 13 12.3284 13 11.5V4.5C13 3.67157 12.3284 3 11.5 3H4.5Z)svg" };
	case Icon::ChromeRestore:
		return { 16, false, R"svg(M5.08496 4C5.29088 3.4174 5.8465 3 6.49961 3H9.99961C11.6565 3 12.9996 4.34315 12.9996 6V9.5C12.9996 10.1531 12.5822 10.7087 11.9996 10.9146V6C11.9996 4.89543 11.1042 4 9.99961 4H5.08496ZM4.5 5H9.5C10.3284 5 11 5.67157 11 6.5V11.5C11 12.3284 10.3284 13 9.5 13H4.5C3.67157 13 3 12.3284 3 11.5V6.5C3 5.67157 3.67157 5 4.5 5ZM4.5 6C4.22386 6 4 6.22386 4 6.5V11.5C4 11.7761 4.22386 12 4.5 12H9.5C9.77614 12 10 11.7761 10 11.5V6.5C10 6.22386 9.77614 6 9.5 6H4.5Z)svg" };
	case Icon::ChromeClose:
		return { 16, false, R"svg(M2.58859 2.71569L2.64645 2.64645C2.82001 2.47288 3.08944 2.4536 3.28431 2.58859L3.35355 2.64645L8 7.293L12.6464 2.64645C12.8417 2.45118 13.1583 2.45118 13.3536 2.64645C13.5488 2.84171 13.5488 3.15829 13.3536 3.35355L8.707 8L13.3536 12.6464C13.5271 12.82 13.5464 13.0894 13.4114 13.2843L13.3536 13.3536C13.18 13.5271 12.9106 13.5464 12.7157 13.4114L12.6464 13.3536L8 8.707L3.35355 13.3536C3.15829 13.5488 2.84171 13.5488 2.64645 13.3536C2.45118 13.1583 2.45118 12.8417 2.64645 12.6464L7.293 8L2.64645 3.35355C2.47288 3.17999 2.4536 2.91056 2.58859 2.71569L2.64645 2.64645L2.58859 2.71569Z)svg" };
	case Icon::GitBranch:
		return { 16, false, R"svg(M14 5.5C14 4.121 12.879 3 11.5 3C10.121 3 9 4.121 9 5.5C9 6.682 9.826 7.669 10.93 7.928C10.744 8.546 10.177 9 9.5 9H6.5C5.935 9 5.419 9.195 5 9.512V4.949C6.14 4.717 7 3.707 7 2.5C7 1.121 5.879 0 4.5 0C3.121 0 2 1.121 2 2.5C2 3.708 2.86 4.717 4 4.949V11.05C2.86 11.282 2 12.292 2 13.499C2 14.878 3.121 15.999 4.5 15.999C5.879 15.999 7 14.878 7 13.499C7 12.317 6.174 11.33 5.07 11.071C5.256 10.453 5.823 9.999 6.5 9.999H9.5C10.723 9.999 11.74 9.115 11.954 7.953C13.116 7.738 14 6.723 14 5.5ZM3 2.5C3 1.673 3.673 1 4.5 1C5.327 1 6 1.673 6 2.5C6 3.327 5.327 4 4.5 4C3.673 4 3 3.327 3 2.5ZM6 13.5C6 14.327 5.327 15 4.5 15C3.673 15 3 14.327 3 13.5C3 12.673 3.673 12 4.5 12C5.327 12 6 12.673 6 13.5ZM11.5 7C10.673 7 10 6.327 10 5.5C10 4.673 10.673 4 11.5 4C12.327 4 13 4.673 13 5.5C13 6.327 12.327 7 11.5 7Z)svg" };
	case Icon::Target:
		return { 16, false, R"svg(M9 8C9 8.552 8.552 9 8 9C7.448 9 7 8.552 7 8C7 7.448 7.448 7 8 7C8.552 7 9 7.448 9 8ZM12 8C12 10.209 10.209 12 8 12C5.791 12 4 10.209 4 8C4 5.791 5.791 4 8 4C10.209 4 12 5.791 12 8ZM11 8C11 6.343 9.657 5 8 5C6.343 5 5 6.343 5 8C5 9.657 6.343 11 8 11C9.657 11 11 9.657 11 8ZM15 8C15 11.866 11.866 15 8 15C4.134 15 1 11.866 1 8C1 4.134 4.134 1 8 1C11.866 1 15 4.134 15 8ZM14 8C14 4.686 11.314 2 8 2C4.686 2 2 4.686 2 8C2 11.314 4.686 14 8 14C11.314 14 14 11.314 14 8Z)svg" };
	case Icon::Newline:
		return { 16, false, R"svg(M14 3.49999V6.49999C14 7.87899 12.879 8.99999 11.5 8.99999H3.70703L6.35303 11.646C6.54803 11.841 6.54803 12.158 6.35303 12.353C6.25503 12.451 6.12703 12.499 5.99903 12.499C5.87103 12.499 5.74303 12.45 5.64503 12.353L2.14503 8.85299C1.95003 8.65799 1.95003 8.34099 2.14503 8.14599L5.64503 4.64599C5.84003 4.45099 6.15703 4.45099 6.35203 4.64599C6.54703 4.84099 6.54703 5.15799 6.35203 5.35299L3.70603 7.99899H11.499C12.326 7.99899 12.999 7.32599 12.999 6.49899V3.49899C12.999 3.22299 13.223 2.99899 13.499 2.99899C13.775 2.99899 13.999 3.22299 13.999 3.49899L14 3.49999Z)svg" };
	case Icon::Code:
		return { 16, false, R"svg(M9.80307 3.0431C10.0554 3.15525 10.1691 3.45073 10.0569 3.70307L6.05691 12.7031C5.94475 12.9554 5.64927 13.0691 5.39693 12.9569C5.14459 12.8448 5.03094 12.5493 5.14309 12.2969L9.14309 3.29693C9.25525 3.04459 9.55073 2.93094 9.80307 3.0431ZM4.33218 5.3763C4.53857 5.55976 4.55716 5.87579 4.3737 6.08218L2.66898 8L4.3737 9.91782C4.55716 10.1242 4.53857 10.4402 4.33218 10.6237C4.12579 10.8072 3.80975 10.7886 3.6263 10.5822L1.6263 8.33218C1.4579 8.14274 1.4579 7.85726 1.6263 7.66782L3.6263 5.41782C3.80975 5.21143 4.12579 5.19284 4.33218 5.3763ZM11.6678 5.3763C11.8742 5.19284 12.1902 5.21143 12.3737 5.41782L14.3737 7.66782C14.5421 7.85726 14.5421 8.14274 14.3737 8.33218L12.3737 10.5822C12.1902 10.7886 11.8742 10.8072 11.6678 10.6237C11.4614 10.4402 11.4428 10.1242 11.6263 9.91782L13.331 8L11.6263 6.08218C11.4428 5.87579 11.4614 5.55976 11.6678 5.3763Z)svg" };
	case Icon::FileBinary:
		return { 16, false, R"svg(M5 1C3.895 1 3 1.895 3 3V13C3 14.105 3.895 15 5 15H11C12.105 15 13 14.105 13 13V5.414C13 5.016 12.842 4.635 12.561 4.353L9.647 1.439C9.366 1.158 8.984 1 8.586 1H5ZM4 3C4 2.448 4.448 2 5 2H8V4.5C8 5.328 8.672 6 9.5 6H12V13C12 13.552 11.552 14 11 14H5C4.448 14 4 13.552 4 13V3ZM11.793 5H9.5C9.224 5 9 4.776 9 4.5V2.207L11.793 5ZM10.801 8.204C10.901 8.298 11 8.451 11 8.655V12.501C11 12.777 10.776 13.001 10.5 13.001C10.224 13.001 10 12.777 10 12.501V9.459C9.781 9.641 9.519 9.814 9.207 9.956C8.956 10.07 8.659 9.959 8.545 9.708C8.431 9.457 8.542 9.16 8.793 9.046C9.26 8.833 9.573 8.522 9.77 8.262C9.993 7.967 10.343 7.996 10.495 8.037C10.593 8.064 10.705 8.114 10.801 8.204ZM6.5 8C5.672 8 5 8.672 5 9.5V11.5C5 12.328 5.672 13 6.5 13C7.328 13 8 12.328 8 11.5V9.5C8 8.672 7.328 8 6.5 8ZM6 9.5C6 9.224 6.224 9 6.5 9C6.776 9 7 9.224 7 9.5V11.5C7 11.776 6.776 12 6.5 12C6.224 12 6 11.776 6 11.5V9.5Z)svg" };
	case Icon::RecordSmall:
		return { 16, false, R"svg(M8 8.99988C8.55228 8.99988 9 8.55216 9 7.99988C9 7.44759 8.55228 6.99988 8 6.99988C7.44772 6.99988 7 7.44759 7 7.99988C7 8.55216 7.44772 8.99988 8 8.99988ZM12 7.99988C12 10.209 10.2091 11.9999 8 11.9999C5.79086 11.9999 4 10.209 4 7.99988C4 5.79074 5.79086 3.99988 8 3.99988C10.2091 3.99988 12 5.79074 12 7.99988ZM11 7.99988C11 6.34302 9.65685 4.99988 8 4.99988C6.34315 4.99988 5 6.34302 5 7.99988C5 9.65673 6.34315 10.9999 8 10.9999C9.65685 10.9999 11 9.65673 11 7.99988Z)svg" };
	case Icon::Insert:
		return { 16, true, R"svg(M11 6H14C14.551 6 15 5.551 15 5V2C15 1.449 14.551 1 14 1H11C10.449 1 10 1.449 10 2V5C10 5.551 10.449 6 11 6ZM11 5V2H14V5H11ZM11 14H14C14.551 14 15 13.551 15 13V10C15 9.449 14.551 9 14 9H11C10.449 9 10 9.449 10 10V13C10 13.551 10.449 14 11 14ZM11 13V10H14V13H11ZM7.854 5.14602L9.854 7.14602L9.855 7.14502C10.05 7.34002 10.05 7.65702 9.855 7.85202L7.855 9.85202C7.757 9.94902 7.629 9.99802 7.501 9.99802C7.373 9.99802 7.245 9.95002 7.147 9.85202C6.952 9.65702 6.952 9.34002 7.147 9.14502L8.293 7.99902H4V8.99902C4 9.55002 3.551 9.99902 3 9.99902H1C0.724 9.99902 0.5 9.77502 0.5 9.49902C0.5 9.22302 0.724 8.99902 1 8.99902H3V5.99902H1C0.724 5.99902 0.5 5.77502 0.5 5.49902C0.5 5.22302 0.724 4.99902 1 4.99902H3C3.551 4.99902 4 5.44802 4 5.99902V6.99902H8.293L7.147 5.85302C6.952 5.65802 6.952 5.34102 7.147 5.14602C7.342 4.95102 7.659 4.95102 7.854 5.14602Z)svg" };
	case Icon::ZoomIn:
		return { 16, false, R"svg(M6.5 4C6.77614 4 7 4.22386 7 4.5V6H8.5C8.77614 6 9 6.22386 9 6.5C9 6.77614 8.77614 7 8.5 7H7V8.5C7 8.77614 6.77614 9 6.5 9C6.22386 9 6 8.77614 6 8.5V7H4.5C4.22386 7 4 6.77614 4 6.5C4 6.22386 4.22386 6 4.5 6H6V4.5C6 4.22386 6.22386 4 6.5 4ZM6.5 1C9.53757 1 12 3.46243 12 6.5C12 7.83875 11.5216 9.06578 10.7266 10.0195L13.8535 13.1465C14.0488 13.3417 14.0488 13.6583 13.8535 13.8535C13.6583 14.0488 13.3417 14.0488 13.1465 13.8535L10.0195 10.7266C9.06578 11.5216 7.83875 12 6.5 12C3.46243 12 1 9.53757 1 6.5C1 3.46243 3.46243 1 6.5 1ZM6.5 2C4.01472 2 2 4.01472 2 6.5C2 8.98528 4.01472 11 6.5 11C8.98528 11 11 8.98528 11 6.5C11 4.01472 8.98528 2 6.5 2Z)svg" };
	case Icon::File:
		return { 16, false, R"svg(M5 1C3.89543 1 3 1.89543 3 3V13C3 14.1046 3.89543 15 5 15H11C12.1046 15 13 14.1046 13 13V5.41421C13 5.01639 12.842 4.63486 12.5607 4.35355L9.64645 1.43934C9.36514 1.15804 8.98361 1 8.58579 1H5ZM4 3C4 2.44772 4.44772 2 5 2H8V4.5C8 5.32843 8.67157 6 9.5 6H12V13C12 13.5523 11.5523 14 11 14H5C4.44772 14 4 13.5523 4 13V3ZM11.7929 5H9.5C9.22386 5 9 4.77614 9 4.5V2.20711L11.7929 5Z)svg" };
	case Icon::OpenPreview:
		return { 16, false, R"svg(M13.5 1H4.5C3.122 1 2 2.122 2 3.5V6.276C2.319 6.162 2.653 6.089 3 6.05V3.499C3 2.672 3.673 1.999 4.5 1.999H8.5V13.385L9.557 14.442C9.714 14.591 9.831 14.786 9.907 14.999H13.5C14.878 14.999 16 13.877 16 12.499V3.5C16 2.122 14.878 1 13.5 1ZM15 12.5C15 13.327 14.327 14 13.5 14H9.5V2H13.5C14.327 2 15 2.673 15 3.5V12.5ZM6.29 12.59C6.74 12.01 7 11.28 7 10.5C7 8.57 5.43 7 3.5 7C1.57 7 0 8.57 0 10.5C0 12.43 1.57 14 3.5 14C4.28 14 5.01 13.74 5.59 13.29L8.15 15.85C8.24 15.95 8.37 16 8.5 16C8.63 16 8.76 15.95 8.85 15.85C9.05 15.66 9.05 15.34 8.85 15.15L6.29 12.59ZM5.5 12C5.36 12.19 5.19 12.36 5 12.5C4.59 12.81 4.06 13 3.5 13C2.12 13 1 11.88 1 10.5C1 9.12 2.12 8 3.5 8C4.88 8 6 9.12 6 10.5C6 11.06 5.81 11.59 5.5 12Z)svg" };
	case Icon::ChevronDown:
		return { 16, false, R"svg(M3.14598 5.85423L7.64598 10.3542C7.84098 10.5492 8.15798 10.5492 8.35298 10.3542L12.853 5.85423C13.048 5.65923 13.048 5.34223 12.853 5.14723C12.658 4.95223 12.341 4.95223 12.146 5.14723L7.99998 9.29323L3.85398 5.14723C3.65898 4.95223 3.34198 4.95223 3.14698 5.14723C2.95198 5.34223 2.95098 5.65923 3.14598 5.85423Z)svg" };
	case Icon::Close:
		return { 16, false, R"svg(M13.85 13.1502C14.05 13.3502 14.05 13.6602 13.85 13.8602C13.75 13.9602 13.62 14.0102 13.5 14.0102C13.38 14.0102 13.24 13.9602 13.15 13.8602L8 8.71023L2.85 13.8602C2.75 13.9602 2.62 14.0102 2.5 14.0102C2.38 14.0102 2.24 13.9602 2.15 13.8602C1.95 13.6602 1.95 13.3502 2.15 13.1502L7.3 8.00023L2.15 2.85023C1.95 2.65023 1.95 2.34023 2.15 2.14023C2.35 1.94023 2.66 1.94023 2.86 2.14023L8.01 7.29023L13.16 2.14023C13.36 1.94023 13.67 1.94023 13.87 2.14023C14.07 2.34023 14.07 2.65023 13.87 2.85023L8.72 8.00023L13.87 13.1502H13.85Z)svg" };
	case Icon::CloseAll:
		return { 16, false, R"svg(M15 6V11C15 13.21 13.21 15 11 15H6C5.26 15 4.62 14.6 4.27 14H11C12.65 14 14 12.65 14 11V4.27C14.6 4.62 15 5.26 15 6ZM11 13H4C2.897 13 2 12.103 2 11V4C2 2.897 2.897 2 4 2H11C12.103 2 13 2.897 13 4V11C13 12.103 12.103 13 11 13ZM4 12H11C11.552 12 12 11.552 12 11V4C12 3.449 11.552 3 11 3H4C3.448 3 3 3.449 3 4V11C3 11.552 3.448 12 4 12ZM9.854 5.146C9.659 4.951 9.342 4.951 9.147 5.146L7.501 6.792L5.855 5.146C5.66 4.951 5.343 4.951 5.148 5.146C4.953 5.341 4.953 5.658 5.148 5.853L6.794 7.499L5.148 9.145C4.953 9.34 4.953 9.657 5.148 9.852C5.246 9.95 5.374 9.998 5.502 9.998C5.63 9.998 5.758 9.949 5.856 9.852L7.502 8.206L9.148 9.852C9.246 9.95 9.374 9.998 9.502 9.998C9.63 9.998 9.758 9.949 9.856 9.852C10.051 9.657 10.051 9.34 9.856 9.145L8.21 7.499L9.856 5.853C10.051 5.658 10.051 5.341 9.856 5.146H9.854Z)svg" };
	case Icon::Loading:
		return { 16, false, R"svg(M13.5 8.5C13.224 8.5 13 8.276 13 8C13 5.243 10.757 3 8 3C5.243 3 3 5.243 3 8C3 8.276 2.776 8.5 2.5 8.5C2.224 8.5 2 8.276 2 8C2 4.691 4.691 2 8 2C11.309 2 14 4.691 14 8C14 8.276 13.776 8.5 13.5 8.5Z)svg" };
	}
	return {};
}

[[nodiscard]] inline bool DrawSvgIcon(HDC dc, const IconRect& box, Icon icon, COLORREF color) noexcept
{
	if (dc == nullptr || box.Width() <= 0 || box.Height() <= 0) return false;
	const SvgIconData data = DataFor(icon);
	if (data.path.empty() || data.viewBox <= 0) return false;
	const int saved = ::SaveDC(dc);
	if (saved == 0) return false;
	if (::SetGraphicsMode(dc, GM_ADVANCED) == 0) {
		::RestoreDC(dc, saved);
		return false;
	}
	::SetPolyFillMode(dc, data.evenOdd ? ALTERNATE : WINDING);
	const int logicalExtent = data.viewBox * kCoordinateScale;
	const IconRect viewport = SvgIconLetterboxBounds(box);
	const XFORM transform{
		static_cast<float>(viewport.Width()) / logicalExtent, 0.0F,
		0.0F, static_cast<float>(viewport.Height()) / logicalExtent,
		static_cast<float>(viewport.left), static_cast<float>(viewport.top),
	};
	if (::SetWorldTransform(dc, &transform) == FALSE || ::BeginPath(dc) == FALSE
		|| !AppendSvgPath(dc, data.path) || ::EndPath(dc) == FALSE) {
		::RestoreDC(dc, saved);
		return false;
	}
	const HBRUSH brush = ::CreateSolidBrush(color);
	if (brush == nullptr) {
		::RestoreDC(dc, saved);
		return false;
	}
	const HGDIOBJ oldBrush = ::SelectObject(dc, brush);
	const bool drawn = ::FillPath(dc) != FALSE;
	::SelectObject(dc, oldBrush);
	::DeleteObject(brush);
	::RestoreDC(dc, saved);
	return drawn;
}

[[nodiscard]] inline bool BeginIconPath(HDC dc, const IconRect& box) noexcept
{
	if (dc == nullptr || box.Width() <= 0 || box.Height() <= 0) return false;
	if (::SetGraphicsMode(dc, GM_ADVANCED) == 0) return false;
	::SetPolyFillMode(dc, WINDING); // SVG defaults to the nonzero fill rule.
	const XFORM transform{
		static_cast<float>(box.Width()) / kLogicalExtent, 0.0F,
		0.0F, static_cast<float>(box.Height()) / kLogicalExtent,
		static_cast<float>(box.left), static_cast<float>(box.top),
	};
	return ::SetWorldTransform(dc, &transform) != FALSE && ::BeginPath(dc) != FALSE;
}

inline void MoveTo(HDC dc, int x, int y) noexcept { ::MoveToEx(dc, x, y, nullptr); }
inline void PathLineTo(HDC dc, int x, int y) noexcept { ::LineTo(dc, x, y); }
inline void CubicTo(HDC dc, int x1, int y1, int x2, int y2, int x3, int y3) noexcept
{
	const POINT points[] = { { x1, y1 }, { x2, y2 }, { x3, y3 } };
	::PolyBezierTo(dc, points, 3);
}

} // namespace detail

//! Draws an exact imported Codicon path into a caller-provided optical square.
inline void Draw(HDC dc, const IconRect& box, Icon icon, COLORREF color) noexcept
{
	static_cast<void>(detail::DrawSvgIcon(dc, box, icon, color));
}

//! Exact geometry adapted from vscode-codicons `files` (24x24 SVG path).
inline void DrawFiles(HDC dc, const IconRect& box, COLORREF color) noexcept
{
	const int saved = ::SaveDC(dc);
	if (saved == 0) return;
	if (!detail::BeginIconPath(dc, box)) {
		::RestoreDC(dc, saved);
		return;
	}
	using namespace detail;
	MoveTo(dc, 15000, 45000);
	PathLineTo(dc, 35190, 45000);
	CubicTo(dc, 34140, 46800, 32220, 48000, 30000, 48000);
	PathLineTo(dc, 15000, 48000);
	CubicTo(dc, 8370, 48000, 3000, 42630, 3000, 36000);
	PathLineTo(dc, 3000, 12000);
	CubicTo(dc, 3000, 9780, 4200, 7860, 6000, 6810);
	PathLineTo(dc, 6000, 36000);
	CubicTo(dc, 6000, 40950, 10050, 45000, 15000, 45000);
	::CloseFigure(dc);
	MoveTo(dc, 42000, 16242);
	PathLineTo(dc, 42000, 36000);
	CubicTo(dc, 42000, 39309, 39309, 42000, 36000, 42000);
	PathLineTo(dc, 15000, 42000);
	CubicTo(dc, 11691, 42000, 9000, 39309, 9000, 36000);
	PathLineTo(dc, 9000, 6000);
	CubicTo(dc, 9000, 2691, 11691, 0, 15000, 0);
	PathLineTo(dc, 25758, 0);
	CubicTo(dc, 26943, 0, 28101, 480, 28941, 1317);
	PathLineTo(dc, 40683, 13059);
	CubicTo(dc, 41532, 13908, 42000, 15039, 42000, 16242);
	::CloseFigure(dc);
	MoveTo(dc, 27000, 13500);
	CubicTo(dc, 27000, 14328, 27675, 15000, 28500, 15000);
	PathLineTo(dc, 38379, 15000);
	PathLineTo(dc, 27000, 3621);
	PathLineTo(dc, 27000, 13500);
	::CloseFigure(dc);
	MoveTo(dc, 39000, 36000);
	PathLineTo(dc, 39000, 18000);
	PathLineTo(dc, 28500, 18000);
	CubicTo(dc, 26019, 18000, 24000, 15981, 24000, 13500);
	PathLineTo(dc, 24000, 3000);
	PathLineTo(dc, 15000, 3000);
	CubicTo(dc, 13344, 3000, 12000, 4347, 12000, 6000);
	PathLineTo(dc, 12000, 36000);
	CubicTo(dc, 12000, 37653, 13344, 39000, 15000, 39000);
	PathLineTo(dc, 36000, 39000);
	CubicTo(dc, 37656, 39000, 39000, 37653, 39000, 36000);
	::CloseFigure(dc);
	::EndPath(dc);
	const HBRUSH brush = ::CreateSolidBrush(color);
	if (brush != nullptr) {
		const HGDIOBJ oldBrush = ::SelectObject(dc, brush);
		::FillPath(dc);
		::SelectObject(dc, oldBrush);
		::DeleteObject(brush);
	}
	::RestoreDC(dc, saved);
}

//! Exact geometry adapted from vscode-codicons `source-control` (24x24 SVG path).
inline void DrawSourceControl(HDC dc, const IconRect& box, COLORREF color) noexcept
{
	const int saved = ::SaveDC(dc);
	if (saved == 0) return;
	if (!detail::BeginIconPath(dc, box)) {
		::RestoreDC(dc, saved);
		return;
	}
	using namespace detail;
	MoveTo(dc, 42000, 16500);
	CubicTo(dc, 42000, 12363, 38637, 9000, 34500, 9000);
	CubicTo(dc, 30363, 9000, 27000, 12363, 27000, 16500);
	CubicTo(dc, 27000, 20046, 29478, 23007, 32790, 23784);
	CubicTo(dc, 32232, 25638, 30531, 27000, 28500, 27000);
	PathLineTo(dc, 19500, 27000);
	CubicTo(dc, 17805, 27000, 16257, 27585, 15000, 28536);
	PathLineTo(dc, 15000, 14847);
	CubicTo(dc, 18420, 14150, 21000, 11121, 21000, 7500);
	CubicTo(dc, 21000, 3363, 17637, 0, 13500, 0);
	CubicTo(dc, 9363, 0, 6000, 3363, 6000, 7500);
	CubicTo(dc, 6000, 11124, 8580, 14150, 12000, 14847);
	PathLineTo(dc, 12000, 33150);
	CubicTo(dc, 8580, 33846, 6000, 36876, 6000, 40497);
	CubicTo(dc, 6000, 44634, 9363, 47997, 13500, 47997);
	CubicTo(dc, 17637, 47997, 21000, 44634, 21000, 40497);
	CubicTo(dc, 21000, 36951, 18522, 33991, 15210, 33213);
	CubicTo(dc, 15768, 31358, 17469, 29997, 19500, 29997);
	PathLineTo(dc, 28500, 29997);
	CubicTo(dc, 32169, 29997, 35220, 27345, 35862, 23858);
	CubicTo(dc, 39348, 23214, 42000, 20168, 42000, 16500);
	::CloseFigure(dc);
	MoveTo(dc, 9000, 7500);
	CubicTo(dc, 9000, 5019, 11019, 3000, 13500, 3000);
	CubicTo(dc, 15981, 3000, 18000, 5019, 18000, 7500);
	CubicTo(dc, 18000, 9981, 15981, 12000, 13500, 12000);
	CubicTo(dc, 11019, 12000, 9000, 9981, 9000, 7500);
	::CloseFigure(dc);
	MoveTo(dc, 18000, 40500);
	CubicTo(dc, 18000, 42981, 15981, 45000, 13500, 45000);
	CubicTo(dc, 11019, 45000, 9000, 42981, 9000, 40500);
	CubicTo(dc, 9000, 38019, 11019, 36000, 13500, 36000);
	CubicTo(dc, 15981, 36000, 18000, 38019, 18000, 40500);
	::CloseFigure(dc);
	MoveTo(dc, 34500, 21000);
	CubicTo(dc, 32019, 21000, 30000, 18981, 30000, 16500);
	CubicTo(dc, 30000, 14019, 32019, 12000, 34500, 12000);
	CubicTo(dc, 36981, 12000, 39000, 14019, 39000, 16500);
	CubicTo(dc, 39000, 18981, 36981, 21000, 34500, 21000);
	::CloseFigure(dc);
	::EndPath(dc);
	const HBRUSH brush = ::CreateSolidBrush(color);
	if (brush != nullptr) {
		const HGDIOBJ oldBrush = ::SelectObject(dc, brush);
		::FillPath(dc);
		::SelectObject(dc, oldBrush);
		::DeleteObject(brush);
	}
	::RestoreDC(dc, saved);
}

} // namespace workbench::icons::codicons
