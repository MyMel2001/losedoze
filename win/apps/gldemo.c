/* gldemo.c — desktop OpenGL demo app for the losedoze Win32 subsystem.
 * Creates a window, binds an opengl32.dll context (EGL, desktop GL compat
 * profile) to its DC, draws a triangle with fixed-function GL and presents it
 * with SwapBuffers. The marker is printed from the first frame that actually
 * reached the window, carrying the renderer/version reported by GL itself.
 * Original code. */
#include "w32.h"

static HGLRC glrc_;
static int presented_;            /* first successful present -> marker */

static void paint(HWND w) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(w, &ps);
  RECT rc;
  GetClientRect(w, &rc);
  if (!glrc_) glrc_ = wglCreateContext(dc);
  if (!glrc_ || !wglMakeCurrent(dc, glrc_)) {
    EndPaint(w, &ps);
    return;
  }

  glViewport(0, 0, rc.right, rc.bottom);
  glClearColor(0.04f, 0.06f, 0.09f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();

  glBegin(GL_TRIANGLES);
  glColor3f(1.0f, 0.25f, 0.25f);
  glVertex2f(-0.65f, -0.55f);
  glColor3f(0.25f, 1.0f, 0.35f);
  glVertex2f(0.65f, -0.55f);
  glColor3f(0.35f, 0.55f, 1.0f);
  glVertex2f(0.0f, 0.75f);
  glEnd();

  glFlush();
  int presented = SwapBuffers(dc);   /* readback -> window canvas */
  EndPaint(w, &ps);

  if (presented && !presented_) {
    const char *rs = (const char *)glGetString(GL_RENDERER);
    const char *vs = (const char *)glGetString(GL_VERSION);
    if (!rs) rs = "?";
    if (!vs) vs = "?";
    char line[224];
    wsprintfA(line, "LOSEDOZE GL DEMO OK %s | %s\r\n", rs, vs);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD n;
    WriteFile(out, line, (DWORD)lstrlenA(line), &n, 0);
    HANDLE f = CreateFileA("/tmp/w32glok", GENERIC_WRITE, 0, 0, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, 0);
    if (f && f != INVALID_HANDLE_VALUE) {
      WriteFile(f, line, (DWORD)lstrlenA(line), &n, 0);
      CloseHandle(f);
    }
    presented_ = 1;
  }
}

static LRESULT CALLBACK wndproc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
  case WM_PAINT:
    paint(w);
    return 0;
  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  default:
    return DefWindowProcW(w, msg, wp, lp);
  }
}

void gldemo_entry(void) {
  static W16 cname[] = {'G','l','D','e','m','o','C','l','s',0};
  static W16 title[] = {'G','L',' ','d','e','m','o',' ','-',' ','L','o','s','e','d','o','z','e',0};
  static W16 curi[] = {0, 0};
  WNDCLASSW wc;
  wc.style = 0;
  wc.lpfnWndProc = wndproc;
  wc.cbClsExtra = wc.cbWndExtra = 0;
  wc.hInstance = GetModuleHandleW(0);
  wc.hIcon = LoadIconW(0, curi);
  wc.hCursor = LoadCursorW(0, curi);
  wc.hbrBackground = GetStockObject(BLACK_BRUSH);
  wc.lpszMenuName = 0;
  wc.lpszClassName = cname;
  RegisterClassExW(&wc);

  HWND hwnd = CreateWindowExW(0, cname, title,
                               WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               CW_USEDEFAULT, CW_USEDEFAULT, 480, 400,
                               0, 0, GetModuleHandleW(0), 0);
  if (!hwnd) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD n;
    WriteFile(out, "LOSEDOZE GL DEMO FAIL (no window)\r\n", 35, &n, 0);
    ExitProcess(1);
  }
  InvalidateRect(hwnd, 0, 1);   /* force the first paint through the GL path */

  MSG m;
  while (GetMessageW(&m, 0, 0, 0) > 0) {
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
  ExitProcess(presented_ ? 0 : 1);
}
