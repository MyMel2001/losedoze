/* notes.c — homemade Win32 demo app for losedoze (clean-room, original).
 * A tiny notepad: type text, backspace, Enter, lines scroll, autosaves to
 * /tmp/notes.txt on close. Exercises user32 message loop, gdi32 text,
 * kernel32 file IO over the NT layer. Built as PE32+ (clang windows-msvc). */
#include "w32.h"

#define MAXCH 6000
#define COLS 78
#define LINES 40
#define LH 18
#define PAD 10

static W16 text_[MAXCH];
static int tlen;
static int cx_, cy_;            /* caret: column, line */
static HWND hwnd_;
static char status_[96];

static char utf8buf[MAXCH * 2];

static void save_buffer(void) {
  HANDLE f = CreateFileW((LPCWSTR)L"/tmp/notes.txt", GENERIC_WRITE, 0, 0,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
  if (!f || f == INVALID_HANDLE_VALUE) return;
  char *utf8 = utf8buf;
  int n = 0;
  for (int i = 0; i < tlen && n < (int)sizeof utf8 - 1; i++)
    utf8[n++] = text_[i] < 0x80 ? (char)text_[i] : '?';
  DWORD w;
  WriteFile(f, utf8, (DWORD)n, &w, 0);
  CloseHandle(f);
}

static void draw_caret(HDC dc) {
  int x = PAD + cx_ * 8, y = PAD + cy_ * LH;
  RECT r = { x, y + 2, x + 7, y + 16 };
  HBRUSH b = CreateSolidBrush(W32RGB(30, 90, 200));
  FillRect(dc, &r, b);
  DeleteObject(b);
}

static void paint(HWND w) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(w, &ps);
  RECT rc;
  GetClientRect(w, &rc);
  RECT bg = { 0, 0, rc.right, rc.bottom - 26 };
  HBRUSH white = GetStockObject(WHITE_BRUSH);
  FillRect(dc, &bg, white);

  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, W32RGB(20, 20, 20));
  /* draw text lines */
  int ln = 0, col = 0;
  W16 line[COLS + 1];
  int llen = 0;
  for (int i = 0; i <= tlen; i++) {
    W16 c = i < tlen ? text_[i] : '\n';
    if (c == '\n' || llen >= COLS) {
      if (ln < LINES) TextOutW(dc, PAD, PAD + ln * LH, line, llen);
      if (c == '\n') {
        ln++;
        if (ln >= LINES) { ln = LINES - 1; }
      }
      llen = 0;
      if (c == '\n') continue;
    }
    if (llen < COLS) line[llen++] = c;
  }
  draw_caret(dc);

  /* status bar */
  RECT sb = { 0, rc.bottom - 26, rc.right, rc.bottom };
  HBRUSH gray = CreateSolidBrush(W32RGB(240, 240, 240));
  FillRect(dc, &sb, gray);
  DeleteObject(gray);
  SetTextColor(dc, W32RGB(90, 90, 90));
  wsprintfA(status_, "Losedoze Win32  |  Notes  |  chars: %d  |  saved: /tmp/notes.txt", tlen);
  TextOutA(dc, PAD, rc.bottom - 20, status_, -1);
  EndPaint(w, &ps);
}

static void insert_char(W16 c) {
  if (c == 13) c = '\n';
  if (tlen >= MAXCH - 1) return;
  if (c == 8) {                       /* backspace */
    if (tlen) {
      tlen--;
      if (text_[tlen] == '\n') { /* handled below */ }
    }
  } else {
    text_[tlen++] = c;
  }
  /* recompute caret pos */
  cx_ = 0;
  cy_ = 0;
  for (int i = 0; i < tlen; i++) {
    if (text_[i] == '\n') { cx_ = 0; cy_++; }
    else if (++cx_ >= COLS) { cx_ = 0; cy_++; }
  }
  if (cy_ >= LINES) {
    /* scroll: drop oldest line */
    int i = 0, dropped = 0;
    while (i < tlen && dropped <= cy_ - LINES) {
      if (text_[i] == '\n') dropped++;
      i++;
    }
    int rem = tlen - i;
    for (int k = 0; k < rem; k++) text_[k] = text_[i + k];
    tlen = rem;
    cy_ = LINES - 1;
  }
  InvalidateRect(hwnd_, 0, 1);
}

static LRESULT CALLBACK wndproc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
  case WM_CHAR:
    insert_char((W16)wp);
    return 0;
  case WM_PAINT:
    paint(w);
    return 0;
  case WM_DESTROY:
    save_buffer();
    PostQuitMessage(0);
    return 0;
  case WM_CLOSE:
    return DefWindowProcW(w, msg, wp, lp);
  default:
    return DefWindowProcW(w, msg, wp, lp);
  }
}

static void print(const char *s) {
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD n;
  WriteFile(out, s, (DWORD)lstrlenA(s), &n, 0);
}

void notes_entry(void) {
  WNDCLASSW wc;
  static W16 cname[] = {'N','o','t','e','s','C','l','a','s','s',0};
  static W16 title[] = {'N','o','t','e','s',' ','-',' ','L','o','s','e','d','o','z','e',' ','W','i','n','3','2',0};
  static W16 curi[] = {0, 0};
  wc.style = 0;
  wc.lpfnWndProc = wndproc;
  wc.cbClsExtra = wc.cbWndExtra = 0;
  wc.hInstance = GetModuleHandleW(0);
  wc.hIcon = LoadIconW(0, curi);
  wc.hCursor = LoadCursorW(0, curi);
  wc.hbrBackground = GetStockObject(WHITE_BRUSH);
  wc.lpszMenuName = 0;
  wc.lpszClassName = cname;
  RegisterClassExW(&wc);

  hwnd_ = CreateWindowExW(0, cname, title,
                          WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                          CW_USEDEFAULT, CW_USEDEFAULT, 660, 460,
                          0, 0, GetModuleHandleW(0), 0);
  if (!hwnd_) {
    print("LOSEDOZE WIN32 FAIL (no window)\r\n");
    ExitProcess(1);
  }

  /* marker files/status proving the full path (loader -> kernel32 -> NT) */
  print("LOSEDOZE WIN32 APP OK\r\n");
  HANDLE f = CreateFileW((LPCWSTR)L"/tmp/w32ok", GENERIC_WRITE, 0, 0,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
  if (f && f != INVALID_HANDLE_VALUE) {
    DWORD n;
    WriteFile(f, "WIN32OK\n", 8, &n, 0);
    CloseHandle(f);
  }

  MSG m;
  while (GetMessageW(&m, 0, 0, 0) > 0) {
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
  save_buffer();
  ExitProcess(0);
}
