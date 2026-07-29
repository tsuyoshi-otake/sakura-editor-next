# Language Resource DLL Guidance

## Scope

This directory contains the resource DLL projects for English (`en-US`) and Simplified Chinese (`zh-CN`). The Japanese resources are compiled into the main application from `sakura_core/sakura_rc.rc`.

## Resource Consistency

- Resource IDs come from `src/main/resources/sakura_rc.h`. Keep IDs aligned across the main resource and every language resource; do not create locale-specific numeric meanings.
- Keep each locale's `LANGUAGE` declaration and `STR_SELLANG_LANGID` value correct (`0x0409` for en-US and `0x0804` for zh-CN).
- When adding or removing a dialog control, menu item, string, icon, or accelerator, update all affected `.rc` / `.rc2` files and verify that translated layouts still fit.
- These projects are resource DLLs. Avoid introducing application behavior or shared mutable state here.

## Project Maintenance

- Keep `.vcxproj` and `.vcxproj.filters` entries synchronized.
- Use `sakura_lang/sakura_lang.sln` for focused MSVC language-DLL verification. Shared CMake language-DLL generation is defined from the root CMake build, so resource changes that affect both paths should verify both.
