#include <windows.h>
#include <mmdeviceapi.h>
#include <propidl.h>
#include <functiondiscoverykeys_devpkey.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include "resource.h" // 你的魔法背包！

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib") // 操作注册表需要的库

#define WM_TRAYMSG (WM_USER + 1)
#define ID_M_SWITCH 1001
#define ID_M_EXIT   1002
#define ID_M_AUTOSTART 1003
#define ID_M_LANGUAGE  1004
#define ID_M_SET_A_BASE 2000
#define ID_M_SET_B_BASE 3000

// Windows 核心音频接口
interface DECLSPEC_UUID("f8679f50-850a-41cf-9c72-430f290290c8") IPolicyConfig;
class DECLSPEC_UUID("870af99c-171d-4f9e-af0d-e63df40c2bc9") CPolicyConfigClient;
interface IPolicyConfig : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, BOOL, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, BOOL, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, PVOID) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, PVOID) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, BOOL, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR pszDeviceName, ERole eRole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, BOOL) = 0;
};

// ============================================================================
// 全局变量
// ============================================================================
std::wstring g_DeviceA = L"";
std::wstring g_DeviceB = L"";
std::wstring g_ConfigPath = L"";
std::wstring g_ExePath = L""; // 记录程序的路径，用来设置开机自启
bool g_isEnglish = false;     // 语言开关
std::vector<std::wstring> g_DeviceList;
HWND g_hWnd = NULL;
NOTIFYICONDATA g_nid = { sizeof(g_nid) };

// ============================================================================
// 配置与注册表管理（开机自启）
// ============================================================================
void InitPaths() {
    wchar_t path[MAX_PATH];
    GetModuleFileName(NULL, path, MAX_PATH);
    g_ExePath = path;
    size_t pos = g_ExePath.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        g_ConfigPath = g_ExePath.substr(0, pos) + L"\\config.ini";
    }
}

void LoadConfig() {
    wchar_t buffer[512];
    GetPrivateProfileString(L"Settings", L"DeviceA", L"", buffer, 512, g_ConfigPath.c_str());
    g_DeviceA = buffer;
    GetPrivateProfileString(L"Settings", L"DeviceB", L"", buffer, 512, g_ConfigPath.c_str());
    g_DeviceB = buffer;
    g_isEnglish = GetPrivateProfileInt(L"Settings", L"Language", 0, g_ConfigPath.c_str()) == 1;
}

void SaveConfig() {
    WritePrivateProfileString(L"Settings", L"DeviceA", g_DeviceA.c_str(), g_ConfigPath.c_str());
    WritePrivateProfileString(L"Settings", L"DeviceB", g_DeviceB.c_str(), g_ConfigPath.c_str());
    WritePrivateProfileString(L"Settings", L"Language", g_isEnglish ? L"1" : L"0", g_ConfigPath.c_str());
}

// 检查是否已经开机自启
bool IsAutoStartEnabled() {
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD type, size;
        LSTATUS status = RegQueryValueEx(hKey, L"MyAudioSwitcher", NULL, &type, NULL, &size);
        RegCloseKey(hKey);
        return (status == ERROR_SUCCESS);
    }
    return false;
}

// 开启或关闭开机自启
void ToggleAutoStart() {
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        if (IsAutoStartEnabled()) {
            RegDeleteValue(hKey, L"MyAudioSwitcher"); // 关闭自启
        }
        else {
            std::wstring quotedPath = L"\"" + g_ExePath + L"\"";
            RegSetValueEx(hKey, L"MyAudioSwitcher", 0, REG_SZ, (BYTE*)quotedPath.c_str(), (quotedPath.length() + 1) * sizeof(wchar_t)); // 开启自启
        }
        RegCloseKey(hKey);
    }
}

// ============================================================================
// 音频设备获取与切换
// ============================================================================
std::wstring GetDeviceFriendlyName(IMMDevice* pDevice) {
    IPropertyStore* pProps = NULL;
    std::wstring name = g_isEnglish ? L"Unknown Device" : L"未知设备";
    if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &pProps))) {
        PROPVARIANT varName;
        PropVariantInit(&varName);
        if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.pwszVal != NULL) {
            name = varName.pwszVal;
        }
        PropVariantClear(&varName);
        pProps->Release();
    }
    return name;
}

// 实时获取当前正在发声的设备名字
std::wstring GetCurrentDeviceName() {
    std::wstring currentName = g_isEnglish ? L"Unknown" : L"未知";
    IMMDeviceEnumerator* pEnumerator = NULL;
    IMMDevice* pDefaultDevice = NULL;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator))) {
        if (SUCCEEDED(pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDefaultDevice))) {
            currentName = GetDeviceFriendlyName(pDefaultDevice);
            pDefaultDevice->Release();
        }
        pEnumerator->Release();
    }
    return currentName;
}

// 更新鼠标悬浮在小图标上的提示文字
void UpdateTrayTooltip() {
    std::wstring tip = (g_isEnglish ? L"Current: " : L"当前设备: ") + GetCurrentDeviceName();
    if (tip.length() >= 128) tip = tip.substr(0, 127); // 防止名字太长撑爆气泡
    wcscpy_s(g_nid.szTip, tip.c_str());
    Shell_NotifyIcon(NIM_MODIFY, &g_nid);
}

void RefreshDeviceList() {
    g_DeviceList.clear();
    IMMDeviceEnumerator* pEnumerator = NULL;
    IMMDeviceCollection* pCollection = NULL;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator))) {
        if (SUCCEEDED(pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection))) {
            UINT count = 0;
            pCollection->GetCount(&count);
            for (UINT i = 0; i < count; i++) {
                IMMDevice* pDevice = NULL;
                if (SUCCEEDED(pCollection->Item(i, &pDevice))) {
                    g_DeviceList.push_back(GetDeviceFriendlyName(pDevice));
                    pDevice->Release();
                }
            }
            pCollection->Release();
        }
        pEnumerator->Release();
    }
}

void SwitchAudioDevice() {
    if (g_DeviceA.empty() || g_DeviceB.empty()) return;

    std::wstring currentName = GetCurrentDeviceName();
    std::wstring targetName = (currentName == g_DeviceA) ? g_DeviceB : g_DeviceA;

    IMMDeviceEnumerator* pEnumerator = NULL;
    IMMDeviceCollection* pCollection = NULL;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator))) {
        if (SUCCEEDED(pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection))) {
            UINT count = 0;
            pCollection->GetCount(&count);
            for (UINT i = 0; i < count; i++) {
                IMMDevice* pDevice = NULL;
                if (SUCCEEDED(pCollection->Item(i, &pDevice))) {
                    if (GetDeviceFriendlyName(pDevice) == targetName) {
                        LPWSTR wzID = NULL;
                        if (SUCCEEDED(pDevice->GetId(&wzID))) {
                            IPolicyConfig* pPolicyConfig = NULL;
                            if (SUCCEEDED(CoCreateInstance(__uuidof(CPolicyConfigClient), NULL, CLSCTX_ALL, __uuidof(IPolicyConfig), (LPVOID*)&pPolicyConfig))) {
                                pPolicyConfig->SetDefaultEndpoint(wzID, eConsole);
                                pPolicyConfig->SetDefaultEndpoint(wzID, eMultimedia);
                                pPolicyConfig->SetDefaultEndpoint(wzID, eCommunications);
                                pPolicyConfig->Release();
                            }
                            CoTaskMemFree(wzID);
                        }
                    }
                    pDevice->Release();
                }
            }
            pCollection->Release();
        }
        pEnumerator->Release();
    }

    // 切换完给系统一点点反应时间，然后更新悬浮提示
    Sleep(100);
    UpdateTrayTooltip();
}

// ============================================================================
// 窗口与菜单消息处理
// ============================================================================
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_TRAYMSG:
        if (lParam == WM_LBUTTONDBLCLK) {
            SwitchAudioDevice();
        }
        else if (lParam == WM_RBUTTONUP) {
            RefreshDeviceList();

            HMENU hMenu = CreatePopupMenu();
            HMENU hSubMenuA = CreatePopupMenu();
            HMENU hSubMenuB = CreatePopupMenu();

            for (size_t i = 0; i < g_DeviceList.size(); i++) {
                UINT flagA = (g_DeviceA == g_DeviceList[i]) ? (MF_STRING | MF_CHECKED) : MF_STRING;
                UINT flagB = (g_DeviceB == g_DeviceList[i]) ? (MF_STRING | MF_CHECKED) : MF_STRING;
                AppendMenu(hSubMenuA, flagA, ID_M_SET_A_BASE + i, g_DeviceList[i].c_str());
                AppendMenu(hSubMenuB, flagB, ID_M_SET_B_BASE + i, g_DeviceList[i].c_str());
            }

            // 动态生成菜单文字
            std::wstring strSwitch = (g_isEnglish ? L"🎵 Current: " : L"🎵 当前: ") + GetCurrentDeviceName();
            std::wstring strSetA = g_isEnglish ? L"⚙️ Set [Device A]" : L"⚙️ 设定【设备 A】";
            std::wstring strSetB = g_isEnglish ? L"⚙️ Set [Device B]" : L"⚙️ 设定【设备 B】";
            std::wstring strAutoStart = g_isEnglish ? L"💻 Start on Boot" : L"💻 开机自启动";
            std::wstring strLang = g_isEnglish ? L"🌐 切换为中文" : L"🌐 Switch to English";
            std::wstring strExit = g_isEnglish ? L"❌ Exit" : L"❌ 退出程序";

            // 组装菜单
            AppendMenu(hMenu, MF_STRING, ID_M_SWITCH, strSwitch.c_str());
            AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenu(hMenu, MF_POPUP, (UINT_PTR)hSubMenuA, strSetA.c_str());
            AppendMenu(hMenu, MF_POPUP, (UINT_PTR)hSubMenuB, strSetB.c_str());
            AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);

            // 开机自启动如果开启了，就打个勾
            UINT autoStartFlag = IsAutoStartEnabled() ? (MF_STRING | MF_CHECKED) : MF_STRING;
            AppendMenu(hMenu, autoStartFlag, ID_M_AUTOSTART, strAutoStart.c_str());
            AppendMenu(hMenu, MF_STRING, ID_M_LANGUAGE, strLang.c_str());
            AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenu(hMenu, MF_STRING, ID_M_EXIT, strExit.c_str());

            POINT curPoint;
            GetCursorPos(&curPoint);
            SetForegroundWindow(hWnd);
            TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_RIGHTBUTTON, curPoint.x, curPoint.y, 0, hWnd, NULL);

            DestroyMenu(hSubMenuA);
            DestroyMenu(hSubMenuB);
            DestroyMenu(hMenu);
        }
        break;

    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        if (wmId == ID_M_SWITCH) {
            SwitchAudioDevice();
        }
        else if (wmId == ID_M_AUTOSTART) {
            ToggleAutoStart();
        }
        else if (wmId == ID_M_LANGUAGE) {
            g_isEnglish = !g_isEnglish; // 翻转语言开关
            SaveConfig();
            UpdateTrayTooltip(); // 语言变了，悬浮提示的语言也要跟着变
        }
        else if (wmId == ID_M_EXIT) {
            DestroyWindow(hWnd);
        }
        else if (wmId >= ID_M_SET_A_BASE && wmId < ID_M_SET_A_BASE + 100) {
            int index = wmId - ID_M_SET_A_BASE;
            if (index < g_DeviceList.size()) {
                g_DeviceA = g_DeviceList[index];
                SaveConfig();
            }
        }
        else if (wmId >= ID_M_SET_B_BASE && wmId < ID_M_SET_B_BASE + 100) {
            int index = wmId - ID_M_SET_B_BASE;
            if (index < g_DeviceList.size()) {
                g_DeviceB = g_DeviceList[index];
                SaveConfig();
            }
        }
        break;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    InitPaths();
    LoadConfig();

    const wchar_t CLASS_NAME[] = L"AudioSwitcherClass";
    WNDCLASS wc = { };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    RegisterClass(&wc);

    g_hWnd = CreateWindowEx(0, CLASS_NAME, L"Audio Switcher", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, NULL, NULL, hInstance, NULL);

    if (g_hWnd == NULL) {
        CoUninitialize();
        return 0;
    }

    // 设置托盘图标，提取你的魔法图标
    g_nid.hWnd = g_hWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYMSG;
    g_nid.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDB_PNG1)); // 使用你导入的图标！

    Shell_NotifyIcon(NIM_ADD, &g_nid);

    // 启动时立刻更新一次悬浮提示
    UpdateTrayTooltip();

    MSG msg = { };
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    Shell_NotifyIcon(NIM_DELETE, &g_nid);
    CoUninitialize();
    return 0;
}