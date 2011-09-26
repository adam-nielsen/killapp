/*
 * Killapp - Monitor a Windows program via a tray icon, and provide a
 *           right-click menu to forcefully terminate it.
 *
 * Copyright 2010 Adam Nielsen <adam.nielsen@uq.edu.au>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <windows.h>
#include <assert.h>
#include <stdarg.h>
#pragma hdrstop

// Message used by tray icon for notification
#define TRAY_MSG  WM_USER
#define IDC_KILL  100
#define IDC_EXIT  101

// Wait up to ten seconds for the app we're monitoring to start
#define WAIT_SECS  10

#define QUIT_EVENT_NAME  "KillAppQuitEvent"
#define WINDOW_CLASS     "KillAppMessageWindowClass"

// These are the PSAPI functions we will be using.  We load them dynamically
// as it's easier than trying to link in with psapi.lib
typedef BOOL (WINAPI *FN_ENUMPROC)(DWORD *, DWORD, DWORD *);
typedef DWORD (WINAPI *FN_GETPROCFN)(HANDLE, LPTSTR, DWORD);
FN_ENUMPROC EnumProcesses;
FN_GETPROCFN GetProcessImageFileName;

// Data to pass to the monitoring thread
typedef struct {
	HANDLE hApp;        // Handle to the app we are monitoring
	HWND hWndNotify;    // Send WM_CLOSE here when monitored app exits
	HANDLE hQuitEvent;  // Thread will exit when this event is signalled
} THREAD_DATA;

HANDLE hApp; // Copy for terminateProcess()

// Find a running process where the full .exe filename (including path) ends
// with the given value.  This means the value can include a partial path,
// e.g. "folder\\test.exe".  Avoid using "test.exe" with no path at all, as
// this will match "mytest.exe" and "yourtest.exe", instead use "\\test.exe"
// as this will match only test.exe loaded from any path.
// Note that you cannot use drive letters as the full name being matched is of
// the form: \Device\HarddiskVolume1\WINDOWS\explorer.exe
// The return value is NULL if the process cannot be found, or a handle which
// you must remember to close with CloseHandle().
HANDLE lookupProcess(const char *targetName)
{
	DWORD processIds[5000];
	DWORD len = 0;
	EnumProcesses(processIds, sizeof(processIds), &len);

	int lenTargetName = strlen(targetName);
	assert(lenTargetName > 0);

	DWORD numProcesses = len / sizeof(DWORD);
	char procName[512];
	for (DWORD i = 0; i < numProcesses; i++) {
		HANDLE hProc = OpenProcess(
			PROCESS_QUERY_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE,
			FALSE,
			processIds[i]
		);
		if (hProc == NULL) continue; // Access denied == not our process
		DWORD lenData = GetProcessImageFileName(hProc, procName, sizeof(procName));
		if (lenData > 0) {
			int lenProcName = strlen(procName); // buffer contains other data too
			if (lenProcName > lenTargetName) {
				// See if targetName matches the end of the process name.
				if (strcmpi(targetName, &procName[lenProcName - lenTargetName]) == 0) {
					return hProc;
				}
			}
		}
		CloseHandle(hProc);
	}
	return NULL;
}

// This function is called by the window procedure when the time comes to
// forcefully terminate the process we're monitoring.
void terminateProcess(void)
{
	TerminateProcess(::hApp, 99);
}

// Call GetLastError() and show the message to the user.  The function can be
// called like printf, except each percent sign must be doubled ("%%s") as
// it goes through FormatMessage() first.  A "%0" (no double-percent) will be
// replaced with a description of the last error, as returned by GetLastError()
void showSystemError(const char *message, ...)
{
	// Get the system message
	char *sysMsg;
	if (FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
		NULL, GetLastError(), 0, (LPTSTR) &sysMsg, 0, NULL
	) == 0) {
		MessageBox(NULL, "There was an error, and it was not possible to get a "
			"description of the error.", "Error", MB_OK | MB_ICONERROR);
		return;
	}

	// Replace %1 in the user message with the system one
	char *buffer;
	if (FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_STRING |
		FORMAT_MESSAGE_ARGUMENT_ARRAY,
		message, 0, 0, (LPTSTR) &buffer, 0,
		&sysMsg
	) == 0) {
		MessageBox(NULL, sysMsg, "Error (couldn't get accurate error description)", MB_OK | MB_ICONERROR);
		return;
	}

	LocalFree(sysMsg);

	// Run it all through sprintf
	va_list argptr;
	va_start(argptr, message);
	char finalMessage[512];
	wvsprintf(finalMessage, buffer, argptr);
	va_end(argptr);

	LocalFree(buffer);

	MessageBox(NULL, finalMessage, "Error", MB_OK | MB_ICONERROR);

	return;
}

DWORD WINAPI waitThread(LPVOID data)
{
	THREAD_DATA *threadData = (THREAD_DATA *)data;
	HANDLE handles[] = {threadData->hApp, threadData->hQuitEvent};
	DWORD result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
	if (result == WAIT_FAILED) {
		showSystemError("Wait for app termination failed: %1");
	}
	// result will == WAIT_OBJECT_0 when the app has terminated

	// Make us exit now there's no app to monitor
	PostMessage(threadData->hWndNotify, WM_CLOSE, 0, 0);
	return 0;
}

LRESULT CALLBACK windowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg) {
		case WM_COMMAND:
			switch (wParam) {
				case IDC_KILL:
					if (MessageBox(hWnd, "Are you sure you want to terminate the application?", "Confirm", MB_YESNO | MB_ICONQUESTION) == IDYES) {
						terminateProcess();
					}
					break;
				case IDC_EXIT:
					if (MessageBox(hWnd, "Are you sure you want to remove this icon?  You will no longer be able to use it to terminate the application.", "Confirm", MB_YESNO | MB_ICONQUESTION) == IDYES) {
						DestroyWindow(hWnd);
						//PostQuitMessage(0); // Doesn't work under Windows Server 2008, does work under Win7
					}
					break;
			}
			break;
		case TRAY_MSG:
			switch (lParam) {
				case WM_RBUTTONDOWN: {
					HMENU popupMenu = CreatePopupMenu();
					AppendMenu(popupMenu, MF_ENABLED, IDC_KILL, "Terminate application");
					AppendMenu(popupMenu, MF_ENABLED, IDC_EXIT, "Remove this icon");
					POINT mp;
					GetCursorPos(&mp);
					SetForegroundWindow(hWnd);
					TrackPopupMenuEx(popupMenu, 0, mp.x, mp.y, hWnd, NULL);
					PostMessage(hWnd, WM_NULL, 0, 0);
					DestroyMenu(popupMenu);
					break;
				}
			}
			break;
	}
	return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR cmdLine, int)
{
	// Parse command line options
	char *targetExe = strtok(cmdLine, " ");
	char *trayIconFile = strtok(NULL, " ");
	if (!targetExe || !trayIconFile) {
		MessageBox(NULL, "KillApp: Monitor a Windows program via a tray icon,\n"
			"and provide a right-click menu to forcefully terminate it.\n"
			"http://github.com/adam-nielsen/killapp\n\n"
			"Usage: killapp target.exe trayicon.ico",
			"Error", MB_OK | MB_ICONERROR);
		return 1;
	}

	// Load PSAPI.DLL and get the functions we need
	HMODULE hPS = LoadLibrary("PSAPI.DLL");
	if (hPS == NULL) {
		showSystemError("Unable to load PSAPI.DLL: %1");
		return 1;
	}
	EnumProcesses = (FN_ENUMPROC)GetProcAddress(hPS, "EnumProcesses");
	if (EnumProcesses == NULL) {
		showSystemError("EnumProcesses() not found in PSAPI.DLL: %1");
		return 1;
	}
	GetProcessImageFileName = (FN_GETPROCFN)GetProcAddress(hPS, "GetProcessImageFileNameA");
	if (GetProcessImageFileName == NULL) {
		showSystemError("GetProcessImageFileName() not found in PSAPI.DLL: %1");
		return 1;
	}

	// Load the icon we'll be using in the system tray
	HICON hTrayIcon = (HICON)LoadImage(NULL, trayIconFile, IMAGE_ICON,
		GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
		LR_LOADFROMFILE);
	if (hTrayIcon == NULL) {
		showSystemError("Couldn't load icon file \"%%s\": %1", trayIconFile);
	}
	// Try for five seconds to find the process.  If it doesn't appear after this
	// time we assume it exited/crashed and exit ourselves.
	int waited = 0;
	while (((::hApp = lookupProcess(targetExe)) == NULL) && (waited < WAIT_SECS)) {
		Sleep(1000);
		waited++;
	}
	if (::hApp == NULL) return 2;

	// Create a window to receive messages from the tray icon
	WNDCLASS wc;
	wc.style = 0;
	wc.lpfnWndProc = windowProc;
	wc.cbClsExtra = 0;
	wc.cbWndExtra = 0;
	wc.hInstance = hInst;
	wc.hIcon = NULL;
	wc.hCursor = NULL;
	wc.hbrBackground = NULL;
	wc.lpszMenuName = NULL;
	wc.lpszClassName = WINDOW_CLASS;
	ATOM aWndClass = RegisterClass(&wc);
	if (aWndClass == 0) {
		showSystemError("Could not register window class: %1");
		return 1;
	}
	HWND hWndHidden = CreateWindowEx(
		WS_EX_TOOLWINDOW,
		WINDOW_CLASS,
		"KillApp", 0,
		CW_USEDEFAULT, 0,
		CW_USEDEFAULT, 0,
		NULL,
		NULL,
		hInst,
		0
	);

	// Create a thread to wait until the app exits
	HANDLE hQuitEvent = CreateEvent(NULL, TRUE, FALSE, QUIT_EVENT_NAME);
	if (hQuitEvent == NULL) {
		showSystemError("Could not create quit event: %1");
		return 3;
	}
	DWORD threadId;
	THREAD_DATA threadData;
	threadData.hApp = ::hApp;
	threadData.hWndNotify = hWndHidden;
	threadData.hQuitEvent = hQuitEvent;
	HANDLE hThread = CreateThread(NULL, 0, waitThread, (LPVOID)&threadData, 0, &threadId);
	if (hThread == NULL) {
		showSystemError("Cannot create monitoring thread: %1");
		return 3;
	}

	// Add a tray icon
	NOTIFYICONDATA trayIcon;
	trayIcon.cbSize = sizeof(NOTIFYICONDATA);
	trayIcon.hWnd = hWndHidden;
	trayIcon.uID = 0;
	trayIcon.uFlags = NIF_MESSAGE | NIF_ICON;
	trayIcon.uCallbackMessage = TRAY_MSG;
	trayIcon.hIcon = hTrayIcon;
	trayIcon.szTip[0] = 0;
	if (!Shell_NotifyIcon(NIM_ADD, &trayIcon)) {
		MessageBox(NULL, "Unable to add tray icon.", "Error", MB_OK | MB_ICONERROR);
		return 3;
	}

	MSG msg;
	while (GetMessage(&msg, hWndHidden, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	// Signal the quit event and wait for up to two seconds for the thread to
	// terminate.
	SignalObjectAndWait(hQuitEvent, hThread, 2000, FALSE);

	// Clean up
	CloseHandle(hThread);
	CloseHandle(hQuitEvent);
	Shell_NotifyIcon(NIM_DELETE, &trayIcon);
	DestroyWindow(hWndHidden);
	UnregisterClass(WINDOW_CLASS, hInst);
	if (hTrayIcon) DestroyIcon(hTrayIcon);
	CloseHandle(::hApp);
	return 0;
}
