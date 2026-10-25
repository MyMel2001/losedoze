/* w32.h — clean-room Win32/NT declarations for the losedoze homemade Win32
 * subsystem (w32host). Original subset written from public ABI facts; no
 * Microsoft code or headers. Used by both the host (implementations) and
 * Win32 PE apps (import surface). x64: ms ABI; i386: stdcall (callee pops).
 * Sizes/pointers are W64 so the header works in freestanding PE builds. */
#ifndef W32_H
#define W32_H

/* API + callback calls cross the PE boundary: mark the calling convention so
 * the Linux host and the PE app agree (ms_abi on x86-64, stdcall on i386 —
 * where Win32 is callee-cleans on both sides: this host build and clang's
 * windows-i386 app build, which would otherwise default to cdecl). On the app
 * side clang's windows target uses both conventions natively. */
#if defined(W32_ABI_SYSV)
/* shared-library build for managed callers (libwin32.so): .NET P/Invoke on
 * Linux uses the SysV ABI, so the exported entry points must use it too */
#define W32API
#elif defined(__i386__)
#define W32API __attribute__((stdcall))
#elif defined(__x86_64__) && defined(__linux__)
#define W32API __attribute__((ms_abi))
#else
#define W32API
#endif

/* source-compatible spellings (stdcall on i386, nothing on x64 where there is
 * a single convention) */
#if defined(__i386__)
#define WINAPI __attribute__((stdcall))
#define CALLBACK __attribute__((stdcall))
#define APIENTRY __attribute__((stdcall))
#else
#define WINAPI
#define CALLBACK
#define APIENTRY
#endif

#ifndef NULL
#define NULL ((void *)0)
#endif

/* ---------------- scalar types (Windows x64 ABI) ---------------- */
typedef unsigned char W8;
typedef unsigned short W16;      /* WCHAR */
typedef signed short W16S;
typedef unsigned int W32;
typedef signed int W32S;
typedef unsigned long long W64;
typedef signed long long W64S;

typedef W32 DWORD;
typedef W32S LONG;
typedef W32 ULONG;
typedef W32 BOOL;
typedef W32 UINT;
typedef W16 WCHAR;
#if defined(__i386__)
typedef W32 UINT_PTR, ULONG_PTR, DWORD_PTR, WPARAM;
typedef W32S LONG_PTR, LPARAM, LRESULT;
#else
typedef W64 UINT_PTR, ULONG_PTR, DWORD_PTR, WPARAM;
typedef W64S LONG_PTR, LPARAM, LRESULT;
#endif
typedef void *PVOID;
typedef void *HANDLE;
typedef HANDLE HMODULE, HINSTANCE, HWND, HDC, HICON, HCURSOR, HBRUSH, HPEN,
    HFONT, HGDIOBJ, HMENU, HGLRC;
typedef W32 COLORREF;
typedef const char *LPCSTR;
typedef char *LPSTR;
typedef const W16 *LPCWSTR;
typedef W16 *LPWSTR;
typedef void *LPVOID;
typedef const void *LPCVOID;
typedef W16 ATOM;

typedef LRESULT (W32API *WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef void (W32API *FARPROC)(void);

typedef struct W32Point { LONG x, y; } POINT;
typedef struct W32Rect { LONG left, top, right, bottom; } RECT;
typedef struct W32Size { LONG cx, cy; } SIZE;
typedef struct W32Msg {
  HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; DWORD time; POINT pt;
} MSG;
typedef struct W32PaintStruct {
  HDC hdc; BOOL fErase; RECT rcPaint; BOOL fRestore; BOOL fIncUpdate;
  W8 rgbReserved[32];
} PAINTSTRUCT;
typedef struct W32WndClassW {
  UINT style; WNDPROC lpfnWndProc;
  int cbClsExtra, cbWndExtra; HINSTANCE hInstance; HICON hIcon;
  HCURSOR hCursor; HBRUSH hbrBackground; LPCWSTR lpszMenuName;
  LPCWSTR lpszClassName;
} WNDCLASSW;
typedef struct W32WndClassA {
  UINT style; WNDPROC lpfnWndProc;
  int cbClsExtra, cbWndExtra; HINSTANCE hInstance; HICON hIcon;
  HCURSOR hCursor; HBRUSH hbrBackground; LPCSTR lpszMenuName;
  LPCSTR lpszClassName;
} WNDCLASSA;

/* ---------------- constants (public ABI values) ---------------- */
#define W32_TRUE 1
#define W32_FALSE 0
#define INVALID_HANDLE_VALUE ((HANDLE)(W64S)-1)
#define INFINITE 0xffffffffu

#define STD_INPUT_HANDLE ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE ((DWORD)-12)

#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 1u
#define FILE_SHARE_WRITE 2u
#define CREATE_NEW 1u
#define CREATE_ALWAYS 2u
#define OPEN_EXISTING 3u
#define OPEN_ALWAYS 4u
#define FILE_ATTRIBUTE_NORMAL 0x80u

#define WAIT_OBJECT_0 0u
#define WAIT_FAILED 0xffffffffu

#define HEAP_ZERO_MEMORY 0x8u
#define MEM_COMMIT 0x1000u
#define MEM_RESERVE 0x2000u
#define MEM_RELEASE 0x8000u
#define PAGE_NOACCESS 0x01u
#define PAGE_READONLY 0x02u
#define PAGE_READWRITE 0x04u
#define PAGE_EXECUTE_READWRITE 0x40u

/* window styles */
#define WS_OVERLAPPED 0x00000000u
#define WS_POPUP 0x80000000u
#define WS_CHILD 0x40000000u
#define WS_MINIMIZE 0x20000000u
#define WS_VISIBLE 0x10000000u
#define WS_SYSMENU 0x00080000u
#define WS_THICKFRAME 0x00040000u
#define WS_MINIMIZEBOX 0x00020000u
#define WS_MAXIMIZEBOX 0x00010000u
#define WS_CAPTION 0x00C00000u
#define WS_BORDER 0x00800000u
#define WS_VSCROLL 0x00200000u
#define WS_HSCROLL 0x00100000u
#define WS_OVERLAPPEDWINDOW (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | \
                             WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)
#define WS_EX_TOPMOST 0x00000008u
#define CW_USEDEFAULT ((int)0x80000000)
#define SW_HIDE 0
#define SW_SHOWNORMAL 1
#define SW_SHOW 5
#define SW_RESTORE 9

/* messages */
#define WM_NULL 0x0000
#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_MOVE 0x0003
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_SETTEXT 0x000C
#define WM_GETTEXT 0x000D
#define WM_GETTEXTLENGTH 0x000E
#define WM_PAINT 0x000F
#define WM_CLOSE 0x0010
#define WM_ERASEBKGND 0x0014
#define WM_QUIT 0x0012
#define WM_SHOWWINDOW 0x0018
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP 0x0205
#define PM_NOREMOVE 0x0000
#define PM_REMOVE 0x0001

/* virtual keys (subset) */
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_INSERT 0x2D
#define VK_DELETE 0x2E
#define VK_F1 0x70

/* gdi */
#define W32RGB(r, g, b) ((COLORREF)((W8)(r) | ((W32)(W8)(g) << 8) | ((W32)(W8)(b) << 16)))
#define TRANSPARENT 1
#define OPAQUE 2
#define PS_SOLID 0
#define WHITE_BRUSH 0
#define LTGRAY_BRUSH 1
#define GRAY_BRUSH 2
#define DKGRAY_BRUSH 3
#define BLACK_BRUSH 4
#define NULL_BRUSH 5
#define WHITE_PEN 6
#define BLACK_PEN 7
#define DEFAULT_GUI_FONT 17
#define ANSI_FIXED_FONT 11

/* messagebox */
#define MB_OK 0x00000000u
#define IDOK 1
#define IDCANCEL 2

/* opengl32.dll constants (public GL values, the ones the demo needs) */
#define GL_RENDERER 0x1F01u
#define GL_VERSION 0x1F02u
#define GL_COLOR_BUFFER_BIT 0x00004000u
#define GL_TRIANGLES 0x0004u
#define GL_PROJECTION 0x1701u
#define GL_MODELVIEW 0x1700u

/* misc */
#define CP_UTF8 65001u
#define CP_ACP 0u
#define MAX_PATH 260

/* ---------------- NT types (subset) ---------------- */
typedef LONG NTSTATUS;
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)
#define STATUS_SUCCESS ((NTSTATUS)0x00000000)
#define STATUS_PENDING ((NTSTATUS)0x00000103)
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000005u)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC0000003u)
#define STATUS_INVALID_HANDLE ((NTSTATUS)0xC0000008u)
#define STATUS_NO_MEMORY ((NTSTATUS)0xC0000017u)
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xC0000022u)
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xC0000034u)
#define STATUS_OBJECT_PATH_NOT_FOUND ((NTSTATUS)0xC000003Au)
#define STATUS_OBJECT_NAME_COLLISION ((NTSTATUS)0xC0000035u)

typedef struct W32UnicodeStr {
  W16 Length; W16 MaximumLength; W16 *Buffer;
} UNICODE_STRING;
typedef struct W32Iosb {
  union { NTSTATUS Status; PVOID Pointer; } u;
  ULONG_PTR Information;
} IO_STATUS_BLOCK;
typedef struct W32ObjAttr {
  ULONG_PTR RootDirectory; ULONG Attributes; UNICODE_STRING *ObjectName;
  ULONG_PTR SecurityDescriptor; ULONG_PTR SecurityQualityOfService;
} OBJECT_ATTRIBUTES;
typedef struct W32FileStandardInfo {
  W64 EndOfFile; W64 AllocationSize; ULONG NumberOfLinks;
  BOOL DeletePending; BOOL Directory;
} FILE_STANDARD_INFORMATION;
typedef struct W32FilePosInfo { W64 CurrentByteOffset; } FILE_POSITION_INFORMATION;

/* ---------------- API surface (implemented by w32host) ---------------- */

#ifdef __cplusplus
extern "C" {
#endif

/* kernel32 */
W32API HMODULE GetModuleHandleA(LPCSTR);
W32API HMODULE GetModuleHandleW(LPCWSTR);
W32API FARPROC GetProcAddress(HMODULE, LPCSTR);
W32API HMODULE LoadLibraryA(LPCSTR);
W32API HMODULE LoadLibraryW(LPCWSTR);
W32API BOOL FreeLibrary(HMODULE);
W32API HANDLE CreateFileW(LPCWSTR, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
W32API HANDLE CreateFileA(LPCSTR, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
W32API BOOL ReadFile(HANDLE, void *, DWORD, DWORD *, void *);
W32API BOOL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
W32API BOOL CloseHandle(HANDLE);
W32API HANDLE GetStdHandle(DWORD);
W32API void ExitProcess(DWORD);
W32API DWORD GetLastError(void);
W32API void SetLastError(DWORD);
W32API HANDLE GetProcessHeap(void);
W32API void *HeapAlloc(HANDLE, DWORD, W64);
W32API BOOL HeapFree(HANDLE, DWORD, void *);
W32API HANDLE GetCurrentProcess(void);
W32API DWORD GetTickCount(void);
W32API void Sleep(DWORD);
W32API LPWSTR GetCommandLineW(void);
W32API LPSTR GetCommandLineA(void);
W32API int WideCharToMultiByte(UINT, DWORD, LPCWSTR, int, LPSTR, int, LPCSTR, BOOL *);
W32API int MultiByteToWideChar(UINT, DWORD, LPCSTR, int, LPWSTR, int);
W32API LPVOID VirtualAlloc(LPVOID, W64, DWORD, DWORD);
W32API BOOL VirtualFree(LPVOID, W64, DWORD);
W32API int lstrlenW(LPCWSTR);
W32API int lstrlenA(LPCSTR);
W32API LPWSTR lstrcpyW(LPWSTR, LPCWSTR);
W32API LPSTR lstrcpyA(LPSTR, LPCSTR);

/* user32 */
W32API ATOM RegisterClassExW(const WNDCLASSW *);
W32API ATOM RegisterClassA(const WNDCLASSA *);
W32API HWND CreateWindowExW(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int,
                     HWND, HMENU, HINSTANCE, LPVOID);
W32API HWND CreateWindowExA(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int,
                     HWND, HMENU, HINSTANCE, LPVOID);
W32API BOOL ShowWindow(HWND, int);
W32API BOOL UpdateWindow(HWND);
W32API LRESULT DefWindowProcW(HWND, UINT, WPARAM, LPARAM);
W32API LRESULT DefWindowProcA(HWND, UINT, WPARAM, LPARAM);
W32API BOOL GetMessageW(MSG *, HWND, UINT, UINT);
W32API BOOL GetMessageA(MSG *, HWND, UINT, UINT);
W32API BOOL PeekMessageW(MSG *, HWND, UINT, UINT, UINT);
W32API BOOL TranslateMessage(const MSG *);
W32API LRESULT DispatchMessageW(const MSG *);
W32API LRESULT DispatchMessageA(const MSG *);
W32API void PostQuitMessage(int);
W32API BOOL PostMessageW(HWND, UINT, WPARAM, LPARAM);
W32API LRESULT SendMessageW(HWND, UINT, WPARAM, LPARAM);
W32API HDC BeginPaint(HWND, PAINTSTRUCT *);
W32API BOOL EndPaint(HWND, const PAINTSTRUCT *);
W32API HDC GetDC(HWND);
W32API int ReleaseDC(HWND, HDC);
W32API int FillRect(HDC, const RECT *, HBRUSH);
W32API BOOL InvalidateRect(HWND, const RECT *, BOOL);
W32API BOOL ValidateRect(HWND, const RECT *);
W32API BOOL SetWindowTextW(HWND, LPCWSTR);
W32API int GetWindowTextW(HWND, LPWSTR, int);
W32API BOOL GetClientRect(HWND, RECT *);
W32API BOOL GetWindowRect(HWND, RECT *);
W32API int MessageBoxW(HWND, LPCWSTR, LPCWSTR, UINT);
W32API int MessageBoxA(HWND, LPCSTR, LPCSTR, UINT);
/* The host implements wsprintf* with an explicit arity (GCC's ms_abi
 * variadic support is broken) — it defines W32_NO_VSPRINTF_DECL and skips
 * the variadic prototypes below. Apps call them the normal Win32 way. */
#ifndef W32_NO_VSPRINTF_DECL
#if defined(__i386__)
/* i386: variadic functions are cdecl (stdcall is ignored on variadic, both
 * by MSVC and GCC), so no attribute here — the host picks args via stdarg. */
int wsprintfW(LPWSTR, LPCWSTR, ...);
int wsprintfA(LPSTR, LPCSTR, ...);
#else
W32API int wsprintfW(LPWSTR, LPCWSTR, ...);
W32API int wsprintfA(LPSTR, LPCSTR, ...);
#endif
#endif
W32API HCURSOR LoadCursorW(HINSTANCE, LPCWSTR);
W32API HICON LoadIconW(HINSTANCE, LPCWSTR);
W32API BOOL IsWindow(HWND);

/* gdi32 */
W32API BOOL TextOutW(HDC, int, int, LPCWSTR, int);
W32API BOOL TextOutA(HDC, int, int, LPCSTR, int);
W32API BOOL Rectangle(HDC, int, int, int, int);
W32API COLORREF SetTextColor(HDC, COLORREF);
W32API COLORREF SetBkColor(HDC, COLORREF);
W32API int SetBkMode(HDC, int);
W32API HBRUSH CreateSolidBrush(COLORREF);
W32API HPEN CreatePen(int, int, COLORREF);
W32API BOOL DeleteObject(HGDIOBJ);
W32API HGDIOBJ SelectObject(HDC, HGDIOBJ);
W32API HGDIOBJ GetStockObject(int);
W32API BOOL MoveToEx(HDC, int, int, POINT *);
W32API BOOL LineTo(HDC, int, int);
W32API BOOL GetTextExtentPoint32W(HDC, LPCWSTR, int, SIZE *);   /* gdi32 */
W32API BOOL SetFontSize(HDC, float);   /* text rasterization height in px */
W32API BOOL SelectClipRect(HDC, const RECT *);
W32API int IntersectClipRect(HDC, int, int, int, int);
W32API int GetClipBox(HDC, RECT *);
W32API int SaveDC(HDC);
W32API BOOL RestoreDC(HDC, int);
W32API BOOL DestroyWindow(HWND);                                    /* user32 */
W32API BOOL SwapBuffers(HDC);   /* presents the opengl32 context */

/* opengl32.dll: desktop OpenGL (compat profile) over EGL, resolved through
 * eglGetProcAddress. WGL: bind the context to a window DC, then draw. */
W32API HGLRC wglCreateContext(HDC);
W32API BOOL wglMakeCurrent(HDC, HGLRC);
W32API BOOL wglDeleteContext(HGLRC);
W32API void *wglGetProcAddress(const char *);   /* any GL entry point by name */
W32API const unsigned char *glGetString(unsigned);
W32API unsigned glGetError(void);
W32API void glGetIntegerv(unsigned, int *);
W32API unsigned glCheckFramebufferStatus(unsigned);
W32API void glGenTextures(int, unsigned *);
W32API void glGenFramebuffers(int, unsigned *);
W32API void glDeleteTextures(int, const unsigned *);
W32API void glDeleteFramebuffers(int, const unsigned *);
W32API void glFramebufferTexture2D(unsigned, unsigned, unsigned, unsigned, int);
W32API void glViewport(int, int, int, int);
W32API void glClearColor(float, float, float, float);
W32API void glClear(unsigned);
W32API void glEnable(unsigned);
W32API void glDisable(unsigned);
W32API void glDepthMask(unsigned char);
W32API void glFrontFace(unsigned);
W32API void glShadeModel(unsigned);
W32API void glMatrixMode(unsigned);
W32API void glLoadIdentity(void);
W32API void glPushMatrix(void);
W32API void glPopMatrix(void);
W32API void glOrtho(double, double, double, double, double, double);
W32API void glTranslatef(float, float, float);
W32API void glRotatef(float, float, float, float);
W32API void glScalef(float, float, float);
W32API void glBegin(unsigned);
W32API void glEnd(void);
W32API void glVertex2f(float, float);
W32API void glVertex3f(float, float, float);
W32API void glColor3f(float, float, float);
W32API void glColor4f(float, float, float, float);
W32API void glFlush(void);
W32API void glFinish(void);
W32API void glPixelStorei(unsigned, int);
W32API void glBindTexture(unsigned, unsigned);
W32API void glBindFramebuffer(unsigned, unsigned);
W32API void glTexImage2D(unsigned, int, int, int, int, int, unsigned,
                         unsigned, const void *);
W32API void glTexParameteri(unsigned, unsigned, int);
W32API void glReadPixels(int, int, int, int, unsigned, unsigned, void *);
W32API void glDrawArrays(unsigned, int, int);

/* ntdll subset (numbers: windows-syscalls nt.csv) */
W32API NTSTATUS NtCreateFile(HANDLE *, ULONG, OBJECT_ATTRIBUTES *, IO_STATUS_BLOCK *,
                      PVOID, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
W32API NTSTATUS NtOpenFile(HANDLE *, ULONG, OBJECT_ATTRIBUTES *, IO_STATUS_BLOCK *,
                    ULONG, ULONG);
W32API NTSTATUS NtReadFile(HANDLE, HANDLE, PVOID, PVOID, IO_STATUS_BLOCK *, PVOID,
                    ULONG, PVOID, PVOID);
W32API NTSTATUS NtWriteFile(HANDLE, HANDLE, PVOID, PVOID, IO_STATUS_BLOCK *, PVOID,
                     ULONG, PVOID, PVOID);
W32API NTSTATUS NtClose(HANDLE);
W32API NTSTATUS NtAllocateVirtualMemory(HANDLE, PVOID *, ULONG_PTR, ULONG_PTR *,
                                 ULONG, ULONG);
W32API NTSTATUS NtFreeVirtualMemory(HANDLE, PVOID *, ULONG_PTR *, ULONG);
W32API NTSTATUS NtDelayExecution(BOOL, W64S *);
W32API NTSTATUS NtWaitForSingleObject(HANDLE, BOOL, W64S *);
W32API NTSTATUS NtQueryInformationFile(HANDLE, IO_STATUS_BLOCK *, PVOID, ULONG, int);
W32API NTSTATUS NtSetInformationFile(HANDLE, IO_STATUS_BLOCK *, PVOID, ULONG, int);
W32API NTSTATUS NtCreateEvent(HANDLE *, W32, W32);
W32API NTSTATUS NtOpenEvent(HANDLE *, OBJECT_ATTRIBUTES *);
W32API NTSTATUS NtSetEvent(HANDLE);
W32API NTSTATUS NtResetEvent(HANDLE);
W32API NTSTATUS NtCloseEvent(HANDLE);
W32API NTSTATUS NtCreateSemaphore(HANDLE *, W32, OBJECT_ATTRIBUTES *);
W32API NTSTATUS NtOpenSemaphore(HANDLE *, OBJECT_ATTRIBUTES *);
W32API NTSTATUS NtReleaseSemaphore(HANDLE, W32);
W32API NTSTATUS NtCloseSemaphore(HANDLE);
W32API NTSTATUS NtCreateMutex(HANDLE *, W32, OBJECT_ATTRIBUTES *);
W32API NTSTATUS NtOpenMutex(HANDLE *, OBJECT_ATTRIBUTES *);
W32API NTSTATUS NtReleaseMutex(HANDLE);
W32API NTSTATUS NtCloseMutex(HANDLE);
W32API NTSTATUS NtCreateTimer(HANDLE *, W64, OBJECT_ATTRIBUTES *);
W32API NTSTATUS NtOpenTimer(HANDLE *, OBJECT_ATTRIBUTES *);
W32API NTSTATUS NtSetTimer(HANDLE, W64);
W32API NTSTATUS NtCancelTimer(HANDLE);
W32API NTSTATUS NtCloseTimer(HANDLE);
W32API NTSTATUS NtSignalAndWaitForSingleObject(HANDLE, HANDLE);
W32API NTSTATUS ZwCreateEvent(HANDLE *, W32, W32);
W32API NTSTATUS ZwOpenEvent(HANDLE *, OBJECT_ATTRIBUTES *);
W32API NTSTATUS ZwSetEvent(HANDLE);
W32API NTSTATUS ZwResetEvent(HANDLE);
W32API NTSTATUS ZwCloseEvent(HANDLE);
W32API NTSTATUS ZwCreateSemaphore(HANDLE *, W32, OBJECT_ATTRIBUTES *);
W32API NTSTATUS ZwOpenSemaphore(HANDLE *, OBJECT_ATTRIBUTES *);
W32API NTSTATUS ZwReleaseSemaphore(HANDLE, W32);
W32API NTSTATUS ZwCloseSemaphore(HANDLE);
W32API NTSTATUS ZwCreateMutex(HANDLE *, W32, OBJECT_ATTRIBUTES *);
W32API NTSTATUS ZwOpenMutex(HANDLE *, OBJECT_ATTRIBUTES *);
W32API NTSTATUS ZwReleaseMutex(HANDLE);
W32API NTSTATUS ZwCloseMutex(HANDLE);
W32API NTSTATUS ZwCreateTimer(HANDLE *, W64, OBJECT_ATTRIBUTES *);
W32API NTSTATUS ZwOpenTimer(HANDLE *, OBJECT_ATTRIBUTES *);
W32API NTSTATUS ZwSetTimer(HANDLE, W64);
W32API NTSTATUS ZwCancelTimer(HANDLE);
W32API NTSTATUS ZwCloseTimer(HANDLE);
W32API NTSTATUS ZwSignalAndWaitForSingleObject(HANDLE, HANDLE);
W32API NTSTATUS ZwCreateFile(HANDLE *, ULONG, OBJECT_ATTRIBUTES *, IO_STATUS_BLOCK *,
                      PVOID, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
W32API NTSTATUS ZwOpenFile(HANDLE *, ULONG, OBJECT_ATTRIBUTES *, IO_STATUS_BLOCK *,
                    ULONG, ULONG);
W32API NTSTATUS ZwReadFile(HANDLE, HANDLE, PVOID, PVOID, IO_STATUS_BLOCK *, PVOID,
                    ULONG, PVOID, PVOID);
W32API NTSTATUS ZwWriteFile(HANDLE, HANDLE, PVOID, PVOID, IO_STATUS_BLOCK *, PVOID,
                     ULONG, PVOID, PVOID);
W32API NTSTATUS ZwClose(HANDLE);
W32API NTSTATUS ZwAllocateVirtualMemory(HANDLE, PVOID *, ULONG_PTR, ULONG_PTR *,
                                 ULONG, ULONG);
W32API NTSTATUS ZwFreeVirtualMemory(HANDLE, PVOID *, ULONG_PTR *, ULONG);
W32API NTSTATUS ZwDelayExecution(BOOL, W64S *);
W32API NTSTATUS ZwWaitForSingleObject(HANDLE, BOOL, W64S *);
W32API NTSTATUS ZwQueryInformationFile(HANDLE, IO_STATUS_BLOCK *, PVOID, ULONG, int);
W32API NTSTATUS ZwSetInformationFile(HANDLE, IO_STATUS_BLOCK *, PVOID, ULONG, int);

W32API NTSTATUS NtReadVirtualMemory(HANDLE h, PVOID BaseAddress, PVOID Buffer,
                                   ULONG Length, ULONG *ReturnLength);
W32API NTSTATUS NtWriteVirtualMemory(HANDLE h, PVOID BaseAddress, PVOID Buffer,
                                   ULONG Length, ULONG *ReturnLength);
W32API NTSTATUS NtUnmapViewOfSection(HANDLE h, PVOID BaseAddress);
W32API NTSTATUS NtProtectVirtualMemory(HANDLE h, PVOID BaseAddress, ULONG Protect,
                                     ULONG *PageProtection);
W32API NTSTATUS NtFlushVirtualMemory(HANDLE h, PVOID BaseAddress, ULONG Length);
W32API NTSTATUS NtLockVirtualMemory(HANDLE h, PVOID BaseAddress, ULONG Length);
W32API NTSTATUS NtUnlockVirtualMemory(HANDLE h, PVOID BaseAddress, ULONG Length);
W32API NTSTATUS NtReadVirtualMemoryEx(HANDLE h, HANDLE section, PVOID BaseAddress,
                                    PVOID Buffer, ULONG Len, ULONG *ReturnLength);
W32API NTSTATUS NtWriteVirtualMemoryEx(HANDLE h, HANDLE section, PVOID BaseAddress,
                                    PVOID Buffer, ULONG Len, ULONG *ReturnLength);
W32API NTSTATUS NtAllocateVirtualMemoryEx(HANDLE h, PVOID *BaseAddress, ULONG AllocType,
                                        ULONG Protect, ULONG_PTR *Size);
W32API NTSTATUS ZwReadVirtualMemory(HANDLE h, PVOID BaseAddress, PVOID Buffer,
                                   ULONG Length, ULONG *ReturnLength);
W32API NTSTATUS ZwWriteVirtualMemory(HANDLE h, PVOID BaseAddress, PVOID Buffer,
                                   ULONG Length, ULONG *ReturnLength);
W32API NTSTATUS ZwUnmapViewOfSection(HANDLE h, PVOID BaseAddress);
W32API NTSTATUS ZwProtectVirtualMemory(HANDLE h, PVOID BaseAddress, ULONG Protect,
                                     ULONG *PageProtection);
W32API NTSTATUS ZwFlushVirtualMemory(HANDLE h, PVOID BaseAddress, ULONG Length);
W32API NTSTATUS ZwLockVirtualMemory(HANDLE h, PVOID BaseAddress, ULONG Length);
W32API NTSTATUS ZwUnlockVirtualMemory(HANDLE h, PVOID BaseAddress, ULONG Length);
W32API NTSTATUS ZwReadVirtualMemoryEx(HANDLE h, HANDLE section, PVOID BaseAddress,
                                    PVOID Buffer, ULONG Len, ULONG *ReturnLength);
W32API NTSTATUS ZwWriteVirtualMemoryEx(HANDLE h, HANDLE section, PVOID BaseAddress,
                                    PVOID Buffer, ULONG Len, ULONG *ReturnLength);
W32API NTSTATUS ZwAllocateVirtualMemoryEx(HANDLE h, PVOID *BaseAddress, ULONG AllocType,
                                        ULONG Protect, ULONG_PTR *Size);
W32API NTSTATUS NtFlushBuffersFile(HANDLE h, IO_STATUS_BLOCK *iosb);
W32API NTSTATUS NtFlushBuffersFileEx(HANDLE h, IO_STATUS_BLOCK *iosb, ULONG flags);
W32API NTSTATUS NtLockFile(HANDLE h, IO_STATUS_BLOCK *iosb, ULONG lockflags, BOOL exclusive,
                           W64 off, W64 timeout);
W32API NTSTATUS NtUnlockFile(HANDLE h, IO_STATUS_BLOCK *iosb);
W32API NTSTATUS NtDeviceIoControlFile(HANDLE h, IO_STATUS_BLOCK *iosb, ULONG code, PVOID in,
                                    ULONG inlen, PVOID out, ULONG outlen, ULONG *br);
W32API NTSTATUS NtFsControlFile(HANDLE h, IO_STATUS_BLOCK *iosb, ULONG code, PVOID in,
                               ULONG inlen, PVOID out, ULONG outlen, ULONG *br);
W32API NTSTATUS NtReadFileScatter(HANDLE h, HANDLE ev, PVOID apc, PVOID apcctx,
                                 IO_STATUS_BLOCK *iosb, PVOID buf, ULONG count, ULONG len);
W32API NTSTATUS NtWriteFileGather(HANDLE h, HANDLE ev, PVOID apc, PVOID apcctx,
                                 IO_STATUS_BLOCK *iosb, PVOID buf, ULONG count, ULONG len);
W32API NTSTATUS ZwFlushBuffersFile(HANDLE h, IO_STATUS_BLOCK *iosb);
W32API NTSTATUS ZwFlushBuffersFileEx(HANDLE h, IO_STATUS_BLOCK *iosb, ULONG flags);
W32API NTSTATUS ZwLockFile(HANDLE h, IO_STATUS_BLOCK *iosb, ULONG lockflags, BOOL exclusive,
                           W64 off, W64 timeout);
W32API NTSTATUS ZwUnlockFile(HANDLE h, IO_STATUS_BLOCK *iosb);
W32API NTSTATUS ZwDeviceIoControlFile(HANDLE h, IO_STATUS_BLOCK *iosb, ULONG code, PVOID in,
                                    ULONG inlen, PVOID out, ULONG outlen, ULONG *br);
W32API NTSTATUS ZwFsControlFile(HANDLE h, IO_STATUS_BLOCK *iosb, ULONG code, PVOID in,
                               ULONG inlen, PVOID out, ULONG outlen, ULONG *br);
W32API NTSTATUS ZwReadFileScatter(HANDLE h, HANDLE ev, PVOID apc, PVOID apcctx,
                                 IO_STATUS_BLOCK *iosb, PVOID buf, ULONG count, ULONG len);
W32API NTSTATUS ZwWriteFileGather(HANDLE h, HANDLE ev, PVOID apc, PVOID apcctx,
                                 IO_STATUS_BLOCK *iosb, PVOID buf, ULONG count, ULONG len);
W32API NTSTATUS NtCreateSection(HANDLE *SectionHandle, W32 Class, PVOID Buffer, ULONG Size,
                              ULONG SectionFlags, HANDLE Parent);
W32API NTSTATUS NtMapViewOfSection(HANDLE SectionHandle, HANDLE hProcess, PVOID BaseAddress,
                                 ULONG ViewLength, PVOID *BaseOut);
W32API NTSTATUS NtExtendSection(HANDLE SectionHandle, PVOID Extend);
W32API NTSTATUS ZwCreateSection(HANDLE *SectionHandle, W32 Class, PVOID Buffer, ULONG Size,
                               ULONG SectionFlags, HANDLE Parent);
W32API NTSTATUS ZwMapViewOfSection(HANDLE SectionHandle, HANDLE hProcess, PVOID BaseAddress,
                                 ULONG ViewLength, PVOID *BaseOut);
W32API NTSTATUS ZwExtendSection(HANDLE SectionHandle, PVOID Extend);
W32API NTSTATUS NtSetSystemTime(W64 *SystemTime, W64 *TimeZoneBias);
W32API NTSTATUS NtQueryTimerResolution(W32 *TimerResolution);
W32API NTSTATUS NtSetTimerResolution(W32 DesiredResolution, BOOL SetResolution, W32 *NewResolution);
W32API NTSTATUS NtQuerySystemInformation(W32 InfoClass, PVOID Buffer, W32 BufferLen, W32 *ReturnLength);
W32API NTSTATUS ZwSetSystemTime(W64 *SystemTime, W64 *TimeZoneBias);
W32API NTSTATUS ZwQueryTimerResolution(W32 *TimerResolution);
W32API NTSTATUS ZwSetTimerResolution(W32 DesiredResolution, BOOL SetResolution, W32 *NewResolution);
W32API NTSTATUS ZwQuerySystemInformation(W32 InfoClass, PVOID Buffer, W32 BufferLen, W32 *ReturnLength);
W32API NTSTATUS NtCreateKey(HANDLE *KeyHandle, OBJECT_ATTRIBUTES *ObjectAttributes, ULONG DesiredAccess);
W32API NTSTATUS NtOpenKey(HANDLE *KeyHandle, ULONG DesiredAccess, OBJECT_ATTRIBUTES *ObjectAttributes);
W32API NTSTATUS NtQueryKey(HANDLE KeyHandle, W32 InfoClass, PVOID Buffer, W32 BufferLen, W32 *ReturnLength);
W32API NTSTATUS NtCloseKey(HANDLE KeyHandle);
W32API NTSTATUS ZwCreateKey(HANDLE *KeyHandle, OBJECT_ATTRIBUTES *ObjectAttributes, ULONG DesiredAccess);
W32API NTSTATUS ZwOpenKey(HANDLE *KeyHandle, ULONG DesiredAccess, OBJECT_ATTRIBUTES *ObjectAttributes);
W32API NTSTATUS ZwQueryKey(HANDLE KeyHandle, W32 InfoClass, PVOID Buffer, W32 BufferLen, W32 *ReturnLength);
W32API NTSTATUS ZwCloseKey(HANDLE KeyHandle);

W32API NTSTATUS NtWaitForMultipleObjects(const HANDLE *Handles, W32 Count, BOOL WaitAll,
                                      W64S *Timeout);
W32API NTSTATUS NtWaitForMultipleObjects32(const HANDLE *Handles, W32 Count, BOOL WaitAll,
                                        W64S *Timeout);
W32API NTSTATUS ZwWaitForMultipleObjects(const HANDLE *Handles, W32 Count, BOOL WaitAll,
                                      W64S *Timeout);
W32API NTSTATUS ZwWaitForMultipleObjects32(const HANDLE *Handles, W32 Count, BOOL WaitAll,
                                        W64S *Timeout);

/* diagnostics */
W32API const char *nt_name(W32 nr);

#ifdef __cplusplus
}
#endif
#endif
