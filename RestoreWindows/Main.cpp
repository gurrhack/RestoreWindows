//#define _CRT_SECURE_NO_WARNINGS

#define WIN32_LEAN_AND_MEAN
#define STRICT
#define NOMINMAX
//#define _WIN32_WINNT 0x0400

#include <windows.h>
#include <vector>
#include <map>
#include <set>
#include <io.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <strsafe.h>
#include <Shlobj.h>
#include <Shlwapi.h>
#include <algorithm>
#include <string>
#include <sstream>

// _____________________________________________________________________ //
//
// Data
// _____________________________________________________________________ //
static std::vector<RECT> DesiredMonitorPlacements;
static std::map<HWND, WINDOWPLACEMENT> WindowPlacements;
static std::map<HWND, WINDOWPLACEMENT> PendingUpdates0;
static std::map<HWND, WINDOWPLACEMENT> PendingUpdates1;
static bool IsUpdateScheduled;
static bool IsPaused;
static HWND DetectWindow;
static UINT_PTR ResumeTimer;
static int ResumeTimerValue = 1000;
static int ScheduleTimerValue = 1000;
static int logfile;



// _____________________________________________________________________ //
//
// Helpers
// _____________________________________________________________________ //
int clamp(int min, int max, int value)
{
	return std::min(std::max(min, value), max);
}

void Log(const char* fmt, ...)
{
	static std::vector<char> buf;
	va_list	args;
	va_start(args, fmt);
	int len = _vscprintf(fmt, args);
	if(len >= (int)buf.size())
		buf.resize(len + 1);
	len = _vsnprintf_s(buf.data(), buf.size(), len, fmt, args);
	va_end(args);
	if(len > 0)
	{
		OutputDebugStringA(buf.data());
		_write(logfile, buf.data(), len);
	}
}

wchar_t* WndText(HWND hWnd)
{
	static WCHAR title[200];
	GetWindowText(hWnd, title, 200);
	return title;
}

char* PlacementText(const WINDOWPLACEMENT& placement)
{
	static char text[200];
	sprintf_s(text, sizeof(text), "%d, %d (%dx%d)\n", placement.rcNormalPosition.left, placement.rcNormalPosition.top, placement.rcNormalPosition.right-placement.rcNormalPosition.left, placement.rcNormalPosition.bottom-placement.rcNormalPosition.top);
	return text;
}




// _____________________________________________________________________ //
//
// Monitor placement
// _____________________________________________________________________ //
BOOL CALLBACK EnumMonitorProc(HMONITOR hMonitor, HDC hdc, LPRECT rect, LPARAM lParam)
{
	std::vector<RECT>* monitors = (std::vector<RECT>*)lParam;
	monitors->push_back(*rect);
	return TRUE;
}

std::vector<RECT> GetAllMonitors()
{
	std::vector<RECT> monitors;
	EnumDisplayMonitors(NULL, NULL, EnumMonitorProc, (LPARAM)&monitors);
	return monitors;
}

bool IsDesiredMonitorLayout()
{
	std::vector<RECT> monitors = GetAllMonitors();
	if(monitors.size() != DesiredMonitorPlacements.size())
		return false;
	// This assumes deterministic order which might not work on every system
	for(size_t i = 0; i < monitors.size(); i++)
	{
		const RECT& m0 = monitors[i];
		const RECT& m1 = DesiredMonitorPlacements[i];

		// Only care about the size
		if(m0.right - m0.left != m1.right - m1.left)
			return false;
		if(m0.top - m0.bottom != m1.top - m1.bottom)
			return false;
	}
	return true;
}

bool IsIdenticalMonitorLayout()
{
	std::vector<RECT> monitors = GetAllMonitors();
	if(monitors.size() != DesiredMonitorPlacements.size())
		return false;
	// This assumes deterministic order which might not work on every system
	for(size_t i = 0; i < monitors.size(); i++)
	{
		const RECT& m0 = monitors[i];
		const RECT& m1 = DesiredMonitorPlacements[i];

		if(memcmp(&m0, &m1, sizeof(m0)))
			return false;
	}
	return true;
}




// _____________________________________________________________________ //
//
// Window placement
// _____________________________________________________________________ //
bool IsApplicationWindow(HWND hWnd)
{
	return IsWindowVisible(hWnd) && !GetParent(hWnd);
}

BOOL GetProperWindowPlacement(HWND hWnd, WINDOWPLACEMENT *placement)
{
	placement->length = sizeof(WINDOWPLACEMENT);
	if(GetWindowPlacement(hWnd, placement))
	{
		if(placement->showCmd == SW_SHOWNORMAL)
		{
			// If the window is "docked" the normalposition contains the last non-docked position.
			// That is not where we want to restore it, so extract the current rect with GetWindowRect
			// and translate the position to client coordinates
			GetWindowRect(hWnd, &placement->rcNormalPosition);

			// From MSDN WINDOWPLACEMENT reference:
			// "If the window is a top-level window that does not have the WS_EX_TOOLWINDOW window style,
			// then the coordinates represented by the following members are in workspace coordinates"
			// "Otherwise, these members are in screen coordinates"

			DWORD exStyle = GetWindowLong(hWnd, GWL_EXSTYLE);
			if(!(exStyle & WS_EX_TOOLWINDOW))
			{
				MONITORINFO monitorInfo;
				monitorInfo.cbSize = sizeof(MONITORINFO);

				HMONITOR hMonitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
				GetMonitorInfo(hMonitor, &monitorInfo);
				// 
				int dx = monitorInfo.rcMonitor.left - monitorInfo.rcWork.left;
				int dy = monitorInfo.rcMonitor.top - monitorInfo.rcWork.top;
				placement->rcNormalPosition.left += dx;
				placement->rcNormalPosition.top += dy;
				placement->rcNormalPosition.right += dx;
				placement->rcNormalPosition.bottom += dy;
			}
		}
		return TRUE;
	}
	return FALSE;
}

void AddWindow(HWND hWnd)
{
	WINDOWPLACEMENT placement;
	if(GetProperWindowPlacement(hWnd, &placement))
	{
		WindowPlacements[hWnd] = placement;
		if(GetWindowTextLength(hWnd))
			Log("Adding %S\n\tat %s\n", WndText(hWnd), PlacementText(placement));
	}
}

void RemoveWindow(HWND hWnd)
{
	if(WindowPlacements.erase(hWnd))
	{
		if(GetWindowTextLength(hWnd))
			Log("Removing %S\n", WndText(hWnd));
		PendingUpdates0.erase(hWnd);
		PendingUpdates1.erase(hWnd);
	}
}

bool HasWindow(HWND hWnd)
{
	return WindowPlacements.find(hWnd) != WindowPlacements.end();
}

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam)
{
	std::vector<HWND>* handles = (std::vector<HWND>*)lParam;
	handles->push_back(hwnd);
	return TRUE;
}

void LoadAllWindowPlacements()
{
	std::vector<HWND> handles;
	EnumWindows(EnumWindowsProc, (LPARAM)&handles);

	for(HWND hWnd : handles)
	{
		if(IsApplicationWindow(hWnd))
			AddWindow(hWnd);
	}
}

void CALLBACK UpdateWindowPlacements(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
{
	if(!IsUpdateScheduled)
	{
		KillTimer(hwnd, idEvent);
		return;
	}

	if(PendingUpdates1.size())
		printf("Updating WindowPlacements\n");

	for(auto it = PendingUpdates1.begin(); it != PendingUpdates1.end(); ++it)
	{
		WindowPlacements[it->first] = it->second;
	}
	PendingUpdates1.clear();
	PendingUpdates0.swap(PendingUpdates1);
	if(PendingUpdates1.size())
	{
		printf("Scheduling update\n");
		SetTimer(0, 0, ScheduleTimerValue, UpdateWindowPlacements);
	}
	else
	{
		KillTimer(hwnd, idEvent);
		IsUpdateScheduled = false;
	}
}

void ScheduleWindowPlacementUpdate(HWND hWnd)
{
	WINDOWPLACEMENT placement;
	placement.length = sizeof(WINDOWPLACEMENT);
	if(GetProperWindowPlacement(hWnd, &placement))
	{
		PendingUpdates0[hWnd] = placement;

		if(!IsUpdateScheduled)
		{
			printf("Scheduling update\n");
			IsUpdateScheduled = true;
			SetTimer(0, 0, ScheduleTimerValue, UpdateWindowPlacements);
		}
	}
}

void RestoreWindowPlacements()
{
	for(auto it = WindowPlacements.begin(); it != WindowPlacements.end(); ++it)
	{
		WINDOWPLACEMENT& placement = it->second;

		// The only thing that change when a monitor is connected or disconnected is the
		// window's "normal position". Everything else remain the same (Z order,
		// maximized/minimized status, focus, etc)"

		// Since the maximized status cannot be catched with SetWinEventHook we have
		// to extract the placement again
		WINDOWPLACEMENT current;
		GetProperWindowPlacement(it->first, &current);

		// No need to restore it if it's already in its correct position
		if(!memcmp(&current, &placement, sizeof(current)))
			continue;

		if(GetWindowTextLength(it->first))
		{
			Log("%S\n", WndText(it->first));
			Log("\tRestore %s to %s\n", current.showCmd == SW_SHOWMINIMIZED ? "minimized" : (current.showCmd == SW_SHOWMAXIMIZED ? "maximized" : ""), PlacementText(placement));
		}

		if(current.showCmd == SW_SHOWMINIMIZED)
		{
			// Restore its minimized position
			placement.showCmd = SW_SHOWMINNOACTIVE;
			placement.flags |= WPF_ASYNCWINDOWPLACEMENT;
			SetWindowPlacement(it->first, &placement);
		}
		else if(current.showCmd == SW_SHOWMAXIMIZED)
		{
			// The window was maximized

			// In order to restore the window on the correct display we have to move it to its normal position first, and then maximize it.
			// If we only maximize it (without moving it first) it will be maximized on the display it's currently on.
			// Before we maximize it we have to show it in its normal state. Otherwise it will retain its size on the new monitor which
			// will be incorrect if the new monitor has a different resolution

			// Restore
			ShowWindowAsync(it->first, SW_SHOWNOACTIVATE);
			// Move
			placement.showCmd = SW_SHOWNOACTIVATE;
			placement.flags |= WPF_ASYNCWINDOWPLACEMENT;
			SetWindowPlacement(it->first, &placement);
			// Maximize
			ShowWindowAsync(it->first, SW_SHOWMAXIMIZED);
		}
		else
		{
			// Restore its normal position
			placement.showCmd = SW_SHOWNOACTIVATE;
			placement.flags |= WPF_ASYNCWINDOWPLACEMENT;
			SetWindowPlacement(it->first, &placement);
		}
	}
}

void PauseWindowTracking()
{
	if(IsPaused)
		return;
	Log(" =========== PAUSE WINDOW TRACKING ===========\n");

	PendingUpdates0.clear();
	PendingUpdates1.clear();

	IsPaused = true;
}

void CALLBACK ResumeWindowTracking(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
{
	KillTimer(hwnd, idEvent);

	if(!IsPaused)
		return;

	// The layout could've changed since the call to ScheduleResumeWindowTracking()
	if(!IsDesiredMonitorLayout())
		return;

	Log(" =========== RESUME WINDOW TRACKING ===========\n");
	IsPaused = false;

	RestoreWindowPlacements();
}

void ScheduleResumeWindowTracking()
{
	ResumeTimer = SetTimer(0, ResumeTimer, ResumeTimerValue, ResumeWindowTracking);
}




// _____________________________________________________________________ //
//
// Dummy window for message handling
// _____________________________________________________________________ //
LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if(msg == WM_DISPLAYCHANGE)
	{
		if(!IsPaused && IsIdenticalMonitorLayout())
		{
			// On some systems it's possible to disconnect and reconnect a monitor without 
			// triggering a monitor layout change, but WM_DISPLAYCHANGE is still sent.
			// Do a pause and immediate resume to catch that case
			Log("Forced restore\n");
			PauseWindowTracking();
			ScheduleResumeWindowTracking();
		}
		else if(IsDesiredMonitorLayout())
		{
			// Extract monitor placements again in case the positions were changed
			DesiredMonitorPlacements = GetAllMonitors();

			ScheduleResumeWindowTracking();
		}
		else
		{
			PauseWindowTracking();
		}
	}

	return DefWindowProc(hWnd, msg, wParam, lParam);
}

HWND CreateDetectionWnd()
{
	HMODULE hInstance = GetModuleHandle(0);

	WNDCLASS wc = {};
	wc.lpfnWndProc = (WNDPROC)WindowProc;
	wc.hInstance = hInstance;
	wc.lpszClassName = L"RestoreWindows detection window class";
	ATOM hClass = RegisterClass(&wc);

	DWORD style = WS_DISABLED;
	HWND hWnd = CreateWindow((LPCWSTR)hClass, L"RestoreWindows detection window", WS_OVERLAPPEDWINDOW, 0, 0, 0, 0, HWND_DESKTOP, (HMENU)NULL, hInstance, (LPARAM)NULL);

	if(!hWnd)
	{
		DWORD error = GetLastError();
		error = error;
	}
	return hWnd;
}




// _____________________________________________________________________ //
//
// Global event listener
// _____________________________________________________________________ //
void CALLBACK WinEventProc(HWINEVENTHOOK hWinEventHook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime)
{
	if(IsPaused)
		return;

	switch(event)
	{
	case EVENT_OBJECT_LOCATIONCHANGE:
		if(!idObject && HasWindow(hwnd))
			ScheduleWindowPlacementUpdate(hwnd);
	break;

	case EVENT_OBJECT_CREATE:
	case EVENT_OBJECT_SHOW:
		if(!HasWindow(hwnd) && IsApplicationWindow(hwnd))
			AddWindow(hwnd);
	break;

	case EVENT_OBJECT_DESTROY:
	case EVENT_OBJECT_HIDE:
		if(!idObject)
			RemoveWindow(hwnd);
	break;

	case EVENT_OBJECT_PARENTCHANGE:
		if(HasWindow(hwnd))
		{
			if(!IsApplicationWindow(hwnd))
				RemoveWindow(hwnd);
		}
		else if(IsApplicationWindow(hwnd))
		{
			AddWindow(hwnd);
		}
	break;
	}
}




// _____________________________________________________________________ //
//
// MAIN
// _____________________________________________________________________ //
int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nShowCmd)
{
	// One instance is enough
	HANDLE hGlobalLock = CreateMutex(NULL, TRUE, L"RestoreWindowsMutex");
	if(hGlobalLock == INVALID_HANDLE_VALUE || GetLastError() == ERROR_ALREADY_EXISTS)
		return 0;

	const char* tok = 0;
	char* context = NULL;
	const char* delims = " =\t";
	tok = strtok_s(lpCmdLine, delims, &context);
	while(tok)
	{
		if(!_stricmp(tok, "--debuglog"))
		{
			_sopen_s(&logfile, "RestoreWindows.log", _O_WRONLY | _O_BINARY | _O_TRUNC | _O_CREAT, _SH_DENYNO, _S_IWRITE);
		}
		else if(!_stricmp(tok, "--delay"))
		{
			tok = strtok_s(0, delims, &context);
			if(tok)
				ResumeTimerValue = clamp(0, 1000*60*10, atoi(tok));
		}
		tok = strtok_s(0, delims, &context);
	}

	DesiredMonitorPlacements = GetAllMonitors();

	Log("Delay: %d\n\n", ResumeTimerValue);
	for(auto it = DesiredMonitorPlacements.begin(); it != DesiredMonitorPlacements.end(); ++it)
		Log("Monitor: %d, %d\n", it->right-it->left, it->bottom-it->top);
	 Log("\n");

	LoadAllWindowPlacements();

	HWINEVENTHOOK hWinEventHook = SetWinEventHook(EVENT_MIN, EVENT_MAX, NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
	DetectWindow = CreateDetectionWnd();

	MSG msg;
	while(GetMessage(&msg, NULL, 0, 0) > 0)
	{
		DispatchMessage(&msg);
	}

	// Usually never gets here...
	UnhookWinEvent(hWinEventHook);
	_close(logfile);
	CloseHandle(hGlobalLock);

	return 0;
}

