/* platform_win32.c - window, keyboard, sound and timing on Windows.
 *
 * Plain Win32: GDI for the picture, waveOut for sound.  No third-party
 * libraries are needed to build a recompiled program.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include "internal.h"

#define AUDIO_BUFFERS 8
#define AUDIO_FRAMES 512

static HWND g_wnd;
static int g_quit;
static uint32_t g_keys;
static uint32_t g_frame[GP32_LCD_W * GP32_LCD_H];

static HWAVEOUT g_wave;
static WAVEHDR g_hdr[AUDIO_BUFFERS];
static int16_t g_pcm[AUDIO_BUFFERS][AUDIO_FRAMES * 2];
static int g_cur = -1, g_fill, g_rate;

static uint32_t key_bit(WPARAM vk) {
    switch (vk) {
    case VK_LEFT: return GP32_KEY_LEFT;
    case VK_RIGHT: return GP32_KEY_RIGHT;
    case VK_UP: return GP32_KEY_UP;
    case VK_DOWN: return GP32_KEY_DOWN;
    case 'X': case VK_SPACE: return GP32_KEY_A;
    case 'Z': case VK_CONTROL: return GP32_KEY_B;
    case 'A': return GP32_KEY_L;
    case 'S': return GP32_KEY_R;
    case VK_RETURN: return GP32_KEY_START;
    case VK_BACK: case VK_TAB: return GP32_KEY_SELECT;
    default: return 0;
    }
}

static void blit(HDC dc) {
    BITMAPINFO bi;
    RECT rc;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = GP32_LCD_W;
    bi.bmiHeader.biHeight = -GP32_LCD_H; /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    GetClientRect(g_wnd, &rc);
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, GP32_LCD_W, GP32_LCD_H, g_frame, &bi, DIB_RGB_COLORS, SRCCOPY);
}

static LRESULT CALLBACK wnd_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE:
        g_quit = 1;
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) g_quit = 1;
        g_keys |= key_bit(wp);
        return 0;
    case WM_KEYUP:
        g_keys &= ~key_bit(wp);
        return 0;
    case WM_KILLFOCUS:
        g_keys = 0;
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(wnd, &ps);
        blit(dc);
        EndPaint(wnd, &ps);
        return 0;
    }
    default:
        return DefWindowProcA(wnd, msg, wp, lp);
    }
}

int plat_init(const char *title, int scale) {
    WNDCLASSA wc;
    RECT rc;
    DWORD style = WS_OVERLAPPEDWINDOW;
    HINSTANCE inst = GetModuleHandleA(NULL);
    if (gp32_opt.frames) return 0; /* batch run: no window */
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.lpszClassName = "gp32recomp";
    if (!RegisterClassA(&wc)) return -1;
    rc.left = 0;
    rc.top = 0;
    rc.right = GP32_LCD_W * scale;
    rc.bottom = GP32_LCD_H * scale;
    AdjustWindowRect(&rc, style, FALSE);
    g_wnd = CreateWindowA("gp32recomp", title && *title ? title : "GP32", style, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left,
                          rc.bottom - rc.top, NULL, NULL, inst, NULL);
    if (!g_wnd) return -1;
    ShowWindow(g_wnd, SW_SHOW);
    timeBeginPeriod(1);
    return 0;
}

static void audio_close(void) {
    int i;
    if (!g_wave) return;
    waveOutReset(g_wave);
    for (i = 0; i < AUDIO_BUFFERS; i++)
        if (g_hdr[i].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(g_wave, &g_hdr[i], sizeof(WAVEHDR));
    waveOutClose(g_wave);
    g_wave = NULL;
}

void plat_shutdown(void) {
    audio_close();
    if (g_wnd) {
        DestroyWindow(g_wnd);
        g_wnd = NULL;
        timeEndPeriod(1);
    }
}

int plat_present(const uint32_t *xrgb) {
    MSG msg;
    if (!g_wnd) return 0;
    memcpy(g_frame, xrgb, sizeof(g_frame));
    {
        HDC dc = GetDC(g_wnd);
        blit(dc);
        ReleaseDC(g_wnd, dc);
    }
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return g_quit;
}

uint32_t plat_buttons(void) { return g_keys; }

static void audio_open(int rate) {
    WAVEFORMATEX wf;
    audio_close();
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = (DWORD)rate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = (DWORD)rate * 4;
    memset(g_hdr, 0, sizeof(g_hdr));
    g_cur = -1;
    g_rate = rate;
    if (waveOutOpen(&g_wave, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) g_wave = NULL;
}

static int audio_free_buffer(void) {
    int i;
    for (i = 0; i < AUDIO_BUFFERS; i++) {
        if (g_hdr[i].dwFlags & WHDR_INQUEUE) continue;
        if (g_hdr[i].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(g_wave, &g_hdr[i], sizeof(WAVEHDR));
        return i;
    }
    return -1;
}

void plat_audio(const int16_t *stereo, int frames, int rate) {
    if (!g_wnd) return;
    if (rate != g_rate) audio_open(rate);
    if (!g_wave) return;
    while (frames > 0) {
        int n;
        if (g_cur < 0) {
            g_cur = audio_free_buffer();
            if (g_cur < 0) return; /* the device is behind: drop rather than stall the game */
            g_fill = 0;
        }
        n = AUDIO_FRAMES - g_fill;
        if (n > frames) n = frames;
        memcpy(&g_pcm[g_cur][g_fill * 2], stereo, (size_t)n * 4);
        g_fill += n;
        stereo += n * 2;
        frames -= n;
        if (g_fill == AUDIO_FRAMES) {
            WAVEHDR *h = &g_hdr[g_cur];
            memset(h, 0, sizeof(*h));
            h->lpData = (LPSTR)g_pcm[g_cur];
            h->dwBufferLength = AUDIO_FRAMES * 4;
            if (waveOutPrepareHeader(g_wave, h, sizeof(*h)) == MMSYSERR_NOERROR) waveOutWrite(g_wave, h, sizeof(*h));
            g_cur = -1;
        }
    }
}

uint64_t plat_time_us(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (uint64_t)(now.QuadPart / freq.QuadPart) * 1000000u + (uint64_t)(now.QuadPart % freq.QuadPart) * 1000000u / (uint64_t)freq.QuadPart;
}

void plat_sleep_us(uint64_t us) {
    if (us >= 1000) Sleep((DWORD)(us / 1000));
}
