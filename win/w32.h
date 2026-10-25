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

W32API NTSTATUS NtCreateProcess(HANDLE *ProcessHandle, PVOID StartInfo);
W32API NTSTATUS NtCreateProcessEx(HANDLE *ProcessHandle, PVOID StartInfo);
W32API NTSTATUS NtCreateThread(HANDLE *ThreadHandle, void *Arg, long (*Start)(const long *));
W32API NTSTATUS NtCreateThreadEx(HANDLE *ThreadHandle, void *Arg, long (*Start)(const long *));
W32API NTSTATUS NtOpenProcess(HANDLE *ProcessHandle, W32 Access, OBJECT_ATTRIBUTES *ObjectAttributes);
W32API NTSTATUS NtOpenThread(HANDLE *ThreadHandle, W32 Access, OBJECT_ATTRIBUTES *ObjectAttributes);
W32API NTSTATUS NtTerminateProcess(HANDLE ProcessHandle, W32 ExitStatus);
W32API NTSTATUS NtTerminateThread(HANDLE ThreadHandle, W32 ExitStatus);
W32API NTSTATUS NtQueryInformationProcess(HANDLE ProcessHandle, W32 InfoClass, PVOID Buffer, W32 BufferLen, W32 *ReturnLength);
W32API NTSTATUS NtQueryInformationThread(HANDLE ThreadHandle, W32 InfoClass, PVOID Buffer, W32 BufferLen, W32 *ReturnLength);
W32API NTSTATUS ZwCreateProcess(HANDLE *ProcessHandle, PVOID StartInfo);
W32API NTSTATUS ZwCreateProcessEx(HANDLE *ProcessHandle, PVOID StartInfo);
W32API NTSTATUS ZwCreateThread(HANDLE *ThreadHandle, void *Arg, long (*Start)(const long *));
W32API NTSTATUS ZwCreateThreadEx(HANDLE *ThreadHandle, void *Arg, long (*Start)(const long *));
W32API NTSTATUS ZwOpenProcess(HANDLE *ProcessHandle, W32 Access, OBJECT_ATTRIBUTES *ObjectAttributes);
W32API NTSTATUS ZwOpenThread(HANDLE *ThreadHandle, W32 Access, OBJECT_ATTRIBUTES *ObjectAttributes);
W32API NTSTATUS ZwTerminateProcess(HANDLE ProcessHandle, W32 ExitStatus);
W32API NTSTATUS ZwTerminateThread(HANDLE ThreadHandle, W32 ExitStatus);
W32API NTSTATUS ZwQueryInformationProcess(HANDLE ProcessHandle, W32 InfoClass, PVOID Buffer, W32 BufferLen, W32 *ReturnLength);
W32API NTSTATUS ZwQueryInformationThread(HANDLE ThreadHandle, W32 InfoClass, PVOID Buffer, W32 BufferLen, W32 *ReturnLength);

W32API NTSTATUS NtQueryVirtualMemory(HANDLE h, W32 InfoClass, PVOID BaseAddress, W32 Length, W32 *ReturnLength);
W32API NTSTATUS ZwQueryVirtualMemory(HANDLE h, W32 InfoClass, PVOID BaseAddress, W32 Length, W32 *ReturnLength);
W32API NTSTATUS NtMapViewOfSectionEx(HANDLE SectionHandle, HANDLE hProcess, PVOID BaseAddress, ULONG ViewLength, PVOID *BaseOut);
W32API NTSTATUS ZwMapViewOfSectionEx(HANDLE SectionHandle, HANDLE hProcess, PVOID BaseAddress, ULONG ViewLength, PVOID *BaseOut);
W32API NTSTATUS NtUnmapViewOfSectionEx(HANDLE h, PVOID BaseAddress);
W32API NTSTATUS ZwUnmapViewOfSectionEx(HANDLE h, PVOID BaseAddress);
W32API NTSTATUS NtCreateSectionEx(HANDLE *SectionHandle, W32 Class, PVOID Buffer, ULONG Size, ULONG SectionFlags, HANDLE Parent);
W32API NTSTATUS ZwCreateSectionEx(HANDLE *SectionHandle, W32 Class, PVOID Buffer, ULONG Size, ULONG SectionFlags, HANDLE Parent);

W32API NTSTATUS NtQueryEvent(HANDLE h, W32 *PreviousState);
W32API NTSTATUS ZwQueryEvent(HANDLE h, W32 *PreviousState);
W32API NTSTATUS NtQuerySemaphore(HANDLE h, W32 *Count);
W32API NTSTATUS ZwQuerySemaphore(HANDLE h, W32 *Count);
W32API NTSTATUS NtClearEvent(HANDLE h);
W32API NTSTATUS ZwClearEvent(HANDLE h);
W32API NTSTATUS NtPulseEvent(HANDLE h);
W32API NTSTATUS ZwPulseEvent(HANDLE h);

W32API NTSTATUS NtQueryPerformanceCounter(W64 *Frequency, W64 *Counter);
W32API NTSTATUS ZwQueryPerformanceCounter(W64 *Frequency, W64 *Counter);
W32API NTSTATUS NtGetCurrentProcessorNumber(W32 *Number);
W32API NTSTATUS ZwGetCurrentProcessorNumber(W32 *Number);
W32API NTSTATUS NtGetCurrentProcessorNumberEx(W32 Info, W32 *Number);
W32API NTSTATUS ZwGetCurrentProcessorNumberEx(W32 Info, W32 *Number);

W32API NTSTATUS NtYieldExecution();
W32API NTSTATUS ZwYieldExecution();
W32API NTSTATUS NtFlushInstructionCache(HANDLE ImageHandle);
W32API NTSTATUS ZwFlushInstructionCache(HANDLE ImageHandle);
W32API NTSTATUS NtFlushProcessWriteBuffers(HANDLE Process);
W32API NTSTATUS ZwFlushProcessWriteBuffers(HANDLE Process);
W32API NTSTATUS NtGetDevicePowerState(HANDLE Device, W32 *State);
W32API NTSTATUS ZwGetDevicePowerState(HANDLE Device, W32 *State);
W32API NTSTATUS NtGetWriteWatch(W32 *WatchInfo);
W32API NTSTATUS ZwGetWriteWatch(W32 *WatchInfo);
W32API NTSTATUS NtGetCachedSigningLevel(W32 *Level);
W32API NTSTATUS ZwGetCachedSigningLevel(W32 *Level);
W32API NTSTATUS NtGetNlsSectionPtr(W32 Section, PVOID *Pointer);
W32API NTSTATUS ZwGetNlsSectionPtr(W32 Section, PVOID *Pointer);
W32API NTSTATUS NtIsProcessInJob(HANDLE ProcessHandle, HANDLE JobHandle);
W32API NTSTATUS ZwIsProcessInJob(HANDLE ProcessHandle, HANDLE JobHandle);
W32API NTSTATUS NtIsSystemResumeAutomatic();
W32API NTSTATUS ZwIsSystemResumeAutomatic();
W32API NTSTATUS NtIsUILanguageComitted(W32 *Committed);
W32API NTSTATUS ZwIsUILanguageComitted(W32 *Committed);
W32API NTSTATUS NtInitiatePowerAction();
W32API NTSTATUS ZwInitiatePowerAction();
W32API NTSTATUS NtLoadDriver();
W32API NTSTATUS ZwLoadDriver();
W32API NTSTATUS NtLockProductActivationKeys(W32 *Locked);
W32API NTSTATUS ZwLockProductActivationKeys(W32 *Locked);
W32API NTSTATUS NtLockRegistryKey(HANDLE Key);
W32API NTSTATUS ZwLockRegistryKey(HANDLE Key);
W32API NTSTATUS NtMakeTemporaryObject(HANDLE Object);
W32API NTSTATUS ZwMakeTemporaryObject(HANDLE Object);
W32API NTSTATUS NtMakePermanentObject(HANDLE Object);
W32API NTSTATUS ZwMakePermanentObject(HANDLE Object);
W32API NTSTATUS NtManageHotPatch(HANDLE HotPatchObject);
W32API NTSTATUS ZwManageHotPatch(HANDLE HotPatchObject);
W32API NTSTATUS NtManagePartition(HANDLE Partition);
W32API NTSTATUS ZwManagePartition(HANDLE Partition);
W32API NTSTATUS NtMapUserPhysicalPages(HANDLE Handle, PVOID *VirtualAddress, W64 Size);
W32API NTSTATUS ZwMapUserPhysicalPages(HANDLE Handle, PVOID *VirtualAddress, W64 Size);
W32API NTSTATUS NtMapUserPhysicalPagesScatter(HANDLE Handle, PVOID *VirtualAddress, W64 Size);
W32API NTSTATUS ZwMapUserPhysicalPagesScatter(HANDLE Handle, PVOID *VirtualAddress, W64 Size);
W32API NTSTATUS NtModifyBootEntry(HANDLE BootEntry);
W32API NTSTATUS ZwModifyBootEntry(HANDLE BootEntry);
W32API NTSTATUS NtModifyDriverEntry(HANDLE DriverEntry);
W32API NTSTATUS ZwModifyDriverEntry(HANDLE DriverEntry);
W32API NTSTATUS NtNotifyChangeSession(HANDLE Session);
W32API NTSTATUS ZwNotifyChangeSession(HANDLE Session);
W32API NTSTATUS NtPowerInformation(W32 *Information);
W32API NTSTATUS ZwPowerInformation(W32 *Information);

W32API NTSTATUS ZwAddBootEntry();
W32API NTSTATUS NtAddBootEntry();
W32API NTSTATUS ZwAddDriverEntry();
W32API NTSTATUS NtAddDriverEntry();
W32API NTSTATUS ZwDeleteBootEntry();
W32API NTSTATUS NtDeleteBootEntry();
W32API NTSTATUS ZwDeleteDriverEntry();
W32API NTSTATUS NtDeleteDriverEntry();
W32API NTSTATUS ZwEnumerateBootEntries();
W32API NTSTATUS NtEnumerateBootEntries();
W32API NTSTATUS ZwEnumerateDriverEntries();
W32API NTSTATUS NtEnumerateDriverEntries();
W32API NTSTATUS ZwQueryBootEntryOrder();
W32API NTSTATUS NtQueryBootEntryOrder();
W32API NTSTATUS ZwSetBootEntryOrder();
W32API NTSTATUS NtSetBootEntryOrder();
W32API NTSTATUS ZwQueryBootOptions();
W32API NTSTATUS NtQueryBootOptions();
W32API NTSTATUS ZwSetBootOptions();
W32API NTSTATUS NtSetBootOptions();
W32API NTSTATUS ZwDisableLastKnownGood();
W32API NTSTATUS NtDisableLastKnownGood();
W32API NTSTATUS ZwEnableLastKnownGood();
W32API NTSTATUS NtEnableLastKnownGood();
W32API NTSTATUS ZwQueryIntervalProfile();
W32API NTSTATUS NtQueryIntervalProfile();
W32API NTSTATUS ZwSetIntervalProfile();
W32API NTSTATUS NtSetIntervalProfile();
W32API NTSTATUS ZwSerializeBoot();
W32API NTSTATUS NtSerializeBoot();
W32API NTSTATUS ZwTraceControl();
W32API NTSTATUS NtTraceControl();
W32API NTSTATUS ZwShutdownSystem();
W32API NTSTATUS NtShutdownSystem();
W32API NTSTATUS ZwSinglePhaseReject();
W32API NTSTATUS NtSinglePhaseReject();
W32API NTSTATUS ZwPrePrepareComplete();
W32API NTSTATUS NtPrePrepareComplete();
W32API NTSTATUS ZwPrePrepareEnlistment();
W32API NTSTATUS NtPrePrepareEnlistment();
W32API NTSTATUS ZwPrepareComplete();
W32API NTSTATUS NtPrepareComplete();
W32API NTSTATUS ZwPrepareEnlistment();
W32API NTSTATUS NtPrepareEnlistment();
W32API NTSTATUS ZwPropagationComplete();
W32API NTSTATUS NtPropagationComplete();
W32API NTSTATUS ZwPropagationFailed();
W32API NTSTATUS NtPropagationFailed();
W32API NTSTATUS ZwRollbackComplete();
W32API NTSTATUS NtRollbackComplete();
W32API NTSTATUS ZwRollbackEnlistment();
W32API NTSTATUS NtRollbackEnlistment();
W32API NTSTATUS ZwRollbackRegistryTransaction();
W32API NTSTATUS NtRollbackRegistryTransaction();
W32API NTSTATUS ZwRollbackTransaction();
W32API NTSTATUS NtRollbackTransaction();
W32API NTSTATUS ZwRollforwardTransactionManager();
W32API NTSTATUS NtRollforwardTransactionManager();
W32API NTSTATUS ZwRecoverTransactionManager();
W32API NTSTATUS NtRecoverTransactionManager();
W32API NTSTATUS ZwRenameTransactionManager();
W32API NTSTATUS NtRenameTransactionManager();
W32API NTSTATUS ZwCommitComplete();
W32API NTSTATUS NtCommitComplete();
W32API NTSTATUS ZwCommitEnlistment();
W32API NTSTATUS NtCommitEnlistment();
W32API NTSTATUS ZwCommitRegistryTransaction();
W32API NTSTATUS NtCommitRegistryTransaction();
W32API NTSTATUS ZwCommitTransaction();
W32API NTSTATUS NtCommitTransaction();
W32API NTSTATUS ZwRecoverEnlistment();
W32API NTSTATUS NtRecoverEnlistment();
W32API NTSTATUS ZwRecoverResourceManager();
W32API NTSTATUS NtRecoverResourceManager();
W32API NTSTATUS ZwShutdownWorkerFactory();
W32API NTSTATUS NtShutdownWorkerFactory();
W32API NTSTATUS ZwCreateWorkerFactory();
W32API NTSTATUS NtCreateWorkerFactory();
W32API NTSTATUS ZwQueryInformationWorkerFactory();
W32API NTSTATUS NtQueryInformationWorkerFactory();
W32API NTSTATUS ZwSetInformationWorkerFactory();
W32API NTSTATUS NtSetInformationWorkerFactory();
W32API NTSTATUS ZwReleaseWorkerFactoryWorker();
W32API NTSTATUS NtReleaseWorkerFactoryWorker();
W32API NTSTATUS ZwWorkerFactoryWorkerReady();
W32API NTSTATUS NtWorkerFactoryWorkerReady();
W32API NTSTATUS ZwCreateWaitCompletionPacket();
W32API NTSTATUS NtCreateWaitCompletionPacket();
W32API NTSTATUS ZwCancelWaitCompletionPacket();
W32API NTSTATUS NtCancelWaitCompletionPacket();
W32API NTSTATUS ZwAssociateWaitCompletionPacket();
W32API NTSTATUS NtAssociateWaitCompletionPacket();
W32API NTSTATUS ZwCreateWaitablePort();
W32API NTSTATUS NtCreateWaitablePort();
W32API NTSTATUS ZwCreatePort();
W32API NTSTATUS NtCreatePort();
W32API NTSTATUS ZwConnectPort();
W32API NTSTATUS NtConnectPort();
W32API NTSTATUS ZwAcceptConnectPort();
W32API NTSTATUS NtAcceptConnectPort();
W32API NTSTATUS ZwQueryInformationPort();
W32API NTSTATUS NtQueryInformationPort();
W32API NTSTATUS ZwQueryInformationPort();
W32API NTSTATUS NtQueryInformationPort();
W32API NTSTATUS ZwSetInformationPort();
W32API NTSTATUS NtSetInformationPort();
W32API NTSTATUS ZwListenPort();
W32API NTSTATUS NtListenPort();
W32API NTSTATUS ZwRequestPort();
W32API NTSTATUS NtRequestPort();
W32API NTSTATUS ZwReplyPort();
W32API NTSTATUS NtReplyPort();
W32API NTSTATUS ZwReplyWaitReceivePort();
W32API NTSTATUS NtReplyWaitReceivePort();
W32API NTSTATUS ZwReplyWaitReceivePortEx();
W32API NTSTATUS NtReplyWaitReceivePortEx();
W32API NTSTATUS ZwReplyWaitReplyPort();
W32API NTSTATUS NtReplyWaitReplyPort();
W32API NTSTATUS ZwSendWaitReceivePort();
W32API NTSTATUS NtSendWaitReceivePort();
W32API NTSTATUS ZwSecureConnectPort();
W32API NTSTATUS NtSecureConnectPort();
W32API NTSTATUS ZwCompleteConnectPort();
W32API NTSTATUS NtCompleteConnectPort();
W32API NTSTATUS ZwAccessCheckAndAuditAlarm();
W32API NTSTATUS NtAccessCheckAndAuditAlarm();
W32API NTSTATUS ZwAccessCheckByType();
W32API NTSTATUS NtAccessCheckByType();
W32API NTSTATUS ZwAccessCheckByTypeAndAuditAlarm();
W32API NTSTATUS NtAccessCheckByTypeAndAuditAlarm();
W32API NTSTATUS ZwAccessCheckByTypeResultList();
W32API NTSTATUS NtAccessCheckByTypeResultList();
W32API NTSTATUS ZwAccessCheckByTypeResultListAndAuditAlarm();
W32API NTSTATUS NtAccessCheckByTypeResultListAndAuditAlarm();
W32API NTSTATUS ZwAccessCheckByTypeResultListAndAuditAlarmByHandle();
W32API NTSTATUS NtAccessCheckByTypeResultListAndAuditAlarmByHandle();
W32API NTSTATUS ZwAcquireCrossVmMutant();
W32API NTSTATUS NtAcquireCrossVmMutant();
W32API NTSTATUS ZwAcquireProcessActivityReference();
W32API NTSTATUS NtAcquireProcessActivityReference();
W32API NTSTATUS ZwAddAtom();
W32API NTSTATUS NtAddAtom();
W32API NTSTATUS ZwAddAtomEx();
W32API NTSTATUS NtAddAtomEx();
W32API NTSTATUS ZwAdjustGroupsToken();
W32API NTSTATUS NtAdjustGroupsToken();
W32API NTSTATUS ZwAdjustPrivilegesToken();
W32API NTSTATUS NtAdjustPrivilegesToken();
W32API NTSTATUS ZwAdjustTokenClaimsAndDeviceGroups();
W32API NTSTATUS NtAdjustTokenClaimsAndDeviceGroups();
W32API NTSTATUS ZwAlertMultipleThreadByThreadId();
W32API NTSTATUS NtAlertMultipleThreadByThreadId();
W32API NTSTATUS ZwAlertResumeThread();
W32API NTSTATUS NtAlertResumeThread();
W32API NTSTATUS ZwAlertThread();
W32API NTSTATUS NtAlertThread();
W32API NTSTATUS ZwAlertThreadByThreadId();
W32API NTSTATUS NtAlertThreadByThreadId();
W32API NTSTATUS ZwAlertThreadByThreadIdEx();
W32API NTSTATUS NtAlertThreadByThreadIdEx();
W32API NTSTATUS ZwAllocateLocallyUniqueId();
W32API NTSTATUS NtAllocateLocallyUniqueId();
W32API NTSTATUS ZwAllocateReserveObject();
W32API NTSTATUS NtAllocateReserveObject();
W32API NTSTATUS ZwAllocateUserPhysicalPages();
W32API NTSTATUS NtAllocateUserPhysicalPages();
W32API NTSTATUS ZwAllocateUserPhysicalPagesEx();
W32API NTSTATUS NtAllocateUserPhysicalPagesEx();
W32API NTSTATUS ZwAllocateUuids();
W32API NTSTATUS NtAllocateUuids();
W32API NTSTATUS ZwAlpcAcceptConnectPort();
W32API NTSTATUS NtAlpcAcceptConnectPort();
W32API NTSTATUS ZwAlpcCancelMessage();
W32API NTSTATUS NtAlpcCancelMessage();
W32API NTSTATUS ZwAlpcConnectPort();
W32API NTSTATUS NtAlpcConnectPort();
W32API NTSTATUS ZwAlpcConnectPortEx();
W32API NTSTATUS NtAlpcConnectPortEx();
W32API NTSTATUS ZwAlpcCreatePort();
W32API NTSTATUS NtAlpcCreatePort();
W32API NTSTATUS ZwAlpcCreatePortSection();
W32API NTSTATUS NtAlpcCreatePortSection();
W32API NTSTATUS ZwAlpcCreateResourceReserve();
W32API NTSTATUS NtAlpcCreateResourceReserve();
W32API NTSTATUS ZwAlpcCreateSectionView();
W32API NTSTATUS NtAlpcCreateSectionView();
W32API NTSTATUS ZwAlpcCreateSecurityContext();
W32API NTSTATUS NtAlpcCreateSecurityContext();
W32API NTSTATUS ZwAlpcDeletePortSection();
W32API NTSTATUS NtAlpcDeletePortSection();
W32API NTSTATUS ZwAlpcDeleteResourceReserve();
W32API NTSTATUS NtAlpcDeleteResourceReserve();
W32API NTSTATUS ZwAlpcDeleteSectionView();
W32API NTSTATUS NtAlpcDeleteSectionView();
W32API NTSTATUS ZwAlpcDeleteSecurityContext();
W32API NTSTATUS NtAlpcDeleteSecurityContext();
W32API NTSTATUS ZwAlpcDisconnectPort();
W32API NTSTATUS NtAlpcDisconnectPort();
W32API NTSTATUS ZwAlpcImpersonateClientContainerOfPort();
W32API NTSTATUS NtAlpcImpersonateClientContainerOfPort();
W32API NTSTATUS ZwAlpcImpersonateClientOfPort();
W32API NTSTATUS NtAlpcImpersonateClientOfPort();
W32API NTSTATUS ZwAlpcOpenSenderProcess();
W32API NTSTATUS NtAlpcOpenSenderProcess();
W32API NTSTATUS ZwAlpcOpenSenderThread();
W32API NTSTATUS NtAlpcOpenSenderThread();
W32API NTSTATUS ZwAlpcQueryInformation();
W32API NTSTATUS NtAlpcQueryInformation();
W32API NTSTATUS ZwAlpcQueryInformationMessage();
W32API NTSTATUS NtAlpcQueryInformationMessage();
W32API NTSTATUS ZwAlpcRevokeSecurityContext();
W32API NTSTATUS NtAlpcRevokeSecurityContext();
W32API NTSTATUS ZwAlpcSendWaitReceivePort();
W32API NTSTATUS NtAlpcSendWaitReceivePort();
W32API NTSTATUS ZwAlpcSetInformation();
W32API NTSTATUS NtAlpcSetInformation();
W32API NTSTATUS ZwApphelpCacheControl();
W32API NTSTATUS NtApphelpCacheControl();
W32API NTSTATUS ZwAreMappedFilesTheSame();
W32API NTSTATUS NtAreMappedFilesTheSame();
W32API NTSTATUS ZwAssignProcessToJobObject();
W32API NTSTATUS NtAssignProcessToJobObject();
W32API NTSTATUS ZwCallEnclave();
W32API NTSTATUS NtCallEnclave();
W32API NTSTATUS ZwCallbackReturn();
W32API NTSTATUS NtCallbackReturn();
W32API NTSTATUS ZwCancelIoFile();
W32API NTSTATUS NtCancelIoFile();
W32API NTSTATUS ZwCancelIoFileEx();
W32API NTSTATUS NtCancelIoFileEx();
W32API NTSTATUS ZwCancelSynchronousIoFile();
W32API NTSTATUS NtCancelSynchronousIoFile();
W32API NTSTATUS ZwCancelTimer2();
W32API NTSTATUS NtCancelTimer2();
W32API NTSTATUS ZwChangeProcessState();
W32API NTSTATUS NtChangeProcessState();
W32API NTSTATUS ZwChangeThreadState();
W32API NTSTATUS NtChangeThreadState();
W32API NTSTATUS ZwCloseObjectAuditAlarm();
W32API NTSTATUS NtCloseObjectAuditAlarm();
W32API NTSTATUS ZwCompactKeys();
W32API NTSTATUS NtCompactKeys();
W32API NTSTATUS ZwCompareObjects();
W32API NTSTATUS NtCompareObjects();
W32API NTSTATUS ZwCompareSigningLevels();
W32API NTSTATUS NtCompareSigningLevels();
W32API NTSTATUS ZwCompareTokens();
W32API NTSTATUS NtCompareTokens();
W32API NTSTATUS ZwCompressKey();
W32API NTSTATUS NtCompressKey();
W32API NTSTATUS ZwContinue();
W32API NTSTATUS NtContinue();
W32API NTSTATUS ZwContinueEx();
W32API NTSTATUS NtContinueEx();
W32API NTSTATUS ZwConvertBetweenAuxiliaryCounterAndPerformanceCounter();
W32API NTSTATUS NtConvertBetweenAuxiliaryCounterAndPerformanceCounter();
W32API NTSTATUS ZwCopyFileChunk();
W32API NTSTATUS NtCopyFileChunk();
W32API NTSTATUS ZwCreateCpuPartition();
W32API NTSTATUS NtCreateCpuPartition();
W32API NTSTATUS ZwCreateCrossVmEvent();
W32API NTSTATUS NtCreateCrossVmEvent();
W32API NTSTATUS ZwCreateCrossVmMutant();
W32API NTSTATUS NtCreateCrossVmMutant();
W32API NTSTATUS ZwCreateDebugObject();
W32API NTSTATUS NtCreateDebugObject();
W32API NTSTATUS ZwCreateDirectoryObject();
W32API NTSTATUS NtCreateDirectoryObject();
W32API NTSTATUS ZwCreateDirectoryObjectEx();
W32API NTSTATUS NtCreateDirectoryObjectEx();
W32API NTSTATUS ZwCreateEnclave();
W32API NTSTATUS NtCreateEnclave();
W32API NTSTATUS ZwCreateEnlistment();
W32API NTSTATUS NtCreateEnlistment();
W32API NTSTATUS ZwCreateEventPair();
W32API NTSTATUS NtCreateEventPair();
W32API NTSTATUS ZwCreateIRTimer();
W32API NTSTATUS NtCreateIRTimer();
W32API NTSTATUS ZwCreateIoCompletion();
W32API NTSTATUS NtCreateIoCompletion();
W32API NTSTATUS ZwCreateIoRing();
W32API NTSTATUS NtCreateIoRing();
W32API NTSTATUS ZwCreateJobObject();
W32API NTSTATUS NtCreateJobObject();
W32API NTSTATUS ZwCreateJobSet();
W32API NTSTATUS NtCreateJobSet();
W32API NTSTATUS ZwCreateKeyTransacted();
W32API NTSTATUS NtCreateKeyTransacted();
W32API NTSTATUS ZwCreateKeyedEvent();
W32API NTSTATUS NtCreateKeyedEvent();
W32API NTSTATUS ZwCreateLowBoxToken();
W32API NTSTATUS NtCreateLowBoxToken();
W32API NTSTATUS ZwCreateMailslotFile();
W32API NTSTATUS NtCreateMailslotFile();
W32API NTSTATUS ZwCreateMutant();
W32API NTSTATUS NtCreateMutant();
W32API NTSTATUS ZwCreateNamedPipeFile();
W32API NTSTATUS NtCreateNamedPipeFile();
W32API NTSTATUS ZwCreatePagingFile();
W32API NTSTATUS NtCreatePagingFile();
W32API NTSTATUS ZwCreatePartition();
W32API NTSTATUS NtCreatePartition();
W32API NTSTATUS ZwCreatePrivateNamespace();
W32API NTSTATUS NtCreatePrivateNamespace();
W32API NTSTATUS ZwCreateProcessStateChange();
W32API NTSTATUS NtCreateProcessStateChange();
W32API NTSTATUS ZwCreateProfile();
W32API NTSTATUS NtCreateProfile();
W32API NTSTATUS ZwCreateProfileEx();
W32API NTSTATUS NtCreateProfileEx();
W32API NTSTATUS ZwCreateRegistryTransaction();
W32API NTSTATUS NtCreateRegistryTransaction();
W32API NTSTATUS ZwCreateResourceManager();
W32API NTSTATUS NtCreateResourceManager();
W32API NTSTATUS ZwCreateSymbolicLinkObject();
W32API NTSTATUS NtCreateSymbolicLinkObject();
W32API NTSTATUS ZwCreateThreadStateChange();
W32API NTSTATUS NtCreateThreadStateChange();
W32API NTSTATUS ZwCreateTimer2();
W32API NTSTATUS NtCreateTimer2();
W32API NTSTATUS ZwCreateToken();
W32API NTSTATUS NtCreateToken();
W32API NTSTATUS ZwCreateTokenEx();
W32API NTSTATUS NtCreateTokenEx();
W32API NTSTATUS ZwCreateTransaction();
W32API NTSTATUS NtCreateTransaction();
W32API NTSTATUS ZwCreateTransactionManager();
W32API NTSTATUS NtCreateTransactionManager();
W32API NTSTATUS ZwCreateUserProcess();
W32API NTSTATUS NtCreateUserProcess();
W32API NTSTATUS ZwCreateWnfStateName();
W32API NTSTATUS NtCreateWnfStateName();
W32API NTSTATUS ZwDebugActiveProcess();
W32API NTSTATUS NtDebugActiveProcess();
W32API NTSTATUS ZwDebugContinue();
W32API NTSTATUS NtDebugContinue();
W32API NTSTATUS ZwDeleteAtom();
W32API NTSTATUS NtDeleteAtom();
W32API NTSTATUS ZwDeleteFile();
W32API NTSTATUS NtDeleteFile();
W32API NTSTATUS ZwDeleteKey();
W32API NTSTATUS NtDeleteKey();
W32API NTSTATUS ZwDeleteObjectAuditAlarm();
W32API NTSTATUS NtDeleteObjectAuditAlarm();
W32API NTSTATUS ZwDeletePrivateNamespace();
W32API NTSTATUS NtDeletePrivateNamespace();
W32API NTSTATUS ZwDeleteValueKey();
W32API NTSTATUS NtDeleteValueKey();
W32API NTSTATUS ZwDeleteWnfStateData();
W32API NTSTATUS NtDeleteWnfStateData();
W32API NTSTATUS ZwDeleteWnfStateName();
W32API NTSTATUS NtDeleteWnfStateName();
W32API NTSTATUS ZwDirectGraphicsCall();
W32API NTSTATUS NtDirectGraphicsCall();
W32API NTSTATUS ZwDisplayString();
W32API NTSTATUS NtDisplayString();
W32API NTSTATUS ZwDrawText();
W32API NTSTATUS NtDrawText();
W32API NTSTATUS ZwDuplicateObject();
W32API NTSTATUS NtDuplicateObject();
W32API NTSTATUS ZwDuplicateToken();
W32API NTSTATUS NtDuplicateToken();
W32API NTSTATUS ZwEnumerateKey();
W32API NTSTATUS NtEnumerateKey();
W32API NTSTATUS ZwEnumerateSystemEnvironmentValuesEx();
W32API NTSTATUS NtEnumerateSystemEnvironmentValuesEx();
W32API NTSTATUS ZwEnumerateTransactionObject();
W32API NTSTATUS NtEnumerateTransactionObject();
W32API NTSTATUS ZwEnumerateValueKey();
W32API NTSTATUS NtEnumerateValueKey();
W32API NTSTATUS ZwFilterBootOption();
W32API NTSTATUS NtFilterBootOption();
W32API NTSTATUS ZwFilterToken();
W32API NTSTATUS NtFilterToken();
W32API NTSTATUS ZwFilterTokenEx();
W32API NTSTATUS NtFilterTokenEx();
W32API NTSTATUS ZwFindAtom();
W32API NTSTATUS NtFindAtom();
W32API NTSTATUS ZwFlushInstallUILanguage();
W32API NTSTATUS NtFlushInstallUILanguage();
W32API NTSTATUS ZwFlushKey();
W32API NTSTATUS NtFlushKey();
W32API NTSTATUS ZwFlushWriteBuffer();
W32API NTSTATUS NtFlushWriteBuffer();
W32API NTSTATUS ZwFreeUserPhysicalPages();
W32API NTSTATUS NtFreeUserPhysicalPages();
W32API NTSTATUS ZwFreezeRegistry();
W32API NTSTATUS NtFreezeRegistry();
W32API NTSTATUS ZwFreezeTransactions();
W32API NTSTATUS NtFreezeTransactions();
W32API NTSTATUS ZwGetCompleteWnfStateSubscription();
W32API NTSTATUS NtGetCompleteWnfStateSubscription();
W32API NTSTATUS ZwGetContextThread();
W32API NTSTATUS NtGetContextThread();
W32API NTSTATUS ZwGetMUIRegistryInfo();
W32API NTSTATUS NtGetMUIRegistryInfo();
W32API NTSTATUS ZwGetNextProcess();
W32API NTSTATUS NtGetNextProcess();
W32API NTSTATUS ZwGetNextThread();
W32API NTSTATUS NtGetNextThread();
W32API NTSTATUS ZwGetNotificationResourceManager();
W32API NTSTATUS NtGetNotificationResourceManager();
W32API NTSTATUS ZwImpersonateAnonymousToken();
W32API NTSTATUS NtImpersonateAnonymousToken();
W32API NTSTATUS ZwImpersonateClientOfPort();
W32API NTSTATUS NtImpersonateClientOfPort();
W32API NTSTATUS ZwImpersonateThread();
W32API NTSTATUS NtImpersonateThread();
W32API NTSTATUS ZwInitializeEnclave();
W32API NTSTATUS NtInitializeEnclave();
W32API NTSTATUS ZwInitializeNlsFiles();
W32API NTSTATUS NtInitializeNlsFiles();
W32API NTSTATUS ZwInitializeRegistry();
W32API NTSTATUS NtInitializeRegistry();
W32API NTSTATUS ZwLoadEnclaveData();
W32API NTSTATUS NtLoadEnclaveData();
W32API NTSTATUS ZwLoadKey();
W32API NTSTATUS NtLoadKey();
W32API NTSTATUS ZwLoadKey2();
W32API NTSTATUS NtLoadKey2();
W32API NTSTATUS ZwLoadKey3();
W32API NTSTATUS NtLoadKey3();
W32API NTSTATUS ZwLoadKeyEx();
W32API NTSTATUS NtLoadKeyEx();
W32API NTSTATUS ZwMapCMFModule();
W32API NTSTATUS NtMapCMFModule();
W32API NTSTATUS ZwNotifyChangeDirectoryFile();
W32API NTSTATUS NtNotifyChangeDirectoryFile();
W32API NTSTATUS ZwNotifyChangeDirectoryFileEx();
W32API NTSTATUS NtNotifyChangeDirectoryFileEx();
W32API NTSTATUS ZwNotifyChangeKey();
W32API NTSTATUS NtNotifyChangeKey();
W32API NTSTATUS ZwNotifyChangeMultipleKeys();
W32API NTSTATUS NtNotifyChangeMultipleKeys();
W32API NTSTATUS ZwOpenCpuPartition();
W32API NTSTATUS NtOpenCpuPartition();
W32API NTSTATUS ZwOpenDirectoryObject();
W32API NTSTATUS NtOpenDirectoryObject();
W32API NTSTATUS ZwOpenEnlistment();
W32API NTSTATUS NtOpenEnlistment();
W32API NTSTATUS ZwOpenEventPair();
W32API NTSTATUS NtOpenEventPair();
W32API NTSTATUS ZwOpenIoCompletion();
W32API NTSTATUS NtOpenIoCompletion();
W32API NTSTATUS ZwOpenJobObject();
W32API NTSTATUS NtOpenJobObject();
W32API NTSTATUS ZwOpenKeyEx();
W32API NTSTATUS NtOpenKeyEx();
W32API NTSTATUS ZwOpenKeyTransacted();
W32API NTSTATUS NtOpenKeyTransacted();
W32API NTSTATUS ZwOpenKeyTransactedEx();
W32API NTSTATUS NtOpenKeyTransactedEx();
W32API NTSTATUS ZwOpenKeyedEvent();
W32API NTSTATUS NtOpenKeyedEvent();
W32API NTSTATUS ZwOpenMutant();
W32API NTSTATUS NtOpenMutant();
W32API NTSTATUS ZwOpenObjectAuditAlarm();
W32API NTSTATUS NtOpenObjectAuditAlarm();
W32API NTSTATUS ZwOpenPartition();
W32API NTSTATUS NtOpenPartition();
W32API NTSTATUS ZwOpenPrivateNamespace();
W32API NTSTATUS NtOpenPrivateNamespace();
W32API NTSTATUS ZwOpenProcessToken();
W32API NTSTATUS NtOpenProcessToken();
W32API NTSTATUS ZwOpenProcessTokenEx();
W32API NTSTATUS NtOpenProcessTokenEx();
W32API NTSTATUS ZwOpenRegistryTransaction();
W32API NTSTATUS NtOpenRegistryTransaction();
W32API NTSTATUS ZwOpenResourceManager();
W32API NTSTATUS NtOpenResourceManager();
W32API NTSTATUS ZwOpenSection();
W32API NTSTATUS NtOpenSection();
W32API NTSTATUS ZwOpenSession();
W32API NTSTATUS NtOpenSession();
W32API NTSTATUS ZwOpenSymbolicLinkObject();
W32API NTSTATUS NtOpenSymbolicLinkObject();
W32API NTSTATUS ZwOpenThreadToken();
W32API NTSTATUS NtOpenThreadToken();
W32API NTSTATUS ZwOpenThreadTokenEx();
W32API NTSTATUS NtOpenThreadTokenEx();
W32API NTSTATUS ZwOpenTransaction();
W32API NTSTATUS NtOpenTransaction();
W32API NTSTATUS ZwOpenTransactionManager();
W32API NTSTATUS NtOpenTransactionManager();
W32API NTSTATUS ZwPlugPlayControl();
W32API NTSTATUS NtPlugPlayControl();
W32API NTSTATUS ZwPrivilegeCheck();
W32API NTSTATUS NtPrivilegeCheck();
W32API NTSTATUS ZwPrivilegeObjectAuditAlarm();
W32API NTSTATUS NtPrivilegeObjectAuditAlarm();
W32API NTSTATUS ZwPrivilegedServiceAuditAlarm();
W32API NTSTATUS NtPrivilegedServiceAuditAlarm();
W32API NTSTATUS ZwPssCaptureVaSpaceBulk();
W32API NTSTATUS NtPssCaptureVaSpaceBulk();
W32API NTSTATUS ZwQueryAttributesFile();
W32API NTSTATUS NtQueryAttributesFile();
W32API NTSTATUS ZwQueryAuxiliaryCounterFrequency();
W32API NTSTATUS NtQueryAuxiliaryCounterFrequency();
W32API NTSTATUS ZwQueryDebugFilterState();
W32API NTSTATUS NtQueryDebugFilterState();
W32API NTSTATUS ZwQueryDefaultLocale();
W32API NTSTATUS NtQueryDefaultLocale();
W32API NTSTATUS ZwQueryDefaultUILanguage();
W32API NTSTATUS NtQueryDefaultUILanguage();
W32API NTSTATUS ZwQueryDirectoryFile();
W32API NTSTATUS NtQueryDirectoryFile();
W32API NTSTATUS ZwQueryDirectoryFileEx();
W32API NTSTATUS NtQueryDirectoryFileEx();
W32API NTSTATUS ZwQueryDirectoryObject();
W32API NTSTATUS NtQueryDirectoryObject();
W32API NTSTATUS ZwQueryDriverEntryOrder();
W32API NTSTATUS NtQueryDriverEntryOrder();
W32API NTSTATUS ZwQueryEaFile();
W32API NTSTATUS NtQueryEaFile();
W32API NTSTATUS ZwQueryFullAttributesFile();
W32API NTSTATUS NtQueryFullAttributesFile();
W32API NTSTATUS ZwQueryInformationAtom();
W32API NTSTATUS NtQueryInformationAtom();
W32API NTSTATUS ZwQueryInformationByName();
W32API NTSTATUS NtQueryInformationByName();
W32API NTSTATUS ZwQueryInformationCpuPartition();
W32API NTSTATUS NtQueryInformationCpuPartition();
W32API NTSTATUS ZwQueryInformationEnlistment();
W32API NTSTATUS NtQueryInformationEnlistment();
W32API NTSTATUS ZwQueryInformationJobObject();
W32API NTSTATUS NtQueryInformationJobObject();
W32API NTSTATUS ZwQueryInformationResourceManager();
W32API NTSTATUS NtQueryInformationResourceManager();
W32API NTSTATUS ZwQueryInformationToken();
W32API NTSTATUS NtQueryInformationToken();
W32API NTSTATUS ZwQueryInformationTransaction();
W32API NTSTATUS NtQueryInformationTransaction();
W32API NTSTATUS ZwQueryInformationTransactionManager();
W32API NTSTATUS NtQueryInformationTransactionManager();
W32API NTSTATUS ZwQueryInstallUILanguage();
W32API NTSTATUS NtQueryInstallUILanguage();
W32API NTSTATUS ZwQueryIoCompletion();
W32API NTSTATUS NtQueryIoCompletion();
W32API NTSTATUS ZwQueryIoRingCapabilities();
W32API NTSTATUS NtQueryIoRingCapabilities();
W32API NTSTATUS ZwQueryLicenseValue();
W32API NTSTATUS NtQueryLicenseValue();
W32API NTSTATUS ZwQueryMultipleValueKey();
W32API NTSTATUS NtQueryMultipleValueKey();
W32API NTSTATUS ZwQueryMutant();
W32API NTSTATUS NtQueryMutant();
W32API NTSTATUS ZwQueryObject();
W32API NTSTATUS NtQueryObject();
W32API NTSTATUS ZwQueryOpenSubKeys();
W32API NTSTATUS NtQueryOpenSubKeys();
W32API NTSTATUS ZwQueryOpenSubKeysEx();
W32API NTSTATUS NtQueryOpenSubKeysEx();
W32API NTSTATUS ZwQueryPortInformationProcess();
W32API NTSTATUS NtQueryPortInformationProcess();
W32API NTSTATUS ZwQueryQuotaInformationFile();
W32API NTSTATUS NtQueryQuotaInformationFile();
W32API NTSTATUS ZwQuerySection();
W32API NTSTATUS NtQuerySection();
W32API NTSTATUS ZwQuerySecurityAttributesToken();
W32API NTSTATUS NtQuerySecurityAttributesToken();
W32API NTSTATUS ZwQuerySecurityObject();
W32API NTSTATUS NtQuerySecurityObject();
W32API NTSTATUS ZwQuerySecurityPolicy();
W32API NTSTATUS NtQuerySecurityPolicy();
W32API NTSTATUS ZwQuerySymbolicLinkObject();
W32API NTSTATUS NtQuerySymbolicLinkObject();
W32API NTSTATUS ZwQuerySystemEnvironmentValue();
W32API NTSTATUS NtQuerySystemEnvironmentValue();
W32API NTSTATUS ZwQuerySystemEnvironmentValueEx();
W32API NTSTATUS NtQuerySystemEnvironmentValueEx();
W32API NTSTATUS ZwQuerySystemInformationEx();
W32API NTSTATUS NtQuerySystemInformationEx();
W32API NTSTATUS ZwQuerySystemTime();
W32API NTSTATUS NtQuerySystemTime();
W32API NTSTATUS ZwQueryTimer();
W32API NTSTATUS NtQueryTimer();
W32API NTSTATUS ZwQueryValueKey();
W32API NTSTATUS NtQueryValueKey();
W32API NTSTATUS ZwQueryVolumeInformationFile();
W32API NTSTATUS NtQueryVolumeInformationFile();
W32API NTSTATUS ZwQueryWnfStateData();
W32API NTSTATUS NtQueryWnfStateData();
W32API NTSTATUS ZwQueryWnfStateNameInformation();
W32API NTSTATUS NtQueryWnfStateNameInformation();
W32API NTSTATUS ZwQueueApcThread();
W32API NTSTATUS NtQueueApcThread();
W32API NTSTATUS ZwQueueApcThreadEx();
W32API NTSTATUS NtQueueApcThreadEx();
W32API NTSTATUS ZwQueueApcThreadEx2();
W32API NTSTATUS NtQueueApcThreadEx2();
W32API NTSTATUS ZwRaiseException();
W32API NTSTATUS NtRaiseException();
W32API NTSTATUS ZwRaiseHardError();
W32API NTSTATUS NtRaiseHardError();
W32API NTSTATUS ZwReadOnlyEnlistment();
W32API NTSTATUS NtReadOnlyEnlistment();
W32API NTSTATUS ZwReadRequestData();
W32API NTSTATUS NtReadRequestData();
W32API NTSTATUS ZwRegisterProtocolAddressInformation();
W32API NTSTATUS NtRegisterProtocolAddressInformation();
W32API NTSTATUS ZwRegisterThreadTerminatePort();
W32API NTSTATUS NtRegisterThreadTerminatePort();
W32API NTSTATUS ZwReleaseKeyedEvent();
W32API NTSTATUS NtReleaseKeyedEvent();
W32API NTSTATUS ZwReleaseMutant();
W32API NTSTATUS NtReleaseMutant();
W32API NTSTATUS ZwRemoveIoCompletion();
W32API NTSTATUS NtRemoveIoCompletion();
W32API NTSTATUS ZwRemoveIoCompletionEx();
W32API NTSTATUS NtRemoveIoCompletionEx();
W32API NTSTATUS ZwRemoveProcessDebug();
W32API NTSTATUS NtRemoveProcessDebug();
W32API NTSTATUS ZwRenameKey();
W32API NTSTATUS NtRenameKey();
W32API NTSTATUS ZwReplaceKey();
W32API NTSTATUS NtReplaceKey();
W32API NTSTATUS ZwReplacePartitionUnit();
W32API NTSTATUS NtReplacePartitionUnit();
W32API NTSTATUS ZwRequestWaitReplyPort();
W32API NTSTATUS NtRequestWaitReplyPort();
W32API NTSTATUS ZwResetWriteWatch();
W32API NTSTATUS NtResetWriteWatch();
W32API NTSTATUS ZwRestoreKey();
W32API NTSTATUS NtRestoreKey();
W32API NTSTATUS ZwResumeProcess();
W32API NTSTATUS NtResumeProcess();
W32API NTSTATUS ZwResumeThread();
W32API NTSTATUS NtResumeThread();
W32API NTSTATUS ZwRevertContainerImpersonation();
W32API NTSTATUS NtRevertContainerImpersonation();
W32API NTSTATUS ZwSaveKey();
W32API NTSTATUS NtSaveKey();
W32API NTSTATUS ZwSaveKeyEx();
W32API NTSTATUS NtSaveKeyEx();
W32API NTSTATUS ZwSaveMergedKeys();
W32API NTSTATUS NtSaveMergedKeys();
W32API NTSTATUS ZwSetCachedSigningLevel();
W32API NTSTATUS NtSetCachedSigningLevel();
W32API NTSTATUS ZwSetCachedSigningLevel2();
W32API NTSTATUS NtSetCachedSigningLevel2();
W32API NTSTATUS ZwSetContextThread();
W32API NTSTATUS NtSetContextThread();
W32API NTSTATUS ZwSetDebugFilterState();
W32API NTSTATUS NtSetDebugFilterState();
W32API NTSTATUS ZwSetDefaultHardErrorPort();
W32API NTSTATUS NtSetDefaultHardErrorPort();
W32API NTSTATUS ZwSetDefaultLocale();
W32API NTSTATUS NtSetDefaultLocale();
W32API NTSTATUS ZwSetDefaultUILanguage();
W32API NTSTATUS NtSetDefaultUILanguage();
W32API NTSTATUS ZwSetDriverEntryOrder();
W32API NTSTATUS NtSetDriverEntryOrder();
W32API NTSTATUS ZwSetEaFile();
W32API NTSTATUS NtSetEaFile();
W32API NTSTATUS ZwSetEventBoostPriority();
W32API NTSTATUS NtSetEventBoostPriority();
W32API NTSTATUS ZwSetEventEx();
W32API NTSTATUS NtSetEventEx();
W32API NTSTATUS ZwSetHighEventPair();
W32API NTSTATUS NtSetHighEventPair();
W32API NTSTATUS ZwSetHighWaitLowEventPair();
W32API NTSTATUS NtSetHighWaitLowEventPair();
W32API NTSTATUS ZwSetIRTimer();
W32API NTSTATUS NtSetIRTimer();
W32API NTSTATUS ZwSetInformationCpuPartition();
W32API NTSTATUS NtSetInformationCpuPartition();
W32API NTSTATUS ZwSetInformationDebugObject();
W32API NTSTATUS NtSetInformationDebugObject();
W32API NTSTATUS ZwSetInformationEnlistment();
W32API NTSTATUS NtSetInformationEnlistment();
W32API NTSTATUS ZwSetInformationIoRing();
W32API NTSTATUS NtSetInformationIoRing();
W32API NTSTATUS ZwSetInformationJobObject();
W32API NTSTATUS NtSetInformationJobObject();
W32API NTSTATUS ZwSetInformationKey();
W32API NTSTATUS NtSetInformationKey();
W32API NTSTATUS ZwSetInformationObject();
W32API NTSTATUS NtSetInformationObject();
W32API NTSTATUS ZwSetInformationProcess();
W32API NTSTATUS NtSetInformationProcess();
W32API NTSTATUS ZwSetInformationResourceManager();
W32API NTSTATUS NtSetInformationResourceManager();
W32API NTSTATUS ZwSetInformationSymbolicLink();
W32API NTSTATUS NtSetInformationSymbolicLink();
W32API NTSTATUS ZwSetInformationThread();
W32API NTSTATUS NtSetInformationThread();
W32API NTSTATUS ZwSetInformationToken();
W32API NTSTATUS NtSetInformationToken();
W32API NTSTATUS ZwSetInformationTransaction();
W32API NTSTATUS NtSetInformationTransaction();
W32API NTSTATUS ZwSetInformationTransactionManager();
W32API NTSTATUS NtSetInformationTransactionManager();
W32API NTSTATUS ZwSetInformationVirtualMemory();
W32API NTSTATUS NtSetInformationVirtualMemory();
W32API NTSTATUS ZwSetIoCompletion();
W32API NTSTATUS NtSetIoCompletion();
W32API NTSTATUS ZwSetIoCompletionEx();
W32API NTSTATUS NtSetIoCompletionEx();
W32API NTSTATUS ZwSetLdtEntries();
W32API NTSTATUS NtSetLdtEntries();
W32API NTSTATUS ZwSetLowEventPair();
W32API NTSTATUS NtSetLowEventPair();
W32API NTSTATUS ZwSetLowWaitHighEventPair();
W32API NTSTATUS NtSetLowWaitHighEventPair();
W32API NTSTATUS ZwSetQuotaInformationFile();
W32API NTSTATUS NtSetQuotaInformationFile();
W32API NTSTATUS ZwSetSecurityObject();
W32API NTSTATUS NtSetSecurityObject();
W32API NTSTATUS ZwSetSystemEnvironmentValue();
W32API NTSTATUS NtSetSystemEnvironmentValue();
W32API NTSTATUS ZwSetSystemEnvironmentValueEx();
W32API NTSTATUS NtSetSystemEnvironmentValueEx();
W32API NTSTATUS ZwSetSystemInformation();
W32API NTSTATUS NtSetSystemInformation();
W32API NTSTATUS ZwSetSystemPowerState();
W32API NTSTATUS NtSetSystemPowerState();
W32API NTSTATUS ZwSetThreadExecutionState();
W32API NTSTATUS NtSetThreadExecutionState();
W32API NTSTATUS ZwSetTimer2();
W32API NTSTATUS NtSetTimer2();
W32API NTSTATUS ZwSetTimerEx();
W32API NTSTATUS NtSetTimerEx();
W32API NTSTATUS ZwSetUuidSeed();
W32API NTSTATUS NtSetUuidSeed();
W32API NTSTATUS ZwSetValueKey();
W32API NTSTATUS NtSetValueKey();
W32API NTSTATUS ZwSetVolumeInformationFile();
W32API NTSTATUS NtSetVolumeInformationFile();
W32API NTSTATUS ZwSetWnfProcessNotificationEvent();
W32API NTSTATUS NtSetWnfProcessNotificationEvent();
W32API NTSTATUS ZwStartProfile();
W32API NTSTATUS NtStartProfile();
W32API NTSTATUS ZwStopProfile();
W32API NTSTATUS NtStopProfile();
W32API NTSTATUS ZwSubmitIoRing();
W32API NTSTATUS NtSubmitIoRing();
W32API NTSTATUS ZwSubscribeWnfStateChange();
W32API NTSTATUS NtSubscribeWnfStateChange();
W32API NTSTATUS ZwSuspendProcess();
W32API NTSTATUS NtSuspendProcess();
W32API NTSTATUS ZwSuspendThread();
W32API NTSTATUS NtSuspendThread();
W32API NTSTATUS ZwSystemDebugControl();
W32API NTSTATUS NtSystemDebugControl();
W32API NTSTATUS ZwTerminateEnclave();
W32API NTSTATUS NtTerminateEnclave();
W32API NTSTATUS ZwTerminateJobObject();
W32API NTSTATUS NtTerminateJobObject();
W32API NTSTATUS ZwTestAlert();
W32API NTSTATUS NtTestAlert();
W32API NTSTATUS ZwThawRegistry();
W32API NTSTATUS NtThawRegistry();
W32API NTSTATUS ZwThawTransactions();
W32API NTSTATUS NtThawTransactions();
W32API NTSTATUS ZwTraceEvent();
W32API NTSTATUS NtTraceEvent();
W32API NTSTATUS ZwTranslateFilePath();
W32API NTSTATUS NtTranslateFilePath();
W32API NTSTATUS ZwUmsThreadYield();
W32API NTSTATUS NtUmsThreadYield();
W32API NTSTATUS ZwUnloadDriver();
W32API NTSTATUS NtUnloadDriver();
W32API NTSTATUS ZwUnloadKey();
W32API NTSTATUS NtUnloadKey();
W32API NTSTATUS ZwUnloadKey2();
W32API NTSTATUS NtUnloadKey2();
W32API NTSTATUS ZwUnloadKeyEx();
W32API NTSTATUS NtUnloadKeyEx();
W32API NTSTATUS ZwUnsubscribeWnfStateChange();
W32API NTSTATUS NtUnsubscribeWnfStateChange();
W32API NTSTATUS ZwUpdateWnfStateData();
W32API NTSTATUS NtUpdateWnfStateData();
W32API NTSTATUS ZwVdmControl();
W32API NTSTATUS NtVdmControl();
W32API NTSTATUS ZwWaitForAlertByThreadId();
W32API NTSTATUS NtWaitForAlertByThreadId();
W32API NTSTATUS ZwWaitForDebugEvent();
W32API NTSTATUS NtWaitForDebugEvent();
W32API NTSTATUS ZwWaitForKeyedEvent();
W32API NTSTATUS NtWaitForKeyedEvent();
W32API NTSTATUS ZwWaitForWorkViaWorkerFactory();
W32API NTSTATUS NtWaitForWorkViaWorkerFactory();
W32API NTSTATUS ZwWaitHighEventPair();
W32API NTSTATUS NtWaitHighEventPair();
W32API NTSTATUS ZwWaitLowEventPair();
W32API NTSTATUS NtWaitLowEventPair();
W32API NTSTATUS ZwWriteRequestData();
W32API NTSTATUS NtWriteRequestData();
/* diagnostics */
W32API const char *nt_name(W32 nr);

#ifdef __cplusplus
}
#endif
#endif
