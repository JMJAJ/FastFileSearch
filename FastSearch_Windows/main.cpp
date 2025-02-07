#define NOMINMAX

#include <windows.h>
#include <commctrl.h>
#include <iostream>
#include <string>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <atomic>
#include <regex>
#include <algorithm>
#include <filesystem>
#include <chrono>
#include <map>
#include <functional>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")

// DirectX and ImGui includes
#include <d3d11.h>
#include <shobjidl.h>
#include <dwmapi.h>

// ImGui includes - automatically selects the right path based on build system
#ifdef CMAKE_BUILD
    #include "imgui.h"
    #include "imgui_impl_win32.h"
    #include "imgui_impl_dx11.h"
#else
    #include "../external/imgui/imgui.h"
    #include "../external/imgui/backends/imgui_impl_win32.h"
    #include "../external/imgui/backends/imgui_impl_dx11.h"
#endif

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
                    } else {
                        std::string filename = wstring_to_string(entry.path().filename().wstring());
                        bool matches = false;

                        // First try to match the filename
                        if (matchesPattern(filename)) {
                            matches = true;
                        }
                        // If no match and not using regex, try to match against the full path
                        else if (!useRegex) {
                            std::string fullPath = wstring_to_string(entry.path().wstring());
                            if (matchesPattern(fullPath)) {
                                matches = true;
                            }
                        }

                        if (matches) {
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
    size_t getQueueSize() const {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mtx));
        return workQueue.size();
    }
};

// Performance monitoring class
class PerformanceMonitor {
private:
    static const size_t MAX_SAMPLES = 100;
    static const size_t SMOOTHING_WINDOW = 3;  // Number of samples to average
    std::vector<float> cpuHistory;
    std::vector<float> ramHistory;
    HANDLE processHandle;
    ULARGE_INTEGER lastCPU, lastSysCPU, lastUserCPU;
    int numProcessors;
    std::chrono::steady_clock::time_point lastUpdate;

    void InitCPUMonitor() {
        SYSTEM_INFO sysInfo;
        FILETIME ftime, fsys, fuser;

        GetSystemInfo(&sysInfo);
        numProcessors = sysInfo.dwNumberOfProcessors;

        GetSystemTimeAsFileTime(&ftime);
        memcpy(&lastCPU, &ftime, sizeof(FILETIME));

        processHandle = GetCurrentProcess();
        GetProcessTimes(processHandle, &ftime, &ftime, &fsys, &fuser);
        memcpy(&lastSysCPU, &fsys, sizeof(FILETIME));
        memcpy(&lastUserCPU, &fuser, sizeof(FILETIME));
    }

    double GetCPUUsage() {
        FILETIME ftime, fsys, fuser;
        ULARGE_INTEGER now, sys, user;
        double percent;

        GetSystemTimeAsFileTime(&ftime);
        memcpy(&now, &ftime, sizeof(FILETIME));

        GetProcessTimes(processHandle, &ftime, &ftime, &fsys, &fuser);
        memcpy(&sys, &fsys, sizeof(FILETIME));
        memcpy(&user, &fuser, sizeof(FILETIME));
        percent = (sys.QuadPart - lastSysCPU.QuadPart) +
            (user.QuadPart - lastUserCPU.QuadPart);
        percent /= (now.QuadPart - lastCPU.QuadPart);
        percent /= numProcessors;
        lastCPU = now;
        lastUserCPU = user;
        lastSysCPU = sys;

        return percent;
    }

    float SmoothValue(const std::vector<float>& history, size_t currentIndex) {
        float sum = 0.0f;
        int count = 0;
        for (size_t i = 0; i < SMOOTHING_WINDOW && i <= currentIndex; i++) {
            sum += history[currentIndex - i];
            count++;
        }
        return sum / count;
    }

public:
    PerformanceMonitor() : cpuHistory(MAX_SAMPLES, 0.0f), ramHistory(MAX_SAMPLES, 0.0f) {
        processHandle = GetCurrentProcess();
        InitCPUMonitor();
    }

    ~PerformanceMonitor() {
        CloseHandle(processHandle);
    }

    void Update() {
        // Update CPU usage with smoothing
        float rawCpuUsage = GetCPUUsage();
        cpuHistory.erase(cpuHistory.begin());
        cpuHistory.push_back(rawCpuUsage);
        
        // Update RAM usage
        PROCESS_MEMORY_COUNTERS_EX pmc;
        GetProcessMemoryInfo(processHandle, (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc));
        float ramUsage = static_cast<float>(pmc.WorkingSetSize) / (1024.0f * 1024.0f); // Convert to MB

        ramHistory.erase(ramHistory.begin());
        ramHistory.push_back(ramUsage);
    }

    void RenderGraphs() {
        const float PERF_WINDOW_WIDTH = 250.0f;
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - PERF_WINDOW_WIDTH, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(PERF_WINDOW_WIDTH, ImGui::GetIO().DisplaySize.y), ImGuiCond_Always);
        ImGui::Begin("Performance", nullptr, 
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | 
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoTitleBar);

        const float GRAPH_HEIGHT = ImGui::GetIO().DisplaySize.y * 0.4f;
        const float GRAPH_WIDTH = PERF_WINDOW_WIDTH - 20.0f;

        // Prepare smoothed CPU data for display
        std::vector<float> smoothedCPU(cpuHistory.size());
        for (size_t i = 0; i < cpuHistory.size(); i++) {
            smoothedCPU[i] = SmoothValue(cpuHistory, i);
        }

        // CPU Graph with custom styling
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.3f, 1.0f));
        ImGui::Text("CPU Usage");
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.1f, 0.1f, 0.1f, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(1.0f, 0.5f, 0.0f, 1.0f));
        ImGui::PlotLines("##cpu", smoothedCPU.data(), smoothedCPU.size(), 0,
            nullptr, 0.0f, 1.0f, ImVec2(GRAPH_WIDTH, GRAPH_HEIGHT));
        ImGui::PopStyleColor(2);
        ImGui::Text("Current: %.1f%%", smoothedCPU.back() * 100.0f);
        ImGui::PopStyleColor();
        
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // RAM Graph with custom styling
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.5f, 1.0f));
        ImGui::Text("RAM Usage (MB)");
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.1f, 0.1f, 0.1f, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0.0f, 1.0f, 0.5f, 1.0f));
        float maxRam = *std::max_element(ramHistory.begin(), ramHistory.end());
        ImGui::PlotLines("##ram", ramHistory.data(), ramHistory.size(), 0,
            nullptr, 0.0f, maxRam * 1.2f, ImVec2(GRAPH_WIDTH, GRAPH_HEIGHT));
        ImGui::PopStyleColor(2);
        ImGui::Text("Current: %.1f MB", ramHistory.back());
        ImGui::PopStyleColor();

        ImGui::End();
    }
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
static std::unique_ptr<PerformanceMonitor> g_perfMonitor;

// Forward declarations
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Helper function to split path into components
std::vector<std::wstring> splitPath(const std::wstring& path) {
    std::vector<std::wstring> components;
    std::wstring current;
    
    for (wchar_t c : path) {
        if (c == L'\\' || c == L'/') {
            if (!current.empty()) {
                components.push_back(current);
                current.clear();
            }
        } else {
            current += c;
        }
    }
    if (!current.empty()) {
        components.push_back(current);
    }
    return components;
}

// Helper function to format file size
std::wstring formatFileSize(uintmax_t bytes) {
    const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    int unitIndex = 0;
    double size = static_cast<double>(bytes);
    
    while (size >= 1024 && unitIndex < 4) {
        size /= 1024;
        unitIndex++;
    }
    
    wchar_t buffer[32];
    if (unitIndex == 0) {
        swprintf(buffer, 32, L"%d %ls", static_cast<int>(size), units[unitIndex]);
    } else {
        swprintf(buffer, 32, L"%.2f %ls", size, units[unitIndex]);
    }
    return buffer;
}

// Helper function to format last modified time
std::wstring formatLastModified(const std::filesystem::file_time_type& time) {
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        time - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    std::time_t tt = std::chrono::system_clock::to_time_t(sctp);
    std::tm tm;
    localtime_s(&tm, &tt);
    wchar_t buffer[32];
    wcsftime(buffer, 32, L"%Y-%m-%d %H:%M", &tm);
    return buffer;
}

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

    // Initialize performance monitor
    g_perfMonitor = std::make_unique<PerformanceMonitor>();

    // State
    std::atomic<bool> searchInProgress{ false };
    std::unique_ptr<FastSearch> searcher;
    static char searchPattern[256] = "";
    static char folderPath[1024] = "C:\\";
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

        // Update performance graphs
        g_perfMonitor->Update();

        // Create the main window
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x - 250, ImGui::GetIO().DisplaySize.y));
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
                size_t queueSize = searcher->getQueueSize();
                
                // Calculate more accurate progress based on files processed and remaining queue
                if (filesProcessed > 0) {
                    float estimatedProgress = static_cast<float>(filesProcessed) / (filesProcessed + queueSize);
                    progress = estimatedProgress;
                }
                
                // Show progress bar
                ImGui::ProgressBar(progress, ImVec2(-1, 0));
                
                // Update results periodically
                if (needsUpdate || !searcher->isSearching()) {
                    currentResults = searcher->getResults();
                    needsUpdate = false;
                }
                
                if (!searcher->isSearching()) {
                    progress = 1.0f;
                    searchInProgress = false;
                    // Calculate and display elapsed time with larger text
                    auto endTime = std::chrono::steady_clock::now();
                    auto elapsedSeconds = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - searcher->getStartTime()).count() / 1000.0f;
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(50, 255, 50, 255));  // Green color
                    ImGui::SetWindowFontScale(1.2f);  // Make text 20% larger
                    ImGui::Text("Search completed in %.2f seconds (%.1f files/sec)", 
                        elapsedSeconds, filesProcessed / elapsedSeconds);
                    ImGui::SetWindowFontScale(1.0f);  // Reset font scale
                    ImGui::PopStyleColor();
                } else {
                    needsUpdate = (ImGui::GetFrameCount() % 30) == 0; // Update every 30 frames
                    
                    // Calculate current speed and ETA
                    auto currentTime = std::chrono::steady_clock::now();
                    auto elapsedSeconds = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - searcher->getStartTime()).count() / 1000.0f;
                    float filesPerSecond = filesProcessed / elapsedSeconds;
                    
                    // Show current speed
                    ImGui::Text("Speed: %.1f files/sec", filesPerSecond);
                    
                    // Show total files discovered so far
                    ImGui::Text("Total files discovered: %zu", filesProcessed + queueSize);
                    
                    // Estimate remaining time based on actual queue size
                    float estimatedRemainingSeconds = queueSize / filesPerSecond;
                    ImGui::Text("ETA: %.1f seconds", estimatedRemainingSeconds);
                }
                
                ImGui::Text("Processed: %zu files | Found: %zu matches", filesProcessed, matchesFound);
                ImGui::Separator();
            }
        }

        // Results list with proper styling
        if (ImGui::BeginChild("Results", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar)) {
            static ImGuiTableFlags flags = ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuterH | ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg;
            
            // Global expansion state map
            static std::map<std::wstring, bool> treeExpansionState;

            // Create a tree structure from paths
            struct TreeNode {
                std::wstring name;
                std::wstring fullPath;
                bool isFile;
                uintmax_t fileSize;
                std::filesystem::file_time_type lastModified;
                std::map<std::wstring, std::unique_ptr<TreeNode>> children;

                // Recursively collapse/expand all children
                void setExpandState(bool expand) {
                    if (!isFile) {
                        treeExpansionState[fullPath] = expand;
                        for (auto& [name, child] : children) {
                            if (!child->isFile) {
                                child->setExpandState(expand);
                            }
                        }
                    }
                }

                // Check if this node has any subdirectories
                bool hasSubdirectories() const {
                    for (const auto& [name, child] : children) {
                        if (!child->isFile) return true;
                    }
                    return false;
                }

                // Get expansion state
                bool getExpandState() const {
                    if (isFile) return false;
                    auto it = treeExpansionState.find(fullPath);
                    return it != treeExpansionState.end() ? it->second : false;
                }
            };

            // Build tree from results
            TreeNode root;
            root.name = L"";
            root.isFile = false;

            for (const auto& result : currentResults) {
                auto components = splitPath(result);
                TreeNode* current = &root;
                std::wstring currentPath;

                for (size_t i = 0; i < components.size(); ++i) {
                    const auto& comp = components[i];
                    if (!currentPath.empty()) {
                        currentPath += L"\\";
                    }
                    currentPath += comp;

                    if (current->children.find(comp) == current->children.end()) {
                        auto newNode = std::make_unique<TreeNode>();
                        newNode->name = comp;
                        newNode->fullPath = currentPath;
                        newNode->isFile = (i == components.size() - 1);
                        
                        // Get file info if it's a file
                        if (newNode->isFile) {
                            std::error_code ec;
                            auto fileStatus = std::filesystem::status(currentPath, ec);
                            if (!ec) {
                                newNode->fileSize = std::filesystem::file_size(currentPath, ec);
                                newNode->lastModified = std::filesystem::last_write_time(currentPath, ec);
                            }
                        }
                        
                        current->children[comp] = std::move(newNode);
                    }
                    current = current->children[comp].get();
                }
            }

            if (ImGui::BeginTable("tree_table", 3, flags)) {
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_NoHide);
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, 150.0f);
                ImGui::TableHeadersRow();

                // Function to recursively render tree
                std::function<void(TreeNode&, int)> renderTree;
                renderTree = [&](TreeNode& node, int depth) {
                    for (auto& [name, child] : node.children) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        
                        ImGui::PushID(wstring_to_string(child->fullPath).c_str());
                        
                        // Indent based on depth
                        if (depth > 0) {
                            ImGui::Indent(20.0f);
                        }

                        // Set color based on whether it's a file or directory
                        ImGui::PushStyleColor(ImGuiCol_Text, 
                            child->isFile ? ImVec4(0.8f, 0.8f, 0.8f, 1.0f) : ImVec4(0.4f, 0.8f, 1.0f, 1.0f));

                        bool isOpen = false;
                        if (child->isFile) {
                            // Files are selectable but not expandable
                            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.3f, 0.3f, 0.3f, 0.5f));
                            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.4f, 0.4f, 0.4f, 0.5f));
                            
                            if (ImGui::Selectable(wstring_to_string(child->name).c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
                                if (ImGui::IsMouseDoubleClicked(0)) {
                                    // Open file on double click
                                    ShellExecuteW(NULL, L"open", child->fullPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
                                } else {
                                    // Select file in explorer on single click
                                    ShellExecuteW(NULL, L"open", L"explorer.exe",
                                        (L"/select,\"" + child->fullPath + L"\"").c_str(),
                                        NULL, SW_SHOWNORMAL);
                                }
                            }
                            
                            ImGui::PopStyleColor(2);
                            
                            // Context menu for files
                            if (ImGui::BeginPopupContextItem()) {
                                if (ImGui::MenuItem("Open")) {
                                    ShellExecuteW(NULL, L"open", child->fullPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
                                }
                                if (ImGui::MenuItem("Open Containing Folder")) {
                                    ShellExecuteW(NULL, L"open", L"explorer.exe",
                                        (L"/select,\"" + child->fullPath + L"\"").c_str(),
                                        NULL, SW_SHOWNORMAL);
                                }
                                if (ImGui::MenuItem("Copy Path")) {
                                    ImGui::SetClipboardText(wstring_to_string(child->fullPath).c_str());
                                }
                                ImGui::EndPopup();
                            }
                        } else {
                            // Directories are tree nodes
                            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | 
                                                     ImGuiTreeNodeFlags_OpenOnDoubleClick | 
                                                     ImGuiTreeNodeFlags_SpanFullWidth;
                            
                            bool wasExpanded = child->getExpandState();
                            if (wasExpanded) {
                                flags |= ImGuiTreeNodeFlags_DefaultOpen;
                            }
                            
                            isOpen = ImGui::TreeNodeEx(wstring_to_string(child->name).c_str(), flags);
                            
                            // Update expansion state only if it changed
                            if (isOpen != wasExpanded) {
                                child->setExpandState(isOpen);
                            }
                            
                            // Context menu for directories
                            if (ImGui::BeginPopupContextItem()) {
                                if (ImGui::MenuItem("Open in Explorer")) {
                                    ShellExecuteW(NULL, L"open", child->fullPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
                                }
                                if (ImGui::MenuItem("Copy Path")) {
                                    ImGui::SetClipboardText(wstring_to_string(child->fullPath).c_str());
                                }
                                
                                // Only show expand/collapse options if there are subdirectories
                                if (child->hasSubdirectories()) {
                                    ImGui::Separator();
                                    if (ImGui::MenuItem("Expand All")) {
                                        child->setExpandState(true);
                                        // Force the current node to expand
                                        ImGui::SetNextItemOpen(true);
                                    }
                                    if (ImGui::MenuItem("Collapse All")) {
                                        child->setExpandState(false);
                                        // Force the current node to collapse
                                        ImGui::SetNextItemOpen(false);
                                    }
                                }
                                ImGui::EndPopup();
                            }

                        }
                        ImGui::PopStyleColor();

                        // Show file size in second column
                        ImGui::TableNextColumn();
                        if (child->isFile) {
                            ImGui::TextUnformatted(wstring_to_string(formatFileSize(child->fileSize)).c_str());
                        }

                        // Show last modified date in third column
                        ImGui::TableNextColumn();
                        if (child->isFile) {
                            ImGui::TextUnformatted(wstring_to_string(formatLastModified(child->lastModified)).c_str());
                        }

                        if (isOpen) {
                            renderTree(*child, depth + 1);
                            ImGui::TreePop();
                        }

                        if (depth > 0) {
                            ImGui::Unindent(20.0f);
                        }
                        
                        ImGui::PopID();
                    }
                };

                // Render the tree
                renderTree(root, 0);
                ImGui::EndTable();
            }

            ImGui::EndChild();
        }

        ImGui::End();

        // Render performance graphs
        g_perfMonitor->RenderGraphs();

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