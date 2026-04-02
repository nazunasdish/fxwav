#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <mmsystem.h>
#include <cmath>
#include <string>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include "resource.h"
#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#endif
#ifndef GET_Y_LPARAM
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#endif

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "comctl32.lib")

bool parseError = false;

const int WIDTH = 800;
const int HEIGHT = 600;

DWORD startMs = 0;

const float MIN_SCALE = 1.0f;
const float MAX_SCALE = 1000.0f;

const int SAMPLE_RATE = 44100;
const int BUFFER_SIZE = SAMPLE_RATE * 10;

float scaleX = 50.0f;
float playX = 0;
const float scaleY = 50.0f;

float offsetX = WIDTH * 0.2f;
float offsetY = HEIGHT / 2.0f;

float volume = 0.5f;
float speed = 1.0f;

float playPos = 0.0f;
float elapsedMs = 0.0f;

bool isPlaying = false;
bool isDragging = false;

POINT dragStart = { 0 };
float offsetXStart = 0;

short audioBuffer[BUFFER_SIZE];

HWAVEOUT hWaveOut = NULL;

UINT_PTR timerId = 1;

HWND hwndEdit, hwndPlay, hwndStop, hwndZoomIn, hwndZoomOut, hwndCenterPlay, hwndExport;
HWND hwndVolLabel, hwndVolSlider, hwndSpeedLabel, hwndSpeedSlider, hwndSpeedValueLabel;

WNDPROC oldEditProc = NULL;

const float speedLevels[6] = { 0.33f, 0.5f, 1.0f, 1.5f, 2.0f, 3.0f };

enum NodeType {
    NODE_CONST, NODE_VAR,
    NODE_ADD, NODE_SUB, NODE_MUL, NODE_DIV,
    NODE_SIN, NODE_COS, NODE_TAN, NODE_TANH,
    NODE_EXP, NODE_LOG, NODE_ABS, NODE_SQRT,
    NODE_SIGN, NODE_FLOOR, NODE_CEIL,
    NODE_MIN, NODE_MAX, NODE_NEG
};

struct ExprNode {
    NodeType type;
    double value;
    ExprNode* left;
    ExprNode* right;

    ExprNode(NodeType t) : type(t), value(0), left(nullptr), right(nullptr) {}
    ExprNode(double val) : type(NODE_CONST), value(val), left(nullptr), right(nullptr) {}
};

ExprNode* rootExpr = nullptr;
const char* expr_ptr_tree;

void skipWhitespace2() {
    while (*expr_ptr_tree && isspace(*expr_ptr_tree)) expr_ptr_tree++;
}

ExprNode* parseExpressionTree();

ExprNode* parseNumberNode() {
    skipWhitespace2();
    char* endptr;
    double val = strtod(expr_ptr_tree, &endptr);
    if (endptr == expr_ptr_tree) {
        MessageBox(NULL, "Syntax error: number expected", "Error", MB_OK);
        parseError = true;
        return nullptr;
    }
    expr_ptr_tree = endptr;
    return new ExprNode(val);
}

ExprNode* parseParenExpression() {
    skipWhitespace2();
    if (*expr_ptr_tree != '(') {
        MessageBox(NULL, "Syntax error: '(' expected", "Error", MB_OK);
        parseError = true;
        return nullptr;
    }
    expr_ptr_tree++; // skip '('
    ExprNode* val = parseExpressionTree();
    skipWhitespace2();
    if (*expr_ptr_tree != ')') {
        MessageBox(NULL, "Syntax error: ')' expected", "Error", MB_OK);
        parseError = true;
        return nullptr;
    }
    expr_ptr_tree++; // skip ')'
    return val;
}

ExprNode* parseFactorNode() {
    skipWhitespace2();

    // Unary functions (sin, cos, etc)
#define PARSE_UNARY_FUNC(name, type) \
    if (strncmp(expr_ptr_tree, name, strlen(name)) == 0 && !isalnum(expr_ptr_tree[strlen(name)])) { \
        expr_ptr_tree += strlen(name); skipWhitespace2(); \
        ExprNode* arg = parseParenExpression(); \
        if (!arg) return nullptr; \
        ExprNode* node = new ExprNode(type); \
        node->left = arg; \
        return node; \
    }

#define PARSE_BINARY_FUNC(name, type) \
    if (strncmp(expr_ptr_tree, name, strlen(name)) == 0 && !isalnum(expr_ptr_tree[strlen(name)])) { \
        expr_ptr_tree += strlen(name); skipWhitespace2(); \
        if (*expr_ptr_tree != '(') { \
            MessageBox(NULL, "Syntax error: '(' expected after " name, "Error", MB_OK); \
            parseError = true; return nullptr; \
        } \
        expr_ptr_tree++; \
        ExprNode* arg1 = parseExpressionTree(); \
        skipWhitespace2(); \
        if (*expr_ptr_tree != ',') { \
            MessageBox(NULL, "Syntax error: ',' expected in " name, "Error", MB_OK); \
            parseError = true; return nullptr; \
        } \
        expr_ptr_tree++; \
        ExprNode* arg2 = parseExpressionTree(); \
        skipWhitespace2(); \
        if (*expr_ptr_tree != ')') { \
            MessageBox(NULL, "Syntax error: ')' expected after " name, "Error", MB_OK); \
            parseError = true; return nullptr; \
        } \
        expr_ptr_tree++; \
        ExprNode* node = new ExprNode(type); \
        node->left = arg1; node->right = arg2; \
        return node; \
    }

    if (*expr_ptr_tree == '(') {
        return parseParenExpression();
    }

    PARSE_UNARY_FUNC("sin", NODE_SIN)
    PARSE_UNARY_FUNC("cos", NODE_COS)
    PARSE_UNARY_FUNC("tan", NODE_TAN)
    PARSE_UNARY_FUNC("tanh", NODE_TANH)
    PARSE_UNARY_FUNC("exp", NODE_EXP)
    PARSE_UNARY_FUNC("log", NODE_LOG)
    PARSE_UNARY_FUNC("abs", NODE_ABS)
    PARSE_UNARY_FUNC("sqrt", NODE_SQRT)
    PARSE_UNARY_FUNC("sign", NODE_SIGN)
    PARSE_UNARY_FUNC("floor", NODE_FLOOR)
    PARSE_UNARY_FUNC("ceil", NODE_CEIL)

    PARSE_BINARY_FUNC("min", NODE_MIN)
    PARSE_BINARY_FUNC("max", NODE_MAX)

    if (*expr_ptr_tree == 'x') {
        expr_ptr_tree++;
        return new ExprNode(NODE_VAR);
    }

    if (isdigit(*expr_ptr_tree) || *expr_ptr_tree == '.') {
        return parseNumberNode();
    }

    MessageBox(NULL, "Syntax error: unexpected token", "Error", MB_OK);
    parseError = true;
    return nullptr;
}

ExprNode* parseUnaryNode() {
    skipWhitespace2();

    if (*expr_ptr_tree == '+') {
        expr_ptr_tree++;
        return parseUnaryNode();
    }

    if (*expr_ptr_tree == '-') {
        expr_ptr_tree++;
        ExprNode* node = new ExprNode(NODE_NEG);
        node->left = parseUnaryNode();
        return node;
    }

    return parseFactorNode();
}

ExprNode* parseTermNode() {
    ExprNode* node = parseUnaryNode();

    while (true) {
        skipWhitespace2();
        if (*expr_ptr_tree == '*') {
            expr_ptr_tree++;
            ExprNode* right = parseUnaryNode();
            ExprNode* mulNode = new ExprNode(NODE_MUL);
            mulNode->left = node;
            mulNode->right = right;
            node = mulNode;
        }
        else if (*expr_ptr_tree == '/') {
            expr_ptr_tree++;
            ExprNode* right = parseUnaryNode();
            ExprNode* divNode = new ExprNode(NODE_DIV);
            divNode->left = node;
            divNode->right = right;
            node = divNode;
        }
        else {
            break;
        }
    }

    return node;
}

ExprNode* parseExpressionTree() {
    ExprNode* val = parseTermNode();
    skipWhitespace2();
    while (*expr_ptr_tree == '+' || *expr_ptr_tree == '-') {
        char op = *expr_ptr_tree++;
        ExprNode* right = parseTermNode();
        ExprNode* node = new ExprNode(op == '+' ? NODE_ADD : NODE_SUB);
        node->left = val;
        node->right = right;
        val = node;
        skipWhitespace2();
    }
    return val;
}

double evalExpr(ExprNode* node, double x) {
    if (!node) return 0.0;
    switch (node->type) {
    case NODE_CONST: return node->value;
    case NODE_VAR: return x;
    case NODE_ADD: return evalExpr(node->left, x) + evalExpr(node->right, x);
    case NODE_SUB: return evalExpr(node->left, x) - evalExpr(node->right, x);
    case NODE_MUL: return evalExpr(node->left, x) * evalExpr(node->right, x);
    case NODE_DIV: return evalExpr(node->left, x) / evalExpr(node->right, x);
    case NODE_SIN: return sin(evalExpr(node->left, x));
    case NODE_COS: return cos(evalExpr(node->left, x));
    case NODE_TAN: return tan(evalExpr(node->left, x));
    case NODE_TANH: return tanh(evalExpr(node->left, x));
    case NODE_EXP: return exp(evalExpr(node->left, x));
    case NODE_LOG: return log(fabs(evalExpr(node->left, x)) + 1e-6);
    case NODE_ABS: return fabs(evalExpr(node->left, x));
    case NODE_SQRT: return sqrt(fabs(evalExpr(node->left, x)));
    case NODE_SIGN: {
        double val = evalExpr(node->left, x);
        return (val > 0) ? 1.0 : (val < 0 ? -1.0 : 0.0);
    }
    case NODE_FLOOR: return floor(evalExpr(node->left, x));
    case NODE_CEIL: return ceil(evalExpr(node->left, x));
    case NODE_MIN: return std::min(evalExpr(node->left, x), evalExpr(node->right, x));
    case NODE_MAX: return std::max(evalExpr(node->left, x), evalExpr(node->right, x));
    case NODE_NEG: return -evalExpr(node->left, x);
    }
    return 0.0;
}

void freeExpr(ExprNode* node) {
    if (!node) return;
    freeExpr(node->left);
    freeExpr(node->right);
    delete node;
}

void generateAudio() {
    if (!rootExpr) return;
    for (int i = 0; i < BUFFER_SIZE; i++) {
        double t = (i / (double)SAMPLE_RATE) * 2.0 * 3.14159265358979323846 * speed;
        double val = evalExpr(rootExpr, t);
        if (val > 1.0) val = 1.0;
        else if (val < -1.0) val = -1.0;
        audioBuffer[i] = (short)(val * volume * 32767);
    }
}


void drawGraph(HDC hdcWindow) {
    RECT rect;
    GetClientRect(WindowFromDC(hdcWindow), &rect);
    HDC hdcMem = CreateCompatibleDC(hdcWindow);
    HBITMAP hbmMem = CreateCompatibleBitmap(hdcWindow, rect.right, rect.bottom);
    HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbmMem);

    FillRect(hdcMem, &rect, (HBRUSH)(COLOR_WINDOW + 1));

    HPEN penAxis = CreatePen(PS_SOLID, 2, RGB(0, 0, 0));
    HPEN penGrid = CreatePen(PS_DOT, 1, RGB(200, 200, 200));
    HPEN penClip = CreatePen(PS_DOT, 1, RGB(255, 0, 0));
    HPEN penGraph = CreatePen(PS_SOLID, 2, RGB(0, 0, 255));
    HPEN penPlay = CreatePen(PS_SOLID, 2, RGB(255, 0, 0));

    HPEN oldPen = (HPEN)SelectObject(hdcMem, penGrid);

    for (int y = (int)offsetY % (int)scaleY; y < HEIGHT; y += (int)scaleY) {
        MoveToEx(hdcMem, 0, y, NULL);
        LineTo(hdcMem, WIDTH, y);
    }

    for (int x = (int)offsetX % (int)scaleX; x < WIDTH; x += (int)scaleX) {
        MoveToEx(hdcMem, x, 0, NULL);
        LineTo(hdcMem, x, HEIGHT);
    }

    SelectObject(hdcMem, penAxis);

    MoveToEx(hdcMem, (int)offsetX, 0, NULL);
    LineTo(hdcMem, (int)offsetX, HEIGHT);

    MoveToEx(hdcMem, 0, (int)offsetY, NULL);
    LineTo(hdcMem, WIDTH, (int)offsetY);

    SelectObject(hdcMem, penClip);
    int clipY1 = (int)(offsetY - scaleY * 1.0);
    int clipY2 = (int)(offsetY + scaleY * 1.0);

    MoveToEx(hdcMem, 0, clipY1, NULL);
    LineTo(hdcMem, WIDTH, clipY1);

    MoveToEx(hdcMem, 0, clipY2, NULL);
    LineTo(hdcMem, WIDTH, clipY2);

    SelectObject(hdcMem, penGraph);

    bool firstPoint = true;
    for (int px = 0; px < WIDTH; px++) {
        double t = (px - offsetX) / scaleX;
        double y = rootExpr ? evalExpr(rootExpr, t) : 0.0;
        if (y > 1.0) y = 1.0;
        if (y < -1.0) y = -1.0;
        int py = (int)(offsetY - y * scaleY);
        if (firstPoint) {
            MoveToEx(hdcMem, px, py, NULL);
            firstPoint = false;
        }
        else {
            LineTo(hdcMem, px, py);
        }
    }

    if (isPlaying) {
        float x = playPos * speed * 2.0f * 3.14159265358979323846f;
        playX = offsetX + x * scaleX;

        if (playX >= 0 && playX <= WIDTH) {
            SelectObject(hdcMem, penPlay);
            MoveToEx(hdcMem, (int)playX, 0, NULL);
            LineTo(hdcMem, (int)playX, HEIGHT);
        }
    }

    SelectObject(hdcMem, oldPen);
    DeleteObject(penAxis);
    DeleteObject(penGrid);
    DeleteObject(penClip);
    DeleteObject(penGraph);
    DeleteObject(penPlay);

    BitBlt(hdcWindow, 0, 0, rect.right, rect.bottom, hdcMem, 0, 0, SRCCOPY);
    SelectObject(hdcMem, hbmOld);
    DeleteObject(hbmMem);
    DeleteDC(hdcMem);
}

void playAudio(HWND hwnd) {
    offsetX = WIDTH * 0.2f;
    playX = offsetX;
    generateAudio();
    int effectiveBufferSize = BUFFER_SIZE;

    WAVEFORMATEX wfx = { WAVE_FORMAT_PCM, 1, SAMPLE_RATE, SAMPLE_RATE * 2, 2, 16, 0 };
    static WAVEHDR waveHdr = { 0 };
    if (hWaveOut) {
        waveOutReset(hWaveOut);
        waveOutClose(hWaveOut);
        hWaveOut = NULL;
    }
    if (waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        MessageBox(NULL, "Failed to open wave out device", "Error", MB_OK);
        return;
    }

    waveHdr.lpData = (LPSTR)audioBuffer;
    waveHdr.dwBufferLength = effectiveBufferSize * sizeof(short);
    waveHdr.dwFlags = 0; waveHdr.dwLoops = 0;
    waveOutPrepareHeader(hWaveOut, &waveHdr, sizeof(WAVEHDR));
    waveOutWrite(hWaveOut, &waveHdr, sizeof(WAVEHDR));
    startMs = timeGetTime();
    playPos = 0.0f; elapsedMs = 0.0f; isPlaying = true;
    SetTimer(hwnd, timerId, 40, NULL);
}


LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'A') {
        SendMessage(hwnd, EM_SETSEL, 0, -1);
        return 0;
    }
    return CallWindowProc(oldEditProc, hwnd, msg, wParam, lParam);
}
void writeWavHeader(FILE* f, int dataSize) {
    fwrite("RIFF", 1, 4, f);
    int chunkSize = 36 + dataSize;
    fwrite(&chunkSize, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    int subChunk1Size = 16;
    short audioFormat = 1;
    short channels = 1;
    short bitsPerSample = 16;
    int byteRate = SAMPLE_RATE * channels * bitsPerSample / 8;
    short blockAlign = channels * bitsPerSample / 8;
    fwrite(&subChunk1Size, 4, 1, f);
    fwrite(&audioFormat, 2, 1, f);
    fwrite(&channels, 2, 1, f);
    fwrite(&SAMPLE_RATE, 4, 1, f);
    fwrite(&byteRate, 4, 1, f);
    fwrite(&blockAlign, 2, 1, f);
    fwrite(&bitsPerSample, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&dataSize, 4, 1, f);
}

void exportToWav(const char* path, int seconds) {
    if (!rootExpr) return;

    int totalSamples = seconds * SAMPLE_RATE;
    FILE* f = fopen(path, "wb");
    if (!f) {
        MessageBox(NULL, "Failed to open file", "Error", MB_OK);
        return;
    }

    writeWavHeader(f, totalSamples * sizeof(short));

    for (int i = 0; i < totalSamples; ++i) {
        double t = (i / (double)SAMPLE_RATE) * speed * 2.0 * 3.1415;
        double val = evalExpr(rootExpr, t);
        if (val > 1.0) val = 1.0;
        else if (val < -1.0) val = -1.0;
        short sample = (short)(val * volume * 32767);
        fwrite(&sample, sizeof(short), 1, f);
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            fclose(f);
            remove(path);
            MessageBox(NULL, "Export cancelled", "Cancelled", MB_OK);
            return;
        }
    }

    fclose(f);
    MessageBox(NULL, "Export completed", "Done", MB_OK);
}


LRESULT CALLBACK ExportDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HWND hwndEdit;

    switch (msg) {
    case WM_CREATE:
        CreateWindow("STATIC", "Duration (1-1200s):", WS_CHILD | WS_VISIBLE,
                     10, 10, 120, 40, hwnd, NULL, NULL, NULL);

        hwndEdit = CreateWindow("EDIT", "10", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                                140, 10, 50, 20, hwnd, (HMENU)1, NULL, NULL);

        CreateWindow("BUTTON", "Export", WS_CHILD | WS_VISIBLE,
                     50, 50, 80, 25, hwnd, (HMENU)2, NULL, NULL);

        CreateWindow("BUTTON", "Cancel", WS_CHILD | WS_VISIBLE,
                     150, 50, 80, 25, hwnd, (HMENU)3, NULL, NULL);
        break;

    case WM_COMMAND:
        if (LOWORD(wParam) == 2) {
            char buf[16];
            GetWindowText(hwndEdit, buf, 15);
            int duration = atoi(buf);
            if (duration < 1 || duration > 1200) {
                MessageBox(hwnd, "Duration must be 1–1200 seconds", "Invalid", MB_OK);
                break;
            }

            char fileName[MAX_PATH] = "";
            OPENFILENAME ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = "WAV Files\0*.wav\0All Files\0*.*\0";
            ofn.lpstrFile = fileName;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_OVERWRITEPROMPT;
            ofn.lpstrDefExt = "wav";

            if (GetSaveFileName(&ofn)) {
                DestroyWindow(hwnd); 
                exportToWav(fileName, duration);
            }
        } else if (LOWORD(wParam) == 3) { 
            DestroyWindow(hwnd);
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        break;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

void exportAudioDialog(HWND hwndParent) {
    WNDCLASS wc = {};
    wc.lpfnWndProc = ExportDlgProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "ExportDialogClass";
    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(WS_EX_DLGMODALFRAME, "ExportDialogClass", "Export WAV",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        300, 200, 300, 130,
        hwndParent, NULL, GetModuleHandle(NULL), NULL);

    ShowWindow(hwnd, SW_SHOW);
    MSG msg;
    while (IsWindow(hwnd) && GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}


LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        hwndEdit = CreateWindow("EDIT", "0.9*sin(x*66)+0.7*sin(x*533+9*sin(x*3))+0.5*sin(x*540+13*sin(x*5))+0.4*sin(x*547+17*sin(x*7))+0.5*sin(x*1110+sin(x*1.2))+0.3*sin(x*72+sin(x*0.2))", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT | ES_AUTOHSCROLL,
                                10, 10, 200, 25, hwnd, NULL, NULL, NULL);
        SendMessage(hwndEdit, EM_SETLIMITTEXT, 8000, 0);
        oldEditProc = (WNDPROC)SetWindowLongPtr(hwndEdit, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc);

        hwndPlay = CreateWindow("BUTTON", "Play", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                220, 10, 50, 25, hwnd, (HMENU)1, NULL, NULL);
        hwndZoomIn = CreateWindow("BUTTON", "Zoom In", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  280, 10, 70, 25, hwnd, (HMENU)2, NULL, NULL);
        hwndZoomOut = CreateWindow("BUTTON", "Zoom Out", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   360, 10, 70, 25, hwnd, (HMENU)3, NULL, NULL);
        hwndVolLabel = CreateWindow("STATIC", "Volume:", WS_CHILD | WS_VISIBLE,
                                    440, 10, 50, 25, hwnd, NULL, NULL, NULL);
        hwndVolSlider = CreateWindow(TRACKBAR_CLASS, NULL, WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
                                     490, 10, 100, 25, hwnd, (HMENU)101, NULL, NULL);
        SendMessage(hwndVolSlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessage(hwndVolSlider, TBM_SETPOS, TRUE, (int)(volume * 100));

        hwndSpeedLabel = CreateWindow("STATIC", "Speed:", WS_CHILD | WS_VISIBLE,
                                      600, 10, 50, 25, hwnd, NULL, NULL, NULL);
        hwndSpeedSlider = CreateWindow(TRACKBAR_CLASS, NULL, WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
                                       650, 10, 120, 25, hwnd, (HMENU)102, NULL, NULL);
        SendMessage(hwndSpeedSlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 5));
        SendMessage(hwndSpeedSlider, TBM_SETPOS, TRUE, 2);

        hwndSpeedValueLabel = CreateWindow("STATIC", "1.00x", WS_CHILD | WS_VISIBLE,
                                           650, 45, 60, 25, hwnd, NULL, NULL, NULL);

        hwndStop = CreateWindow("BUTTON", "Stop", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                10, 45, 70, 25, hwnd, (HMENU)8, NULL, NULL);
        hwndCenterPlay = CreateWindow("BUTTON", "Move to Time", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   90, 45, 100, 25, hwnd, (HMENU)9, NULL, NULL);
        hwndExport = CreateWindow("BUTTON", "Export", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                               200, 45, 70, 25, hwnd, (HMENU)10, NULL, NULL);

        expr_ptr_tree = "0.9*sin(x*66)+0.7*sin(x*533+9*sin(x*3))+0.5*sin(x*540+13*sin(x*5))+0.4*sin(x*547+17*sin(x*7))+0.5*sin(x*1110+sin(x*1.2))+0.3*sin(x*72+sin(x*0.2))";
        rootExpr = parseExpressionTree();
        InvalidateRect(hwnd, NULL, TRUE);
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case 1: {
            char buf[8000];
            GetWindowText(hwndEdit, buf, 8000);
            expr_ptr_tree = buf;
            parseError = false;
            rootExpr = parseExpressionTree();
            if (!parseError && rootExpr) {
                playAudio(hwnd);
            }
            InvalidateRect(hwnd, NULL, TRUE);
            break;
        }
        case 2:
            scaleX *= 1.2f;
            if (scaleX > MAX_SCALE) scaleX = MAX_SCALE;
            InvalidateRect(hwnd, NULL, TRUE);
            break;
        case 3:
            scaleX /= 1.2f;
            if (scaleX < MIN_SCALE) scaleX = MIN_SCALE;
            InvalidateRect(hwnd, NULL, TRUE);
            break;
        case 8:
            if (isPlaying) {
                isPlaying = false;
                KillTimer(hwnd, timerId);
                waveOutReset(hWaveOut);
                waveOutClose(hWaveOut);
                hWaveOut = NULL;
                playPos = 0;
                elapsedMs = 0.0f;
                InvalidateRect(hwnd, NULL, TRUE);
            }
            offsetX=WIDTH*0.2f;
            playX=offsetX;
            InvalidateRect(hwnd, NULL, TRUE);
            break;
        case 9: {
                offsetX = -playX+offsetX+WIDTH*0.2f;
                InvalidateRect(hwnd, NULL, TRUE);
                break;
        }
        case 10:{
            if (rootExpr) exportAudioDialog(hwnd);
            else MessageBox(hwnd, "Invalid function", "Error", MB_OK);
            break;
        }
        
    }
    break;

    case WM_HSCROLL:
        if ((HWND)lParam == hwndVolSlider) {
            int pos = SendMessage(hwndVolSlider, TBM_GETPOS, 0, 0);
            volume = std::max(0.0f, std::min(1.0f, pos / 100.0f));
            if (isPlaying) playAudio(hwnd);
        } else if ((HWND)lParam == hwndSpeedSlider) {
            int pos = SendMessage(hwndSpeedSlider, TBM_GETPOS, 0, 0);
            speed = speedLevels[std::min(5, std::max(0, pos))];
            char speedText[20];
            snprintf(speedText, sizeof(speedText), "%.2fx", speed);
            SetWindowText(hwndSpeedValueLabel, speedText);
            if (isPlaying) playAudio(hwnd);
        }
        break;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        drawGraph(hdc);
        EndPaint(hwnd, &ps);
        break;
    }

    case WM_LBUTTONDOWN:
        isDragging = true;
        dragStart.x = GET_X_LPARAM(lParam);
        offsetXStart = offsetX;
        SetCapture(hwnd);
        break;

    case WM_MOUSEMOVE:
        if (isDragging) {
            int dx = GET_X_LPARAM(lParam) - dragStart.x;
            offsetX = offsetXStart + dx;
            InvalidateRect(hwnd, NULL, false);
        }
        break;

    case WM_MOUSEWHEEL:
        if (GET_WHEEL_DELTA_WPARAM(wParam) > 0) scaleX *= 1.1f;
        else scaleX /= 1.1f;
        if (scaleX < MIN_SCALE) scaleX = MIN_SCALE;
        if (scaleX > MAX_SCALE) scaleX = MAX_SCALE;
        InvalidateRect(hwnd, NULL, false);
        break;

    case WM_LBUTTONUP:
        if (isDragging) {
            isDragging = false;
            ReleaseCapture();
        }
        break;

    case WM_TIMER:
        if (wParam == timerId && isPlaying) {
            elapsedMs += 40;
            playPos = (timeGetTime() - startMs) / 1000.0f;
            float playDuration = 10.0f / speed;
            if (playPos >= playDuration) {
                isPlaying = false;
                KillTimer(hwnd, timerId);
                waveOutReset(hWaveOut);
                waveOutClose(hWaveOut);
                hWaveOut = NULL;
                playPos = 0;
                elapsedMs = 0;
                offsetX = WIDTH*0.2f;
                playX=offsetX;
            }
            InvalidateRect(hwnd, NULL, false);
        }
        break;

    case WM_DESTROY:
        if (rootExpr) freeExpr(rootExpr);
        if (hWaveOut) {
            waveOutReset(hWaveOut);
            waveOutClose(hWaveOut);
        }
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    INITCOMMONCONTROLSEX iccex = { sizeof(INITCOMMONCONTROLSEX), ICC_BAR_CLASSES };
    InitCommonControlsEx(&iccex);

    WNDCLASS wc = {};
    wc.hIcon=LoadIcon(hInstance, MAKEINTRESOURCE(MAINICON));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "FunctionGraphWnd";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    RegisterClass(&wc);

    HWND hwnd = CreateWindow(wc.lpszClassName, "Graph and Audio Synth",
                             WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
                             CW_USEDEFAULT, CW_USEDEFAULT, WIDTH, HEIGHT,
                             NULL, NULL, hInstance, NULL);

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}