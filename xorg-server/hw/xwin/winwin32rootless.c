/*
 *Copyright (C) 1994-2000 The XFree86 Project, Inc. All Rights Reserved.
 *
 *Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 *"Software"), to deal in the Software without restriction, including
 *without limitation the rights to use, copy, modify, merge, publish,
 *distribute, sublicense, and/or sell copies of the Software, and to
 *permit persons to whom the Software is furnished to do so, subject to
 *the following conditions:
 *
 *The above copyright notice and this permission notice shall be
 *included in all copies or substantial portions of the Software.
 *
 *THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 *EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 *NONINFRINGEMENT. IN NO EVENT SHALL THE XFREE86 PROJECT BE LIABLE FOR
 *ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 *CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 *WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 *Except as contained in this notice, the name of the XFree86 Project
 *shall not be used in advertising or otherwise to promote the sale, use
 *or other dealings in this Software without prior written authorization
 *from the XFree86 Project.
 *
 * Authors:	Kensuke Matsuzaki
 *		Earle F. Philhower, III
 *		Harold L Hunt II
 */
/*
 * Look at hw/darwin/quartz/xpr/xprFrame.c and hw/darwin/quartz/cr/crFrame.c
 */
#ifdef HAVE_XWIN_CONFIG_H
#include <xwin-config.h>
#endif
#include "win.h"
#include <winuser.h>
#define _WINDOWSWM_SERVER_
#include <X11/extensions/windowswmstr.h>
#include "winmultiwindowclass.h"
#include <X11/Xatom.h>

/*
 * Constant defines
 */

#ifndef ULW_COLORKEY
#define ULW_COLORKEY	0x00000001
#endif
#ifndef ULW_ALPHA
#define ULW_ALPHA	0x00000002
#endif
#ifndef ULW_OPAQUE
#define ULW_OPAQUE	0x00000004
#endif
#define AC_SRC_ALPHA	0x01

/*
 * Local function
 */

DEFINE_ATOM_HELPER(AtmWindowsWmNativeHwnd, WINDOWSWM_NATIVE_HWND)
static void
winMWExtWMSetNativeProperty(RootlessWindowPtr pFrame);

/*
 * Global variables
 */

Bool g_fNoConfigureWindow = FALSE;

/*
 * Internal function to get the DIB format that is compatible with the screen
 * Fixme: Share code with winshadgdi.c
 */

static
Bool
winMWExtWMQueryDIBFormat(win32RootlessWindowPtr pRLWinPriv,
                         BITMAPINFOHEADER * pbmih)
{
    HBITMAP hbmp;
#ifdef _DEBUG
    LPDWORD pdw = NULL;
#endif

    /* Create a memory bitmap compatible with the screen */
    hbmp = CreateCompatibleBitmap(pRLWinPriv->hdcScreen, 1, 1);
    if (hbmp == NULL) {
        ErrorF("winMWExtWMQueryDIBFormat - CreateCompatibleBitmap failed\n");
        return FALSE;
    }

    /* Initialize our bitmap info header */
    ZeroMemory(pbmih, sizeof(BITMAPINFOHEADER) + 256 * sizeof(RGBQUAD));
    pbmih->biSize = sizeof(BITMAPINFOHEADER);

    /* Get the biBitCount */
    if (!GetDIBits(pRLWinPriv->hdcScreen,
                   hbmp, 0, 1, NULL, (BITMAPINFO *) pbmih, DIB_RGB_COLORS)) {
        ErrorF("winMWExtWMQueryDIBFormat - First call to GetDIBits failed\n");
        DeleteObject(hbmp);
        return FALSE;
    }

#ifdef _DEBUG
    /* Get a pointer to bitfields */
    pdw = (DWORD *) ((CARD8 *) pbmih + sizeof(BITMAPINFOHEADER));

    winDebug("winMWExtWMQueryDIBFormat - First call masks: %08x %08x %08x\n",
             (unsigned int) pdw[0], (unsigned int) pdw[1],
             (unsigned int) pdw[2]);
#endif

    /* Get optimal color table, or the optimal bitfields */
    if (!GetDIBits(pRLWinPriv->hdcScreen,
                   hbmp, 0, 1, NULL, (BITMAPINFO *) pbmih, DIB_RGB_COLORS)) {
        ErrorF("winMWExtWMQueryDIBFormat - Second call to GetDIBits "
               "failed\n");
        DeleteObject(hbmp);
        return FALSE;
    }

    /* Free memory */
    DeleteObject(hbmp);

    return TRUE;
}

static HRGN
winMWExtWMCreateRgnFromRegion(RegionPtr pShape)
{
    int nRects;
    BoxPtr pRects, pEnd;
    HRGN hRgn, hRgnRect;

    if (pShape == NULL)
        return NULL;

    nRects = RegionNumRects(pShape);
    pRects = RegionRects(pShape);

    hRgn = CreateRectRgn(0, 0, 0, 0);
    if (hRgn == NULL) {
        ErrorF("winReshape - Initial CreateRectRgn (%d, %d, %d, %d) "
               "failed: %d\n", 0, 0, 0, 0, (int) GetLastError());
    }

    /* Loop through all rectangles in the X region */
    for (pEnd = pRects + nRects; pRects < pEnd; pRects++) {
        /* Create a Windows region for the X rectangle */
        hRgnRect = CreateRectRgn(pRects->x1,
                                 pRects->y1, pRects->x2, pRects->y2);
        if (hRgnRect == NULL) {
            ErrorF("winReshape - Loop CreateRectRgn (%d, %d, %d, %d) "
                   "failed: %d\n",
                   pRects->x1,
                   pRects->y1, pRects->x2, pRects->y2, (int) GetLastError());
        }

        /* Merge the Windows region with the accumulated region */
        if (CombineRgn(hRgn, hRgn, hRgnRect, RGN_OR) == ERROR) {
            ErrorF("winReshape - CombineRgn () failed: %d\n",
                   (int) GetLastError());
        }

        /* Delete the temporary Windows region */
        DeleteObject(hRgnRect);
    }

    return hRgn;
}

static void
InitWin32RootlessEngine(win32RootlessWindowPtr pRLWinPriv)
{
    pRLWinPriv->hdcScreen = GetDC(pRLWinPriv->hWnd);
    pRLWinPriv->hdcShadow = CreateCompatibleDC(pRLWinPriv->hdcScreen);
    pRLWinPriv->hbmpShadow = NULL;

    /* Allocate bitmap info header */
    pRLWinPriv->pbmihShadow =
        malloc(sizeof(BITMAPINFOHEADER)
               + 256 * sizeof(RGBQUAD));
    if (pRLWinPriv->pbmihShadow == NULL) {
        ErrorF("InitWin32RootlessEngine - malloc () failed\n");
        return;
    }

    /* Query the screen format */
    winMWExtWMQueryDIBFormat(pRLWinPriv, pRLWinPriv->pbmihShadow);
}

Bool
winMWExtWMCreateFrame(RootlessWindowPtr pFrame, ScreenPtr pScreen,
                      int newX, int newY, RegionPtr pShape)
{
#define CLASS_NAME_LENGTH 512
    Bool fResult = TRUE;
    win32RootlessWindowPtr pRLWinPriv;
    WNDCLASSEX wc;
    char pszClass[CLASS_NAME_LENGTH], pszWindowID[12];
    HICON hIcon;
    HICON hIconSmall;
    char *res_name, *res_class, *res_role;
    static int s_iWindowID = 0;

    winDebug("winMWExtWMCreateFrame %d %d - %d %d\n",
             newX, newY, pFrame->width, pFrame->height);

    pRLWinPriv = malloc(sizeof(win32RootlessWindowRec));
    pRLWinPriv->pFrame = pFrame;
    pRLWinPriv->pfb = NULL;
    pRLWinPriv->hbmpShadow = NULL;
    pRLWinPriv->hdcShadow = NULL;
    pRLWinPriv->hdcScreen = NULL;
    pRLWinPriv->pbmihShadow = NULL;
    pRLWinPriv->fResized = TRUE;
    pRLWinPriv->fClose = FALSE;
    pRLWinPriv->fRestackingNow = FALSE;
    pRLWinPriv->fDestroyed = FALSE;
    pRLWinPriv->fMovingOrSizing = FALSE;

    // Store the implementation private frame ID
    pFrame->wid = (RootlessFrameID) pRLWinPriv;

    winSelectIcons(&hIcon, &hIconSmall);

    /* Set standard class name prefix so we can identify window easily */
    strncpy(pszClass, WINDOW_CLASS_X, sizeof(pszClass));

    if (winMultiWindowGetClassHint(pFrame->win, &res_name, &res_class)) {
        strncat(pszClass, "-", 1);
        strncat(pszClass, res_name, CLASS_NAME_LENGTH - strlen(pszClass));
        strncat(pszClass, "-", 1);
        strncat(pszClass, res_class, CLASS_NAME_LENGTH - strlen(pszClass));

        /* Check if a window class is provided by the WM_WINDOW_ROLE property,
         * if not use the WM_CLASS information.
         * For further information see:
         * http://tronche.com/gui/x/icccm/sec-5.html
         */
        if (winMultiWindowGetWindowRole(pFrame->win, &res_role)) {
            strcat(pszClass, "-");
            strcat(pszClass, res_role);
            free(res_role);
        }

        free(res_name);
        free(res_class);
    }

    /* Add incrementing window ID to make unique class name */
    snprintf(pszWindowID, sizeof(pszWindowID), "-%x", s_iWindowID++);
    pszWindowID[sizeof(pszWindowID) - 1] = 0;
    strcat(pszClass, pszWindowID);

    winDebug("winMWExtWMCreateFrame - Creating class: %s\n", pszClass);

    /* Setup our window class */
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = winMWExtWMWindowProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = g_hInstance;
    wc.hIcon = hIcon;
    wc.hIconSm = hIconSmall;
    wc.hCursor = 0;
    wc.hbrBackground = (HBRUSH) GetStockObject(WHITE_BRUSH);
    wc.lpszMenuName = NULL;
    wc.lpszClassName = pszClass;
    RegisterClassEx(&wc);

    /* Create the window */
    g_fNoConfigureWindow = TRUE;
    pRLWinPriv->hWnd = CreateWindowExA(WS_EX_TOOLWINDOW,        /* Extended styles */
                                       pszClass,        /* Class name */
                                       WINDOW_TITLE_X,  /* Window name */
                                       WS_POPUP | WS_CLIPCHILDREN, newX,        /* Horizontal position */
                                       newY,    /* Vertical position */
                                       pFrame->width,   /* Right edge */
                                       pFrame->height,  /* Bottom edge */
                                       (HWND) NULL,     /* No parent or owner window */
                                       (HMENU) NULL,    /* No menu */
                                       GetModuleHandle(NULL),   /* Instance handle */
                                       pRLWinPriv);     /* ScreenPrivates */
    if (pRLWinPriv->hWnd == NULL) {
        ErrorF("winMWExtWMCreateFrame - CreateWindowExA () failed: %d\n",
               (int) GetLastError());
        fResult = FALSE;
    }

    winDebug("winMWExtWMCreateFrame - ShowWindow\n");

    //ShowWindow (pRLWinPriv->hWnd, SW_SHOWNOACTIVATE);
    g_fNoConfigureWindow = FALSE;

    if (pShape != NULL) {
        winMWExtWMReshapeFrame(pFrame->wid, pShape);
    }

    winDebug("winMWExtWMCreateFrame - (%p) %p\n",
             pFrame->wid, pRLWinPriv->hWnd);

    winMWExtWMSetNativeProperty(pFrame);

    return fResult;
}

void
winMWExtWMDestroyFrame(RootlessFrameID wid)
{
    win32RootlessWindowPtr pRLWinPriv = (win32RootlessWindowPtr) wid;
    HICON hIcon;
    HICON hIconSm;
    HMODULE hInstance;
    int iReturn;
    char pszClass[CLASS_NAME_LENGTH];

    winDebug("winMWExtWMDestroyFrame (%p) %p\n",
             pRLWinPriv, pRLWinPriv->hWnd);

    /* Store the info we need to destroy after this window is gone */
    hInstance = (HINSTANCE) GetClassLongPtr(pRLWinPriv->hWnd, GCLP_HMODULE);
    hIcon = (HICON) SendMessage(pRLWinPriv->hWnd, WM_GETICON, ICON_BIG, 0);
    hIconSm = (HICON) SendMessage(pRLWinPriv->hWnd, WM_GETICON, ICON_SMALL, 0);
    iReturn = GetClassName(pRLWinPriv->hWnd, pszClass, CLASS_NAME_LENGTH);

    pRLWinPriv->fClose = TRUE;
    pRLWinPriv->fDestroyed = TRUE;

    /* Destroy the Windows window */
    DestroyWindow(pRLWinPriv->hWnd);

    /* Only if we were able to get the name */
    if (iReturn) {
        winDebug("winMWExtWMDestroyFrame - Unregistering %s: ", pszClass);
        iReturn = UnregisterClass(pszClass, hInstance);
      
        winDebug ("winMWExtWMDestroyFramew - Deleting Icon\n");
    }

    winDestroyIcon(hiconClass);
    winDestroyIcon(hiconSmClass);

    winDebug("winMWExtWMDestroyFrame - done\n");
}

void
winMWExtWMMoveFrame(RootlessFrameID wid, ScreenPtr pScreen, int iNewX,
                    int iNewY)
{
    win32RootlessWindowPtr pRLWinPriv = (win32RootlessWindowPtr) wid;
    RECT rcNew;
    DWORD dwExStyle;
    DWORD dwStyle;
    int iX, iY, iWidth, iHeight;

    winDebug("winMWExtWMMoveFrame (%p) (%d %d)\n", pRLWinPriv, iNewX,
             iNewY);

    /* Get the Windows window style and extended style */
    dwExStyle = GetWindowLongPtr(pRLWinPriv->hWnd, GWL_EXSTYLE);
    dwStyle = GetWindowLongPtr(pRLWinPriv->hWnd, GWL_STYLE);

    /* Get the X and Y location of the X window */
    iX = iNewX + GetSystemMetrics(SM_XVIRTUALSCREEN);
    iY = iNewY + GetSystemMetrics(SM_YVIRTUALSCREEN);

    /* Get the height and width of the X window */
    iWidth = pRLWinPriv->pFrame->width;
    iHeight = pRLWinPriv->pFrame->height;

    /* Store the origin, height, and width in a rectangle structure */
    SetRect(&rcNew, iX, iY, iX + iWidth, iY + iHeight);

    winDebug("\tWindow {%d, %d, %d, %d}, {%d, %d}\n",
             rcNew.left, rcNew.top, rcNew.right, rcNew.bottom,
             rcNew.right - rcNew.left, rcNew.bottom - rcNew.top);
    /*
     * Calculate the required size of the Windows window rectangle,
     * given the size of the Windows window client area.
     */
    AdjustWindowRectEx(&rcNew, dwStyle, FALSE, dwExStyle);

    winDebug("\tAdjusted {%d, %d, %d, %d}, {%d, %d}\n",
             rcNew.left, rcNew.top, rcNew.right, rcNew.bottom,
             rcNew.right - rcNew.left, rcNew.bottom - rcNew.top);
    g_fNoConfigureWindow = TRUE;
    SetWindowPos(pRLWinPriv->hWnd, NULL, rcNew.left, rcNew.top, 0, 0,
                 SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER);
    g_fNoConfigureWindow = FALSE;
    winDebug("winMWExtWMMoveFrame (%p) done\n", pRLWinPriv);
}

void
winMWExtWMResizeFrame(RootlessFrameID wid, ScreenPtr pScreen,
                      int iNewX, int iNewY,
                      unsigned int uiNewWidth, unsigned int uiNewHeight,
                      unsigned int uiGravity)
{
    win32RootlessWindowPtr pRLWinPriv = (win32RootlessWindowPtr) wid;
    RECT rcNew;
    RECT rcOld;
    DWORD dwExStyle;
    DWORD dwStyle;
    int iX, iY;

    winDebug("winMWExtWMResizeFrame (%p) (%d %d)-(%d %d)\n",
             pRLWinPriv, iNewX, iNewY, uiNewWidth, uiNewHeight);

    pRLWinPriv->fResized = TRUE;

    /* Get the Windows window style and extended style */
    dwExStyle = GetWindowLongPtr(pRLWinPriv->hWnd, GWL_EXSTYLE);
    dwStyle = GetWindowLongPtr(pRLWinPriv->hWnd, GWL_STYLE);

    /* Get the X and Y location of the X window */
    iX = iNewX + GetSystemMetrics(SM_XVIRTUALSCREEN);
    iY = iNewY + GetSystemMetrics(SM_YVIRTUALSCREEN);

    /* Store the origin, height, and width in a rectangle structure */
    SetRect(&rcNew, iX, iY, iX + uiNewWidth, iY + uiNewHeight);

    /*
     * Calculate the required size of the Windows window rectangle,
     * given the size of the Windows window client area.
     */
    AdjustWindowRectEx(&rcNew, dwStyle, FALSE, dwExStyle);

    /* Get a rectangle describing the old Windows window */
    GetWindowRect(pRLWinPriv->hWnd, &rcOld);

    /* Check if the old rectangle and new rectangle are the same */
    if (!EqualRect(&rcNew, &rcOld)) {

        g_fNoConfigureWindow = TRUE;
        MoveWindow(pRLWinPriv->hWnd,
                   rcNew.left, rcNew.top,
                   rcNew.right - rcNew.left, rcNew.bottom - rcNew.top, TRUE);
        g_fNoConfigureWindow = FALSE;
    }
}

void
winMWExtWMRestackFrame(RootlessFrameID wid, RootlessFrameID nextWid)
{
    win32RootlessWindowPtr pRLWinPriv = (win32RootlessWindowPtr) wid;
    win32RootlessWindowPtr pRLNextWinPriv = (win32RootlessWindowPtr) nextWid;

    winScreenPriv(pRLWinPriv->pFrame->win->drawable.pScreen);
    winScreenInfo *pScreenInfo = NULL;
    DWORD dwCurrentProcessID = GetCurrentProcessId();
    DWORD dwWindowProcessID = 0;
    HWND hWnd;
    Bool fFirst = TRUE;
    Bool fNeedRestack = TRUE;

    winDebug("winMWExtWMRestackFrame (%p)\n", pRLWinPriv);


    if (pScreenPriv && pScreenPriv->fRestacking)
        return;

    if (pScreenPriv)
        pScreenInfo = pScreenPriv->pScreenInfo;

    pRLWinPriv->fRestackingNow = TRUE;

    /* Show window */
    if (!IsWindowVisible(pRLWinPriv->hWnd))
        ShowWindow(pRLWinPriv->hWnd, SW_SHOWNOACTIVATE);

    if (pRLNextWinPriv == NULL) {
        winDebug("Win %p is top\n", pRLWinPriv);
        pScreenPriv->widTop = wid;
        SetWindowPos(pRLWinPriv->hWnd, HWND_TOP,
                     0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
#ifdef XWIN_MULTIWINDOWINTWM
  else if (winIsInternalWMRunning(pScreenInfo)) {
      /* using mulwinidow wm */
        winDebug("Win %p is not top\n", pRLWinPriv);

        for (hWnd = GetNextWindow(pRLWinPriv->hWnd, GW_HWNDPREV);
             fNeedRestack && hWnd != NULL;
             hWnd = GetNextWindow(hWnd, GW_HWNDPREV)) {
            GetWindowThreadProcessId(hWnd, &dwWindowProcessID);

            if ((dwWindowProcessID == dwCurrentProcessID)
                && GetProp(hWnd, WIN_WINDOW_PROP)) {
                if (hWnd == pRLNextWinPriv->hWnd) {
                    /* Enable interleave X window and Windows window */
                    if (!fFirst) {
                        winDebug("raise: Insert after Win %p\n",
                                 pRLNextWinPriv);
                        SetWindowPos(pRLWinPriv->hWnd, pRLNextWinPriv->hWnd,
                                     0, 0, 0, 0,
                                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                    }
                    else {
                        winDebug("No change\n");
                    }
                    fNeedRestack = FALSE;
                    break;
                }
                if (fFirst)
                    fFirst = FALSE;
            }
        }

        for (hWnd = GetNextWindow(pRLWinPriv->hWnd, GW_HWNDNEXT);
             fNeedRestack && hWnd != NULL;
             hWnd = GetNextWindow(hWnd, GW_HWNDNEXT)) {
            GetWindowThreadProcessId(hWnd, &dwWindowProcessID);

            if ((dwWindowProcessID == dwCurrentProcessID)
                && GetProp(hWnd, WIN_WINDOW_PROP)) {
                if (hWnd == pRLNextWinPriv->hWnd) {
                    winDebug("lower: Insert after Win %p\n", pRLNextWinPriv);

                    SetWindowPos(pRLWinPriv->hWnd, pRLNextWinPriv->hWnd,
                                 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                    fNeedRestack = FALSE;
                    break;
                }
            }
        }
    }
#endif
    else {
        /* using general wm like twm, wmaker etc.
           Interleave X window and Windows window will cause problem. */
        SetWindowPos(pRLWinPriv->hWnd, pRLNextWinPriv->hWnd,
                     0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    winDebug("winMWExtWMRestackFrame - done (%p)\n", pRLWinPriv);

    pRLWinPriv->fRestackingNow = FALSE;
}

void
winMWExtWMReshapeFrame(RootlessFrameID wid, RegionPtr pShape)
{
    win32RootlessWindowPtr pRLWinPriv = (win32RootlessWindowPtr) wid;
    HRGN hRgn, hRgnWindow, hRgnClient;
    RECT rcWindow, rcClient;

    winDebug("winMWExtWMReshapeFrame (%p)\n", pRLWinPriv);

    hRgn = winMWExtWMCreateRgnFromRegion(pShape);

    /* Create region for non-client area */
    GetWindowRect(pRLWinPriv->hWnd, &rcWindow);
    GetClientRect(pRLWinPriv->hWnd, &rcClient);
    MapWindowPoints(pRLWinPriv->hWnd, HWND_DESKTOP, (LPPOINT) &rcClient, 2);
    OffsetRgn(hRgn, rcClient.left - rcWindow.left, rcClient.top - rcWindow.top);
    OffsetRect(&rcClient, -rcWindow.left, -rcWindow.top);
    OffsetRect(&rcWindow, -rcWindow.left, -rcWindow.top);
    hRgnWindow = CreateRectRgnIndirect(&rcWindow);
    hRgnClient = CreateRectRgnIndirect(&rcClient);
    CombineRgn(hRgnWindow, hRgnWindow, hRgnClient, RGN_DIFF);
    CombineRgn(hRgn, hRgnWindow, hRgn, RGN_OR);

    SetWindowRgn(pRLWinPriv->hWnd, hRgn, TRUE);

    DeleteObject(hRgnWindow);
    DeleteObject(hRgnClient);
}

void
winMWExtWMUnmapFrame(RootlessFrameID wid)
{
    win32RootlessWindowPtr pRLWinPriv = (win32RootlessWindowPtr) wid;

    winDebug("winMWExtWMUnmapFrame (%p)\n", pRLWinPriv);

    g_fNoConfigureWindow = TRUE;
    //ShowWindow (pRLWinPriv->hWnd, SW_MINIMIZE);
    ShowWindow(pRLWinPriv->hWnd, SW_HIDE);
    g_fNoConfigureWindow = FALSE;
}

/*
 * Fixme: Code sharing with winshadgdi.c and other engine support
 */
void
winMWExtWMStartDrawing(RootlessFrameID wid, char **pixelData, int *bytesPerRow)
{
    win32RootlessWindowPtr pRLWinPriv = (win32RootlessWindowPtr) wid;
    winPrivScreenPtr pScreenPriv = NULL;
    winScreenInfo *pScreenInfo = NULL;
    ScreenPtr pScreen = NULL;
    DIBSECTION dibsection;
    Bool fReturn = TRUE;
    HDC hdcNew;
    HBITMAP hbmpNew;

    winDebug("winMWExtWMStartDrawing (%p) %08x\n", pRLWinPriv,
             pRLWinPriv->fDestroyed);

    if (!pRLWinPriv->fDestroyed) {
        pScreen = pRLWinPriv->pFrame->win->drawable.pScreen;
        if (pScreen)
            pScreenPriv = winGetScreenPriv(pScreen);
        if (pScreenPriv)
            pScreenInfo = pScreenPriv->pScreenInfo;

        winDebug("\tpScreenPriv %p\n", pScreenPriv);
        winDebug("\tpScreenInfo %p\n", pScreenInfo);
        winDebug("\t(%d, %d)\n", (int) pRLWinPriv->pFrame->width,
                 (int) pRLWinPriv->pFrame->height);

        if (pRLWinPriv->hdcScreen == NULL) {
            InitWin32RootlessEngine(pRLWinPriv);
        }

        if (pRLWinPriv->fResized) {
            /* width * bpp must be multiple of 4 to match 32bit alignment */
            int stridesize;
            int misalignment;

            pRLWinPriv->pbmihShadow->biWidth = pRLWinPriv->pFrame->width;
            pRLWinPriv->pbmihShadow->biHeight = -pRLWinPriv->pFrame->height;

            stridesize = pRLWinPriv->pFrame->width * (pScreenInfo->dwBPP >> 3);
            misalignment = stridesize & 3;
            if (misalignment != 0) {
                stridesize += 4 - misalignment;
                pRLWinPriv->pbmihShadow->biWidth =
                    stridesize / (pScreenInfo->dwBPP >> 3);
                winDebug("\tresizing to %d (was %d)\n",
                         pRLWinPriv->pbmihShadow->biWidth,
                         pRLWinPriv->pFrame->width);
            }

            hdcNew = CreateCompatibleDC(pRLWinPriv->hdcScreen);
            /* Create a DI shadow bitmap with a bit pointer */
            hbmpNew = CreateDIBSection(pRLWinPriv->hdcScreen,
                                       (BITMAPINFO *) pRLWinPriv->pbmihShadow,
                                       DIB_RGB_COLORS,
                                       (VOID **) &pRLWinPriv->pfb, NULL, 0);
            if (hbmpNew == NULL || pRLWinPriv->pfb == NULL) {
                ErrorF("winMWExtWMStartDrawing - CreateDIBSection failed\n");
                //return FALSE;
            }
            else {
                winDebug("winMWExtWMStartDrawing - Shadow buffer allocated\n");
            }

            /* Get information about the bitmap that was allocated */
            GetObject(hbmpNew, sizeof(dibsection), &dibsection);

            /* Print information about bitmap allocated */
            winDebug("winMWExtWMStartDrawing - Dibsection width: %d height: %d "
                     "depth: %d size image: %d\n",
                     (unsigned int) dibsection.dsBmih.biWidth,
                     (unsigned int) dibsection.dsBmih.biHeight,
                     (unsigned int) dibsection.dsBmih.biBitCount,
                     (unsigned int) dibsection.dsBmih.biSizeImage);

            /* Select the shadow bitmap into the shadow DC */
            SelectObject(hdcNew, hbmpNew);

            winDebug("winMWExtWMStartDrawing - Attempting a shadow blit\n");

            /* Blit from the old shadow to the new shadow */
            fReturn = BitBlt(hdcNew,
                             0, 0,
                             pRLWinPriv->pFrame->width,
                             pRLWinPriv->pFrame->height, pRLWinPriv->hdcShadow,
                             0, 0, SRCCOPY);
            if (fReturn) {
                winDebug("winMWExtWMStartDrawing - Shadow blit success\n");
            }
            else {
                ErrorF("winMWExtWMStartDrawing - Shadow blit failure\n");
            }

            /* Look for height weirdness */
            if (dibsection.dsBmih.biHeight < 0) {
                /* FIXME: Figure out why biHeight is sometimes negative */
                ErrorF("winMWExtWMStartDrawing - WEIRDNESS - "
                       "biHeight still negative: %d\n",
                       (int) dibsection.dsBmih.biHeight);
                ErrorF("winMWExtWMStartDrawing - WEIRDNESS - "
                       "Flipping biHeight sign\n");
                dibsection.dsBmih.biHeight = -dibsection.dsBmih.biHeight;
            }

            pRLWinPriv->dwWidthBytes = dibsection.dsBm.bmWidthBytes;

            winDebug("winMWExtWMStartDrawing - bytesPerRow: %d\n",
                     (unsigned int) dibsection.dsBm.bmWidthBytes);

            /* Free the old shadow bitmap */
            DeleteObject(pRLWinPriv->hdcShadow);
            DeleteObject(pRLWinPriv->hbmpShadow);

            pRLWinPriv->hdcShadow = hdcNew;
            pRLWinPriv->hbmpShadow = hbmpNew;

            pRLWinPriv->fResized = FALSE;
            winDebug("winMWExtWMStartDrawing - 0x%08x %d\n",
                     (unsigned int) pRLWinPriv->pfb,
                     (unsigned int) dibsection.dsBm.bmWidthBytes);
        }
    }
    else {
        ErrorF("winMWExtWMStartDrawing - Already window was destroyed \n");
    }
    winDebug("winMWExtWMStartDrawing - done (%p) %p %d\n",
             pRLWinPriv,
             pRLWinPriv->pfb,
             (unsigned int) pRLWinPriv->dwWidthBytes);
    *pixelData = pRLWinPriv->pfb;
    *bytesPerRow = pRLWinPriv->dwWidthBytes;
}

void
winMWExtWMStopDrawing(RootlessFrameID wid, Bool fFlush)
{
}

void
winMWExtWMUpdateRegion(RootlessFrameID wid, RegionPtr pDamage)
{
    win32RootlessWindowPtr pRLWinPriv = (win32RootlessWindowPtr) wid;

    if (!g_fNoConfigureWindow)
        UpdateWindow(pRLWinPriv->hWnd);
}

void
winMWExtWMDamageRects(RootlessFrameID wid, int nCount, const BoxRec * pRects,
                      int shift_x, int shift_y)
{
    win32RootlessWindowPtr pRLWinPriv = (win32RootlessWindowPtr) wid;
    const BoxRec *pEnd;

    winDebug("winMWExtWMDamageRects (%08x, %d, %08x, %d, %d)\n",
             pRLWinPriv, nCount, pRects, shift_x, shift_y);

    for (pEnd = pRects + nCount; pRects < pEnd; pRects++) {
        RECT rcDmg;

        rcDmg.left = pRects->x1 + shift_x;
        rcDmg.top = pRects->y1 + shift_y;
        rcDmg.right = pRects->x2 + shift_x;
        rcDmg.bottom = pRects->y2 + shift_y;

        InvalidateRect(pRLWinPriv->hWnd, &rcDmg, FALSE);
    }
}

void
winMWExtWMRootlessSwitchWindow(RootlessWindowPtr pFrame, WindowPtr oldWin)
{
    win32RootlessWindowPtr pRLWinPriv = (win32RootlessWindowPtr) pFrame->wid;

    winDebug("winMWExtWMRootlessSwitchWindow (%p) %p\n",
             pRLWinPriv, pRLWinPriv->hWnd);
    pRLWinPriv->pFrame = pFrame;
    pRLWinPriv->fResized = TRUE;

    /* Set the window extended style flags */
    SetWindowLongPtr(pRLWinPriv->hWnd, GWL_EXSTYLE, WS_EX_TOOLWINDOW);

    /* Set the window standard style flags */
    SetWindowLongPtr(pRLWinPriv->hWnd, GWL_STYLE, WS_POPUP | WS_CLIPCHILDREN);

    DeleteProperty(serverClient, oldWin, AtmWindowsWmNativeHwnd());
    winMWExtWMSetNativeProperty(pFrame);
}

void
winMWExtWMCopyBytes(unsigned int width, unsigned int height,
                    const void *src, unsigned int srcRowBytes,
                    void *dst, unsigned int dstRowBytes)
{
    winDebug("winMWExtWMCopyBytes - Not implemented\n");
}

void
winMWExtWMCopyWindow(RootlessFrameID wid, int nDstRects,
                     const BoxRec * pDstRects, int nDx, int nDy)
{
    win32RootlessWindowPtr pRLWinPriv = (win32RootlessWindowPtr) wid;
    const BoxRec *pEnd;
    RECT rcDmg;

    winDebug("winMWExtWMCopyWindow (%p, %d, %p, %d, %d)\n",
             pRLWinPriv, nDstRects, pDstRects, nDx, nDy);

    for (pEnd = pDstRects + nDstRects; pDstRects < pEnd; pDstRects++) {
        winDebug("BitBlt (%d, %d, %d, %d) (%d, %d)\n",
                 pDstRects->x1, pDstRects->y1,
                 pDstRects->x2 - pDstRects->x1,
                 pDstRects->y2 - pDstRects->y1,
                 pDstRects->x1 + nDx, pDstRects->y1 + nDy);

        if (!BitBlt(pRLWinPriv->hdcShadow,
                    pDstRects->x1, pDstRects->y1,
                    pDstRects->x2 - pDstRects->x1,
                    pDstRects->y2 - pDstRects->y1,
                    pRLWinPriv->hdcShadow,
                    pDstRects->x1 + nDx, pDstRects->y1 + nDy, SRCCOPY)) {
            ErrorF("winMWExtWMCopyWindow - BitBlt failed.\n");
        }

        rcDmg.left = pDstRects->x1;
        rcDmg.top = pDstRects->y1;
        rcDmg.right = pDstRects->x2;
        rcDmg.bottom = pDstRects->y2;

        InvalidateRect(pRLWinPriv->hWnd, &rcDmg, FALSE);
    }
    winDebug("winMWExtWMCopyWindow - done\n");
}

/*
 * winMWExtWMSetNativeProperty
 */

static void
winMWExtWMSetNativeProperty(RootlessWindowPtr pFrame)
{
    win32RootlessWindowPtr pRLWinPriv = (win32RootlessWindowPtr) pFrame->wid;
    long lData;

    /* FIXME: move this to WindowsWM extension */

    lData = (long) pRLWinPriv->hWnd;
    dixChangeWindowProperty(serverClient, pFrame->win, AtmWindowsWmNativeHwnd(),
                            XA_INTEGER, 32, PropModeReplace, 1, &lData, TRUE);
}
