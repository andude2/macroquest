/*
 * MacroQuest: The extension platform for EverQuest
 * Copyright (C) 2002-present MacroQuest Authors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#pragma once

#include "mq/base/String.h"

#include <TlHelp32.h>
#include <Psapi.h>

#include <wil/resource.h>

namespace mq {

inline bool IsProcessRunning(std::string_view exeName)
{
	wil::unique_tool_help_snapshot hSnapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));

	PROCESSENTRY32 proc = { sizeof(PROCESSENTRY32) };
	if (Process32First(hSnapshot.get(), &proc))
	{
		do
		{
			if (ci_equals(proc.szExeFile, exeName))
			{
				return true;
			}
		} while (Process32Next(hSnapshot.get(), &proc));
	}

	return false;
}

inline bool IsInModuleList(const char* moduleName, int processID = GetCurrentProcessId())
{
	const wil::unique_process_handle process(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, false, processID));
	if (process.is_valid())
	{
		HMODULE hMods[1024];
		DWORD cbNeeded;
		if(EnumProcessModules(process.get(), hMods, sizeof(hMods), &cbNeeded))
		{
			for (DWORD i = 0; i < (cbNeeded / sizeof(HMODULE)); ++i)
			{
				char szModName[MAX_PATH];
				if (GetModuleFileNameEx(process.get(), hMods[i], szModName, sizeof(szModName) / sizeof(char)))
				{
					const std::filesystem::path mod_path = szModName;
					if (ci_equals(mod_path.filename().string(), moduleName) || ci_equals(mod_path.stem().string(), moduleName))
					{
						return true;
					}
				}
			}
		}
	}
	return false;
}

/**
 * @fn BringWindowToForeground
 *
 * @brief Brings a window to the foreground using the AttachThreadInput technique
 *
 * Attaches the calling thread's input queue to both the current foreground
 * thread and the target window's thread so that SetForegroundWindow, SetFocus,
 * and SetActiveWindow take effect across processes. The caller must have
 * foreground privilege (be the foreground process, or have been granted it via
 * AllowSetForegroundWindow). The order of operations matters due to how Windows
 * activates the window. The below order is meant to minimize ghost windows
 * and situations where the window appears active and is functioanlly active
 * but is not in the correct MRU order or doesn't have the active title bar.
 *
 * Beware: Here there be dragons.
 *
 * @param hWnd The window to bring to the foreground
 **/
inline void BringWindowToForeground(HWND hWnd)
{
	// Attempt to restore the window before the activation dance
	if (IsIconic(hWnd))
	{
		ShowWindow(hWnd, SW_RESTORE);
	}

	const DWORD ourThread = ::GetCurrentThreadId();
	const DWORD fgThread = ::GetWindowThreadProcessId(::GetForegroundWindow(), nullptr);
	const DWORD tgtThread = ::GetWindowThreadProcessId(hWnd, nullptr);
	const bool attachedFg = fgThread && fgThread != ourThread && ::AttachThreadInput(ourThread, fgThread, true);
	const bool attachedTgt = tgtThread && tgtThread != ourThread && tgtThread != fgThread && ::AttachThreadInput(ourThread, tgtThread, true);
	auto detach = wil::scope_exit([&]
		{
			if (attachedTgt)
			{
				::AttachThreadInput(ourThread, tgtThread, false);
			}
			if (attachedFg)
			{
				::AttachThreadInput(ourThread, fgThread, false);
			}
		});

	::SetForegroundWindow(hWnd);
	::BringWindowToTop(hWnd);

	// Re-check: the first restore may fail before the activation dance.
	if (IsIconic(hWnd))
	{
		ShowWindow(hWnd, SW_RESTORE);
	}

	::SetFocus(hWnd);
	::SetActiveWindow(hWnd);
}

} // namespace mq