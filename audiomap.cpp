#define _CRT_SECURE_NO_WARNINGS
#define COBJMACROS

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdio.h>
#include <math.h>
#include <float.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <gdiplus.h>
#include <initguid.h>
#include <stdlib.h>

#pragma comment (lib,"Gdiplus.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

#ifndef MF_SOURCE_READER_ENABLE_ADVANCED_PROCESSING
DEFINE_GUID(MF_SOURCE_READER_ENABLE_ADVANCED_PROCESSING, 
0xf6636c07, 0xd52e, 0x415f, 0x95, 0xec, 0x6a, 0x74, 0x61, 0x5b, 0x6c, 0x1e);
#endif

using namespace Gdiplus;

#define MAX_FILES 2000
#define MAX_FILE_SIZE_MB 50
#define WAVEFORM_RES 64 
#define WIN_WIDTH 800
#define WIN_HEIGHT 600

// Smooth animation helper
class UIAnim {
public:
    float value;
    void Update(bool active, float speed = 0.1f) { 
        value += active ? speed : -speed;
        if (value < 0.0f) value = 0.0f;
        if (value > 1.0f) value = 1.0f;
    }
    int GetAlpha(int min, int max) { return min + (int)((max - min) * value); }
    float GetFloat(float min, float max) { return min + (value * (max - min)); }
};

// FPS counter
class FpsCounter {
public:
    int frames, displayFps;
    float timeAccum;
    void Update(float dt) {
        frames++; 
        timeAccum += dt;
        if (timeAccum >= 1000.0f) { 
            displayFps = frames; 
            frames = 0; 
            timeAccum = 0.0f; 
        }
    }
    void Draw(HDC hdc) {
        char buf[16]; 
        sprintf(buf, "fps: %d", displayFps);
        SetTextColor(hdc, RGB(80, 80, 80)); 
        TextOutA(hdc, 15, 15, buf, (int)strlen(buf));
    }
};

// Single audio file data
typedef struct {
    char filename[MAX_PATH];
    char fullpath[MAX_PATH];
    float* visualData;
    float zcr, rms;
    int bitsPerSample, numSamples, sampleRate, channels;
    float duration;
    long fileSize;
    int screenX, screenY;
    COLORREF color;
    UIAnim listHoverAnim, textAnim;
} AudioSample;

// Global app state
typedef struct {
    AudioSample samples[MAX_FILES];
    int count;
    
    // Viewport
    float offsetX, offsetY, scale, targetScale;
    
    // Input
    int isDragging, isRightClickDrag;
    POINT lastMouse, currentMouse, rightClickStart;
    float anchorX, anchorY;
    bool keys[6];
    
    // Data bounds
    float minX, maxX, minY, maxY;
    
    // UI state
    int hoverIndex, menuIndex, menuVisible;
    UIAnim hoverAnim, menuAnim;
    
    // List view
    int sortedIndices[MAX_FILES];
    bool isListOpen;
    UIAnim listOpenAnim;
    float listScrollY, targetListScrollY;
    int listHoverIdx, listClickedIdx;
    float listClickAnim;
    
    // Scrollbar
    bool isScrollDragging;
    UIAnim scrollAnim;
    
    // Minimap
    int isMinimapDragging;
    RECT minimapRect;
    
    // Button animations
    UIAnim animBtnOpen, animBtnList, animMinimap;
    
    char statusMsg[256];
    DWORD msgStartTime;
    char* audioMem;
    FpsCounter fps;
} AppState;

AppState app = {0};

// Backbuffer
HDC g_hdcBack = NULL;
HBITMAP g_hbmBack = NULL;
int g_bbWidth = 0, g_bbHeight = 0;

// Sort by color
int CompareSamplesColor(const void* a, const void* b) {
    int idxA = *(const int*)a;
    int idxB = *(const int*)b;
    if (app.samples[idxA].color < app.samples[idxB].color) return -1;
    if (app.samples[idxA].color > app.samples[idxB].color) return 1;
    return 0;
}

void SortSamples() {
    for(int i=0; i<app.count; i++) app.sortedIndices[i] = i;
    qsort(app.sortedIndices, app.count, sizeof(int), CompareSamplesColor);
}

// Check if rect overlaps any existing dots
int CheckOverlap(RECT r) {
    for (int i = 0; i < app.count; i++) {
        if (i == app.hoverIndex) continue;
        int x = app.samples[i].screenX;
        int y = app.samples[i].screenY;
        if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) return 1;
    }
    return 0;
}

// Check if rect overlaps any in array
bool IsRectOverlap(RECT r, RECT* existing, int count) {
    for(int i=0; i<count; i++) {
        RECT dest;
        if (IntersectRect(&dest, &r, &existing[i])) return true;
    }
    return false;
}

// Decode audio to PCM
class AudioDecoder {
public:
    static short* Load(const char* filepath, int* outSamples, int* outRate, int* outChannels) {
        *outSamples = 0; *outRate = 0; *outChannels = 0;

        // Check if file exists and is not empty
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (!GetFileAttributesExA(filepath, GetFileExInfoStandard, &fad) || 
            (fad.nFileSizeHigh == 0 && fad.nFileSizeLow == 0)) {
            return NULL; 
        }

        WCHAR wPath[MAX_PATH]; 
        MultiByteToWideChar(CP_ACP, 0, filepath, -1, wPath, MAX_PATH);

        IMFSourceReader* pReader = NULL;
        if (FAILED(MFCreateSourceReaderFromURL(wPath, NULL, &pReader))) return NULL;

        // Set PCM output format
        IMFMediaType* pType = NULL;
        MFCreateMediaType(&pType);
        pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        pType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        pType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);

        if (FAILED(pReader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, pType))) {
            pType->Release(); 
            pReader->Release(); 
            return NULL;
        }
        pType->Release();

        // Get actual format
        IMFMediaType* pCurrentType = NULL;
        if (FAILED(pReader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &pCurrentType))) {
            pReader->Release(); 
            return NULL;
        }

        UINT32 rate = 44100, ch = 2;
        pCurrentType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        pCurrentType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
        pCurrentType->Release();

        // Read samples
        size_t capacity = 4096; 
        size_t count = 0; 
        short* buffer = (short*)malloc(capacity * sizeof(short));
        if (!buffer) { pReader->Release(); return NULL; }

        while (true) {
            IMFSample* pSample = NULL; 
            DWORD flags = 0;
            HRESULT hr = pReader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, NULL, &flags, NULL, &pSample);
            
            if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
                if (pSample) pSample->Release();
                break; 
            }

            if (!pSample) continue;

            IMFMediaBuffer* pMediaBuf = NULL;
            if (SUCCEEDED(pSample->ConvertToContiguousBuffer(&pMediaBuf))) {
                BYTE* data = NULL; 
                DWORD len = 0;
                if (SUCCEEDED(pMediaBuf->Lock(&data, NULL, &len))) {
                    len &= ~1; 
                    int newShorts = len / 2;
                    
                    if (count + newShorts > capacity) {
                        size_t newCap = (capacity + newShorts) * 2;
                        short* temp = (short*)realloc(buffer, newCap * sizeof(short));
                        if (!temp) { 
                            free(buffer); 
                            buffer = NULL; 
                            pMediaBuf->Unlock(); 
                            pMediaBuf->Release(); 
                            pSample->Release(); 
                            break; 
                        }
                        buffer = temp; 
                        capacity = newCap;
                    }

                    if (buffer) {
                        memcpy(buffer + count, data, len);
                        count += newShorts;
                    }
                    pMediaBuf->Unlock();
                }
                pMediaBuf->Release();
            }
            pSample->Release();
            
            if (!buffer || count > 15000000) break;
        }

        pReader->Release();

        if (count == 0 || !buffer) {
            if (buffer) free(buffer);
            return NULL;
        }

        *outSamples = (int)count;
        *outRate = rate;
        *outChannels = ch;
        return buffer;
    }
};

// Process one audio file
void ProcessFile(const char* filepath) {
    int numSamples, rate, ch;
    short* rawData = AudioDecoder::Load(filepath, &numSamples, &rate, &ch);
    if (!rawData) return;
    
    AudioSample* s = &app.samples[app.count];
    s->visualData = (float*)calloc(WAVEFORM_RES, sizeof(float));
    
    // Compute RMS and ZCR
    double totalSq = 0; 
    int crossings = 0;
    int visualStep = numSamples / WAVEFORM_RES; 
    if (visualStep < 1) visualStep = 1;
    
    for (int i = 0; i < numSamples; i++) {
        float val = rawData[i] / 32768.0f;
        if (i > 0 && rawData[i] * rawData[i-1] < 0) crossings++;
        totalSq += val * val;
        if (i % visualStep == 0 && (i/visualStep) < WAVEFORM_RES) 
            s->visualData[i/visualStep] = val;
    }
    
    float rawRms = (float)sqrt(sqrt(totalSq / numSamples)); 
    float rawZcr = (float)sqrt((float)crossings / numSamples);
    float spreadRms = powf(rawRms, 0.33f); 
    float spreadZcr = powf(rawZcr, 0.33f);
    
    // Add jitter for spacing
    float jitterX = ((float)(rand() % 100) / 100.0f - 0.5f) * 0.05f; 
    float jitterY = ((float)(rand() % 100) / 100.0f - 0.5f) * 0.05f;
    s->rms = (spreadRms + jitterY) * 5.0f; 
    s->zcr = (spreadZcr + jitterX) * 5.0f; 
    
    // File info
    const char* p = strrchr(filepath, '\\'); 
    strcpy(s->filename, p ? p + 1 : filepath); 
    strcpy(s->fullpath, filepath);
    s->bitsPerSample = 16; 
    s->numSamples = numSamples / ch; 
    s->sampleRate = rate; 
    s->channels = ch;
    s->duration = (float)s->numSamples / (float)rate; 
    s->fileSize = numSamples * 2; 
    
    // Color from ZCR
    float t = rawZcr * 3.0f; 
    if (t > 1.0f) t = 1.0f;
    int r, g, b;
    if (t < 0.5f) { 
        float lt = t*2.0f; 
        r=255-(int)(lt*100); 
        g=100+(int)(lt*155); 
        b=100; 
    } else { 
        float lt = (t-0.5f)*2.0f; 
        r=155-(int)(lt*100); 
        g=255-(int)(lt*100); 
        b=100+(int)(lt*155); 
    }
    s->color = RGB((r+255)/2, (g+255)/2, (b+255)/2);
    
    app.count++; 
    free(rawData);
}

// Play audio file
void PlayAudio(int index) {
    if (index < 0 || index >= app.count) return;

    PlaySoundA(NULL, NULL, 0);

    if (app.audioMem) { 
        free(app.audioMem); 
        app.audioMem = NULL; 
    }

    int samples, rate, ch; 
    short* pcm = AudioDecoder::Load(app.samples[index].fullpath, &samples, &rate, &ch);
    
    if (!pcm || samples <= 0) {
        if(pcm) free(pcm);
        return;
    }

    // Reduce volume
    for (int i = 0; i < samples; i++) 
        pcm[i] = (short)(pcm[i] * 0.5f);

    int dataSize = samples * 2; 
    int totalSize = 44 + dataSize;
    
    app.audioMem = (char*)malloc(totalSize); 
    if (!app.audioMem) { 
        free(pcm); 
        return; 
    }

    // Build WAV header
    char* p = app.audioMem;
    memcpy(p, "RIFF", 4); p+=4; 
    *(int*)p = totalSize - 8; p+=4; 
    memcpy(p, "WAVE", 4); p+=4;
    memcpy(p, "fmt ", 4); p+=4; 
    *(int*)p = 16; p+=4; 
    *(short*)p = 1; p+=2; 
    *(short*)p = ch; p+=2;
    *(int*)p = rate; p+=4; 
    *(int*)p = rate * ch * 2; p+=4; 
    *(short*)p = ch * 2; p+=2; 
    *(short*)p = 16; p+=2;
    memcpy(p, "data", 4); p+=4; 
    *(int*)p = dataSize; p+=4; 
    memcpy(p, pcm, dataSize); 
    
    free(pcm);

    PlaySoundA((LPCSTR)app.audioMem, NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

// Scan directory for audio files
void ScanDirectory(const char* folder) {
    char searchPath[MAX_PATH]; 
    WIN32_FIND_DATAA fd;
    const char* exts[] = { "wav", "mp3", "flac" }; 

    for(int e=0; e<3; e++) {
        sprintf(searchPath, "%s\\*.%s", folder, exts[e]);
        HANDLE hFind = FindFirstFileA(searchPath, &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (app.count >= MAX_FILES) break;
                if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
                
                char fullPath[MAX_PATH]; 
                sprintf(fullPath, "%s\\%s", folder, fd.cFileName);
                ProcessFile(fullPath);
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);
        }
    }

    // Auto-scale canvas for large datasets
    float density = (float)app.count;
    float spreadFactor = 1.0f;
    
    if (density > 50) {
        spreadFactor = logf(density) * 2.5f; 
    }
    
    for(int i=0; i<app.count; i++) {
        app.samples[i].zcr *= spreadFactor;
        app.samples[i].rms *= spreadFactor;
    }

    SortSamples();
}

// Update world bounds
void UpdateBounds() {
    app.minX = FLT_MAX; app.maxX = -FLT_MAX; 
    app.minY = FLT_MAX; app.maxY = -FLT_MAX;
    
    for (int i = 0; i < app.count; i++) {
        if (app.samples[i].zcr < app.minX) app.minX = app.samples[i].zcr; 
        if (app.samples[i].zcr > app.maxX) app.maxX = app.samples[i].zcr;
        if (app.samples[i].rms < app.minY) app.minY = app.samples[i].rms; 
        if (app.samples[i].rms > app.maxY) app.maxY = app.samples[i].rms;
    }
    
    if (app.maxX - app.minX < 0.001f) app.maxX = app.minX + 0.1f;
    if (app.maxY - app.minY < 0.001f) app.maxY = app.minY + 0.1f;
}

// Folder picker dialog
int PickFolder(HWND hwnd, char* outPath) {
    IFileDialog *pfd = NULL; 
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, (void**)&pfd);
    if (SUCCEEDED(hr)) {
        DWORD dwOptions; 
        pfd->GetOptions(&dwOptions); 
        pfd->SetOptions(dwOptions | FOS_PICKFOLDERS);
        
        if (SUCCEEDED(pfd->Show(hwnd))) {
            IShellItem *psi; 
            if (SUCCEEDED(pfd->GetResult(&psi))) {
                PWSTR pszPath; 
                if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath))) {
                    wcstombs(outPath, pszPath, MAX_PATH); 
                    CoTaskMemFree(pszPath); 
                    psi->Release(); 
                    pfd->Release(); 
                    return 1;
                } 
                psi->Release();
            }
        } 
        pfd->Release();
    } 
    return 0;
}

// Create/resize backbuffer
void ResizeBackBuffer(HDC hdc, int w, int h) {
    if (g_hbmBack) DeleteObject(g_hbmBack); 
    if (!g_hdcBack) g_hdcBack = CreateCompatibleDC(hdc);
    g_hbmBack = CreateCompatibleBitmap(hdc, w, h); 
    SelectObject(g_hdcBack, g_hbmBack);
    g_bbWidth = w; 
    g_bbHeight = h;
}

// Main rendering
void DrawMap(HDC hdc, RECT clientRect) {
    if (clientRect.right == 0 || clientRect.bottom == 0) return;
    if (!g_hdcBack || g_bbWidth != clientRect.right || g_bbHeight != clientRect.bottom) 
        ResizeBackBuffer(hdc, clientRect.right, clientRect.bottom);

    Gdiplus::Graphics g(g_hdcBack);

    // Clear background
    RECT bgR = {0, 0, clientRect.right, clientRect.bottom};
    HBRUSH hBr = CreateSolidBrush(RGB(20, 20, 25));
    FillRect(g_hdcBack, &bgR, hBr);
    DeleteObject(hBr);

    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighSpeed);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit); 

    // Grid (no antialiasing for speed)
    g.SetSmoothingMode(Gdiplus::SmoothingModeNone); 
    Gdiplus::Pen gridPen(Gdiplus::Color(255, 35, 35, 40), 1.0f);
    for (int x = 0; x < clientRect.right; x += 50) 
        g.DrawLine(&gridPen, x, 0, x, clientRect.bottom);
    for (int y = 0; y < clientRect.bottom; y += 50) 
        g.DrawLine(&gridPen, 0, y, clientRect.right, y);

    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    // Connection lines to nearest neighbors
    if (app.hoverIndex != -1 && app.hoverAnim.value > 0.01f) {
        AudioSample* t = &app.samples[app.hoverIndex];
        int alpha = app.hoverAnim.GetAlpha(20, 200);
        Gdiplus::Pen linePen(Gdiplus::Color(alpha, alpha, alpha, alpha), 1.5f);
        
        struct { int id; float dist; } closest[5];
        for(int k=0; k<5; k++) closest[k] = { -1, FLT_MAX };

        for (int i = 0; i < app.count; i++) {
             if (i == app.hoverIndex) continue;
             if (abs(t->screenX - app.samples[i].screenX) > 500) continue; 

             float dist = (t->zcr - app.samples[i].zcr)*(t->zcr - app.samples[i].zcr) + 
                          (t->rms - app.samples[i].rms)*(t->rms - app.samples[i].rms);
             if (dist < 0.5f) { 
                 for(int k=0; k<5; k++) {
                     if(dist < closest[k].dist) {
                         for(int j=4; j>k; j--) closest[j] = closest[j-1];
                         closest[k] = { i, dist };
                         break;
                     }
                 }
             }
        }

        for(int k=0; k<5; k++) {
            if(closest[k].id != -1) {
                AudioSample* target = &app.samples[closest[k].id];
                if (alpha > 5) {
                     g.DrawLine(&linePen, (float)t->screenX, (float)t->screenY, 
                               (float)target->screenX, (float)target->screenY);
                }
            }
        }
    }

    int cx = clientRect.right / 2; 
    int cy = clientRect.bottom / 2;
    int viewLeft = 0, viewTop = 0, viewRight = clientRect.right, viewBottom = clientRect.bottom;
    
    RECT mmRect = { clientRect.right - 180, clientRect.bottom - 130, 
                    clientRect.right - 15, clientRect.bottom - 15 };
    app.minimapRect = mmRect;

    // Font setup
    HFONT hFontUI = CreateFontA(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, 
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, 
                                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HFONT hOldFont = (HFONT)SelectObject(g_hdcBack, hFontUI);
    SetBkMode(g_hdcBack, TRANSPARENT);

    auto BlendColor = [](int alpha) -> COLORREF {
        float a = alpha / 255.0f;
        int r = 20 + (int)((237 - 20) * a);
        return RGB(r, r, r + (int)(5 * a));
    };

    RECT drawnRects[MAX_FILES]; 
    int drawnCount = 0;

    // Draw all samples
    for (int i = 0; i < app.count; i++) {
        AudioSample* s = &app.samples[i];

        // Screen position
        float fx = cx + (s->zcr + app.offsetX) * app.scale * 2.0f;
        float fy = cy - (s->rms + app.offsetY) * app.scale;
        s->screenX = (int)fx; 
        s->screenY = (int)fy;

        // Cull offscreen
        if (s->screenX < viewLeft - 20 || s->screenX > viewRight + 20 || 
            s->screenY < viewTop - 20 || s->screenY > viewBottom + 20) continue;

        // Skip if in minimap
        if (s->screenX >= mmRect.left && s->screenX <= mmRect.right && 
            s->screenY >= mmRect.top && s->screenY <= mmRect.bottom) continue;

        // Draw dot
        int isHover = (i == app.hoverIndex);
        float r = 12.0f + (6.0f * app.hoverAnim.value * (isHover ? 1.0f : 0.0f));
        
        Gdiplus::Color dotCol; 
        dotCol.SetFromCOLORREF(s->color);
        if (isHover) dotCol = Gdiplus::Color(255, 237, 237, 237);
        Gdiplus::SolidBrush br(dotCol);
        g.FillEllipse(&br, s->screenX - r, s->screenY - r, r*2, r*2);

        // Text label (zoom dependent)
        bool showText = (!isHover && app.scale > 80.0f && drawnCount < 100);
        
        if (showText) {
            SIZE sz; 
            GetTextExtentPoint32A(g_hdcBack, s->filename, (int)strlen(s->filename), &sz);
            RECT rTxt = { s->screenX + (int)r + 4, s->screenY - sz.cy/2, 
                          s->screenX + (int)r + 4 + sz.cx, s->screenY + sz.cy/2 };
            
            if (rTxt.right >= clientRect.right || IsRectOverlap(rTxt, drawnRects, drawnCount)) {
                showText = false;
            } else {
                drawnRects[drawnCount++] = rTxt;
            }
        }
        
        s->textAnim.Update(showText, 0.08f); 
        if (s->textAnim.value > 0.01f) {
            int val = 20 + (int)((130 - 20) * s->textAnim.value);
            SetTextColor(g_hdcBack, RGB(val, val, val));
            SIZE sz; 
            GetTextExtentPoint32A(g_hdcBack, s->filename, (int)strlen(s->filename), &sz);
            TextOutA(g_hdcBack, s->screenX + (int)r + 4, s->screenY - sz.cy/2, 
                    s->filename, (int)strlen(s->filename));
        }

        // Hover widget
        if (isHover && app.hoverAnim.value > 0.01f && !app.menuVisible) {
            if (!app.menuVisible) {
                SetTextColor(g_hdcBack, RGB(120, 120, 120)); 
                TextOutA(g_hdcBack, s->screenX + (int)r + 12, s->screenY - 5, 
                        "right click for more info", 25);
            }
            
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias); 

            // Waveform
            int pulse = 180 + (int)(sinf(GetTickCount() * 0.0069f) * 50.0f);
            if(pulse>255) pulse=255;
            Gdiplus::Color waveCol((int)(pulse * app.hoverAnim.value), 237, 237, 237);
            float wfW = 80.0f, wfH = 30.0f;
            float drawX = s->screenX - wfW/2.0f;
            float drawY = s->screenY - r - 10.0f - wfH;
            
            Gdiplus::Pen wPen(waveCol, 1.0f);
            for(int w=0; w<WAVEFORM_RES-1; w++) {
                float x1 = drawX + ((float)w * wfW / (float)WAVEFORM_RES);
                float y1 = (drawY + wfH/2.0f) - (s->visualData[w] * (wfH/2.0f));
                float x2 = drawX + ((float)(w+1) * wfW / (float)WAVEFORM_RES);
                float y2 = (drawY + wfH/2.0f) - (s->visualData[w+1] * (wfH/2.0f));
                g.DrawLine(&wPen, x1, y1, x2, y2);
            }

            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            SetTextColor(g_hdcBack, BlendColor((int)(255 * app.hoverAnim.value)));
            SIZE sz; 
            GetTextExtentPoint32A(g_hdcBack, s->filename, (int)strlen(s->filename), &sz);
            TextOutA(g_hdcBack, (int)(drawX + wfW/2 - sz.cx/2), (int)(drawY - sz.cy - 2), 
                    s->filename, (int)strlen(s->filename));
        }
    }

    // Context menu
    if (app.menuIndex >= 0 && app.menuIndex < app.count && app.menuVisible && app.menuAnim.value > 0.01f) {
        AudioSample* s = &app.samples[app.menuIndex];
        
        // Find most similar sample
        int simIdx = -1; 
        float minDist = FLT_MAX;
        for(int i=0; i<app.count; i++) { 
            if(i==app.menuIndex) continue; 
            float d = (s->zcr - app.samples[i].zcr) * (s->zcr - app.samples[i].zcr) + 
                      (s->rms - app.samples[i].rms) * (s->rms - app.samples[i].rms); 
            if(d < minDist) {
                minDist = d; 
                simIdx = i;
            }
        }

        char lines[5][128]; 
        sprintf(lines[0], "%.2f MB", s->fileSize/(1024.0*1024.0)); 
        sprintf(lines[1], "%.2fs (%d Hz)", s->duration, s->sampleRate);
        sprintf(lines[2], "%d Samples", s->numSamples); 
        sprintf(lines[3], "%d-bit %s", s->bitsPerSample, s->channels==2?"Stereo":"Mono");
        sprintf(lines[4], "ZCR: %.2f  RMS: %.2f", s->zcr, s->rms);

        int maxW=0; 
        SIZE sz; 
        for(int i=0; i<5; i++){
            GetTextExtentPoint32A(g_hdcBack, lines[i], (int)strlen(lines[i]), &sz); 
            if(sz.cx>maxW) maxW=sz.cx;
        }

        int mx = s->screenX + 20; 
        int my = s->screenY + 25; 
        int menuW = maxW + 20; 
        int menuH = 6 * 16 + 12; 

        if(my + menuH > clientRect.bottom) my = s->screenY - menuH - 25; 
        if(mx + menuW > clientRect.right) mx = s->screenX - menuW - 20;

        int a = app.menuAnim.GetAlpha(0, 210); 
        Gdiplus::SolidBrush mb(Gdiplus::Color(a, 15, 15, 15)); 
        g.FillRectangle(&mb, mx-5, my-5, menuW, menuH);

        int ta = app.menuAnim.GetAlpha(0, 220); 
        SetTextColor(g_hdcBack, RGB(ta, ta, ta+5));
        for(int i=0; i<5; i++) 
            TextOutA(g_hdcBack, mx, my+i*16, lines[i], (int)strlen(lines[i]));
        
        TextOutA(g_hdcBack, mx, my+5*16, "Sim:", 4);
        if(simIdx != -1) { 
            Gdiplus::Color c; 
            c.SetFromCOLORREF(app.samples[simIdx].color); 
            Gdiplus::SolidBrush sb(Gdiplus::Color(ta, c.GetRed(), c.GetGreen(), c.GetBlue())); 
            g.FillEllipse(&sb, mx+30, my+5*16+5, 7, 7); 
            TextOutA(g_hdcBack, mx+45, my+5*16, app.samples[simIdx].filename, 
                    (int)strlen(app.samples[simIdx].filename));
        }
    }

    app.fps.Draw(g_hdcBack);

    g.SetSmoothingMode(Gdiplus::SmoothingModeNone); 

    // Minimap
    if (app.count > 0) {
        const char* hint = "             minimap             ";
        SIZE sz; 
        GetTextExtentPoint32A(g_hdcBack, hint, (int)strlen(hint), &sz);
        int mmW = sz.cx, mmH = 100;
        int mmX = clientRect.right - mmW - 15;
        int mmY = clientRect.bottom - mmH - 15;
        app.minimapRect = { mmX, mmY, mmX + mmW, mmY + mmH };

        int alpha = app.animMinimap.GetAlpha(100, 237);
        SetTextColor(g_hdcBack, BlendColor(alpha));
        TextOutA(g_hdcBack, mmX, mmY - sz.cy - 5, hint, (int)strlen(hint));
        
        Gdiplus::SolidBrush mmBrush(Gdiplus::Color(app.animMinimap.GetAlpha(255, 200), 10, 10, 15));
        g.FillRectangle(&mmBrush, mmX, mmY, mmW, mmH);

        float rangeX = app.maxX - app.minX; 
        float rangeY = app.maxY - app.minY;
        for(int i=0; i<app.count; i++) {
            int mx = mmX + (int)((app.samples[i].zcr - app.minX) / rangeX * mmW);
            int my = mmY + mmH - (int)((app.samples[i].rms - app.minY) / rangeY * mmH);
            if(mx >= mmX && mx < mmX+mmW && my >= mmY && my < mmY+mmH) 
                SetPixel(g_hdcBack, mx, my, app.samples[i].color);
        }
        
        // Viewport rect
        Gdiplus::Pen viewPen(Gdiplus::Color(alpha, 237, 237, 237), 1.0f);
        float wL = -app.offsetX - (cx / (app.scale * 2.0f)); 
        float wR = -app.offsetX + ((clientRect.right - cx) / (app.scale * 2.0f));
        float wT = -app.offsetY + (cy / app.scale); 
        float wB = -app.offsetY - ((clientRect.bottom - cy) / app.scale);
        int vL = mmX + (int)((wL - app.minX) / rangeX * mmW); 
        int vR = mmX + (int)((wR - app.minX) / rangeX * mmW);
        int vT = mmY + mmH - (int)((wT - app.minY) / rangeY * mmH); 
        int vB = mmY + mmH - (int)((wB - app.minY) / rangeY * mmH);
        if (vL < mmX) vL = mmX; if (vR > mmX + mmW) vR = mmX + mmW; 
        if (vT < mmY) vT = mmY; if (vB > mmY + mmH) vB = mmY + mmH;
        g.DrawRectangle(&viewPen, vL, vT, vR - vL, vB - vT);
    }

    // Buttons
    int btnW = 70, btnH = 26, btnY = clientRect.bottom - 40; 
    Gdiplus::SolidBrush btnBg(Gdiplus::Color(255, 20, 20, 25));

    // Open button
    int alpha1 = app.animBtnOpen.GetAlpha(100, 237);
    Gdiplus::Pen btnPen1(Gdiplus::Color(alpha1, 237, 237, 237), 1.0f);
    g.FillRectangle(&btnBg, 15, btnY, btnW, btnH);
    g.DrawRectangle(&btnPen1, 15, btnY, btnW, btnH);
    SetTextColor(g_hdcBack, BlendColor(alpha1));
    const char* txt1 = "open"; 
    SIZE sz1; 
    GetTextExtentPoint32A(g_hdcBack, txt1, 4, &sz1);
    TextOutA(g_hdcBack, 15 + (btnW - sz1.cx)/2, btnY + (btnH - sz1.cy)/2, txt1, 4);

    // Key hint
    const char* key1 = "o";
    SIZE szKey1;
    GetTextExtentPoint32A(g_hdcBack, key1, 1, &szKey1);
    int keyAlpha1 = app.animBtnOpen.GetAlpha(60, 150);
    SetTextColor(g_hdcBack, RGB(keyAlpha1, keyAlpha1, keyAlpha1));
    TextOutA(g_hdcBack, 15 + btnW - szKey1.cx - 4, btnY + 2, key1, 1);

    // List button
    int btn2X = 95;
    int alpha2 = app.animBtnList.GetAlpha(100, 237);
    Gdiplus::Pen btnPen2(Gdiplus::Color(alpha2, 237, 237, 237), 1.0f);
    g.FillRectangle(&btnBg, btn2X, btnY, btnW, btnH);
    g.DrawRectangle(&btnPen2, btn2X, btnY, btnW, btnH);
    SetTextColor(g_hdcBack, BlendColor(alpha2));
    const char* txt2 = "list"; 
    SIZE sz2; 
    GetTextExtentPoint32A(g_hdcBack, txt2, 4, &sz2);
    TextOutA(g_hdcBack, btn2X + (btnW - sz2.cx)/2, btnY + (btnH - sz2.cy)/2, txt2, 4);

    // Key hint
    const char* key2 = "l";
    SIZE szKey2;
    GetTextExtentPoint32A(g_hdcBack, key2, 1, &szKey2);
    int keyAlpha2 = app.animBtnList.GetAlpha(60, 150);
    SetTextColor(g_hdcBack, RGB(keyAlpha2, keyAlpha2, keyAlpha2));
    TextOutA(g_hdcBack, btn2X + btnW - szKey2.cx - 4, btnY + 2, key2, 1);

    // Status message
    if (strlen(app.statusMsg) > 0) {
        DWORD elapsed = GetTickCount() - app.msgStartTime;
        if (elapsed < 5000) { 
            int val = 237; 
            if (elapsed > 3000) val = 237 - (int)((elapsed - 3000) * 237 / 2000);
            SetTextColor(g_hdcBack, RGB(val, val, val)); 
            TextOutA(g_hdcBack, 15, clientRect.bottom - 58, app.statusMsg, (int)strlen(app.statusMsg));
        }
    }

    // List panel
    if (app.listOpenAnim.value > 0.01f) {
        int listW = 400, listH = clientRect.bottom - 100;
        int listX = (clientRect.right - listW) / 2, listY = 50;
        
        // Dim overlay
        int alphaDim = app.listOpenAnim.GetAlpha(0, 200);
        Gdiplus::SolidBrush dimBrush(Gdiplus::Color(alphaDim, 0, 0, 0));
        g.FillRectangle(&dimBrush, 0, 0, clientRect.right, clientRect.bottom);

        // List background
        int alphaWin = app.listOpenAnim.GetAlpha(0, 255);
        Gdiplus::SolidBrush listBg(Gdiplus::Color(alphaWin, 30, 30, 35));
        g.FillRectangle(&listBg, listX, listY, listW, listH);

        // "esc to close" hint
        const char* escHint = "esc to close";
        SIZE szHint;
        GetTextExtentPoint32A(g_hdcBack, escHint, (int)strlen(escHint), &szHint);
        int hintAlpha = app.listOpenAnim.GetAlpha(0, 150);
        SetTextColor(g_hdcBack, RGB(hintAlpha, hintAlpha, hintAlpha));
        TextOutA(g_hdcBack, listX + 8, listY + 6, escHint, (int)strlen(escHint));

        // Scrollbar
        int itemH = 25, totalH = app.count * itemH;
        if (totalH > listH) {
             float ratio = (float)listH / (float)totalH; 
             int sbH = (int)(listH * ratio); 
             if(sbH < 20) sbH = 20;
             int sbY = listY + (int)((app.listScrollY / (totalH - listH)) * (listH - sbH));
             int sbW = (int)app.scrollAnim.GetFloat(4.0f, 12.0f); 
             int sbAlpha = app.listOpenAnim.GetAlpha(0, app.scrollAnim.GetAlpha(100, 180));
             int sbX = (listX + listW) - sbW; 
             
             Gdiplus::SolidBrush sbBrush(Gdiplus::Color(sbAlpha, 80, 80, 90));
             g.FillRectangle(&sbBrush, sbX, sbY, sbW, sbH);
        }

        // Clip region (exclude top area for hint)
        int clipTopPadding = 24; // Reserve space for "esc to close" hint
        Gdiplus::Region region(Gdiplus::Rect(listX, listY + clipTopPadding, listW, listH - clipTopPadding)); 
        g.SetClip(&region);
        HRGN hRgn = CreateRectRgn(listX, listY + clipTopPadding, listX + listW, listY + listH); 
        SelectClipRgn(g_hdcBack, hRgn);

        int startIdx = (int)(app.listScrollY / itemH); 
        int visibleCount = (listH / itemH) + 2;

        SelectObject(g_hdcBack, hFontUI);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        int endLoop = startIdx + visibleCount;
        if (endLoop > app.count) endLoop = app.count;

        for(int i = startIdx; i < endLoop; i++) {
            if(i < 0) continue;
            
            int sIdx = app.sortedIndices[i]; 
            AudioSample* s = &app.samples[sIdx];
            int yPos = listY + (i * itemH) - (int)app.listScrollY;
            
            // Edge fade - increased fade zone from 40 to 60px
            float fadeAlpha = 1.0f;
            int distTop = yPos - listY;
            int distBot = (listY + listH) - (yPos + itemH);
            
            if (distTop < 60) fadeAlpha = (float)distTop / 60.0f;  // Changed from 40 to 60
            if (distBot < 60) fadeAlpha = (float)distBot / 60.0f;  // Changed from 40 to 60
            if (fadeAlpha < 0.0f) fadeAlpha = 0.0f;
            if (fadeAlpha > 1.0f) fadeAlpha = 1.0f;

            // Dot
            Gdiplus::Color c; 
            c.SetFromCOLORREF(s->color); 
            int finalDotAlpha = (int)(alphaWin * fadeAlpha);
            
            Gdiplus::SolidBrush b(Gdiplus::Color(finalDotAlpha, c.GetRed(), c.GetGreen(), c.GetBlue()));
            g.FillEllipse(&b, listX + 10, yPos + 7, 10, 10);
            
            // Click flash
            if (sIdx == app.listClickedIdx && app.listClickAnim > 0.01f) {
                int wAlpha = (int)(200 * app.listClickAnim * fadeAlpha); 
                Gdiplus::SolidBrush flash(Gdiplus::Color(wAlpha, 255, 255, 255));
                g.FillEllipse(&flash, listX + 10, yPos + 7, 10, 10);
            }
            
            // Text
            g.SetSmoothingMode(Gdiplus::SmoothingModeNone);
            int baseVal = 200; 
            int hVal = s->listHoverAnim.GetAlpha(baseVal, 255);
            int rT = 30 + (int)((hVal - 30) * fadeAlpha);
            int gT = 30 + (int)((hVal - 30) * fadeAlpha);
            int bT = 35 + (int)((hVal - 35) * fadeAlpha);
            
            SetTextColor(g_hdcBack, RGB(rT, gT, bT));
            TextOutA(g_hdcBack, listX + 30, yPos + 4, s->filename, (int)strlen(s->filename));
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias); 
        }
        
        SelectClipRgn(g_hdcBack, NULL); 
        DeleteObject(hRgn); 
        g.ResetClip();
    }

    SelectObject(g_hdcBack, hOldFont); 
    DeleteObject(hFontUI);
    BitBlt(hdc, 0, 0, clientRect.right, clientRect.bottom, g_hdcBack, 0, 0, SRCCOPY);
}

// Create app icon
HICON CreateSineIcon(HINSTANCE hInst) {
    int w = 64, h = 64;

    Bitmap bmp(w, h, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics g(&bmp);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

        // Background
        Rect rect(1, 1, w - 2, h - 2);
        Gdiplus::SolidBrush bgBrush(Gdiplus::Color(255, 20, 20, 25));
        g.FillEllipse(&bgBrush, rect);
        Pen borderPen(Gdiplus::Color(255, 60, 60, 70), 2.0f);
        g.DrawEllipse(&borderPen, rect);

        // Generate sine wave points
        int dotCount = 7;
        PointF pts[7];
        Gdiplus::Color dotColors[7];

        float startX = w * 0.2f;
        float endX = w * 0.8f;
        float step = (endX - startX) / (dotCount - 1);

        for (int i = 0; i < dotCount; i++) {
            float t = (float)i / (dotCount - 1);

            float x = startX + i * step;
            float val = sinf(t * 3.14159f * 2.0f);
            float y = (h / 2.0f) + (val * (h * 0.25f));
            pts[i] = PointF(x, y);

            // Color gradient
            int r, gr, b;
            if (t < 0.5f) { 
                float lt = t * 2.0f;
                r = 255 - (int)(lt * 100); 
                gr = 100 + (int)(lt * 155); 
                b = 100;
            } else { 
                float lt = (t - 0.5f) * 2.0f;
                r = 155 - (int)(lt * 100); 
                gr = 255 - (int)(lt * 100); 
                b = 100 + (int)(lt * 155);
            }
            dotColors[i] = Gdiplus::Color(255, (r+255)/2, (gr+255)/2, (b+255)/2);
        }

        // Draw connections
        Pen linePen(Gdiplus::Color(150, 200, 200, 200), 1.5f);
        g.DrawLines(&linePen, pts, dotCount);

        // Draw dots
        for (int i = 0; i < dotCount; i++) {
            float r = 4.0f;
            Gdiplus::SolidBrush brush(dotColors[i]);
            g.FillEllipse(&brush, pts[i].X - r, pts[i].Y - r, r*2, r*2);
        }
    }

    HICON hIcon = NULL;
    bmp.GetHICON(&hIcon);
    return hIcon;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_CREATE:
        app.scale = 300.0f; 
        app.targetScale = 300.0f; 
        app.hoverIndex = -1; 
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        MFStartup(MF_VERSION);
        sprintf(app.statusMsg, "click 'open' or press 'o' to load samples.");
        app.msgStartTime = GetTickCount();
        return 0;

    case WM_SETCURSOR:
        if (app.hoverIndex != -1 || app.animBtnOpen.value > 0.1f || app.animBtnList.value > 0.1f) { 
            SetCursor(LoadCursor(NULL, IDC_HAND)); 
            return TRUE; 
        }
        break;

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        
        if (app.isListOpen) {
            RECT r; 
            GetClientRect(hwnd, &r);
            int listH = r.bottom - 100;
            if (app.count * 25 > listH) {
                app.targetListScrollY -= (delta / 2.0f); 
            }
        } else {
            app.targetScale *= (delta > 0) ? 1.2f : 0.8f;
            if (app.targetScale < 20.0f) app.targetScale = 20.0f; 
            if (app.targetScale > 100000.0f) app.targetScale = 100000.0f;
            
            POINT pt; 
            GetCursorPos(&pt); 
            ScreenToClient(hwnd, &pt); 
            app.currentMouse = pt;
        }
    } return 0;

    case WM_KEYDOWN: {
        switch(wParam) {
            case VK_LEFT:  app.keys[0] = true; break;
            case VK_RIGHT: app.keys[1] = true; break;
            case VK_UP:    app.keys[2] = true; break;
            case VK_DOWN:  app.keys[3] = true; break;
            case VK_PRIOR: app.targetScale *= 1.2f; break; 
            case VK_NEXT:  app.targetScale *= 0.8f; break;
            case VK_ESCAPE:
                if (app.isListOpen) {
                    app.isListOpen = false;
                } else {
                    PostQuitMessage(0);
                }
                break;
            case 'O': // Open folder
                {
                    char path[MAX_PATH];
                    if (PickFolder(hwnd, path)) {
                        for(int i=0; i<app.count; i++) free(app.samples[i].visualData);
                        app.count = 0; 
                        ScanDirectory(path);
                        if (app.count > 0) {
                            UpdateBounds(); 
                            app.offsetX = -(app.maxX + app.minX) / 2.0f; 
                            app.offsetY = -(app.maxY + app.minY) / 2.0f;
                            sprintf(app.statusMsg, "loaded %d samples.", app.count);
                        }
                        app.msgStartTime = GetTickCount(); 
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                }
                break;
            case 'L': // Toggle list
                app.isListOpen = !app.isListOpen;
                break;
        }
    } return 0;
    
    case WM_KEYUP: {
        switch(wParam) {
            case VK_LEFT:  app.keys[0] = false; break;
            case VK_RIGHT: app.keys[1] = false; break;
            case VK_UP:    app.keys[2] = false; break;
            case VK_DOWN:  app.keys[3] = false; break;
        }
    } return 0;

    case WM_LBUTTONDOWN: {
        int mx = (short)LOWORD(lParam); 
        int my = (short)HIWORD(lParam);
        RECT r; 
        GetClientRect(hwnd, &r);
        
        // Minimap
        if (app.count > 0 && mx >= app.minimapRect.left && mx <= app.minimapRect.right &&
            my >= app.minimapRect.top && my <= app.minimapRect.bottom) {
            app.isMinimapDragging = 1; 
            SetCapture(hwnd);
            return 0;
        }
        
        // Open button
        if (mx >= 15 && mx <= 85 && my >= r.bottom - 40 && my <= r.bottom - 14) {
            char path[MAX_PATH];
            if (PickFolder(hwnd, path)) {
                for(int i=0; i<app.count; i++) free(app.samples[i].visualData);
                app.count = 0; 
                ScanDirectory(path);
                if (app.count > 0) {
                    UpdateBounds(); 
                    app.offsetX = -(app.maxX + app.minX) / 2.0f; 
                    app.offsetY = -(app.maxY + app.minY) / 2.0f;
                    sprintf(app.statusMsg, "loaded %d samples.", app.count);
                }
                app.msgStartTime = GetTickCount(); 
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }

        // List button
        if (mx >= 95 && mx <= 165 && my >= r.bottom - 40 && my <= r.bottom - 14) {
            app.isListOpen = !app.isListOpen;
            return 0;
        }

        // List interaction
        if (app.isListOpen) {
            int listW = 400, listX = (r.right - listW) / 2;
            int listH = r.bottom - 100, listY = 50;
            
            // Scrollbar
            if (mx >= listX + listW - 20 && mx <= listX + listW + 10 && my >= listY && my <= listY + listH) {
                app.isScrollDragging = true; 
                SetCapture(hwnd); 
                return 0;
            }

            // Close or item click
            if (mx < listX || mx > listX + listW || my < listY || my > listY + listH) {
                app.isListOpen = false;
            } else {
                if(my > listY && my < listY + listH) {
                    int idx = (int)((my - listY + app.listScrollY) / 25);
                    if(idx >= 0 && idx < app.count) {
                        int actualIdx = app.sortedIndices[idx];
                        PlayAudio(actualIdx);
                        app.listClickedIdx = actualIdx; 
                        app.listClickAnim = 1.0f; 
                    }
                }
            }
            return 0;
        }
        
        // Canvas click
        app.menuVisible = 0; 
        if (app.hoverIndex != -1) {
            PlayAudio(app.hoverIndex);
        }

        app.isDragging = 1; 
        app.lastMouse.x = mx; 
        app.lastMouse.y = my;
        app.anchorX = app.offsetX; 
        app.anchorY = app.offsetY; 
        SetCapture(hwnd); 
        return 0;
    }

    case WM_MBUTTONDOWN:
        app.menuVisible = 0; 
        app.isDragging = 1; 
        app.lastMouse.x = LOWORD(lParam); 
        app.lastMouse.y = HIWORD(lParam);
        app.anchorX = app.offsetX; 
        app.anchorY = app.offsetY; 
        SetCapture(hwnd); 
        return 0;

    case WM_LBUTTONUP: 
    case WM_MBUTTONUP:
        app.isDragging = 0; 
        app.isMinimapDragging = 0; 
        app.isScrollDragging = 0;
        ReleaseCapture(); 
        return 0;

    case WM_RBUTTONDOWN:
        if (app.isListOpen) {
            RECT r; 
            GetClientRect(hwnd, &r);
            int listW = 400, listX = (r.right - listW) / 2;
            int listH = r.bottom - 100, listY = 50;
            int mx = (short)LOWORD(lParam);
            int my = (short)HIWORD(lParam);
            if (mx < listX || mx > listX + listW || my < listY || my > listY + listH) {
                app.isListOpen = false;
            }
            return 0; 
        }

        app.isDragging = 1; 
        app.isRightClickDrag = 1;
        app.lastMouse.x = (short)LOWORD(lParam); 
        app.lastMouse.y = (short)HIWORD(lParam);
        app.rightClickStart = app.lastMouse;
        app.anchorX = app.offsetX; 
        app.anchorY = app.offsetY; 
        SetCapture(hwnd); 
        return 0;

    case WM_RBUTTONUP:
        if (app.isRightClickDrag) {
            int dx = abs(LOWORD(lParam) - app.rightClickStart.x);
            int dy = abs(HIWORD(lParam) - app.rightClickStart.y);
            
            if (dx < 10 && dy < 10) {
                if (app.hoverIndex != -1) {
                    app.menuIndex = app.hoverIndex; 
                    app.menuVisible = 1;
                } else {
                    app.menuVisible = 0;
                }
            }
        }
        app.isDragging = 0; 
        app.isRightClickDrag = 0; 
        ReleaseCapture(); 
        return 0;

    case WM_MOUSEMOVE: {
        int mx = (short)LOWORD(lParam); 
        int my = (short)HIWORD(lParam);
        app.currentMouse.x = mx; 
        app.currentMouse.y = my;

        // Scrollbar drag
        if (app.isListOpen) {
            RECT r; 
            GetClientRect(hwnd, &r);
            int listW = 400, listH = r.bottom - 100; 
            int listY = 50, listX = (r.right - listW) / 2;
            int rightEdge = listX + listW;
            bool hoverScroll = (mx >= rightEdge - 20 && mx <= rightEdge && my >= listY && my <= listY + listH);
            
            app.scrollAnim.Update(hoverScroll || app.isScrollDragging, 0.2f);

            if (app.isScrollDragging) {
                int itemH = 25, totalH = app.count * itemH;
                if (totalH > listH) {
                    float pct = (float)(my - listY) / (float)listH;
                    if (pct < 0.0f) pct = 0.0f; 
                    if (pct > 1.0f) pct = 1.0f;
                    app.targetListScrollY = pct * (totalH - listH);
                    app.listScrollY = app.targetListScrollY;
                }
                return 0;
            }
        }

        // Minimap drag
        if (app.isMinimapDragging) {
            float mmW = (float)(app.minimapRect.right - app.minimapRect.left);
            float mmH = (float)(app.minimapRect.bottom - app.minimapRect.top);
            if (mmW > 0 && mmH > 0) {
                float ratioX = (float)(mx - app.minimapRect.left) / mmW;
                float ratioY = (float)(my - app.minimapRect.top) / mmH;
                if(ratioX < 0.0f) ratioX = 0.0f; 
                if(ratioX > 1.0f) ratioX = 1.0f;
                if(ratioY < 0.0f) ratioY = 0.0f; 
                if(ratioY > 1.0f) ratioY = 1.0f;
                float rangeX = app.maxX - app.minX; 
                float rangeY = app.maxY - app.minY;
                app.offsetX = -(app.minX + ratioX * rangeX);
                app.offsetY = -(app.minY + (1.0f - ratioY) * rangeY); 
            }
            return 0; 
        }

        // List hover
        if (app.isListOpen) {
            RECT r; 
            GetClientRect(hwnd, &r);
            int listW = 400, listX = (r.right - listW) / 2;
            int listH = r.bottom - 100, listY = 50; 
            if (mx >= listX && mx <= listX + listW && my >= listY && my <= listY + listH) {
                app.listHoverIdx = (int)((my - listY + app.listScrollY) / 25);
            } else {
                app.listHoverIdx = -1;
            }
            return 0; 
        } 

        // Canvas panning
        if (app.isDragging) {
            app.menuVisible = 0;
            
            float deltaX = (float)(mx - app.lastMouse.x) / (app.scale * 2.0f);
            float deltaY = (float)(my - app.lastMouse.y) / app.scale;
            
            app.offsetX = app.anchorX + deltaX;
            app.offsetY = app.anchorY - deltaY;

            // Constrain bounds
            float rangeX = app.maxX - app.minX; 
            float rangeY = app.maxY - app.minY;
            
            if (rangeX < 1.0f) rangeX = 1.0f;
            if (rangeY < 1.0f) rangeY = 1.0f;

            float padX = rangeX * 0.05f;
            float padY = rangeY * 0.05f; 
            
            if (-app.offsetX < app.minX - padX) app.offsetX = -(app.minX - padX);
            if (-app.offsetX > app.maxX + padX) app.offsetX = -(app.maxX + padX);
            if (-app.offsetY < app.minY - padY) app.offsetY = -(app.minY - padY);
            if (-app.offsetY > app.maxY + padY) app.offsetY = -(app.maxY + padY);
        }
        
        // Dot hover
        app.hoverIndex = -1;
        
        bool inMinimap = (app.count > 0 && mx >= app.minimapRect.left && mx <= app.minimapRect.right && 
                        my >= app.minimapRect.top && my <= app.minimapRect.bottom);
                        
        if (!inMinimap) {
            RECT r; 
            GetClientRect(hwnd, &r);
            int safeLeft = -50, safeRight = r.right + 50;
            int safeTop = -50, safeBottom = r.bottom + 50;

            for (int i = 0; i < app.count; i++) {
                int sx = app.samples[i].screenX;
                int sy = app.samples[i].screenY;

                if (sx < safeLeft || sx > safeRight || sy < safeTop || sy > safeBottom) continue;

                if(abs(mx - sx) + abs(my - sy) < 15) {
                    app.hoverIndex = i; 
                    break;
                }
            }
        }
    } return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps; 
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rect; 
        GetClientRect(hwnd, &rect);
        DrawMap(hdc, rect); 
        EndPaint(hwnd, &ps);
    } return 0;

    case WM_DESTROY:
        for(int i=0; i<app.count; i++) free(app.samples[i].visualData);
        if(app.audioMem) free(app.audioMem);
        if(g_hbmBack) DeleteObject(g_hbmBack); 
        if(g_hdcBack) DeleteDC(g_hdcBack);
        MFShutdown(); 
        CoUninitialize(); 
        PostQuitMessage(0); 
        return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    timeBeginPeriod(1);

    srand(GetTickCount()); 
    ULONG_PTR gdiplusToken; 
    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    WNDCLASSA wc = { 0 }; 
    wc.lpfnWndProc = WindowProc; 
    wc.hInstance = hInstance; 
    wc.lpszClassName = "AudioMapClass"; 
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW); 
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    RegisterClassA(&wc);

    int scrW = GetSystemMetrics(SM_CXSCREEN); 
    int scrH = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowExA(0, "AudioMapClass", "audiomap", WS_OVERLAPPEDWINDOW, 
                                (scrW - WIN_WIDTH)/2, (scrH - WIN_HEIGHT)/2, 
                                WIN_WIDTH, WIN_HEIGHT, NULL, NULL, hInstance, NULL);
    if (!hwnd) return 0;

    HICON hIcon = CreateSineIcon(hInstance);
    SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
    SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
    
    ShowWindow(hwnd, nCmdShow);

    MSG msg = { 0 };
    LARGE_INTEGER perfFreq, perfCount, lastPerfCount;
    QueryPerformanceFrequency(&perfFreq); 
    QueryPerformanceCounter(&lastPerfCount);
    double timeScale = 1000.0 / (double)perfFreq.QuadPart;

    while (msg.message != WM_QUIT) {
        if (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { 
            TranslateMessage(&msg); 
            DispatchMessageA(&msg); 
        } else {
             QueryPerformanceCounter(&perfCount);
             double dt = (perfCount.QuadPart - lastPerfCount.QuadPart) * timeScale;
             lastPerfCount = perfCount;
             
             app.fps.Update((float)dt);

             RECT r; 
             GetClientRect(hwnd, &r);

             // Update animations
             app.hoverAnim.Update(app.hoverIndex != -1, 0.08f);
             app.menuAnim.Update(app.menuVisible, 0.1f);
             app.listOpenAnim.Update(app.isListOpen, 0.1f);
             
             if(app.listClickAnim > 0.0f) { 
                 app.listClickAnim -= 0.1f; 
                 if(app.listClickAnim < 0.0f) app.listClickAnim = 0.0f; 
             }

             if (app.isListOpen) {
                 int startIdx = (int)(app.listScrollY / 25);
                 int endIdx = startIdx + (r.bottom - 100) / 25 + 2; 
                 for (int i = 0; i < app.count; i++) {
                     int sIdx = app.sortedIndices[i];
                     bool isActive = (i == app.listHoverIdx) && (app.isListOpen);
                     app.samples[sIdx].listHoverAnim.Update(isActive, 0.15f);
                 }
             }

             // Keyboard panning
             float panSpeed = 2.0f / app.scale;
             if(app.keys[0]) app.offsetX += panSpeed; 
             if(app.keys[1]) app.offsetX -= panSpeed;
             if(app.keys[2]) app.offsetY -= panSpeed; 
             if(app.keys[3]) app.offsetY += panSpeed;

             POINT pt; 
             GetCursorPos(&pt); 
             ScreenToClient(hwnd, &pt);

             bool hOpen = (pt.x >= 15 && pt.x <= 85 && pt.y >= r.bottom - 40 && pt.y <= r.bottom - 14);
             bool hList = (pt.x >= 95 && pt.x <= 165 && pt.y >= r.bottom - 40 && pt.y <= r.bottom - 14);
             bool hMini = (pt.x >= r.right - 120 && pt.y >= r.bottom - 120);
             
             app.animBtnOpen.Update(hOpen); 
             app.animBtnList.Update(hList); 
             app.animMinimap.Update(hMini);

             // Smooth scroll
             if(app.isListOpen) {
                 if (!app.isScrollDragging) {
                    float diff = app.targetListScrollY - app.listScrollY;
                    if(fabs(diff) > 0.5f) app.listScrollY += diff * 0.2f;
                    else app.listScrollY = app.targetListScrollY;
                 }
                 
                 int maxS = app.count * 25 - (r.bottom - 100); 
                 if(maxS < 0) maxS = 0;
                 if(app.targetListScrollY < 0) app.targetListScrollY = 0;
                 if(app.targetListScrollY > maxS) app.targetListScrollY = (float)maxS;
                 if (app.listScrollY < 0) app.listScrollY = 0;
                 if (app.listScrollY > maxS) app.listScrollY = (float)maxS;
             }

             // Smooth zoom
             float diff = app.targetScale - app.scale;
             if (fabs(diff) > 0.1f) {
                 int cx = r.right / 2; 
                 int cy = r.bottom / 2;
                 float mxWorld = (app.currentMouse.x - cx) / (app.scale * 2.0f) - app.offsetX;
                 float myWorld = -(app.currentMouse.y - cy) / app.scale - app.offsetY;
                 app.scale += diff * 0.1f;
                 app.offsetX = (app.currentMouse.x - cx) / (app.scale * 2.0f) - mxWorld;
                 app.offsetY = -(app.currentMouse.y - cy) / app.scale - myWorld;
             }

             HDC hdc = GetDC(hwnd); 
             DrawMap(hdc, r); 
             ReleaseDC(hwnd, hdc);
             
             if (dt < 2.0) Sleep(1);
        }
    }

    timeEndPeriod(1);
    GdiplusShutdown(gdiplusToken);
    return 0;
}