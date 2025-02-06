#define NOMINMAX

#include <windows.h>
#include <commctrl.h>
#include <iostream>
#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <regex>
#include <d3d11.h>
#include <shobjidl.h>
#include <dwmapi.h>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#pragma comment(lib, "dwmapi.lib")

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

std::wstring string_to_wstring(const std::string& str) {
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

std::string wstring_to_string(const std::wstring& wstr) {
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

class FastSearch {
private:
    std::mutex mtx;
    std::condition_variable cv;
    std::queue<std::filesystem::path> workQueue;
    std::vector<std::thread> threads;
    std::atomic<bool>& searchInProgress;
    std::atomic<int> activeThreads;
    std::atomic<size_t> filesProcessed{ 0 };
    std::atomic<size_t> matchesFound{ 0 };
    std::string searchPattern;
    bool caseSensitive;
    bool useRegex;
    bool shouldStop{ false };
    std::vector<std::wstring> results;
    std::chrono::steady_clock::time_point startTime;
    std::chrono::steady_clock::time_point lastUpdateTime;
    static constexpr auto UPDATE_INTERVAL = std::chrono::milliseconds(16); // ~60 FPS

    // KMP algorithm helper functions
    std::vector<int> computeLPSArray(const std::string& pattern) {
        int len = 0;
        std::vector<int> lps(pattern.length(), 0);
        int i = 1;
        
        while (i < pattern.length()) {
            if (pattern[i] == pattern[len]) {
                len++;
                lps[i] = len;
                i++;
            } else {
                if (len != 0) {
                    len = lps[len - 1];
                } else {
                    lps[i] = 0;
                    i++;
                }
            }
        }
        return lps;
    }

    bool kmpSearch(const std::string& text, const std::string& pattern) {
        if (pattern.empty()) return false;
        
        std::string textToSearch = text;
        std::string patternToSearch = pattern;
        
        if (!caseSensitive) {
            std::transform(textToSearch.begin(), textToSearch.end(), textToSearch.begin(), ::tolower);
            std::transform(patternToSearch.begin(), patternToSearch.end(), patternToSearch.begin(), ::tolower);
        }
        
        std::vector<int> lps = computeLPSArray(patternToSearch);
        int i = 0; // index for text
        int j = 0; // index for pattern
        
        while (i < textToSearch.length()) {
            if (patternToSearch[j] == textToSearch[i]) {
                j++;
                i++;
            }
            
            if (j == patternToSearch.length()) {
                return true;
            } else if (i < textToSearch.length() && patternToSearch[j] != textToSearch[i]) {
                if (j != 0) {
                    j = lps[j - 1];
                } else {
                    i++;
                }
            }
        }
        return false;
    }

    bool matchesPattern(const std::string& filename) {
        if (useRegex) {
            try {
                std::regex pattern(searchPattern,
                    caseSensitive ? std::regex::ECMAScript : std::regex::ECMAScript | std::regex::icase);
                return std::regex_search(filename, pattern);
            }
            catch (const std::regex_error&) {
                return false;
            }
        } else {
            return kmpSearch(filename, searchPattern);
        }
    }

    void searchWorker() {
        const auto YIELD_INTERVAL = std::chrono::milliseconds(1);
        auto lastYield = std::chrono::steady_clock::now();
        
        while (!shouldStop) {
            std::filesystem::path currentPath;
            {
                std::unique_lock<std::mutex> lock(mtx);
                if (workQueue.empty()) {
                    if (--activeThreads == 0) {
                        searchInProgress.store(false);
                    }
                    break;
                }
                currentPath = workQueue.front();
                workQueue.pop();
            }

            try {
                for (const auto& entry : std::filesystem::directory_iterator(currentPath)) {
                    if (!searchInProgress.load()) break;

                    // Periodically yield to reduce CPU usage
                    auto now = std::chrono::steady_clock::now();
                    if (now - lastYield > YIELD_INTERVAL) {
                        std::this_thread::yield();
                        lastYield = now;
                    }

                    if (entry.is_directory()) {
                        std::lock_guard<std::mutex> lock(mtx);
                        workQueue.push(entry.path());
                    }
                    else {
                        std::string filename = wstring_to_string(entry.path().filename().wstring());

                        if (matchesPattern(filename)) {
                            ++matchesFound;
                            std::lock_guard<std::mutex> lock(mtx);
                            results.push_back(entry.path().wstring());
                        }
                        ++filesProcessed;
                    }
                }
            }
            catch (const std::exception&) {
                // Skip inaccessible directories
            }
        }
    }

public:
    FastSearch(const std::string& pattern, bool caseSensitive, bool useRegex, std::atomic<bool>& searchInProgress)
        : searchPattern(pattern), caseSensitive(caseSensitive), useRegex(useRegex),
        searchInProgress(searchInProgress), activeThreads(0) {}

    ~FastSearch() {
        shouldStop = true;
        cv.notify_all();
        waitForCompletion();
    }

    void search(const std::wstring& startPath) {
        waitForCompletion();
        
        shouldStop = false;
        startTime = std::chrono::steady_clock::now();
        lastUpdateTime = startTime;
        filesProcessed = 0;
        matchesFound = 0;
        results.clear();

        unsigned int threadCount = std::thread::hardware_concurrency();
        if (threadCount == 0) threadCount = 4;

        workQueue.push(startPath);
        activeThreads.store(threadCount);

        for (unsigned int i = 0; i < threadCount; ++i) {
            threads.emplace_back(&FastSearch::searchWorker, this);
        }
    }

    void waitForCompletion() {
        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        threads.clear();
    }

    // Getters for UI
    size_t getFilesProcessed() const { return filesProcessed; }
    size_t getMatchesFound() const { return matchesFound; }
    bool isSearching() const { return searchInProgress; }
    const std::vector<std::wstring>& getResults() const { return results; }
    std::chrono::steady_clock::time_point getStartTime() const { return startTime; }
};

// Function to show folder browser dialog
bool BrowseFolder(std::string& selected_path) {
    IFileOpenDialog* pFileDialog;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL,
        IID_IFileOpenDialog, reinterpret_cast<void**>(&pFileDialog));

    if (SUCCEEDED(hr)) {
        // Set options to pick folders
        DWORD dwOptions;
        hr = pFileDialog->GetOptions(&dwOptions);
        if (SUCCEEDED(hr)) {
            hr = pFileDialog->SetOptions(dwOptions | FOS_PICKFOLDERS);
        }

        // Show the dialog
        if (SUCCEEDED(hr)) {
            hr = pFileDialog->Show(NULL);
            if (SUCCEEDED(hr)) {
                IShellItem* pItem;
                hr = pFileDialog->GetResult(&pItem);
                if (SUCCEEDED(hr)) {
                    PWSTR pszFilePath;
                    hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);
                    if (SUCCEEDED(hr)) {
                        selected_path = wstring_to_string(pszFilePath);
                        CoTaskMemFree(pszFilePath);
                    }
                    pItem->Release();
                }
            }
        }
        pFileDialog->Release();
        return SUCCEEDED(hr);
    }
    return false;
}

// Global state
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static UINT g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

// Forward declarations
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Main code
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {
    // Initialize COM for the folder browser
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    
    WNDCLASSEXW wc = {
        sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr,
        L"FastSearch", nullptr
    };
    RegisterClassExW(&wc);

    // Create application window
    HWND hwnd = CreateWindowW(wc.lpszClassName, reinterpret_cast<LPCWSTR>("Fast File Search"),
        WS_OVERLAPPEDWINDOW & ~WS_CAPTION, 100, 100, 1280, 800, nullptr, nullptr, wc.hInstance, nullptr);

    // Initialize Direct3D
    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    // Show the window
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();

    // Setup Platform/Renderer backends
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // State
    std::atomic<bool> searchInProgress{ false };
    std::unique_ptr<FastSearch> searcher;
    static char searchPattern[256] = "";
    static char folderPath[1024] = "";
    static bool caseSensitive = false;
    static bool useRegex = false;
    std::vector<std::wstring> currentResults;
    float progress = 0.0f;
    bool needsUpdate = false;

    // Main loop
    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        // Handle window resize
        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        // Start the Dear ImGui frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Create the main window
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("FastSearch", nullptr, 
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

        // Search controls
        ImGui::InputText("Search Pattern", searchPattern, IM_ARRAYSIZE(searchPattern));
        ImGui::InputText("Folder Path", folderPath, IM_ARRAYSIZE(folderPath));
        ImGui::SameLine();
        if (ImGui::Button("Browse")) {
            std::string selected_path;
            if (BrowseFolder(selected_path)) {
                strncpy_s(folderPath, selected_path.c_str(), sizeof(folderPath) - 1);
            }
        }

        ImGui::Checkbox("Case Sensitive", &caseSensitive);
        ImGui::SameLine();
        ImGui::Checkbox("Use Regex", &useRegex);

        // Search button and progress
        if (!searchInProgress) {
            if (ImGui::Button("Search")) {
                if (strlen(folderPath) > 0 && strlen(searchPattern) > 0) {
                    searcher = std::make_unique<FastSearch>(searchPattern, caseSensitive, useRegex, searchInProgress);
                    searcher->search(string_to_wstring(folderPath));
                    searchInProgress = true;
                    progress = 0.0f;
                    currentResults.clear();
                    needsUpdate = true;
                }
            }
        } else {
            if (ImGui::Button("Stop")) {
                searchInProgress = false;
            }
            
            // Show search status and timing
            if (searcher) {
                size_t filesProcessed = searcher->getFilesProcessed();
                size_t matchesFound = searcher->getMatchesFound();
                
                // Show progress bar
                ImGui::ProgressBar(progress, ImVec2(-1, 0));
                
                if (!searcher->isSearching()) {
                    progress = 1.0f;
                    searchInProgress = false;
                    // Calculate and display elapsed time with larger text
                    auto endTime = std::chrono::steady_clock::now();
                    auto elapsedSeconds = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - searcher->getStartTime()).count() / 1000.0f;
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(50, 255, 50, 255));  // Green color
                    ImGui::SetWindowFontScale(1.2f);  // Make text 20% larger
                    ImGui::Text("Search completed in %.2f seconds", elapsedSeconds);
                    ImGui::SetWindowFontScale(1.0f);  // Reset font scale
                    ImGui::PopStyleColor();
                } else {
                    progress = std::min(0.99f, progress + 0.001f);
                    needsUpdate = (ImGui::GetFrameCount() % 30) == 0; // Update every 30 frames
                }
                
                ImGui::Text("Processed: %zu files | Found: %zu matches", filesProcessed, matchesFound);
                ImGui::Separator();
            }
        }

        // Results list with proper styling
        if (ImGui::BeginChild("Results", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar)) {
            for (const auto& result : currentResults) {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200, 200, 200, 255));
                if (ImGui::Selectable(wstring_to_string(result).c_str(), false)) {
                    // Open file in explorer when clicked
                    std::wstring command = L"explorer.exe /select,\"" + result + L"\"";
                    ShellExecuteW(NULL, L"open", L"explorer.exe",
                        (L"/select,\"" + result + L"\"").c_str(),
                        NULL, SW_SHOWNORMAL);
                }
                ImGui::PopStyleColor();
            }
        }
        ImGui::EndChild();

        ImGui::End();

        // Rendering
        ImGui::Render();
        const float clear_color_with_alpha[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    // Cleanup
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    CoUninitialize(); // Clean up COM
    return 0;
}

// Helper functions
bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2,
        D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevelArray, 2,
            D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            g_ResizeWidth = LOWORD(lParam);
            g_ResizeHeight = HIWORD(lParam);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
            return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}