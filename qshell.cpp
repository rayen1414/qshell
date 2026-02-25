// ============================================================================
//  Q-Shell  —  qshell.cpp  v3.0  (Direct2D / DirectWrite backend)
//
//  RAYLIB IS FULLY REMOVED. Replaced with:
//    Window / loop : Win32 WinMain + WndProc + PeekMessage loop
//    Rendering     : D2DRenderer (d2d_renderer.hpp / .cpp)
//    Text          : DirectWrite (Segoe UI, ClearType subpixel AA)
//    Images        : WIC via D2DRenderer
//    Audio         : miniaudio (header-only — drop miniaudio.h in project root
//                    and compile one .cpp with #define MINIAUDIO_IMPLEMENTATION)
//    Input (keys)  : GetAsyncKeyState  (key state polled every frame)
//    Input (pad)   : XInput (same runtime as before)
//    Input (hook)  : SetWindowsHookEx  (same keyboard hook thread as before)
//
//  Every struct, every business-logic function, every UI overlay is preserved
//  exactly — only the rendering calls are translated.
//
//  Colour type: D2D1_COLOR_F (float r,g,b,a) everywhere.
//               Use C(r,g,b) / C(r,g,b,a) helper macros.
//               Use CA(col,alpha) to fade.
//
//  Build additions:
//    Link:  d2d1.lib  dwrite.lib  windowscodecs.lib  (from d2d_renderer.cpp)
//    Link:  dwmapi.lib  winmm.lib  wininet.lib  urlmon.lib
//           comdlg32.lib  shell32.lib  user32.lib  gdi32.lib
//    Files: d2d_renderer.hpp / d2d_renderer.cpp  (new)
//           miniaudio.h                           (header-only, add one impl .cpp)
//           qshell_plugin_api.h                   (updated — D2DPluginAPI)
//           plugin_manager.hpp / .cpp             (updated)
//           host_api.hpp                          (updated)
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <wininet.h>
#include <commdlg.h>
#include <urlmon.h>
#include <mmsystem.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

#include "d2d_renderer.hpp"   // must precede plugin API headers

#include <vector>
#include <string>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <ctime>
#include <algorithm>
#include <cmath>
#include <thread>
#include <mutex>
#include <atomic>
#include <cassert>

#include "game_finder.hpp"
#include "system_control.hpp"
#include "steam_integration.hpp"
#include "desktop_apps.hpp"

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────────────────────
// Colour helpers (shorthand replacing raylib Color literals)
// ─────────────────────────────────────────────────────────────────────────────

static inline D2D1_COLOR_F C(int r, int g, int b, int a = 255) {
    return D2D1::ColorF(r/255.f, g/255.f, b/255.f, a/255.f);
}
static inline D2D1_COLOR_F CA(D2D1_COLOR_F c, float a) {
    return D2D1::ColorF(c.r, c.g, c.b, c.a * a);
}
static inline D2D1_COLOR_F LerpColor(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return D2D1::ColorF(a.r+(b.r-a.r)*t, a.g+(b.g-a.g)*t,
                        a.b+(b.b-a.b)*t, a.a+(b.a-a.a)*t);
}
// Named colours
static const D2D1_COLOR_F BLACK_COL  = C(0,0,0);
static const D2D1_COLOR_F WHITE_COL  = C(255,255,255);
static const D2D1_COLOR_F GRAY_COL   = C(128,128,128);
static const D2D1_COLOR_F ORANGE_COL = C(255,165,0);
static const D2D1_COLOR_F PURPLE_COL = C(128,0,128);
static const D2D1_COLOR_F GREEN_COL  = C(50,205,50);
static const D2D1_COLOR_F RED_COL    = C(220,53,69);
static const D2D1_COLOR_F BLUE_COL   = C(30,144,255);
static const D2D1_COLOR_F YELLOW_COL = C(255,215,0);

// ─────────────────────────────────────────────────────────────────────────────
// Math helpers
// ─────────────────────────────────────────────────────────────────────────────

static inline float Clamp(float v,float lo,float hi){return v<lo?lo:v>hi?hi:v;}
static inline int   Clamp(int   v,int   lo,int   hi){return v<lo?lo:v>hi?hi:v;}
static inline float LerpF(float a,float b,float t){ return a+(b-a)*t; }

#ifndef PI
#define PI 3.14159265358979323846f
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Timer (replaces GetTime / GetFrameTime)
// ─────────────────────────────────────────────────────────────────────────────

static LARGE_INTEGER g_qpcFreq={}, g_qpcStart={};
static float g_time=0.f, g_dt=0.f;

static void InitTimer(){
    QueryPerformanceFrequency(&g_qpcFreq);
    QueryPerformanceCounter(&g_qpcStart);
}
static void TickTimer(){
    static LARGE_INTEGER prev=g_qpcStart;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    g_time=(float)((now.QuadPart-g_qpcStart.QuadPart)/(double)g_qpcFreq.QuadPart);
    g_dt  =(float)((now.QuadPart-prev.QuadPart)      /(double)g_qpcFreq.QuadPart);
    g_dt  =Clamp(g_dt,0.0001f,0.1f);
    prev  =now;
}
static float GetTime()      { return g_time; }
static float GetFrameTime() { return g_dt;   }

// ─────────────────────────────────────────────────────────────────────────────
// Keyboard state (replaces IsKeyPressed / IsKeyDown / IsKeyReleased)
// ─────────────────────────────────────────────────────────────────────────────

struct KeyState { bool down=false, prev=false; };
static KeyState g_keys[256]={};
static std::vector<int> g_charQueue;

static void UpdateKeyStates(){
    for(int i=0;i<256;i++){
        g_keys[i].prev=g_keys[i].down;
        g_keys[i].down=(GetAsyncKeyState(i)&0x8000)!=0;
    }
}
static bool IsKeyPressed (int vk){ return  g_keys[vk&0xFF].down && !g_keys[vk&0xFF].prev; }
static bool IsKeyReleased(int vk){ return !g_keys[vk&0xFF].down &&  g_keys[vk&0xFF].prev; }
static bool IsKeyDown    (int vk){ return  g_keys[vk&0xFF].down; }
static void PushChar(int c){ g_charQueue.push_back(c); }
static int  GetCharPressed(){
    if(g_charQueue.empty()) return 0;
    int c=g_charQueue.front(); g_charQueue.erase(g_charQueue.begin()); return c;
}

// ─────────────────────────────────────────────────────────────────────────────
// XInput (unchanged from original)
// ─────────────────────────────────────────────────────────────────────────────

namespace XInput {
    static const WORD BTN_BACK =0x0020, BTN_START=0x0010, BTN_X=0x4000;
    struct GamepadState{ WORD buttons; BYTE lt,rt; SHORT lx,ly,rx,ry; };
    struct State{ DWORD packet; GamepadState gamepad; };
    using GetStateFn=DWORD(WINAPI*)(DWORD,void*);
    static GetStateFn s_getState=nullptr;
    static HMODULE s_lib=nullptr;
    static bool s_loaded=false;
    void Load(){
        if(s_loaded)return; s_loaded=true;
        const char* dlls[]={"xinput1_4.dll","xinput1_3.dll","xinput9_1_0.dll"};
        for(auto d:dlls){
            s_lib=LoadLibraryA(d);
            if(s_lib){ s_getState=reinterpret_cast<GetStateFn>(GetProcAddress(s_lib,"XInputGetState")); if(s_getState)return; FreeLibrary(s_lib);s_lib=nullptr; }
        }
    }
    DWORD GetState(DWORD i,State* s){ return s_getState?s_getState(i,s):ERROR_DEVICE_NOT_CONNECTED; }
    void  Unload(){ if(s_lib){FreeLibrary(s_lib);s_lib=nullptr;s_getState=nullptr;} }
}

// Gamepad axis indices (mimic raylib constants used by InputAdapter)
enum { GAMEPAD_AXIS_LEFT_X=0, GAMEPAD_AXIS_LEFT_Y=1 };

// ============================================================================
// ENUMS / CONSTANTS
// ============================================================================

enum class UIMode { MAIN, TASK_SWITCHER, SHELL_MENU, POWER_MENU, PROFILE_EDIT, THEME_SELECT, ACCOUNTS_VIEW, ADD_APP };
enum class StartupChoice { NONE, NORMAL_APP, SHELL_MODE, EXIT_SHELL };
enum class ShellAction   { NONE, EXPLORER, KEYBOARD, SETTINGS, TASKMGR, RESTART_SHELL, EXIT_SHELL, POWER };
enum class PowerChoice   { NONE, RESTART, SHUTDOWN, SLEEP, CANCEL };

static constexpr DWORD DEBOUNCE_MS     = 400;
static constexpr float HOLD_THRESHOLD  = 1.5f;
static constexpr int   MENU_COUNT      = 4;
static constexpr float STICK_DEADZONE  = 0.5f;
static constexpr float STICK_DELAY     = 0.18f;

// ============================================================================
// THEME
// ============================================================================

struct Theme {
    std::string  name;
    D2D1_COLOR_F primary, secondary, accent, accentAlt;
    D2D1_COLOR_F text, textDim, cardBg;
    D2D1_COLOR_F success, warning, danger;

    void LerpTo(const Theme& tgt, float t){
        primary  =LerpColor(primary,  tgt.primary,  t);
        secondary=LerpColor(secondary,tgt.secondary,t);
        accent   =LerpColor(accent,   tgt.accent,   t);
        accentAlt=LerpColor(accentAlt,tgt.accentAlt,t);
        text     =LerpColor(text,     tgt.text,     t);
        textDim  =LerpColor(textDim,  tgt.textDim,  t);
        cardBg   =LerpColor(cardBg,   tgt.cardBg,   t);
        success  =LerpColor(success,  tgt.success,  t);
        warning  =LerpColor(warning,  tgt.warning,  t);
        danger   =LerpColor(danger,   tgt.danger,   t);
        name     =tgt.name;
    }
};

static const std::vector<Theme> ALL_THEMES = {
    {"Default Blue",     C(12,12,15),    C(20,22,28),   C(100,149,237), C(65,105,225),  C(255,255,255),C(150,150,150),C(35,35,40),  C(50,205,50),C(255,193,7),C(220,53,69)},
    {"Xbox Green",       C(16,16,16),    C(24,24,24),   C(16,124,16),   C(50,168,82),   C(255,255,255),C(140,140,140),C(32,32,32),  C(16,124,16),C(255,193,7),C(220,53,69)},
    {"PlayStation Blue", C(0,18,51),     C(0,30,80),    C(0,112,224),   C(0,68,165),    C(255,255,255),C(130,150,180),C(0,40,100),  C(50,205,50),C(255,193,7),C(220,53,69)},
    {"Steam Dark",       C(23,29,37),    C(27,40,56),   C(102,192,244), C(171,216,237), C(255,255,255),C(142,152,165),C(42,54,69),  C(90,200,90),C(255,193,7),C(220,53,69)},
    {"Nintendo Red",     C(28,28,28),    C(40,40,40),   C(230,0,18),    C(255,70,80),   C(255,255,255),C(150,150,150),C(50,50,50),  C(50,205,50),C(255,193,7),C(230,0,18)},
    {"Purple Haze",      C(18,10,28),    C(30,18,45),   C(138,43,226),  C(186,85,211),  C(255,255,255),C(160,140,180),C(45,30,60),  C(50,205,50),C(255,193,7),C(220,53,69)},
    {"Cyberpunk",        C(10,5,15),     C(20,10,30),   C(255,0,128),   C(0,255,255),   C(255,255,255),C(180,150,200),C(30,15,45),  C(0,255,128),C(255,255,0),C(255,0,64)},
    {"Ocean",            C(10,25,35),    C(15,40,55),   C(0,188,212),   C(64,224,208),  C(255,255,255),C(140,170,180),C(20,50,70),  C(50,205,50),C(255,193,7),C(220,53,69)},
    {"Sunset",           C(30,15,15),    C(45,25,20),   C(255,87,51),   C(255,165,0),   C(255,255,255),C(180,150,140),C(55,35,30),  C(50,205,50),C(255,220,100),C(200,40,40)},
    {"OLED Black",       C(0,0,0),       C(15,15,15),   C(255,255,255), C(200,200,200), C(255,255,255),C(100,100,100),C(20,20,20),  C(50,205,50),C(255,193,7),C(220,53,69)},
};

// ============================================================================
// CUSTOM APP
// ============================================================================

struct CustomApp {
    std::string  name, path, iconPath;
    D2D1_COLOR_F accentColor = C(100,149,237);
    D2DBitmap    icon = {};
    bool hasIcon=false, isWebApp=false;
};

// ============================================================================
// DATA STRUCTS
// ============================================================================

struct UIGame {
    GameInfo  info;
    D2DBitmap poster={};
    bool hasPoster=false;
    float detailAlpha=0, selectAnim=0;
};

struct UserProfile {
    std::string  username="Player", avatarPath;
    D2DBitmap    avatar={};
    bool hasAvatar=false;
    int  themeIndex=0;
    float masterVolume=0.8f, musicVolume=0.3f, sfxVolume=0.7f;
    bool soundEnabled=true, musicEnabled=true;
};

struct RunningTask {
    std::string name, windowTitle;
    HWND hwnd=nullptr; DWORD processId=0;
    bool isQShell=false; HICON hIcon=nullptr;
};

struct Notification {
    std::string title, message;
    D2D1_COLOR_F color;
    float lifetime, elapsed, slideIn;
    int icon;
};

struct PlatformConnection {
    std::string name, icon;
    D2D1_COLOR_F accentColor;
    bool isConnected;
    std::string statusText, connectUrl;
};

struct HubSlider {
    D2DBitmap artCovers[3]={};
    int currentSlide=0;
    float slideTimer=0, transitionProgress=0;
    bool hasTextures=false;
};

// ============================================================================
// APPLICATION STATE
// ============================================================================

struct AppState {
    HWND mainWindow=nullptr;
    bool isShellMode=false, shouldRestart=false, windowOnTop=true;
    std::string exeDir;
    UIMode currentMode=UIMode::MAIN;
    std::mutex modeMutex;

    int   currentThemeIdx=0;
    Theme theme, targetTheme;
    UserProfile profile;
    std::string bgPath;
    D2DBitmap   bgTexture={};

    std::vector<UIGame>             library;
    std::vector<RunningTask>        tasks;
    int   taskFocusIdx=0; float taskSlideIn=0, taskAnimTime=0;
    std::vector<Notification>       notifications;
    std::mutex notifMutex;

    std::atomic<bool> taskSwitchRequested{false}, running{true};
    std::thread inputThread;
    HHOOK kbHook=nullptr; DWORD lastTaskSwitchTime=0;

    // Navigation
    int focused=0, barFocused=0;
    bool inTopBar=false, showDetails=false, showDeleteWarning=false, isFullUninstall=false;
    float scrollY=0, transAlpha=0, holdTimer=0;

    // Media tab
    std::vector<CustomApp> customApps;
    int mediaFocusIdx=0; float mediaScrollY=0;
    char addAppNameBuffer[64]={}, addAppPathBuffer[256]={};
    int addAppFocus=0; bool isAddingWebApp=false;

    // Settings
    int settingsFocusX=0, settingsFocusY=0;

    // Share tab
    int shareFocusIdx=0, shareSection=0;
    std::vector<PlatformConnection> platformConnections;
    bool isRecording=false; float recordingTime=0;
    HubSlider hubSlider;

    // Overlays
    int shellMenuFocus=0; float shellMenuSlide=0;
    int powerMenuFocus=0; float powerMenuSlide=0;
    int profileEditFocus=0; float profileEditSlide=0;
    char usernameBuffer[64]={};
    bool editingUsername=false;
    int themeSelectFocus=0; float themeSelectSlide=0, accountsSlideIn=0;

    // Steam
    D2DBitmap steamAvatarTex={};
    bool steamAvatarLoaded=false, steamAvatarAttempted=false;
    std::string steamAvatarPath;
    SteamProfile steamProfile;
    std::vector<SteamFriend> steamFriends;

    void SetTheme(int i){
        if(i>=0&&i<(int)ALL_THEMES.size()){currentThemeIdx=i;targetTheme=ALL_THEMES[i];}
    }
    void UpdateThemeTransition(float s=0.08f){ theme.LerpTo(targetTheme,s); }
    void ResetTabFocus(){
        focused=mediaFocusIdx=shareFocusIdx=shareSection=0;
        settingsFocusX=settingsFocusY=0; inTopBar=showDetails=false;
        mediaScrollY=0; transAlpha=0.2f;
    }
};

static AppState g_app;

// Plugin system — must come after AppState + g_app
#include "qshell_plugin_api.h"
#include "plugin_manager.hpp"
#include "host_api.hpp"

// ============================================================================
// PATH / LOGGING
// ============================================================================

void SetWorkingDirectoryToExe(){
    char buf[MAX_PATH]; GetModuleFileNameA(nullptr,buf,MAX_PATH);
    std::string p(buf); auto pos=p.find_last_of("\\/");
    if(pos!=std::string::npos){ g_app.exeDir=p.substr(0,pos); SetCurrentDirectoryA(g_app.exeDir.c_str()); }
    SetEnvironmentVariableA("QSHELL_DIR",g_app.exeDir.c_str());
}
std::string GetFullPath(const std::string& r){
    if(r.empty())return ""; if(r.length()>2&&r[1]==':')return r;
    return g_app.exeDir+"\\"+r;
}
void DebugLog(const std::string& msg){
    static bool first=true; static std::mutex m;
    std::lock_guard<std::mutex> l(m);
    std::string p=g_app.exeDir.empty()?"qshell.log":(g_app.exeDir+"\\qshell.log");
    std::ofstream o(p,first?std::ios::trunc:std::ios::app); first=false;
    if(o){ time_t n=time(nullptr);char t[64];strftime(t,sizeof(t),"%Y-%m-%d %H:%M:%S",localtime(&n));o<<"["<<t<<"] "<<msg<<"\n"; }
}

// ============================================================================
// AUDIO SYSTEM  (miniaudio — replaces raylib audio entirely)
// ============================================================================
// Place miniaudio.h in your project root. In ONE .cpp file add:
//   #define MINIAUDIO_IMPLEMENTATION
//   #include "miniaudio.h"

#include "miniaudio.h"

struct AudioSystem {
    ma_engine engine={};
    ma_sound  sndMove={},sndConfirm={},sndBack={},sndStartup={},sndError={},sndNotify={},bgMusic={};
    bool musicEnabled=true,soundEnabled=true,initialized=false,deviceReady=false;
    float masterVolume=0.8f,musicVolume=0.3f,sfxVolume=0.7f;

    bool Init(){
        if(initialized)return deviceReady; initialized=true;
        if(ma_engine_init(nullptr,&engine)!=MA_SUCCESS){ soundEnabled=musicEnabled=false;return false; }
        deviceReady=true;
        auto tryLoad=[&](ma_sound& s,const char* base){
            const char* exts[]={".wav",".ogg",".mp3"};
            for(auto e:exts){ std::string p=GetFullPath(std::string(base)+e);
                if(fs::exists(p)&&ma_sound_init_from_file(&engine,p.c_str(),0,nullptr,nullptr,&s)==MA_SUCCESS)return; }
        };
        try{fs::create_directories(GetFullPath("profile\\sounds"));}catch(...){}
        tryLoad(sndMove,"profile\\sounds\\move"); tryLoad(sndConfirm,"profile\\sounds\\confirm");
        tryLoad(sndBack,"profile\\sounds\\back"); tryLoad(sndStartup,"profile\\sounds\\startup");
        tryLoad(sndError,"profile\\sounds\\error"); tryLoad(sndNotify,"profile\\sounds\\notify");
        const char* mf[]={"profile\\sounds\\ambient.ogg","profile\\sounds\\ambient.mp3","profile\\sounds\\music.ogg","profile\\sounds\\music.mp3"};
        for(auto f:mf){ std::string p=GetFullPath(f);
            if(fs::exists(p)&&ma_sound_init_from_file(&engine,p.c_str(),MA_SOUND_FLAG_STREAM,nullptr,nullptr,&bgMusic)==MA_SUCCESS){
                ma_sound_set_looping(&bgMusic,MA_TRUE); break; } }
        return true;
    }
    void PlayUI(ma_sound& s){
        if(!deviceReady||!soundEnabled)return;
        ma_sound_set_volume(&s,sfxVolume*masterVolume);
        ma_sound_seek_to_pcm_frame(&s,0); ma_sound_start(&s);
    }
    void PlayMove()   {PlayUI(sndMove);}    void PlayConfirm(){PlayUI(sndConfirm);}
    void PlayBack()   {PlayUI(sndBack);}    void PlayError()  {PlayUI(sndError);}
    void PlayNotify() {PlayUI(sndNotify);} void PlayStartup(){if(deviceReady)PlayUI(sndStartup);}
    void StopMusic()  {ma_sound_stop(&bgMusic);}
    void UpdateMusic(){
        if(!deviceReady||!musicEnabled)return;
        bool front=(g_app.mainWindow==nullptr||GetForegroundWindow()==g_app.mainWindow);
        if(!front){StopMusic();return;}
        if(!ma_sound_is_playing(&bgMusic)){ma_sound_set_volume(&bgMusic,musicVolume*masterVolume);ma_sound_start(&bgMusic);}
        else ma_sound_set_volume(&bgMusic,musicVolume*masterVolume);
    }
    void Cleanup(){
        if(!initialized)return; StopMusic();
        ma_sound_uninit(&sndMove);ma_sound_uninit(&sndConfirm);ma_sound_uninit(&sndBack);
        ma_sound_uninit(&sndStartup);ma_sound_uninit(&sndError);ma_sound_uninit(&sndNotify);
        ma_sound_uninit(&bgMusic); ma_engine_uninit(&engine);
        initialized=deviceReady=false;
    }
};

static AudioSystem g_audio;
void PlayMoveSound()   {g_audio.PlayMove();}   void PlayConfirmSound(){g_audio.PlayConfirm();}
void PlayBackSound()   {g_audio.PlayBack();}   void PlayErrorSound()  {g_audio.PlayError();}
void PlayNotifySound() {g_audio.PlayNotify();}

// ============================================================================
// NOTIFICATIONS
// ============================================================================

void ShowNotification(const std::string& ttl,const std::string& msg,D2D1_COLOR_F col,float dur=4.f){
    std::lock_guard<std::mutex> l(g_app.notifMutex);
    g_app.notifications.push_back({ttl,msg,col,dur,0,0,0}); g_audio.PlayNotify();
}
void ShowNotification(const std::string& ttl,const std::string& msg,int icon=0,float dur=4.f){
    static const D2D1_COLOR_F C5[]={C(135,206,235),C(50,205,50),C(255,255,0),C(220,53,69),C(255,215,0)};
    ShowNotification(ttl,msg,C5[icon%5],dur);
}
// Bridge overload: D2DColor (plugin API type) → D2D1_COLOR_F (internal type)
void ShowNotification(const std::string& ttl,const std::string& msg,D2DColor col,float dur=4.f){
    ShowNotification(ttl,msg,D2D1_COLOR_F{col.r,col.g,col.b,col.a},dur);
}

void UpdateAndDrawNotifications(int sw,float dt){
    std::lock_guard<std::mutex> l(g_app.notifMutex);
    static const char* IC[]={"i","+","!","X","*"};
    for(int i=(int)g_app.notifications.size()-1;i>=0;i--){
        auto& n=g_app.notifications[i]; n.elapsed+=dt;
        if(n.elapsed<0.3f) n.slideIn=LerpF(n.slideIn,1.f,0.12f);
        else if(n.elapsed>n.lifetime-0.5f) n.slideIn=LerpF(n.slideIn,0.f,0.12f);
        if(n.elapsed>=n.lifetime){g_app.notifications.erase(g_app.notifications.begin()+i);continue;}
        float x=sw-380*n.slideIn-10, y=130.f+i*83;
        auto& d=D2D();
        d.FillRoundRect(x,y,370,75,9,9,CA(g_app.theme.secondary,0.95f));
        d.FillRect(x,y,4,75,n.color);
        float cx=x+35,cy=y+37; d.FillCircle(cx,cy,18,CA(n.color,0.2f));
        float iw=d.MeasureTextA(IC[n.icon%5],18);
        d.DrawTextA(IC[n.icon%5],cx-iw/2,cy-9,18,n.color);
        d.DrawTextA(n.title.c_str(),x+65,y+15,16,g_app.theme.text);
        d.DrawTextA(n.message.c_str(),x+65,y+38,13,CA(g_app.theme.text,0.6f));
        float p=1-(n.elapsed/n.lifetime); d.FillRect(x+65,y+65,290*p,2,CA(n.color,0.5f));
    }
}

// ============================================================================
// INPUT ADAPTER
// ============================================================================

class InputAdapter {
    int   gp_=-1; float st_=0;
    float axes_[4]={};
    WORD  padButtons_=0, padPrev_=0;

    bool Stick(int a,float th){
        if(gp_<0||st_>0)return false;
        if((th>0&&axes_[a]>th)||(th<0&&axes_[a]<th)){st_=STICK_DELAY;return true;}
        return false;
    }
    bool GPBtn(WORD mask){ return (padButtons_&mask)&&!(padPrev_&mask); }
    bool GPDown(WORD mask){ return (padButtons_&mask)!=0; }

public:
    void Update(){
        if(st_>0)st_-=g_dt; gp_=-1; padPrev_=padButtons_; padButtons_=0;
        if(XInput::s_getState){
            for(DWORD i=0;i<4;i++){
                XInput::State st; ZeroMemory(&st,sizeof(st));
                if(XInput::GetState(i,&st)!=ERROR_SUCCESS) continue;
                gp_=(int)i; padButtons_=st.gamepad.buttons;
                axes_[GAMEPAD_AXIS_LEFT_X] =st.gamepad.lx/32767.f;
                axes_[GAMEPAD_AXIS_LEFT_Y] =st.gamepad.ly/32767.f;
                break;
            }
        }
    }

    bool IsMoveDown() { return IsKeyPressed(VK_DOWN)||IsKeyPressed('S')||Stick(GAMEPAD_AXIS_LEFT_Y,STICK_DEADZONE) ||GPBtn(0x0002); }
    bool IsMoveUp()   { return IsKeyPressed(VK_UP)  ||IsKeyPressed('W')||Stick(GAMEPAD_AXIS_LEFT_Y,-STICK_DEADZONE)||GPBtn(0x0001); }
    bool IsMoveLeft() { return IsKeyPressed(VK_LEFT)||IsKeyPressed('A')||Stick(GAMEPAD_AXIS_LEFT_X,-STICK_DEADZONE)||GPBtn(0x0004); }
    bool IsMoveRight(){ return IsKeyPressed(VK_RIGHT)||IsKeyPressed('D')||Stick(GAMEPAD_AXIS_LEFT_X,STICK_DEADZONE)||GPBtn(0x0008); }
    bool IsConfirm()  { return IsKeyPressed(VK_RETURN)||IsKeyPressed(VK_SPACE)||GPBtn(0x1000); }
    bool IsBack()     { return IsKeyPressed(VK_BACK)||IsKeyPressed(VK_ESCAPE)||GPBtn(0x2000); }
    bool IsChangeArt(){ return IsKeyPressed('Y')||GPBtn(0x8000); }
    bool IsDeleteDown()    { return IsKeyDown('X')||IsKeyDown('H')||GPDown(0x4000); }
    bool IsDeleteReleased(){ return IsKeyReleased('X')||IsKeyReleased('H'); }
    bool IsDeletePressed() { return IsKeyPressed('X')||GPBtn(0x4000); }
    bool IsLB() { return IsKeyPressed('Q')||IsKeyPressed(VK_PRIOR)||GPBtn(0x0100); }
    bool IsRB() { return IsKeyPressed('E')||IsKeyPressed(VK_NEXT) ||GPBtn(0x0200); }
    bool IsMenu(){ return IsKeyPressed(VK_TAB)||IsKeyPressed(VK_F1)||GPBtn(0x0010); }
    bool IsView(){ return IsKeyPressed(VK_F2)||GPBtn(0x0020); }
    bool IsBG()  { return IsKeyPressed('B'); }
    int  GetGamepadID(){ return gp_; }
};

// ─── Skin picker overlay ─────────────────────────────────────────────────────

static void DrawSkinPickerOverlay(int sw,int sh,InputAdapter& input){
    PM().UpdateAndDrawSkinPicker(sw,sh,input.IsConfirm(),input.IsBack(),input.IsMoveUp(),input.IsMoveDown());
}

// ============================================================================
// D2D PLUGIN API TABLE  (g_d2dAPI)
// ============================================================================

static float sinf_wrap(float x){ return sinf(x); }

static const D2DPluginAPI g_d2dAPI = {
    [](float x,float y,float w,float h,D2DColor c){D2D().FillRect(x,y,w,h,{c.r,c.g,c.b,c.a});},
    [](float x,float y,float w,float h,float rx,float ry,D2DColor c){D2D().FillRoundRect(x,y,w,h,rx,ry,{c.r,c.g,c.b,c.a});},
    [](float x,float y,float w,float h,float rx,float ry,float sw2,D2DColor c){D2D().StrokeRoundRect(x,y,w,h,rx,ry,sw2,{c.r,c.g,c.b,c.a});},
    [](float x,float y,float w,float h,D2DColor t2,D2DColor b2){D2D().FillGradientV(x,y,w,h,{t2.r,t2.g,t2.b,t2.a},{b2.r,b2.g,b2.b,b2.a});},
    [](float x,float y,float w,float h,D2DColor l,D2DColor r){D2D().FillGradientH(x,y,w,h,{l.r,l.g,l.b,l.a},{r.r,r.g,r.b,r.a});},
    [](float x,float y,float w,float h,float sg,D2DColor c){D2D().FillBlurRect(x,y,w,h,sg,{c.r,c.g,c.b,c.a});},
    [](float cx,float cy,float r,D2DColor c){D2D().FillCircle(cx,cy,r,{c.r,c.g,c.b,c.a});},
    [](float cx,float cy,float r,float sw2,D2DColor c){D2D().StrokeCircle(cx,cy,r,sw2,{c.r,c.g,c.b,c.a});},
    [](float x0,float y0,float x1,float y1,float sw2,D2DColor c){D2D().DrawLine(x0,y0,x1,y1,sw2,{c.r,c.g,c.b,c.a});},
    [](const wchar_t* t2,float x,float y,float sz,D2DColor c,int wt){D2D().DrawTextW(t2,x,y,sz,{c.r,c.g,c.b,c.a},(DWRITE_FONT_WEIGHT)wt);},
    [](const wchar_t* t2,float sz,int wt)->float{return D2D().MeasureTextW(t2,sz,(DWRITE_FONT_WEIGHT)wt);},
    [](const char* t2,float x,float y,float sz,D2DColor c,int wt){D2D().DrawTextA(t2,x,y,sz,{c.r,c.g,c.b,c.a},(DWRITE_FONT_WEIGHT)wt);},
    [](const char* t2,float sz,int wt)->float{return D2D().MeasureTextA(t2,sz,(DWRITE_FONT_WEIGHT)wt);},
    [](const wchar_t* p)->D2DBitmapHandle{auto b=D2D().LoadBitmap(p);return {b.bmp,b.w,b.h};},
    [](const char* p)->D2DBitmapHandle{auto b=D2D().LoadBitmapA(p);return {b.bmp,b.w,b.h};},
    [](D2DBitmapHandle h){D2DBitmap b{(ID2D1Bitmap*)h.opaque,h.w,h.h};D2D().UnloadBitmap(b);},
    [](D2DBitmapHandle h,float x,float y,float w,float ht,float op){D2DBitmap b{(ID2D1Bitmap*)h.opaque,h.w,h.h};D2D().DrawBitmap(b,x,y,w,ht,op);},
    [](D2DBitmapHandle h,float sx,float sy,float sw2,float sh2,float dx,float dy,float dw,float dh,float op){D2DBitmap b{(ID2D1Bitmap*)h.opaque,h.w,h.h};D2D().DrawBitmapCropped(b,sx,sy,sw2,sh2,dx,dy,dw,dh,op);},
    [](float x,float y,float w,float h){D2D().PushClip(x,y,w,h);},
    []{D2D().PopClip();},
    []()->float{return g_time;},
    []()->int{return D2D().ScreenWidth();},
    []()->int{return D2D().ScreenHeight();},
    sinf_wrap,
};

static void InitSkins(){ PM().Init(g_app.exeDir,&g_d2dAPI,&g_hostAPI); PM().LoadSkinChoice(); }
static void UnloadSkinPlugins(){ PM().Shutdown(); }

// ============================================================================
// STEAM AVATAR
// ============================================================================

std::string FindSteamAvatarPath(){
    char sp[MAX_PATH]={}; HKEY hk; DWORD sz=sizeof(sp);
    const char* rp[]={"SOFTWARE\\WOW6432Node\\Valve\\Steam","SOFTWARE\\Valve\\Steam"};
    for(auto r:rp) if(RegOpenKeyExA(HKEY_LOCAL_MACHINE,r,0,KEY_READ,&hk)==ERROR_SUCCESS){sz=sizeof(sp);RegQueryValueExA(hk,"InstallPath",nullptr,nullptr,(LPBYTE)sp,&sz);RegCloseKey(hk);if(strlen(sp))break;}
    if(!strlen(sp)){sz=sizeof(sp);if(RegOpenKeyExA(HKEY_CURRENT_USER,"SOFTWARE\\Valve\\Steam",0,KEY_READ,&hk)==ERROR_SUCCESS){RegQueryValueExA(hk,"SteamPath",nullptr,nullptr,(LPBYTE)sp,&sz);RegCloseKey(hk);}}
    if(!strlen(sp)) for(auto p:{"C:\\Program Files (x86)\\Steam","C:\\Program Files\\Steam","D:\\Steam"})if(fs::exists(p)){strcpy(sp,p);break;}
    if(!strlen(sp)) return "";
    std::string udp=std::string(sp)+"\\userdata";
    try {
        if(!fs::exists(udp))return "";
        std::string best; fs::file_time_type lt; bool ff=false;
        for(auto& e:fs::directory_iterator(udp)){
            if(!e.is_directory())continue; auto id=e.path().filename().string();
            if(id=="0"||id=="ac"||id=="anonymous")continue;
            auto mt=fs::last_write_time(e.path()); if(!ff||mt>lt){lt=mt;best=e.path().string();ff=true;}
        }
        if(best.empty())return "";
        std::string cd=best+"\\config\\avatarcache";
        if(fs::exists(cd)){
            std::string ba; uintmax_t ls=0; fs::file_time_type nt; bool fa=false;
            for(auto& ce:fs::directory_iterator(cd)){
                if(!ce.is_regular_file())continue; auto ext=ce.path().extension().string();
                std::transform(ext.begin(),ext.end(),ext.begin(),::tolower);
                if(ext==".jpg"||ext==".png"||ext==".jpeg"){
                    try{ auto fsz=fs::file_size(ce.path());
auto ft=fs::last_write_time(ce.path());
                         if(fsz>5000&&(!fa||fsz>ls)){ls=fsz;nt=ft;ba=ce.path().string();fa=true;}
                    }catch(...){}
                }
            }
            if(!ba.empty())return ba;
        }
    }catch(...){}
    return "";
}

void LoadSteamAvatar(){
    if(g_app.steamAvatarAttempted)return; g_app.steamAvatarAttempted=true;
    std::string ap=FindSteamAvatarPath();
    if(!ap.empty()&&fs::exists(ap)){g_app.steamAvatarTex=D2D().LoadBitmapA(ap.c_str());if(g_app.steamAvatarTex.Valid()){g_app.steamAvatarLoaded=true;g_app.steamAvatarPath=ap;return;}}
    for(auto p:{"profile\\avatar.png","profile\\avatar.jpg","profile\\steam_avatar.png"}){
        std::string fp=GetFullPath(p); if(!fs::exists(fp))continue;
        g_app.steamAvatarTex=D2D().LoadBitmapA(fp.c_str()); if(g_app.steamAvatarTex.Valid()){g_app.steamAvatarLoaded=true;g_app.steamAvatarPath=fp;return;}
    }
}

void DrawSteamAvatar(float cx,float cy,float radius,bool showBorder=true){
    auto& t=g_app.theme;
    if(g_app.steamAvatarTex.Valid()){
        D2D().DrawBitmap(g_app.steamAvatarTex,cx-radius,cy-radius,radius*2,radius*2);
    } else {
        D2D().FillGradientV(cx-radius,cy-radius,radius*2,radius*2,CA(t.accent,0.9f),CA(t.accent,0.6f));
        std::string dn=g_app.steamProfile.username.empty()?g_app.profile.username:g_app.steamProfile.username;
        char ini[2]={dn.empty()?'U':(char)toupper(dn[0]),0}; float fs2=radius*0.85f;
        D2D().DrawTextA(ini,cx-D2D().MeasureTextA(ini,fs2)/2,cy-fs2/2,fs2,WHITE_COL,(DWRITE_FONT_WEIGHT)700);
    }
    if(showBorder){
        D2D().StrokeCircle(cx,cy,radius,  1.f,CA(t.accent,0.7f));
        D2D().StrokeCircle(cx,cy,radius+2,1.f,CA(t.accent,0.3f));
    }
}

// ============================================================================
// HUB SLIDER
// ============================================================================

void LoadHubSliderTextures(){
    auto& hs=g_app.hubSlider; if(hs.hasTextures)return;
    std::string ap[]={"img\\artcover.png","img\\artcover2.png","img\\artcover3.png"};
    for(int i=0;i<3;i++){std::string p=GetFullPath(ap[i]);if(fs::exists(p))hs.artCovers[i]=D2D().LoadBitmapA(p.c_str());}
    hs.hasTextures=true;
}
void UpdateHubSlider(float dt){
    auto& hs=g_app.hubSlider; hs.slideTimer+=dt;
    hs.transitionProgress=LerpF(hs.transitionProgress,1.f,0.08f);
    if(hs.slideTimer>=5.f){hs.slideTimer=0;hs.currentSlide=(hs.currentSlide+1)%3;hs.transitionProgress=0;}
}

// ============================================================================
// PROFILE I/O
// ============================================================================

void SaveProfile(){
    auto d=GetFullPath("profile");
    try{fs::create_directories(d);fs::create_directories(GetFullPath("profile\\sounds"));}catch(...){}
    auto& p=g_app.profile;
    std::ofstream c(d+"\\config.txt");
    if(c) c<<g_app.bgPath<<"\n"<<p.username<<"\n"<<p.avatarPath<<"\n"<<p.themeIndex<<"\n"
           <<p.masterVolume<<"\n"<<p.musicVolume<<"\n"<<p.sfxVolume<<"\n"
           <<(p.soundEnabled?1:0)<<"\n"<<(p.musicEnabled?1:0)<<"\n";
    std::ofstream l(d+"\\library.txt");
    if(l) for(auto& g:g_app.library)l<<g.info.name<<"|"<<g.info.exePath<<"|"<<g.info.platform<<"|"<<g.info.appId<<"\n";
    std::ofstream a(d+"\\apps.txt");
    if(a) for(auto& app:g_app.customApps){
        int r2=(int)(app.accentColor.r*255),g2=(int)(app.accentColor.g*255),b2=(int)(app.accentColor.b*255);
        a<<app.name<<"|"<<app.path<<"|"<<app.iconPath<<"|"<<(app.isWebApp?1:0)<<"|"<<r2<<"|"<<g2<<"|"<<b2<<"\n";
    }
}

void LoadProfile(){
    auto& p=g_app.profile; std::string path=GetFullPath("profile\\config.txt");
    if(!fs::exists(path))return; std::ifstream c(path); if(!c)return;
    auto rl=[&](std::string& o)->bool{return!!std::getline(c,o);};
    auto rf=[&](float& o){std::string l;if(rl(l))try{o=std::stof(l);}catch(...){}};
    auto ri=[&](int& o){std::string l;if(rl(l))try{o=std::stoi(l);}catch(...){}};
    auto rb=[&](bool& o){std::string l;if(rl(l))o=(l=="1");};
    rl(g_app.bgPath);rl(p.username);rl(p.avatarPath);ri(p.themeIndex);
    rf(p.masterVolume);rf(p.musicVolume);rf(p.sfxVolume);rb(p.soundEnabled);rb(p.musicEnabled);
    if(p.username.empty())p.username="Player";
    g_audio.masterVolume=p.masterVolume;g_audio.musicVolume=p.musicVolume;g_audio.sfxVolume=p.sfxVolume;
    g_audio.soundEnabled=p.soundEnabled;g_audio.musicEnabled=p.musicEnabled;
    if(p.themeIndex>=0&&p.themeIndex<(int)ALL_THEMES.size())
        g_app.currentThemeIdx=p.themeIndex,g_app.theme=g_app.targetTheme=ALL_THEMES[p.themeIndex];
}

void LoadCustomAppsFromProfile(){
    std::string path=GetFullPath("profile\\apps.txt"); if(!fs::exists(path))return;
    std::ifstream f(path); std::string line;
    while(std::getline(f,line)){
        if(line.empty())continue; std::stringstream ss(line);
        std::string name,appPath,iconPath,isWeb,r2,g2,b2;
        std::getline(ss,name,'|');std::getline(ss,appPath,'|');std::getline(ss,iconPath,'|');
        std::getline(ss,isWeb,'|');std::getline(ss,r2,'|');std::getline(ss,g2,'|');std::getline(ss,b2,'|');
        if(!name.empty()&&!appPath.empty()){
            CustomApp app; app.name=name;app.path=appPath;app.iconPath=iconPath;app.isWebApp=(isWeb=="1");
            try{app.accentColor=C(std::stoi(r2),std::stoi(g2),std::stoi(b2));}catch(...){}
            g_app.customApps.push_back(app);
        }
    }
}

// ============================================================================
// LIBRARY
// ============================================================================

void LoadLibraryFromDisk(){
    std::string p=GetFullPath("profile\\library.txt"); if(!fs::exists(p))return;
    std::ifstream f(p); std::string l;
    while(std::getline(f,l)){if(l.empty())continue;std::stringstream ss(l);std::string n,e,pl,id;
        std::getline(ss,n,'|');std::getline(ss,e,'|');std::getline(ss,pl,'|');std::getline(ss,id,'|');
        if(!n.empty()&&!e.empty())g_app.library.push_back({{n,e,pl,id}});}
}

void RefreshLibrary(){
    auto sc=GetInstalledGames(); bool nw=false;
    for(auto& s:sc){bool ex=false;for(auto& lib:g_app.library)if(lib.info.exePath==s.exePath){ex=true;break;}
        if(!ex){g_app.library.push_back({s});nw=true;}}
    if(nw){SaveProfile();ShowNotification("Library Updated",std::to_string(sc.size())+" games found",1);}
}

void LoadGamePosters(){
    for(auto& g:g_app.library){if(g.hasPoster)continue;
        for(auto e:{".png",".jpg"}){std::string p=GetFullPath("img\\"+g.info.name+e);
            if(fs::exists(p)){g.poster=D2D().LoadBitmapA(p.c_str());g.hasPoster=g.poster.Valid();if(g.hasPoster)break;}}}
}
void LoadCustomAppIcons(){
    for(auto& app:g_app.customApps){if(app.hasIcon||app.iconPath.empty())continue;
        std::string fp=GetFullPath(app.iconPath);
        if(fs::exists(fp)){app.icon=D2D().LoadBitmapA(fp.c_str());app.hasIcon=app.icon.Valid();}}
}

// ============================================================================
// PLATFORM CONNECTIONS
// ============================================================================

std::vector<PlatformConnection> GetPlatformConnections(){
    std::vector<PlatformConnection> pl;
    char buf[MAX_PATH]={}; HKEY hk; DWORD sz=sizeof(buf);
    bool stm=RegOpenKeyExA(HKEY_LOCAL_MACHINE,"SOFTWARE\\WOW6432Node\\Valve\\Steam",0,KEY_READ,&hk)==ERROR_SUCCESS&&
             (RegQueryValueExA(hk,"InstallPath",nullptr,nullptr,(LPBYTE)buf,&sz),RegCloseKey(hk),strlen(buf)>0);
    pl.push_back({"Steam","S",C(102,192,244),stm,stm?"Connected":"Not Found","steam://open/main"});
    bool epic=fs::exists("C:\\Program Files\\Epic Games")||fs::exists("C:\\Program Files (x86)\\Epic Games");
    pl.push_back({"Epic","E",C(40,40,40),epic,epic?"Connected":"Not Found","com.epicgames.launcher://"});
    pl.push_back({"Xbox","X",C(16,124,16),true,"Windows","xbox://"});
    bool gog=fs::exists("C:\\Program Files (x86)\\GOG Galaxy\\GalaxyClient.exe");
    pl.push_back({"GOG","G",C(134,46,191),gog,gog?"Connected":"Not Found",""});
    return pl;
}
void InitPlatformConnections(){g_app.platformConnections=GetPlatformConnections();}

// ============================================================================
// TASK LIST
// ============================================================================

BOOL CALLBACK EnumWindowsForTasks(HWND hwnd,LPARAM lParam){
    auto* tasks=reinterpret_cast<std::vector<RunningTask>*>(lParam);
    if(!IsWindowVisible(hwnd)||GetWindowTextLengthA(hwnd)==0)return TRUE;
    LONG ex=GetWindowLong(hwnd,GWL_EXSTYLE);
    if(ex&WS_EX_TOOLWINDOW||GetWindow(hwnd,GW_OWNER))return TRUE;
    char title[512]; GetWindowTextA(hwnd,title,512); std::string ts(title);
    if(ts=="Program Manager"||ts=="Windows Input Experience"||ts.find("Q-Shell")!=std::string::npos||ts.empty())return TRUE;
    DWORD pid=0; GetWindowThreadProcessId(hwnd,&pid); char pn[MAX_PATH]="App";
    HANDLE h=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if(h){DWORD sz=MAX_PATH;if(QueryFullProcessImageNameA(h,0,pn,&sz)){auto fp=std::string(pn);auto p=fp.find_last_of("\\/");if(p!=std::string::npos)strcpy(pn,fp.substr(p+1).c_str());}CloseHandle(h);}
    HICON ic=(HICON)SendMessage(hwnd,WM_GETICON,ICON_BIG,0);
    if(!ic)ic=(HICON)SendMessage(hwnd,WM_GETICON,ICON_SMALL,0);
    if(!ic)ic=(HICON)GetClassLongPtr(hwnd,GCLP_HICON);
    tasks->push_back({pn,ts,hwnd,pid,false,ic}); return TRUE;
}
void RefreshTaskList(){g_app.tasks.clear();EnumWindows(EnumWindowsForTasks,reinterpret_cast<LPARAM>(&g_app.tasks));}

// ============================================================================
// INPUT MONITORING THREAD
// ============================================================================

static bool s_tabDown=false,s_oDown=false;

LRESULT CALLBACK LowLevelKeyboardProc(int nCode,WPARAM wParam,LPARAM lParam){
    if(nCode==HC_ACTION){
        auto* kb=(KBDLLHOOKSTRUCT*)lParam;
        bool dn=(wParam==WM_KEYDOWN||wParam==WM_SYSKEYDOWN),up=(wParam==WM_KEYUP||wParam==WM_SYSKEYUP);
        if(kb->vkCode==VK_TAB){if(dn)s_tabDown=true;if(up)s_tabDown=false;}
        if(kb->vkCode=='O'){if(dn)s_oDown=true;if(up)s_oDown=false;}
        if(s_tabDown&&s_oDown&&dn){DWORD n=GetTickCount();
            if(n-g_app.lastTaskSwitchTime>DEBOUNCE_MS){g_app.taskSwitchRequested=true;g_app.lastTaskSwitchTime=n;return 1;}}
    }
    return CallNextHookEx(g_app.kbHook,nCode,wParam,lParam);
}

void InputMonitorThread(){
    g_app.kbHook=SetWindowsHookExA(WH_KEYBOARD_LL,LowLevelKeyboardProc,GetModuleHandle(nullptr),0);
    bool wp=false;
    while(g_app.running){
        MSG msg; while(PeekMessage(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessage(&msg);}
        if(XInput::s_getState){
            for(DWORD i=0;i<4;i++){
                XInput::State st; ZeroMemory(&st,sizeof(st));
                if(XInput::GetState(i,&st)!=ERROR_SUCCESS)continue;
                WORD b=st.gamepad.buttons;
                bool p=(b&XInput::BTN_BACK&&b&XInput::BTN_X)||(b&XInput::BTN_START&&b&XInput::BTN_BACK);
                if(p&&!wp){DWORD n=GetTickCount();if(n-g_app.lastTaskSwitchTime>DEBOUNCE_MS){g_app.taskSwitchRequested=true;g_app.lastTaskSwitchTime=n;}}
                wp=p; break;
            }
        }
        Sleep(10);
    }
    if(g_app.kbHook){UnhookWindowsHookEx(g_app.kbHook);g_app.kbHook=nullptr;}
}

void StartInputMonitoring(){XInput::Load();g_app.running=true;g_app.inputThread=std::thread(InputMonitorThread);}
void StopInputMonitoring() {g_app.running=false;if(g_app.inputThread.joinable())g_app.inputThread.join();XInput::Unload();}

// ============================================================================
// WINDOW MANAGEMENT
// ============================================================================

void BringWindowToFront(HWND h){
    if(!h||!IsWindow(h))return; if(IsIconic(h))ShowWindow(h,SW_RESTORE);
    DWORD c=GetCurrentThreadId(),tt=GetWindowThreadProcessId(h,nullptr);
    AttachThreadInput(c,tt,TRUE); SetWindowPos(h,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW);
    SetForegroundWindow(h);BringWindowToTop(h);SetFocus(h); AttachThreadInput(c,tt,FALSE);
}
void PushMainWindowBack(){
    if(!g_app.mainWindow)return;
    SetWindowPos(g_app.mainWindow,HWND_BOTTOM,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE);
    if(!g_app.isShellMode)ShowWindow(g_app.mainWindow,SW_MINIMIZE); else ShowWindow(g_app.mainWindow,SW_HIDE);
    g_app.windowOnTop=false;
}
void BringMainWindowToForeground(){
    if(!g_app.mainWindow)return;
    ShowWindow(g_app.mainWindow,SW_RESTORE); ShowWindow(g_app.mainWindow,SW_SHOW);
    SetWindowPos(g_app.mainWindow,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW);
    SetForegroundWindow(g_app.mainWindow); BringWindowToTop(g_app.mainWindow); g_app.windowOnTop=true;
}
void SwitchToTask(int i){
    if(i<0||i>=(int)g_app.tasks.size())return;
    HWND h=g_app.tasks[i].hwnd; if(!IsWindow(h))return;
    PushMainWindowBack(); Sleep(50); BringWindowToFront(h);
}
void LaunchApp(const std::string& path,bool isWeb=false){
    if(path.empty())return;
    if(isWeb||path.find("://")!=std::string::npos){ShellExecuteA(nullptr,"open",path.c_str(),nullptr,nullptr,SW_SHOWNORMAL);return;}
    // Extract parent directory only — do NOT assign full path to dir first,
    // otherwise a path with no separator would pass the exe filename as lpDirectory
    // and cause ShellExecuteEx to crash with an access violation.
    std::string dir;
    auto p=path.find_last_of("\\/");
    if(p!=std::string::npos)dir=path.substr(0,p);
    SHELLEXECUTEINFOA s={sizeof(s)};s.fMask=SEE_MASK_NOCLOSEPROCESS;s.lpVerb="open";
    s.lpFile=path.c_str();
    s.lpDirectory=dir.empty()?nullptr:dir.c_str();  // nullptr = let shell resolve; never pass garbage
    s.nShow=SW_SHOWNORMAL;
    ShellExecuteExA(&s);if(s.hProcess)CloseHandle(s.hProcess);
}
std::string OpenFilePicker(bool exe){
    char b[MAX_PATH]={}; OPENFILENAMEA o={};
    o.lStructSize=sizeof(o);o.hwndOwner=g_app.mainWindow;o.lpstrFile=b;o.nMaxFile=sizeof(b);
    o.lpstrFilter=exe?"Executables (*.exe)\0*.exe\0All Files (*.*)\0*.*\0"
                     :"Images (*.png;*.jpg;*.jpeg;*.gif;*.bmp)\0*.png;*.jpg;*.jpeg;*.gif;*.bmp\0All Files (*.*)\0*.*\0";
    o.lpstrTitle=exe?"Select Executable":"Select Image";
    o.Flags=OFN_PATHMUSTEXIST|OFN_FILEMUSTEXIST|OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&o)?std::string(b):"";
}

// ============================================================================
// CRASH RECOVERY
// ============================================================================

LONG WINAPI CrashHandler(EXCEPTION_POINTERS*){
    StopInputMonitoring(); g_audio.Cleanup();
    STARTUPINFOA si={sizeof(si)};PROCESS_INFORMATION pi={};
    char cmd[]="explorer.exe"; CreateProcessA(nullptr,cmd,nullptr,nullptr,FALSE,0,nullptr,nullptr,&si,&pi);
    if(pi.hProcess)CloseHandle(pi.hProcess);if(pi.hThread)CloseHandle(pi.hThread);
    return EXCEPTION_EXECUTE_HANDLER;
}
void CreateEmergencyRestoreBatch(){
    auto d=GetFullPath("backup");try{fs::create_directories(d);}catch(...){}
    std::ofstream b(d+"\\EMERGENCY_RESTORE.bat");
    if(b)b<<"@echo off\ntitle Q-SHELL EMERGENCY RESTORE\n"
            "reg delete \"HKCU\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon\" /v Shell /f 2>nul\n"
            "reg add \"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon\" /v Shell /t REG_SZ /d \"explorer.exe\" /f 2>nul\n"
            "start explorer.exe\necho RESTORE COMPLETE!\npause\n";
}

// ============================================================================
// BACKGROUND
// ============================================================================

void LoadBackground(const std::string& p){
    if(g_app.bgTexture.Valid())D2D().UnloadBitmap(g_app.bgTexture);
    g_app.bgTexture={}; if(p.empty()||!fs::exists(p))return;
    g_app.bgTexture=D2D().LoadBitmapA(p.c_str());
}
void ChangeBackground(){
    auto p=OpenFilePicker(false);if(p.empty())return;
    g_app.bgPath=p;LoadBackground(p);SaveProfile();ShowNotification("Background Changed","New wallpaper set",1);
}
void DrawBackground(int w,int h,float alpha=1.f){
    float _time=GetTime();
    if(alpha>=1.f&&PM().DrawBackground(w,h,_time))return;
    auto& t=g_app.theme;
    if(g_app.bgTexture.Valid()){
        D2D().DrawBitmap(g_app.bgTexture,0,0,(float)w,(float)h,alpha);
        if(alpha>=1.f)D2D().FillRect(0,0,(float)w,(float)h,CA(t.primary,0.75f));
    } else {
        D2D().FillGradientV(0,0,(float)w,(float)h,CA(t.secondary,1.1f),t.primary);
    }
}

// ============================================================================
// DRAWING HELPERS
// ============================================================================

void DrawCircularAvatar(float cx,float cy,float r,const UserProfile& p){
    auto& t=g_app.theme;
    if(p.hasAvatar&&p.avatar.Valid()){
        D2D().DrawBitmap(p.avatar,cx-r,cy-r,r*2,r*2);
    } else {
        D2D().FillGradientV(cx-r,cy-r,r*2,r*2,CA(t.accent,0.8f),CA(t.accent,0.5f));
        char i[2]={p.username.empty()?'P':(char)toupper(p.username[0]),0};
        float fs2=r*1.1f,iw=D2D().MeasureTextA(i,fs2,(DWRITE_FONT_WEIGHT)700);
        D2D().DrawTextA(i,cx-iw/2,cy-fs2/2,fs2,WHITE_COL,(DWRITE_FONT_WEIGHT)700);
    }
    D2D().StrokeCircle(cx,cy,r,1.f,CA(WHITE_COL,0.5f));
}

// QRect ↔ library/plugin interface
using QRect_t = QRect;

void DrawGameCard(QRect_t card,UIGame& game,bool foc,float time){
    D2DBitmapHandle ph{game.poster.bmp,game.poster.w,game.poster.h};
    if(PM().DrawGameCard(card,game.info.name.c_str(),foc,ph,time))return;
    auto& t=g_app.theme; float rx=card.height*0.025f;
    D2D().FillRoundRect(card.x+5,card.y+5,card.width,card.height,rx,rx,CA(BLACK_COL,0.25f));
    D2D().FillRoundRect(card.x,card.y,card.width,card.height,rx,rx,t.cardBg);
    float a=foc?1.f:0.25f;
    if(game.hasPoster&&game.poster.Valid()){
        float ta=(float)game.poster.w/game.poster.h,ca=card.width/card.height;
        float sx=0,sy=0,sw2=(float)game.poster.w,sh2=(float)game.poster.h;
        if(ta>ca){sw2=game.poster.h*ca;sx=(game.poster.w-sw2)/2;}
        else{sh2=game.poster.w/ca;sy=(game.poster.h-sh2)/2;}
        D2D().DrawBitmapCropped(game.poster,sx,sy,sw2,sh2,card.x,card.y,card.width,card.height,a);
    } else {
        char i[2]={game.info.name.empty()?'?':(char)toupper(game.info.name[0]),0};
        float iw=D2D().MeasureTextA(i,80,(DWRITE_FONT_WEIGHT)700);
        D2D().DrawTextA(i,card.x+card.width/2-iw/2,card.y+card.height/2-40,80,CA(t.text,a*0.2f),(DWRITE_FONT_WEIGHT)700);
    }
    if(foc){float p2=(sinf(time*4)+1)/2;D2D().StrokeRoundRect(card.x,card.y,card.width,card.height,rx,rx,4.f,CA(t.accent,0.4f+p2*0.4f));}
}

void DrawSettingsTile(QRect_t r,const char* ic,const char* ti,D2D1_COLOR_F ac,bool foc,float time){
    D2DColor dac{ac.r,ac.g,ac.b,ac.a};
    if(PM().DrawSettingsTile(r,ic,ti,dac,foc,time))return;
    auto& t=g_app.theme; float sc=foc?1.04f:1.f;
    QRect_t s={r.x-r.width*(sc-1)/2,r.y-r.height*(sc-1)/2,r.width*sc,r.height*sc};
    float rx=s.width*0.075f;
    D2D().FillRoundRect(s.x+4,s.y+4,s.width,s.height,rx,rx,CA(BLACK_COL,foc?0.3f:0.18f));
    D2D().FillRoundRect(s.x,s.y,s.width,s.height,rx,rx,foc?CA(t.cardBg,1.15f):t.cardBg);
    float iw=D2D().MeasureTextA(ic,42),tw2=D2D().MeasureTextA(ti,16);
    D2D().DrawTextA(ic,s.x+(s.width-iw)/2,s.y+s.height*0.28f,42,foc?ac:CA(ac,0.5f));
    D2D().DrawTextA(ti,s.x+(s.width-tw2)/2,s.y+s.height*0.7f,16,foc?t.text:t.textDim);
    if(foc){float p2=(sinf(time*4)+1)/2;D2D().StrokeRoundRect(s.x,s.y,s.width,s.height,rx,rx,1.f,CA(ac,0.35f+p2*0.3f));}
}

// ============================================================================
// TOP & BOTTOM BARS
// ============================================================================

void DrawTopBar(int sw,float ty){
    float _time=GetTime();
    if(PM().DrawTopBar(sw,D2D().ScreenHeight(),_time))return;
    auto& s=g_app; auto& t=s.theme;
    D2D().FillRect(0,0,(float)sw,110,CA(BLACK_COL,0.85f));
    D2D().FillRect(0,109,(float)sw,1,CA(t.accent,0.1f));
    DrawCircularAvatar(55,ty+5,25,s.profile);
    D2D().DrawTextA(s.profile.username.c_str(),90,ty-5,18,t.text);
    const char* tabs[]={"LIBRARY","MEDIA","SHARE","SETTINGS"};
    float mx=(sw-MENU_COUNT*180.f)/2;
    for(int m=0;m<MENU_COUNT;m++){
        bool sel=(s.barFocused==m);
        D2D().DrawTextA(tabs[m],mx+m*180,ty,22,sel?t.text:t.textDim,sel?(DWRITE_FONT_WEIGHT)700:(DWRITE_FONT_WEIGHT)400);
        if(sel)D2D().FillRect(mx+m*180,ty+35,30,3,t.accent);
    }
    SYSTEM_POWER_STATUS sps; GetSystemPowerStatus(&sps);
    int bt=std::min((int)sps.BatteryLifePercent,100); char btbuf[16]; snprintf(btbuf,16,"%d%%",bt);
    D2D().StrokeRoundRect((float)(sw-300),ty+4,35,18,2,2,1.f,CA(t.text,0.6f));
    D2D().FillRect((float)(sw-298),ty+6,31.f*bt/100,14,bt<20?t.danger:t.success);
    D2D().DrawTextA(btbuf,(float)(sw-255),ty+4,18,t.text);
    time_t now2=time(nullptr);struct tm* lt=localtime(&now2);char tb[16];
    strftime(tb,sizeof(tb),"%H:%M",lt); D2D().DrawTextA(tb,(float)(sw-120),ty,26,t.text);
    if(s.isShellMode){D2D().FillRoundRect((float)(sw-200),ty-5,70,22,11,11,CA(t.success,0.2f));D2D().DrawTextA("SHELL",(float)(sw-190),ty-1,12,t.success);}
    if(s.isRecording){float rp=(sinf(_time*6)+1)/2;D2D().FillCircle((float)(sw-350),ty+12,6,CA(RED_COL,0.5f+rp*0.5f));D2D().DrawTextA("REC",(float)(sw-340),ty+4,14,RED_COL);}
}

void DrawBottomBar(int sw,int sh,float time){
    if(PM().DrawBottomBar(sw,sh,time))return;
    auto& s=g_app; auto& t=s.theme; float y=(float)(sh-70);
    D2D().FillRect(0,y,(float)sw,70,CA(BLACK_COL,0.85f));
    D2D().FillRect(0,y,(float)sw,1,CA(t.accent,0.1f));
    D2D().FillRoundRect(30,y+12,280,45,22,22,CA(PURPLE_COL,0.15f));
    D2D().DrawTextA("TAB+O / SHARE+X: Task Switcher",50,y+26,14,CA(t.text,0.8f));
    float pulse=(sinf(time*4)+1)/2;
    D2D().FillRoundRect((float)(sw-280),y+12,250,45,22,22,CA(t.accent,0.1f+pulse*0.1f));
    D2D().DrawTextA("[B] SET BACKGROUND",(float)(sw-240),y+27,14,t.text);
    const char* hints[]={"[A] Launch | [Y] Art | [X] Delete","[A] Open | [X] Remove | [+] Add","[A] Select | [Arrows] Navigate","[A] Select | [Arrows] Navigate"};
    int hi=Clamp(s.barFocused,0,3); float hw=D2D().MeasureTextA(hints[hi],14);
    D2D().DrawTextA(hints[hi],(sw-hw)/2,y+28,14,CA(t.textDim,0.6f));
}

// ============================================================================
// MEDIA TAB
// ============================================================================

void InitDefaultApps(){
    if(!g_app.customApps.empty())return;
    g_app.customApps={
        {"Google","https://www.google.com","",C(66,133,244),{},false,true},
        {"YouTube","https://www.youtube.com","",C(255,0,0),{},false,true},
        {"Steam","steam://open/main","",C(102,192,244),{},false,true},
        {"Spotify","https://open.spotify.com","",C(30,215,96),{},false,true},
        {"Discord","https://discord.com/app","",C(88,101,242),{},false,true},
        {"Twitch","https://www.twitch.tv","",C(145,70,255),{},false,true},
        {"Netflix","https://www.netflix.com","",C(229,9,20),{},false,true},
        {"Twitter","https://twitter.com","",C(29,161,242),{},false,true},
        {"Prime Video","https://www.primevideo.com","",C(0,168,225),{},false,true},
        {"Crunchyroll","https://www.crunchyroll.com","",C(255,117,24),{},false,true},
    };
}

void DrawMediaTab(int sw,int sh,float contentTop,InputAdapter& input,float dt){
    auto& s=g_app; auto& t=s.theme;
    float time2=GetTime(),pulse=(sinf(time2*4)+1)/2;
    int baseX=60,baseY=(int)contentTop+15,contentW=sw-120;
    D2D().FillGradientH((float)baseX,(float)baseY,(float)contentW,50,CA(t.accent,0.08f),CA(t.accent,0.02f));
    D2D().FillRect((float)baseX,(float)(baseY+49),(float)contentW,1,CA(t.accent,0.2f));
    D2D().DrawTextA("MEDIA & APPS",(float)(baseX+20),(float)(baseY+12),26,t.text,(DWRITE_FONT_WEIGHT)700);
    int appCount=(int)s.customApps.size(); char cs2[32];snprintf(cs2,32,"%d Applications",appCount);
    D2D().DrawTextA(cs2,(float)(baseX+220),(float)(baseY+18),14,CA(t.textDim,0.7f));
    bool addBtnFoc=s.inTopBar; float abx=(float)(sw-180),aby=(float)(baseY+8);
    D2D().FillRoundRect(abx,aby,110,36,14,14,addBtnFoc?CA(t.success,0.25f):CA(t.cardBg,0.6f));
    D2D().StrokeRoundRect(abx,aby,110,36,14,14,1.f,addBtnFoc?CA(t.success,0.6f+pulse*0.3f):CA(t.success,0.3f));
    float pw2=D2D().MeasureTextA("+ Add",15);D2D().DrawTextA("+ Add",abx+55-pw2/2,aby+10,15,addBtnFoc?t.success:CA(t.success,0.7f));
    int gridY=baseY+65,cardW=150,cardH=120,gapX=18,gapY=15;
    int cols2=Clamp((contentW-20)/(cardW+gapX),5,10),totalApps=appCount+1;
    int focRow=s.mediaFocusIdx/cols2,visRows=(sh-gridY-100)/(cardH+gapY);
    float tgtS=0; if(focRow>visRows-1)tgtS=-(float)((focRow-visRows+1)*(cardH+gapY));
    s.mediaScrollY=LerpF(s.mediaScrollY,tgtS,0.15f);
    for(int i=0;i<totalApps;i++){
        int row=i/cols2,col=i%cols2;
        float cardX=(float)(baseX+10+col*(cardW+gapX)),cardY=(float)(gridY+row*(cardH+gapY))+s.mediaScrollY;
        if(cardY<gridY-cardH-10||cardY>sh-60)continue;
        bool isFoc=(!s.inTopBar&&i==s.mediaFocusIdx);
        if(i<appCount){
            auto& app=s.customApps[i]; float sc2=isFoc?1.06f:1.f,sw3=cardW*sc2,sh3=cardH*sc2;
            float sx=cardX-(sw3-cardW)/2,sy=cardY-(sh3-cardH)/2;
            D2D().FillRoundRect(sx+3,sy+3,sw3,sh3,9,9,CA(BLACK_COL,isFoc?0.35f:0.15f));
            D2D().FillRoundRect(sx,sy,sw3,sh3,9,9,isFoc?CA(app.accentColor,0.12f):CA(t.cardBg,0.95f));
            D2D().FillRoundRect(sx,sy,sw3,5,2,2,isFoc?app.accentColor:CA(app.accentColor,0.4f));
            float iconX2=sx+sw3/2,iconY2=sy+45,iconR2=isFoc?28.f:24.f;
            if(isFoc){D2D().FillCircle(iconX2,iconY2,iconR2+10,CA(app.accentColor,0.08f+pulse*0.06f));D2D().FillCircle(iconX2,iconY2,iconR2+5,CA(app.accentColor,0.12f));}
            D2D().FillGradientV(iconX2-iconR2,iconY2-iconR2,iconR2*2,iconR2*2,CA(app.accentColor,isFoc?1.1f:0.9f),CA(app.accentColor,isFoc?0.8f:0.7f));
            char icon2[2]={app.name.empty()?'?':(char)toupper(app.name[0]),0};
            float ifs=isFoc?22.f:18.f,itw=D2D().MeasureTextA(icon2,ifs,(DWRITE_FONT_WEIGHT)700);
            D2D().DrawTextA(icon2,iconX2-itw/2,iconY2-ifs/2,ifs,WHITE_COL,(DWRITE_FONT_WEIGHT)700);
            std::string dn=app.name; if((int)dn.size()>cardW/8)dn=dn.substr(0,cardW/8-2)+"..";
            float nw2=D2D().MeasureTextA(dn.c_str(),13);
            D2D().DrawTextA(dn.c_str(),sx+sw3/2-nw2/2,sy+sh3-38,13,isFoc?t.text:CA(t.text,0.8f));
            const char* tt2=app.isWebApp?"WEB":"APP"; float tw2=D2D().MeasureTextA(tt2,9);
            float bx2=sx+sw3/2-tw2/2-6,by2=sy+sh3-20;
            D2D().FillRoundRect(bx2,by2,tw2+12,14,7,7,CA(app.accentColor,isFoc?0.25f:0.12f));
            D2D().DrawTextA(tt2,bx2+6,by2+2,9,CA(app.accentColor,isFoc?1.f:0.6f));
            if(isFoc)D2D().StrokeRoundRect(sx-2,sy-2,sw3+4,sh3+4,9,9,1.f,CA(app.accentColor,0.5f+pulse*0.35f));
        } else {
            float sc2=isFoc?1.06f:1.f,sw3=cardW*sc2,sh3=cardH*sc2;
            float sx=cardX-(sw3-cardW)/2,sy=cardY-(sh3-cardH)/2;
            D2D().FillRoundRect(sx,sy,sw3,sh3,9,9,isFoc?CA(t.accent,0.12f):CA(t.cardBg,0.5f));
            D2D().StrokeRoundRect(sx,sy,sw3,sh3,9,9,1.f,CA(t.accent,isFoc?0.5f:0.25f));
            float px=sx+sw3/2,py=sy+sh3/2-15;
            if(isFoc)D2D().FillCircle(px,py,25,CA(t.accent,0.1f+pulse*0.08f));
            D2D().DrawTextA("+",px-12,py-18,45,isFoc?t.accent:CA(t.accent,0.4f));
            float aw=D2D().MeasureTextA("Add App",12);
            D2D().DrawTextA("Add App",sx+sw3/2-aw/2,sy+sh3-30,12,isFoc?t.text:CA(t.textDim,0.6f));
        }
    }
    // Input
    if(s.inTopBar){
        if(input.IsMoveDown()){s.inTopBar=false;s.mediaFocusIdx=0;PlayMoveSound();}
        if(input.IsConfirm()){s.currentMode=UIMode::ADD_APP;s.addAppFocus=0;s.addAppNameBuffer[0]=0;s.addAppPathBuffer[0]=0;s.isAddingWebApp=true;PlayConfirmSound();}
    } else {
        if(input.IsMoveUp()){int ni=s.mediaFocusIdx-cols2;if(ni<0)s.inTopBar=true;else s.mediaFocusIdx=ni;PlayMoveSound();}
        if(input.IsMoveDown()){int ni=s.mediaFocusIdx+cols2;if(ni<totalApps){s.mediaFocusIdx=ni;PlayMoveSound();}}
        if(input.IsMoveLeft()&&s.mediaFocusIdx>0){s.mediaFocusIdx--;PlayMoveSound();}
        if(input.IsMoveRight()&&s.mediaFocusIdx<totalApps-1){s.mediaFocusIdx++;PlayMoveSound();}
        if(input.IsConfirm()){PlayConfirmSound();
            if(s.mediaFocusIdx<appCount){auto& app=s.customApps[s.mediaFocusIdx];LaunchApp(app.path,app.isWebApp);ShowNotification("Launching",app.name,0);}
            else{s.currentMode=UIMode::ADD_APP;s.addAppFocus=0;s.addAppNameBuffer[0]=0;s.addAppPathBuffer[0]=0;s.isAddingWebApp=true;}}
        if(input.IsDeletePressed()&&s.mediaFocusIdx<appCount){
            std::string name=s.customApps[s.mediaFocusIdx].name;
            s.customApps.erase(s.customApps.begin()+s.mediaFocusIdx);
            s.mediaFocusIdx=Clamp(s.mediaFocusIdx,0,std::max(0,(int)s.customApps.size()-1));
            SaveProfile();ShowNotification("Removed",name,3);PlayBackSound();}
    }
    D2D().FillRect((float)baseX,(float)(sh-75),(float)contentW,1,CA(t.accent,0.1f));
    D2D().DrawTextA("[A] Launch  |  [X] Remove  |  [+] Add App  |  [LB/RB] Switch Tabs",(float)(baseX+20),(float)(sh-60),12,CA(t.textDim,0.5f));
    (void)dt;
}

// ============================================================================
// ADD APP OVERLAY
// ============================================================================

void HandleAddAppOverlay(int sw,int sh,InputAdapter& input){
    auto& s=g_app; auto& t=s.theme;
    float time2=GetTime(),pulse=(sinf(time2*4)+1)/2;
    D2D().FillRect(0,0,(float)sw,(float)sh,CA(BLACK_COL,0.88f));
    int pW=520,pH=400,pX=(sw-pW)/2,pY=(sh-pH)/2;
    D2D().FillRoundRect((float)pX,(float)pY,(float)pW,(float)pH,8,8,CA(t.secondary,0.98f));
    D2D().FillGradientV((float)pX,(float)pY,(float)pW,6,t.accent,CA(t.accent,0.3f));
    D2D().StrokeRoundRect((float)pX,(float)pY,(float)pW,(float)pH,8,8,1.f,CA(t.accent,0.35f));
    D2D().DrawTextA("ADD APPLICATION",(float)(pX+30),(float)(pY+25),24,t.text,(DWRITE_FONT_WEIGHT)700);
    D2D().FillRect((float)(pX+30),(float)(pY+55),180,2,CA(t.accent,0.4f));
    D2D().DrawTextA("[B] Cancel",(float)(pX+pW-95),(float)(pY+28),13,t.textDim);
    // Type
    int typeY=pY+85; bool typeFoc=(s.addAppFocus==0);
    D2D().DrawTextA("Type:",(float)(pX+30),(float)(typeY+5),15,t.textDim);
    D2D().FillRoundRect((float)(pX+100),(float)typeY,110,35,9,9,s.isAddingWebApp?CA(t.accent,0.25f):CA(t.cardBg,0.5f));
    D2D().FillRoundRect((float)(pX+220),(float)typeY,110,35,9,9,!s.isAddingWebApp?CA(t.accent,0.25f):CA(t.cardBg,0.5f));
    (s.isAddingWebApp?D2D().StrokeRoundRect((float)(pX+100),(float)typeY,110,35,9,9,1.f,CA(t.accent,0.6f))
                    :D2D().StrokeRoundRect((float)(pX+220),(float)typeY,110,35,9,9,1.f,CA(t.accent,0.6f)));
    float wa=D2D().MeasureTextA("Web App",14),da=D2D().MeasureTextA("Desktop",14);
    D2D().DrawTextA("Web App",(float)(pX+155)-wa/2,(float)(typeY+10),14,s.isAddingWebApp?t.text:t.textDim);
    D2D().DrawTextA("Desktop",(float)(pX+275)-da/2,(float)(typeY+10),14,!s.isAddingWebApp?t.text:t.textDim);
    if(typeFoc){float fx=(float)(s.isAddingWebApp?pX+98:pX+218);D2D().StrokeRoundRect(fx,typeY-2,114,39,9,9,1.f,CA(t.accent,0.5f+pulse*0.3f));}
    // Name field
    int nameY=pY+150; bool nameFoc=(s.addAppFocus==1);
    D2D().DrawTextA("Name:",(float)(pX+30),(float)nameY,15,t.textDim);
    D2D().FillRoundRect((float)(pX+30),(float)(nameY+25),(float)(pW-60),42,9,9,CA(t.cardBg,0.85f));
    D2D().StrokeRoundRect((float)(pX+30),(float)(nameY+25),(float)(pW-60),42,9,9,1.f,nameFoc?CA(t.accent,0.5f+pulse*0.3f):CA(t.accent,0.15f));
    {std::string nd=std::string(s.addAppNameBuffer);if(nameFoc)nd+="_";D2D().DrawTextA(nd.c_str(),(float)(pX+48),(float)(nameY+37),15,t.text);}
    // Path field
    int pathY=pY+235; bool pathFoc=(s.addAppFocus==2);
    D2D().DrawTextA(s.isAddingWebApp?"URL:":"Path:",(float)(pX+30),(float)pathY,15,t.textDim);
    D2D().FillRoundRect((float)(pX+30),(float)(pathY+25),(float)(pW-60),42,9,9,CA(t.cardBg,0.85f));
    D2D().StrokeRoundRect((float)(pX+30),(float)(pathY+25),(float)(pW-60),42,9,9,1.f,pathFoc?CA(t.accent,0.5f+pulse*0.3f):CA(t.accent,0.15f));
    {std::string pd=std::string(s.addAppPathBuffer);if(pd.length()>42)pd="..."+pd.substr(pd.length()-39);if(pathFoc)pd+="_";D2D().DrawTextA(pd.c_str(),(float)(pX+48),(float)(pathY+37),13,t.text);}
    if(!s.isAddingWebApp&&pathFoc)D2D().DrawTextA("[Y] Browse",(float)(pX+pW-100),(float)(pathY+3),11,t.accent);
    // Save button
    int saveY=pY+330; bool saveFoc=(s.addAppFocus==3);
    D2D().FillRoundRect((float)(pX+pW/2-70),(float)saveY,140,48,14,14,saveFoc?CA(t.success,0.3f):CA(t.cardBg,0.5f));
    if(saveFoc)D2D().StrokeRoundRect((float)(pX+pW/2-70),(float)saveY,140,48,14,14,1.f,CA(t.success,0.6f+pulse*0.3f));
    float stw=D2D().MeasureTextA("Save App",17); D2D().DrawTextA("Save App",(float)(pX+pW/2)-stw/2,(float)(saveY+15),17,saveFoc?t.success:t.textDim);
    // Input
    if(input.IsBack()){s.currentMode=UIMode::MAIN;PlayBackSound();return;}
    if(input.IsMoveUp()){s.addAppFocus=std::max(0,s.addAppFocus-1);PlayMoveSound();}
    if(input.IsMoveDown()){s.addAppFocus=std::min(3,s.addAppFocus+1);PlayMoveSound();}
    if(s.addAppFocus==0){if(input.IsMoveLeft()||input.IsMoveRight()||input.IsConfirm()){s.isAddingWebApp=!s.isAddingWebApp;PlayMoveSound();}}
    if(s.addAppFocus==1){for(int k=GetCharPressed();k>0;k=GetCharPressed()){int len=(int)strlen(s.addAppNameBuffer);if(len<30&&k>=32&&k<127){s.addAppNameBuffer[len]=(char)k;s.addAppNameBuffer[len+1]=0;}}if(IsKeyPressed(VK_BACK)&&strlen(s.addAppNameBuffer)>0)s.addAppNameBuffer[strlen(s.addAppNameBuffer)-1]=0;}
    if(s.addAppFocus==2){
        if(s.isAddingWebApp){for(int k=GetCharPressed();k>0;k=GetCharPressed()){int len=(int)strlen(s.addAppPathBuffer);if(len<200&&k>=32&&k<127){s.addAppPathBuffer[len]=(char)k;s.addAppPathBuffer[len+1]=0;}}if(IsKeyPressed(VK_BACK)&&strlen(s.addAppPathBuffer)>0)s.addAppPathBuffer[strlen(s.addAppPathBuffer)-1]=0;}
        else if(input.IsChangeArt()||input.IsConfirm()){std::string path=OpenFilePicker(true);if(!path.empty()){strncpy(s.addAppPathBuffer,path.c_str(),255);if(!strlen(s.addAppNameBuffer)){auto nm=fs::path(path).stem().string();strncpy(s.addAppNameBuffer,nm.c_str(),30);}}}
    }
    if(s.addAppFocus==3&&input.IsConfirm()){
        if(strlen(s.addAppNameBuffer)>0&&strlen(s.addAppPathBuffer)>0){
            // For desktop apps, verify the file actually exists before adding.
            // Skipping this check was the crash source: LaunchApp would receive
            // a bad path and pass a garbage lpDirectory to ShellExecuteEx.
            if(!s.isAddingWebApp&&!fs::exists(s.addAppPathBuffer)){
                ShowNotification("Error","File not found — use [Y] Browse to pick it",3);
                PlayErrorSound();
                return;
            }
            CustomApp app; app.name=s.addAppNameBuffer; app.path=s.addAppPathBuffer; app.isWebApp=s.isAddingWebApp;
            int hash=0; for(char c:app.name)hash=hash*31+c;
            app.accentColor=C(80+(hash%175),80+((hash/7)%175),80+((hash/13)%175));
            s.customApps.push_back(app); SaveProfile(); ShowNotification("App Added",app.name,1);
            s.currentMode=UIMode::MAIN; PlayConfirmSound();
        } else {ShowNotification("Error","Name and path required",3);PlayErrorSound();}
    }
}

// ============================================================================
// SHARE TAB
// ============================================================================

void DrawShareTab(int sw,int sh,float contentTop,InputAdapter& input,float dt){
    auto& s=g_app; auto& t=s.theme;
    float time2=GetTime(),pulse=(sinf(time2*4)+1)/2;
    int baseX=60,baseY=(int)contentTop+15,contentW=sw-120;
    int leftW=(int)(contentW*0.48f),rightW=contentW-leftW-30,rightX=baseX+leftW+30;
    // Profile card
    int profileH=180; bool profileFoc=(s.shareSection==0&&!s.inTopBar);
    D2D().FillRoundRect((float)baseX,(float)baseY,(float)leftW,(float)profileH,5,5,CA(C(18,22,32),0.95f));
    D2D().FillGradientH((float)baseX,(float)baseY,5,(float)profileH,t.accent,CA(t.accent,0));
    if(profileFoc)D2D().StrokeRoundRect((float)baseX-2,(float)baseY-2,(float)leftW+4,(float)profileH+4,5,5,1.f,CA(t.accent,0.4f+pulse*0.25f));
    float avatarX=(float)(baseX+80),avatarY=(float)(baseY+profileH/2),avatarR=55.f;
    if(profileFoc)D2D().FillCircle(avatarX,avatarY,avatarR+10,CA(t.accent,0.06f+pulse*0.04f));
    DrawSteamAvatar(avatarX,avatarY,avatarR,true);
    auto& sp=s.steamProfile;
    D2D1_COLOR_F statusCol=(sp.status=="Online")?C(80,220,120):C(140,140,140);
    D2D().FillCircle(avatarX+avatarR-12,avatarY+avatarR-12,14,CA(C(18,22,32),1));
    D2D().FillCircle(avatarX+avatarR-12,avatarY+avatarR-12,10,statusCol);
    int infoX=baseX+120+35,infoY=baseY+35;
    std::string dn2=sp.username.empty()?"Steam User":sp.username;
    if(dn2.length()>16)dn2=dn2.substr(0,14)+"..";
    D2D().DrawTextA(dn2.c_str(),(float)infoX,(float)infoY,24,t.text,(DWRITE_FONT_WEIGHT)700);
    D2D().FillCircle((float)(infoX+5),(float)(infoY+42),5,statusCol);
    D2D().DrawTextA(sp.status.c_str(),(float)(infoX+18),(float)(infoY+36),14,statusCol);
    int statY=infoY+70;
    char g2buf[32],f2buf[32]; snprintf(g2buf,32,"%d",sp.gamesOwned); snprintf(f2buf,32,"%d",sp.friendsCount);
    D2D().DrawTextA(g2buf,(float)infoX,(float)statY,20,t.accent);
    D2D().DrawTextA("Games",(float)infoX,(float)(statY+22),11,CA(t.textDim,0.7f));
    D2D().DrawTextA(f2buf,(float)(infoX+100),(float)statY,20,t.accent);
    D2D().DrawTextA("Friends",(float)(infoX+100),(float)(statY+22),11,CA(t.textDim,0.7f));
    if(profileFoc){float aw=D2D().MeasureTextA("[A] View Profile",12);D2D().DrawTextA("[A] View Profile",(float)infoX,(float)(baseY+profileH-30),12,CA(t.accent,0.7f+pulse*0.3f));(void)aw;}
    // Feature boxes
    int featY=baseY+profileH+20,featH=85,featGap=12;
    struct FeatBox{const char* title,*subtitle,*icon;D2D1_COLOR_F color;};
    FeatBox features[]={{"Quick Resume","Continue where you left",">>",C(100,200,255)},{"Cloud Streaming","Stream your games","~",C(180,100,255)},{"Share Save","Sync your progress","S",C(100,255,180)}};
    for(int i=0;i<3;i++){
        int fy=featY+i*(featH+featGap); bool isFoc=(s.shareSection==1&&s.shareFocusIdx==i&&!s.inTopBar);
        D2D().FillRoundRect((float)baseX,(float)fy,(float)leftW,(float)featH,6,6,isFoc?CA(features[i].color,0.12f):CA(t.cardBg,0.9f));
        D2D().FillRoundRect((float)baseX,(float)fy,4,(float)featH,2,2,isFoc?features[i].color:CA(features[i].color,0.4f));
        float iconX3=(float)(baseX+55),iconY3=(float)(fy+featH/2);
        if(isFoc){D2D().FillCircle(iconX3,iconY3,32,CA(features[i].color,0.1f+pulse*0.08f));}
        D2D().FillCircle(iconX3,iconY3,26,CA(features[i].color,isFoc?0.25f:0.12f));
        D2D().StrokeCircle(iconX3,iconY3,26,1.f,CA(features[i].color,isFoc?0.7f:0.35f));
        float iw3=D2D().MeasureTextA(features[i].icon,18);
        D2D().DrawTextA(features[i].icon,iconX3-iw3/2,iconY3-9,18,isFoc?features[i].color:CA(features[i].color,0.6f));
        D2D().DrawTextA(features[i].title,(float)(baseX+100),(float)(fy+22),18,isFoc?t.text:CA(t.text,0.85f));
        D2D().DrawTextA(features[i].subtitle,(float)(baseX+100),(float)(fy+48),12,CA(t.textDim,0.65f));
        if(isFoc){float aw=D2D().MeasureTextA(">",22);D2D().DrawTextA(">",(float)(baseX+leftW-35),(float)(fy+32),22,CA(features[i].color,0.6f+pulse*0.4f));(void)aw;
            D2D().StrokeRoundRect((float)baseX-2,(float)fy-2,(float)leftW+4,(float)featH+4,6,6,1.f,CA(features[i].color,0.45f+pulse*0.3f));}
    }
    // Community Hub
    int hubY=baseY,hubH=280; bool hubFoc=(s.shareSection==2&&!s.inTopBar);
    D2D().FillRoundRect((float)rightX,(float)hubY,(float)rightW,(float)hubH,5,5,CA(C(18,22,32),0.95f));
    D2D().DrawTextA("COMMUNITY HUB",(float)(rightX+20),(float)(hubY+15),18,t.text,(DWRITE_FONT_WEIGHT)700);
    D2D().FillRect((float)(rightX+20),(float)(hubY+40),140,2,CA(t.accent,0.4f));
    int slX=rightX+15,slY=hubY+55,slW=rightW-30,slH=hubH-100;
    auto& hs2=s.hubSlider;
    if(hs2.artCovers[hs2.currentSlide].Valid()){
        D2D().DrawBitmap(hs2.artCovers[hs2.currentSlide],(float)slX,(float)slY,(float)slW,(float)slH,hs2.transitionProgress);
    } else {
        D2D().FillRoundRect((float)slX,(float)slY,(float)slW,(float)slH,5,5,CA(t.cardBg,0.5f));
        D2D().DrawTextA("Art Cover",(float)(slX+slW/2-40),(float)(slY+slH/2-10),16,CA(t.textDim,0.4f));
    }
    int dotY=hubY+hubH-30;
    for(int i=0;i<3;i++){float dx=(float)(rightX+rightW/2-25+i*25);D2D().FillCircle(dx,(float)dotY,i==hs2.currentSlide?5.f:3.f,i==hs2.currentSlide?t.accent:CA(t.accent,0.3f));}
    if(hubFoc){D2D().StrokeRoundRect((float)rightX-2,(float)hubY-2,(float)rightW+4,(float)hubH+4,5,5,1.f,CA(t.accent,0.45f+pulse*0.3f));D2D().DrawTextA("[A] Open Hub  [>] Next",(float)(rightX+20),(float)(hubY+hubH-28),11,CA(t.accent,0.7f));}
    // Platforms
    int platY=hubY+hubH+20; D2D().DrawTextA("PLATFORMS",(float)(rightX+20),(float)platY,16,t.text,(DWRITE_FONT_WEIGHT)700);
    D2D().FillRect((float)(rightX+20),(float)(platY+22),90,2,CA(t.accent,0.3f));
    int platStartY=platY+35,platItemH=55,maxPlats=std::min((int)s.platformConnections.size(),4);
    for(int i=0;i<maxPlats;i++){
        auto& plat=s.platformConnections[i]; int py2=platStartY+i*(platItemH+8);
        bool isFoc=(s.shareSection==3&&s.shareFocusIdx==i&&!s.inTopBar);
        D2D().FillRoundRect((float)rightX,(float)py2,(float)rightW,(float)platItemH,5,5,isFoc?CA(plat.accentColor,0.12f):CA(t.cardBg,0.85f));
        D2D1_COLOR_F dotCol=plat.isConnected?C(80,220,120):C(200,160,80);
        D2D().FillCircle((float)(rightX+22),(float)(py2+platItemH/2),6,dotCol);
        D2D().FillCircle((float)(rightX+58),(float)(py2+platItemH/2),18,CA(plat.accentColor,isFoc?0.25f:0.12f));
        float piw=D2D().MeasureTextA(plat.icon.c_str(),14);
        D2D().DrawTextA(plat.icon.c_str(),(float)(rightX+58)-piw/2,(float)(py2+platItemH/2-7),14,plat.accentColor);
        std::string pdn=plat.name; if(pdn.length()>10)pdn=pdn.substr(0,8)+"..";
        D2D().DrawTextA(pdn.c_str(),(float)(rightX+90),(float)(py2+12),15,isFoc?t.text:CA(t.text,0.85f));
        D2D().DrawTextA(plat.statusText.c_str(),(float)(rightX+90),(float)(py2+32),11,CA(dotCol,0.8f));
        if(isFoc)D2D().StrokeRoundRect((float)rightX-2,(float)py2-2,(float)rightW+4,(float)platItemH+4,5,5,1.f,CA(plat.accentColor,0.45f+pulse*0.3f));
    }
    // Recording
    if(s.isRecording){
        s.recordingTime+=dt;
        float rp=(sinf(time2*6)+1)/2;
        int rx3=baseX+leftW-120,ry3=baseY+profileH+20;
        D2D().FillRoundRect((float)rx3,(float)ry3,110,38,19,19,CA(RED_COL,0.15f+rp*0.1f));
        D2D().FillCircle((float)(rx3+20),(float)(ry3+19),7,CA(RED_COL,0.7f+rp*0.3f));
        char rt[32]; snprintf(rt,32,"%.0f s",s.recordingTime);
        D2D().DrawTextA(rt,(float)(rx3+36),(float)(ry3+10),16,RED_COL);
    }
    // Share input
    if(!s.inTopBar){
        if(input.IsMoveUp()){ if(s.shareSection==0){s.inTopBar=true;} else if(s.shareSection==1){if(s.shareFocusIdx>0)s.shareFocusIdx--;else s.shareSection=0;} else if(s.shareSection==2){s.shareSection=0;} else if(s.shareSection==3){if(s.shareFocusIdx>0)s.shareFocusIdx--;else s.shareSection=2;} PlayMoveSound(); }
        if(input.IsMoveDown()){ if(s.shareSection==0){s.shareSection=1;s.shareFocusIdx=0;} else if(s.shareSection==1){if(s.shareFocusIdx<2)s.shareFocusIdx++;} else if(s.shareSection==2){s.shareSection=3;s.shareFocusIdx=0;} else if(s.shareSection==3){if(s.shareFocusIdx<maxPlats-1)s.shareFocusIdx++;} PlayMoveSound(); }
        if(input.IsMoveLeft()){ if(s.shareSection==2||s.shareSection==3){s.shareSection=(s.shareSection==3)?1:0;s.shareFocusIdx=0;} PlayMoveSound(); }
        if(input.IsMoveRight()){ if(s.shareSection==0||s.shareSection==1){s.shareSection=(s.shareSection==0)?2:3;s.shareFocusIdx=0;} PlayMoveSound(); }
        if(input.IsConfirm()){
            if(s.shareSection==2){if(hs2.artCovers[(hs2.currentSlide+1)%3].Valid()){hs2.currentSlide=(hs2.currentSlide+1)%3;hs2.slideTimer=0;hs2.transitionProgress=0;}}
            if(s.shareSection==3&&s.shareFocusIdx<maxPlats){auto& pl2=s.platformConnections[s.shareFocusIdx];if(!pl2.connectUrl.empty())LaunchApp(pl2.connectUrl,true);}
            if(s.shareSection==0){
                ShellExecuteA(nullptr,"open","https://store.steampowered.com/",nullptr,nullptr,SW_SHOWNORMAL);
            }
            PlayConfirmSound();
        }
        if(input.IsDeletePressed()&&s.shareSection==1){s.isRecording=!s.isRecording;if(s.isRecording)s.recordingTime=0;PlayConfirmSound();}
    }
    (void)dt;
}

// ============================================================================
// PROFILE EDIT OVERLAY
// ============================================================================

void HandleProfileEditOverlay(int sw,int sh,InputAdapter& input,float dt){
    auto& s=g_app; auto& p=s.profile; auto& t=s.theme;
    s.profileEditSlide=LerpF(s.profileEditSlide,1.f,0.12f);
    float sl=s.profileEditSlide,ti=GetTime(); const int N=8;
    if(!s.editingUsername){
        if(input.IsMoveUp()){s.profileEditFocus=(s.profileEditFocus-1+N)%N;PlayMoveSound();}
        if(input.IsMoveDown()){s.profileEditFocus=(s.profileEditFocus+1)%N;PlayMoveSound();}
        if(input.IsBack()){s.currentMode=UIMode::MAIN;s.profileEditSlide=0;PlayBackSound();return;}
        if(input.IsConfirm()){PlayConfirmSound();
            switch(s.profileEditFocus){
                case 0: s.editingUsername=true;strncpy(s.usernameBuffer,p.username.c_str(),63);break;
                case 1:{auto f=OpenFilePicker(false);if(!f.empty()){auto d=GetFullPath("profile\\avatar.png");
                    try{fs::copy_file(f,d,fs::copy_options::overwrite_existing);}catch(...){}
                    p.avatarPath="profile\\avatar.png";
                    if(p.hasAvatar)D2D().UnloadBitmap(p.avatar);
                    p.avatar=D2D().LoadBitmapA(d.c_str());p.hasAvatar=p.avatar.Valid();
                    ShowNotification("Avatar Updated","",1);}}break;
                case 2: s.currentMode=UIMode::THEME_SELECT;break;
                case 5: p.soundEnabled=!p.soundEnabled;g_audio.soundEnabled=p.soundEnabled;break;
                case 6: p.musicEnabled=!p.musicEnabled;g_audio.musicEnabled=p.musicEnabled;if(!p.musicEnabled)g_audio.StopMusic();break;
                case 7: SaveProfile();ShowNotification("Saved","",1);s.currentMode=UIMode::MAIN;s.profileEditSlide=0;break;
            }
        }
        if(s.profileEditFocus==3){if(input.IsMoveLeft()){p.sfxVolume=Clamp(p.sfxVolume-0.1f,0.f,1.f);g_audio.sfxVolume=p.sfxVolume;PlayMoveSound();}if(input.IsMoveRight()){p.sfxVolume=Clamp(p.sfxVolume+0.1f,0.f,1.f);g_audio.sfxVolume=p.sfxVolume;PlayMoveSound();}}
        if(s.profileEditFocus==4){if(input.IsMoveLeft()){p.musicVolume=Clamp(p.musicVolume-0.1f,0.f,1.f);g_audio.musicVolume=p.musicVolume;PlayMoveSound();}if(input.IsMoveRight()){p.musicVolume=Clamp(p.musicVolume+0.1f,0.f,1.f);g_audio.musicVolume=p.musicVolume;PlayMoveSound();}}
    } else {
        for(int k=GetCharPressed();k>0;k=GetCharPressed()){int l=(int)strlen(s.usernameBuffer);if(l<20&&k>=32&&k<127){s.usernameBuffer[l]=(char)k;s.usernameBuffer[l+1]=0;}}
        if(IsKeyPressed(VK_BACK)&&strlen(s.usernameBuffer)>0)s.usernameBuffer[strlen(s.usernameBuffer)-1]=0;
        if(IsKeyPressed(VK_RETURN)){p.username=s.usernameBuffer;s.editingUsername=false;ShowNotification("Username Changed",p.username,1);}
        if(IsKeyPressed(VK_ESCAPE))s.editingUsername=false;
    }
    D2D().FillRect(0,0,(float)sw,(float)sh,CA(BLACK_COL,0.85f*sl));
    float pw=550,ph2=580,px2=(sw-pw)/2,py2=(sh-ph2)/2+(1-sl)*50;
    D2D().FillRoundRect(px2,py2,pw,ph2,8,8,CA(t.secondary,0.98f));
    D2D().StrokeRoundRect(px2,py2,pw,ph2,8,8,1.f,CA(t.accent,0.3f));
    D2D().DrawTextA("PROFILE SETTINGS",(float)(px2+30),(float)(py2+25),28,t.text,(DWRITE_FONT_WEIGHT)700);
    D2D().FillRect((float)(px2+30),(float)(py2+60),150,3,t.accent);
    DrawCircularAvatar(px2+pw-80,py2+70,45,p);
    const char* lb[]={"Username","Avatar","Theme","Sound Volume","Music Volume","Sound Effects","Background Music","Save Changes"};
    float oy=py2+100,oh=52,gap=8;
    for(int i=0;i<N;i++){
        float bx3=px2+20,by3=oy+i*(oh+gap);
        bool f=(s.profileEditFocus==i&&!s.editingUsername);
        D2D().FillRoundRect(bx3,by3,pw-40,oh,7,7,f?CA(t.accent,0.15f):CA(t.cardBg,0.5f));
        if(f)D2D().StrokeRoundRect(bx3,by3,pw-40,oh,7,7,1.f,CA(t.accent,0.4f+(sinf(ti*4)+1)/2*0.3f));
        D2D().DrawTextA(lb[i],(float)(bx3+20),(float)(by3+16),18,f?t.text:t.textDim);
        float rx3=bx3+(pw-40);
        switch(i){
            case 0:{std::string tmp=(s.editingUsername?std::string(s.usernameBuffer)+"_":p.username);D2D().DrawTextA(tmp.c_str(),(float)(rx3-200),(float)(by3+16),18,s.editingUsername?t.accent:t.textDim);}break;
            case 2:D2D().DrawTextA(ALL_THEMES[s.currentThemeIdx].name.c_str(),(float)(rx3-180),(float)(by3+16),16,t.accent);break;
            case 3:case 4:{float v=(i==3)?p.sfxVolume:p.musicVolume;D2D().FillRect((float)(rx3-180),(float)(by3+20),120,12,CA(t.cardBg,0.8f));D2D().FillRect((float)(rx3-180),(float)(by3+20),120*v,12,t.accent);char pct[16];snprintf(pct,16,"%d%%",(int)(v*100));D2D().DrawTextA(pct,(float)(rx3-50),(float)(by3+16),16,t.textDim);}break;
            case 5:case 6:{bool on=(i==5)?p.soundEnabled:p.musicEnabled;D2D().DrawTextA(on?"ON":"OFF",(float)(rx3-60),(float)(by3+16),18,on?t.success:t.danger);}break;
            case 7:D2D().DrawTextA(">",(float)(rx3-40),(float)(by3+14),22,t.success);break;
        }
    }
    (void)dt;
}

// ============================================================================
// THEME SELECT OVERLAY
// ============================================================================

void HandleThemeSelectOverlay(int sw,int sh,InputAdapter& input,float dt){
    auto& s=g_app; s.themeSelectSlide=LerpF(s.themeSelectSlide,1.f,0.12f);
    float sl=s.themeSelectSlide,ti=GetTime(); int cnt=(int)ALL_THEMES.size(),cols2=2;
    if(input.IsMoveUp()){s.themeSelectFocus=std::max(0,s.themeSelectFocus-cols2);PlayMoveSound();}
    if(input.IsMoveDown()){s.themeSelectFocus=std::min(cnt-1,s.themeSelectFocus+cols2);PlayMoveSound();}
    if(input.IsMoveLeft()){s.themeSelectFocus=std::max(0,s.themeSelectFocus-1);PlayMoveSound();}
    if(input.IsMoveRight()){s.themeSelectFocus=std::min(cnt-1,s.themeSelectFocus+1);PlayMoveSound();}
    s.SetTheme(s.themeSelectFocus);
    if(input.IsBack()){s.SetTheme(s.profile.themeIndex);s.currentMode=UIMode::PROFILE_EDIT;s.themeSelectSlide=0;PlayBackSound();return;}
    if(input.IsConfirm()){s.profile.themeIndex=s.themeSelectFocus;s.currentMode=UIMode::PROFILE_EDIT;s.themeSelectSlide=0;ShowNotification("Theme Applied",ALL_THEMES[s.themeSelectFocus].name,1);PlayConfirmSound();return;}
    D2D().FillRect(0,0,(float)sw,(float)sh,CA(BLACK_COL,0.9f*sl));
    float tw3=D2D().MeasureTextA("SELECT THEME",36,(DWRITE_FONT_WEIGHT)700);
    D2D().DrawTextA("SELECT THEME",(float)(sw/2)-tw3/2,60,36,CA(s.theme.text,sl),(DWRITE_FONT_WEIGHT)700);
    D2D().FillRect((float)(sw/2-80),105,160,3,CA(s.theme.accent,sl));
    float cw=280,ch=120,gapVal=20,sx3=(sw-(cols2*cw+(cols2-1)*gapVal))/2,sy3=150;
    for(int i=0;i<cnt;i++){
        float x=sx3+(i%cols2)*(cw+gapVal),y=sy3+(i/cols2)*(ch+gapVal); auto& th=ALL_THEMES[i];
        D2D().FillRoundRect(x,y,cw,ch,8,8,CA(th.secondary,0.95f*sl));
        D2D().FillRect(x+20,y+50,40,25,CA(th.primary,sl));D2D().FillRect(x+65,y+50,40,25,CA(th.accent,sl));D2D().FillRect(x+110,y+50,40,25,CA(th.accentAlt,sl));
        D2D().DrawTextA(th.name.c_str(),x+20,y+18,18,CA(th.text,sl));
        if(i==s.profile.themeIndex)D2D().DrawTextA("*",x+cw-35,y+15,24,CA(th.success,sl));
        if(i==s.themeSelectFocus)D2D().StrokeRoundRect(x,y,cw,ch,8,8,2.f,CA(th.accent,(0.5f+(sinf(ti*4)+1)/2*0.5f)*sl));
    }
    (void)dt;
}

// ============================================================================
// TASK SWITCHER OVERLAY
// ============================================================================

bool HandleTaskSwitcherOverlay(int sw,int sh,InputAdapter& input,float dt){
    auto& s=g_app; auto& t=s.theme;
    s.taskAnimTime+=dt; s.taskSlideIn=LerpF(s.taskSlideIn,1.f,0.12f);
    float sl=s.taskSlideIn; int cols2=Clamp((sw-100)/350,2,4),tc=(int)s.tasks.size();
    if(input.IsMoveLeft()){s.taskFocusIdx=std::max(0,s.taskFocusIdx-1);PlayMoveSound();}
    if(input.IsMoveRight()){s.taskFocusIdx=std::min(tc-1,s.taskFocusIdx+1);PlayMoveSound();}
    if(input.IsMoveUp()){s.taskFocusIdx=std::max(0,s.taskFocusIdx-cols2);PlayMoveSound();}
    if(input.IsMoveDown()){s.taskFocusIdx=std::min(tc-1,s.taskFocusIdx+cols2);PlayMoveSound();}
    if(input.IsConfirm()&&!s.tasks.empty()){SwitchToTask(s.taskFocusIdx);s.currentMode=UIMode::MAIN;s.taskSlideIn=0;PlayConfirmSound();return true;}
    if(input.IsBack()){s.currentMode=UIMode::MAIN;s.taskSlideIn=0;PlayBackSound();return true;}
    if(input.IsDeletePressed()&&s.taskFocusIdx<tc){PostMessage(s.tasks[s.taskFocusIdx].hwnd,WM_CLOSE,0,0);Sleep(100);RefreshTaskList();s.taskFocusIdx=Clamp(s.taskFocusIdx,0,std::max(0,(int)s.tasks.size()-1));if(s.tasks.empty()){s.currentMode=UIMode::MAIN;s.taskSlideIn=0;return true;}}
    D2D().FillRect(0,0,(float)sw,(float)sh,CA(BLACK_COL,0.88f*sl));
    float htw=D2D().MeasureTextA("RUNNING APPLICATIONS",40,(DWRITE_FONT_WEIGHT)700);
    D2D().DrawTextA("RUNNING APPLICATIONS",(float)(sw/2)-htw/2,(float)(60-(1-sl)*50),40,CA(t.text,sl),(DWRITE_FONT_WEIGHT)700);
    if(s.tasks.empty()){float ew=D2D().MeasureTextA("No applications running",24);D2D().DrawTextA("No applications running",(float)(sw/2)-ew/2,(float)(sh/2),24,CA(t.textDim,sl));}
    else{
        float cw=320,ch=180,gapVal=25,gw=cols2*cw+(cols2-1)*gapVal,stX=(sw-gw)/2,stY=150;
        int mx=std::min(tc,12);
        for(int i=0;i<mx;i++){
            int row=i/cols2,col=i%cols2;
            float cx3=stX+col*(cw+gapVal),cy3=stY+row*(ch+gapVal);
            bool sel=(i==s.taskFocusIdx); float sc2=sel?1.03f:1.f,sw4=cw*sc2,sh4=ch*sc2;
            float sx4=cx3-(sw4-cw)/2,sy4=cy3-(sh4-ch)/2;
            D2D().FillRoundRect(sx4+6,sy4+8,sw4,sh4,6,6,CA(BLACK_COL,0.4f*sl));
            D2D().FillRoundRect(sx4,sy4,sw4,sh4,6,6,CA(sel?CA(t.cardBg,1.2f):t.cardBg,sl));
            if(sel){float pp=(sinf(s.taskAnimTime*4.5f)+1)/2;D2D().StrokeRoundRect(sx4-2,sy4-2,sw4+4,sh4+4,6,6,3,CA(t.accent,(0.5f+pp*0.5f)*sl));}
            auto& tk=s.tasks[i]; char ini[2]={tk.name.empty()?'?':(char)toupper(tk.name[0]),0};
            D2D().FillRoundRect(sx4+20,sy4+25,60,60,5,5,CA(t.secondary,sl));
            float initw=D2D().MeasureTextA(ini,30,(DWRITE_FONT_WEIGHT)700); D2D().DrawTextA(ini,sx4+50-initw/2,sy4+40,30,CA(sel?t.accent:t.text,sl*0.8f),(DWRITE_FONT_WEIGHT)700);
            std::string nm=tk.name; if(nm.size()>4&&nm.substr(nm.size()-4)==".exe")nm.resize(nm.size()-4);
            if(nm.size()>18)nm=nm.substr(0,16)+"..";
            D2D().DrawTextA(nm.c_str(),sx4+95,sy4+35,18,CA(t.text,sl));
            std::string wt=tk.windowTitle; if(wt.size()>28)wt=wt.substr(0,26)+"..";
            D2D().DrawTextA(wt.c_str(),sx4+95,sy4+60,12,CA(t.textDim,sl*0.8f));
            D2D().FillCircle(sx4+30,sy4+105,6,CA(t.success,sl));
            D2D().DrawTextA("Running",sx4+45,sy4+97,14,CA(t.success,sl*0.9f));
        }
    }
    return false;
}

// ============================================================================
// SHELL MENU OVERLAY
// ============================================================================

ShellAction HandleShellMenuOverlay(int sw,int sh,InputAdapter& input,float dt){
    auto& s=g_app; auto& t=s.theme;
    s.shellMenuSlide=LerpF(s.shellMenuSlide,1.f,0.12f); float ti=GetTime();
    struct Item{const char* l,*d;D2D1_COLOR_F c;};
    Item it[]={{"File Explorer","Open Explorer",t.accent},{"Keyboard","On-screen keyboard",ORANGE_COL},{"Settings","System settings",PURPLE_COL},{"Task Manager","View processes",t.success},{"Restart Q-Shell","Restart interface",YELLOW_COL},{"Exit Shell","Return to Explorer",t.danger},{"Power","Shutdown/Restart/Sleep",GRAY_COL}};
    constexpr int C2=7;
    if(input.IsMoveUp()){s.shellMenuFocus=(s.shellMenuFocus-1+C2)%C2;PlayMoveSound();}
    if(input.IsMoveDown()){s.shellMenuFocus=(s.shellMenuFocus+1)%C2;PlayMoveSound();}
    if(input.IsBack()||input.IsMenu()){s.currentMode=UIMode::MAIN;s.shellMenuSlide=0;PlayBackSound();return ShellAction::NONE;}
    if(input.IsConfirm()){s.currentMode=UIMode::MAIN;s.shellMenuSlide=0;PlayConfirmSound();return(ShellAction)(s.shellMenuFocus+1);}
    float sl=s.shellMenuSlide,mw=450,mh=90+C2*60,mx3=sw-(mw+50)*sl,my3=(sh-mh)/2;
    D2D().FillRect(0,0,(float)sw,(float)sh,CA(BLACK_COL,0.7f*sl));
    D2D().FillRoundRect(mx3+8,my3+10,mw,mh,7,7,CA(BLACK_COL,0.5f));
    D2D().FillRoundRect(mx3,my3,mw,mh,7,7,CA(t.secondary,0.98f));
    D2D().DrawTextA("SHELL MENU",(float)(mx3+28),(float)(my3+22),28,t.text,(DWRITE_FONT_WEIGHT)700);
    for(int i=0;i<C2;i++){
        float bx3=mx3+18,by3=my3+78+i*58; bool f=(s.shellMenuFocus==i);
        D2D().FillRoundRect(bx3,by3,mw-36,52,8,8,f?CA(it[i].c,0.15f):CA(t.cardBg,0.3f));
        if(f)D2D().StrokeRoundRect(bx3,by3,mw-36,52,8,8,1.f,CA(it[i].c,0.4f+(sinf(ti*4)+1)/2*0.3f));
        D2D().DrawTextA(it[i].l,(float)(bx3+18),(float)(by3+9),17,f?t.text:t.textDim);
        D2D().DrawTextA(it[i].d,(float)(bx3+18),(float)(by3+31),11,CA(t.textDim,0.6f));
    }
    (void)dt; return ShellAction::NONE;
}

// ============================================================================
// POWER MENU OVERLAY
// ============================================================================

PowerChoice HandlePowerMenuOverlay(int sw,int sh,InputAdapter& input,float dt){
    auto& s=g_app; auto& t=s.theme;
    s.powerMenuSlide=LerpF(s.powerMenuSlide,1.f,0.15f); float ti=GetTime();
    const char* lb[]={"Restart","Shutdown","Sleep","Cancel"};
    D2D1_COLOR_F cl[]={ORANGE_COL,t.danger,BLUE_COL,GRAY_COL};
    if(input.IsMoveLeft()){s.powerMenuFocus=(s.powerMenuFocus-1+4)%4;PlayMoveSound();}
    if(input.IsMoveRight()){s.powerMenuFocus=(s.powerMenuFocus+1)%4;PlayMoveSound();}
    if(input.IsBack()){s.currentMode=UIMode::MAIN;s.powerMenuSlide=0;PlayBackSound();return PowerChoice::CANCEL;}
    if(input.IsConfirm()){s.currentMode=UIMode::MAIN;s.powerMenuSlide=0;PlayConfirmSound();return(PowerChoice)s.powerMenuFocus;}
    float sl=s.powerMenuSlide,bw=160,bh=110,gapVal=30,stX=(sw-(bw*4+gapVal*3))/2,by2=(float)(sh/2-20);
    D2D().FillRect(0,0,(float)sw,(float)sh,CA(BLACK_COL,0.85f*sl));
    float ptw=D2D().MeasureTextA("POWER OPTIONS",36,(DWRITE_FONT_WEIGHT)700);
    D2D().DrawTextA("POWER OPTIONS",(float)(sw/2)-ptw/2,(float)(sh/2-120),36,CA(t.text,sl),(DWRITE_FONT_WEIGHT)700);
    for(int i=0;i<4;i++){
        float x=stX+i*(bw+gapVal); bool sel=(i==s.powerMenuFocus);
        D2D().FillRoundRect(x,by2,bw,bh,13,13,CA(sel?cl[i]:t.cardBg,sel?0.2f:0.5f));
        const char* ic[]={"R","S","Z","X"};
        float iw4=D2D().MeasureTextA(ic[i],40,(DWRITE_FONT_WEIGHT)700);D2D().DrawTextA(ic[i],x+(bw-iw4)/2,by2+25,40,CA(cl[i],sel?1.f:0.5f),(DWRITE_FONT_WEIGHT)700);
        float lw=D2D().MeasureTextA(lb[i],18);D2D().DrawTextA(lb[i],x+(bw-lw)/2,by2+75,18,CA(t.text,sel?1.f:0.6f));
        if(sel)D2D().StrokeRoundRect(x,by2,bw,bh,13,13,2.f,CA(cl[i],0.5f+(sinf(ti*4)+1)/2*0.35f));
    }
    (void)dt; return PowerChoice::NONE;
}

// ============================================================================
// THEME SONG HELPERS
// ============================================================================

std::string OpenMusicFilePicker(){
    char b[MAX_PATH]={}; OPENFILENAMEA o={};
    o.lStructSize=sizeof(o);o.lpstrFile=b;o.nMaxFile=sizeof(b);
    o.lpstrFilter="Music Files\0*.MP3;*.OGG;*.WAV;*.FLAC\0All Files\0*.*\0";
    o.Flags=OFN_PATHMUSTEXIST|OFN_FILEMUSTEXIST;
    return GetOpenFileNameA(&o)?std::string(b):"";
}

void UploadThemeSong(){
    auto f=OpenMusicFilePicker(); if(f.empty())return;
    auto d=GetFullPath("profile\\sounds"); try{fs::create_directories(d);}catch(...){}
    auto ext=fs::path(f).extension().string(); std::transform(ext.begin(),ext.end(),ext.begin(),::tolower);
    auto dst=d+"\\music"+ext;
    try{
        fs::copy_file(f,dst,fs::copy_options::overwrite_existing);
        ma_sound_uninit(&g_audio.bgMusic);
        if(ma_sound_init_from_file(&g_audio.engine,dst.c_str(),MA_SOUND_FLAG_STREAM,nullptr,nullptr,&g_audio.bgMusic)==MA_SUCCESS){
            ma_sound_set_looping(&g_audio.bgMusic,MA_TRUE);
            if(g_audio.musicEnabled)ma_sound_start(&g_audio.bgMusic);
            ShowNotification("Theme Song","Updated!",1,3);
        } else ShowNotification("Error","Failed to load",3,3);
    }catch(...){ShowNotification("Error","Failed to copy",3,3);}
}

void RemoveThemeSong(){
    auto d=GetFullPath("profile\\sounds"); bool rm=false;
    for(auto e:{std::string("\\music.mp3"),std::string("\\music.ogg"),std::string("\\music.wav"),std::string("\\music.flac")}){
        auto p=d+e; if(fs::exists(p))try{fs::remove(p);rm=true;}catch(...){}
    }
    ma_sound_stop(&g_audio.bgMusic); ma_sound_uninit(&g_audio.bgMusic); g_audio.bgMusic={};
    ShowNotification("Theme Song",rm?"Removed":"None to remove",1,3);
}

// ============================================================================
// STARTUP DIALOGS  (Win32 modal — no raylib window)
// ============================================================================

// Simple Win32 dialog helper: creates a temp fullscreen D2D window, runs a loop
// until the user makes a choice, then destroys the window.
// We reuse the same D2DRenderer singleton; we just create a second HWND and
// re-init the render target on it, then restore after.

// Dialog window proc — does NOT post WM_QUIT on destroy.
// PostQuitMessage would poison the main message queue and cause
// WinMain's PeekMessage loop to exit prematurely after the dialog closes.
static LRESULT CALLBACK DialogWndProc(HWND hw,UINT msg,WPARAM wp,LPARAM lp){
    if(msg==WM_DESTROY){return 0;}  // deliberately no PostQuitMessage
    if(msg==WM_CHAR){PushChar((int)wp);return 0;}
    return DefWindowProcA(hw,msg,wp,lp);
}

static HWND CreateDialogWindow(int w,int h,const char* title){
    static bool reg=false;
    if(!reg){WNDCLASSEXA wc={sizeof(wc)};wc.lpfnWndProc=DialogWndProc;wc.hInstance=GetModuleHandle(nullptr);wc.lpszClassName="QShellDlg";wc.hCursor=LoadCursor(nullptr,IDC_ARROW);RegisterClassExA(&wc);reg=true;}
    int sx=(GetSystemMetrics(SM_CXSCREEN)-w)/2,sy=(GetSystemMetrics(SM_CYSCREEN)-h)/2;
    HWND hw=CreateWindowExA(WS_EX_TOPMOST,"QShellDlg",title,WS_POPUP|WS_VISIBLE,sx,sy,w,h,nullptr,nullptr,GetModuleHandle(nullptr),nullptr);
    return hw;
}

StartupChoice ShowLaunchDialog(){
    int W=550,H=400; bool isShell=CheckIfShellMode();
    HWND hw=CreateDialogWindow(W,H,"Q-Shell");
    if(!hw) return StartupChoice::NONE;
    D2D().Init(hw,W,H);
    InputAdapter input; int sel=0; float an=0; StartupChoice res=StartupChoice::NORMAL_APP;
    MSG msg; bool running2=true;
    while(running2){
        while(PeekMessage(&msg,nullptr,0,0,PM_REMOVE)){
            TranslateMessage(&msg); DispatchMessage(&msg);
        }
        TickTimer(); UpdateKeyStates(); input.Update(); an+=g_dt;
        if(input.IsMoveUp())   sel=(sel+2)%3;
        if(input.IsMoveDown()) sel=(sel+1)%3;
        if(input.IsConfirm()){
            if(sel==0)      res=StartupChoice::NORMAL_APP;
            else if(sel==1) res=isShell?StartupChoice::EXIT_SHELL:StartupChoice::SHELL_MODE;
            else            res=StartupChoice::NONE;
            running2=false;
        }
        if(input.IsBack()){ res=StartupChoice::NONE; running2=false; }
        if(!running2) break;  // don't render after a choice is made
        D2D().BeginFrame(C(12,14,20));
        D2D().DrawTextA("Q-SHELL",40,35,48,WHITE_COL,(DWRITE_FONT_WEIGHT)700);
        D2D().DrawTextA("Gaming Console Interface",40,90,16,GRAY_COL);
        const char* opts[]={"Normal Application",isShell?"Exit Shell Mode":"Shell Mode","Cancel"};
        D2D1_COLOR_F cols3[]={C(135,206,235),isShell?ORANGE_COL:GREEN_COL,GRAY_COL};
        for(int i=0;i<3;i++){
            bool f=(sel==i);
            D2D().FillRoundRect(40,(float)(145+i*70),(float)(W-80),60,7,7,f?CA(cols3[i],0.12f):CA(WHITE_COL,0.02f));
            if(f)D2D().StrokeRoundRect(40,(float)(145+i*70),(float)(W-80),60,7,7,1.f,CA(cols3[i],0.5f));
            D2D().DrawTextA(opts[i],60,(float)(163+i*70),20,f?cols3[i]:CA(WHITE_COL,0.7f));
        }
        D2D().EndFrame();
    }
    D2D().Shutdown();
    DestroyWindow(hw);
    // Flush any messages posted by DestroyWindow (e.g. WM_NCDESTROY) so they
    // don't end up in the main loop's queue.
    while(PeekMessage(&msg,nullptr,0,0,PM_REMOVE)){}
    return res;
}

bool ShowExitShellConfirmation(){
    int W=480,H=260; HWND hw=CreateDialogWindow(W,H,"Exit Shell");
    if(!hw) return false;
    D2D().Init(hw,W,H);
    InputAdapter input; int sel=0; bool res=false,done=false;
    MSG msg;
    while(!done){
        while(PeekMessage(&msg,nullptr,0,0,PM_REMOVE)){
            TranslateMessage(&msg); DispatchMessage(&msg);
        }
        TickTimer(); UpdateKeyStates(); input.Update();
        if(input.IsMoveLeft()) sel=0;
        if(input.IsMoveRight()) sel=1;
        if(input.IsConfirm()){ res=(sel==0); done=true; }
        if(input.IsBack()) done=true;
        if(done) break;  // don't render after choice
        D2D().BeginFrame(C(18,20,28));
        D2D().DrawTextA("Exit Shell Mode?",40,35,26,WHITE_COL,(DWRITE_FONT_WEIGHT)700);
        D2D().DrawTextA("This will restore Explorer and restart.",40,75,14,GRAY_COL);
        D2D().FillRoundRect(40,160,190,55,12,12,sel==0?CA(GREEN_COL,0.25f):CA(WHITE_COL,0.05f));
        D2D().FillRoundRect(250,160,190,55,12,12,sel==1?CA(RED_COL,0.25f):CA(WHITE_COL,0.05f));
        D2D().DrawTextA("Yes, Exit",95,178,18,sel==0?GREEN_COL:WHITE_COL);
        D2D().DrawTextA("Cancel",315,178,18,sel==1?RED_COL:CA(WHITE_COL,0.7f));
        D2D().EndFrame();
    }
    D2D().Shutdown();
    DestroyWindow(hw);
    while(PeekMessage(&msg,nullptr,0,0,PM_REMOVE)){}
    return res;
}

void ShowBootScreen(){
    int sw2=GetSystemMetrics(SM_CXSCREEN),sh2=GetSystemMetrics(SM_CYSCREEN);
    if(sw2<=0)sw2=1920; if(sh2<=0)sh2=1080;
    HWND hw=CreateDialogWindow(sw2,sh2,"Q-Shell Boot");
    SetWindowPos(hw,HWND_TOPMOST,0,0,sw2,sh2,SWP_SHOWWINDOW);
    D2D().Init(hw,sw2,sh2); SetCursor(nullptr);
    float el=0,dur=3.f; MSG msg; bool running2=true;
    while(running2&&el<dur){
        while(PeekMessage(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessage(&msg);if(msg.message==WM_QUIT)running2=false;}
        TickTimer(); el+=g_dt; UpdateKeyStates();
        if(IsKeyPressed(VK_RETURN)||IsKeyPressed(VK_SPACE)||IsKeyPressed(VK_ESCAPE))break;
        float a=Clamp(el<0.5f?el/0.5f:(el>dur-0.5f?(dur-el)/0.5f:1),0.f,1.f);
        D2D().BeginFrame(C(0,0,0));
        float qtw=D2D().MeasureTextA("Q-SHELL",120,(DWRITE_FONT_WEIGHT)700);
        D2D().DrawTextA("Q-SHELL",(float)(sw2/2)-qtw/2,(float)(sh2/2-60),120,CA(WHITE_COL,a),(DWRITE_FONT_WEIGHT)700);
        float gctw=D2D().MeasureTextA("GAMING CONSOLE",24);
        D2D().DrawTextA("GAMING CONSOLE",(float)(sw2/2)-gctw/2,(float)(sh2/2+70),24,CA(GRAY_COL,a*0.8f));
        D2D().EndFrame();
    }
    D2D().Shutdown(); DestroyWindow(hw);
}

// ============================================================================
// WIN32 WINDOW PROC (main window)
// ============================================================================

static bool g_shouldClose=false;

LRESULT CALLBACK MainWndProc(HWND hw,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){
        case WM_DESTROY: g_shouldClose=true; PostQuitMessage(0); return 0;
        case WM_CLOSE:   g_shouldClose=true; return 0;
        case WM_CHAR:    PushChar((int)wp); return 0;
        case WM_SIZE:    if(D2D().Hwnd()==hw) D2D().Resize(LOWORD(lp),HIWORD(lp)); return 0;
        case WM_KEYDOWN: if(wp==VK_F11){ /* fullscreen toggle optional */ } return 0;
        default: return DefWindowProcA(hw,msg,wp,lp);
    }
}

static HWND CreateMainWindow(int w,int h,bool shellMode){
    WNDCLASSEXA wc={sizeof(wc)};
    wc.lpfnWndProc=MainWndProc; wc.hInstance=GetModuleHandle(nullptr);
    wc.lpszClassName="QShellMain"; wc.hCursor=shellMode?nullptr:LoadCursor(nullptr,IDC_ARROW);
    wc.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExA(&wc);
    DWORD style=WS_POPUP|WS_VISIBLE; DWORD exStyle=shellMode?WS_EX_TOPMOST:(WS_EX_APPWINDOW);
    HWND hw=CreateWindowExA(exStyle,"QShellMain","Q-Shell Launcher",style,0,0,w,h,nullptr,nullptr,GetModuleHandle(nullptr),nullptr);
    return hw;
}

// ============================================================================
// MAIN  (WinMain)
// ============================================================================

int WINAPI WinMain(HINSTANCE,HINSTANCE,LPSTR,int){
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SetWorkingDirectoryToExe();
    DebugLog("======== Q-Shell v3.0 (D2D) Starting ========");
    SetUnhandledExceptionFilter(CrashHandler);
    for(auto d:{"img","profile","profile\\sounds","profile\\apps","backup","profile\\screenshots","profile\\recordings"})
        try{fs::create_directories(GetFullPath(d));}catch(...){}
    CreateEmergencyRestoreBatch();
    g_app.theme=g_app.targetTheme=ALL_THEMES[0];
    InitTimer();

    SystemConfig sysCfg=ReadSystemConfig();
    g_app.isShellMode=sysCfg.isShellMode||CheckIfShellMode();
    StartInputMonitoring();

    if(g_app.isShellMode){
        ShowBootScreen(); TerminateExplorer(); Sleep(500);
    } else {
        auto choice=ShowLaunchDialog();
        switch(choice){
            case StartupChoice::NONE: StopInputMonitoring(); CoUninitialize(); return 0;
            case StartupChoice::SHELL_MODE:
                if(!CheckAdminRights()){StopInputMonitoring();RequestAdminRights();CoUninitialize();return 0;}
                CreateSystemBackup();
                if(ActivateShellMode()){sysCfg.isShellMode=true;WriteSystemConfig(sysCfg);MessageBoxA(nullptr,"Shell mode activated! Restart.","Q-Shell",MB_OK);StopInputMonitoring();PerformRestart();CoUninitialize();return 0;}
                break;
            case StartupChoice::EXIT_SHELL:
                if(ShowExitShellConfirmation()){
                    if(!CheckAdminRights()){StopInputMonitoring();RequestAdminRights();CoUninitialize();return 0;}
                    DeactivateShellMode();LaunchExplorer();sysCfg.isShellMode=false;WriteSystemConfig(sysCfg);StopInputMonitoring();PerformRestart();CoUninitialize();return 0;}
                break;
            default: break;
        }
    }

    int sw=GetSystemMetrics(SM_CXSCREEN),sh=GetSystemMetrics(SM_CYSCREEN);
    if(sw<=0)sw=1920; if(sh<=0)sh=1080;
    LoadProfile(); LoadLibraryFromDisk(); LoadCustomAppsFromProfile();
    InitDefaultApps(); InitPlatformConnections();

    // Create main Win32 window + D2D render target
    HWND hw=CreateMainWindow(sw,sh,g_app.isShellMode);
    D2D().Init(hw,sw,sh);
    g_app.mainWindow=hw;

    if(g_app.isShellMode){
        SetWindowPos(hw,HWND_TOPMOST,0,0,sw,sh,SWP_SHOWWINDOW);
        SetForegroundWindow(hw); SetCursor(nullptr);
    } else {
        SetWindowLong(hw,GWL_EXSTYLE,(GetWindowLong(hw,GWL_EXSTYLE)&~WS_EX_TOOLWINDOW)|WS_EX_APPWINDOW);
        g_app.windowOnTop=false;
    }

    InitSkins(); g_audio.Init(); g_audio.PlayStartup();
    LoadBackground(g_app.bgPath); LoadCustomAppIcons(); LoadHubSliderTextures();

    if(!g_app.profile.avatarPath.empty()){
        std::string af=GetFullPath(g_app.profile.avatarPath);
        if(fs::exists(af)){g_app.profile.avatar=D2D().LoadBitmapA(af.c_str());g_app.profile.hasAvatar=g_app.profile.avatar.Valid();}
    }
    RefreshLibrary(); LoadGamePosters();
    g_app.steamProfile=GetSteamProfile(); g_app.steamFriends=GetRealSteamFriends(); LoadSteamAvatar();

    InputAdapter input; bool shouldExit=false;
    ShellAction pendingAction=ShellAction::NONE;
    float dataRefreshTimer=0;

    // ─── MAIN LOOP ────────────────────────────────────────────────────────────
    MSG msg2; g_shouldClose=false;

    while(!g_shouldClose&&!shouldExit){
        while(PeekMessage(&msg2,nullptr,0,0,PM_REMOVE)){
            TranslateMessage(&msg2); DispatchMessage(&msg2);
            if(msg2.message==WM_QUIT)shouldExit=true;
        }
        if(shouldExit)break;

        TickTimer();
        float dt=g_dt, time2=g_time;
        float pulse=(sinf(time2*4)+1)/2;
        auto& s=g_app; auto& t=s.theme;

        UpdateKeyStates();
        s.UpdateThemeTransition(); g_audio.UpdateMusic();
        UpdateHubSlider(dt); PM().Tick(dt);

        // Plugin input
        {
            QShellInput pi={};
            pi.up=input.IsMoveUp(); pi.down=input.IsMoveDown();
            pi.left=input.IsMoveLeft(); pi.right=input.IsMoveRight();
            pi.confirm=input.IsConfirm(); pi.cancel=input.IsBack();
            pi.menu=input.IsMenu(); pi.view=input.IsView();
            pi.lb=input.IsLB(); pi.rb=input.IsRB();
            pi.triangle=input.IsChangeArt(); pi.square=input.IsDeletePressed();
            pi.square_held=input.IsDeleteDown(); pi.gamepadId=input.GetGamepadID();
            UpdatePluginInput(pi);
        }

        dataRefreshTimer+=dt;
        if(dataRefreshTimer>60){ dataRefreshTimer=0; s.steamProfile=GetSteamProfile(); s.platformConnections=GetPlatformConnections(); }

        if(s.taskSwitchRequested.exchange(false)){
            RefreshTaskList(); s.taskFocusIdx=0; s.taskSlideIn=s.taskAnimTime=0;
            BringMainWindowToForeground(); s.currentMode=UIMode::TASK_SWITCHER; g_audio.PlayNotify();
        }

        input.Update();
        int totalItems=(int)s.library.size()+1;

        // Pending shell actions (resolved next frame)
        if(pendingAction!=ShellAction::NONE){
            switch(pendingAction){
                case ShellAction::EXPLORER: LaunchExplorer(); ShowNotification("Explorer","Opened",0); break;
                case ShellAction::KEYBOARD: ShellExecuteA(nullptr,"open","osk.exe",nullptr,nullptr,SW_SHOWNORMAL); break;
                case ShellAction::SETTINGS: ShellExecuteA(nullptr,"open","ms-settings:",nullptr,nullptr,SW_SHOWNORMAL); break;
                case ShellAction::TASKMGR:  ShellExecuteA(nullptr,"open","taskmgr.exe",nullptr,nullptr,SW_SHOWNORMAL); break;
                case ShellAction::RESTART_SHELL: s.shouldRestart=true; shouldExit=true; break;
                case ShellAction::EXIT_SHELL:
                    if(ShowExitShellConfirmation()&&CheckAdminRights()){
                        DeactivateShellMode();LaunchExplorer();sysCfg.isShellMode=false;WriteSystemConfig(sysCfg);
                        StopInputMonitoring();g_audio.Cleanup();PerformRestart();shouldExit=true;}
                    break;
                case ShellAction::POWER:
                    s.currentMode=UIMode::POWER_MENU; s.powerMenuFocus=0; s.powerMenuSlide=0; break;
                default: break;
            }
            pendingAction=ShellAction::NONE;
        }

        // ─── OVERLAY MODES ────────────────────────────────────────────────────
        if(s.currentMode!=UIMode::MAIN){
            D2D().BeginFrame(t.primary);
            DrawBackground(sw,sh,g_app.bgTexture.Valid()?0.3f:1.f);
            switch(s.currentMode){
                case UIMode::TASK_SWITCHER:  HandleTaskSwitcherOverlay(sw,sh,input,dt); break;
                case UIMode::SHELL_MENU:     { auto a=HandleShellMenuOverlay(sw,sh,input,dt); if(a!=ShellAction::NONE)pendingAction=a; break; }
                case UIMode::POWER_MENU:     {
                    auto p2=HandlePowerMenuOverlay(sw,sh,input,dt);
                    if(p2==PowerChoice::RESTART){StopInputMonitoring();g_audio.Cleanup();LaunchExplorer();PerformRestart();shouldExit=true;}
                    else if(p2==PowerChoice::SHUTDOWN){StopInputMonitoring();g_audio.Cleanup();LaunchExplorer();PerformShutdown();shouldExit=true;}
                    else if(p2==PowerChoice::SLEEP){PerformSleep();s.currentMode=UIMode::MAIN;}
                    else if(p2==PowerChoice::CANCEL){s.currentMode=UIMode::MAIN;}
                    break;
                }
                case UIMode::PROFILE_EDIT:   HandleProfileEditOverlay(sw,sh,input,dt); break;
                case UIMode::THEME_SELECT:   HandleThemeSelectOverlay(sw,sh,input,dt); break;
                case UIMode::ACCOUNTS_VIEW: {
    static std::vector<GamingAccount> accs = GetGamingAccounts();
    static int accFocus = 0;
    if(input.IsMoveUp()  && accFocus > 0)                      { accFocus--; PlayMoveSound(); }
    if(input.IsMoveDown()&& accFocus < (int)accs.size()-1)     { accFocus++; PlayMoveSound(); }
    if(input.IsBack())   { s.currentMode=UIMode::MAIN; PlayBackSound(); }
    RenderAccountsOverlay(0, 0, sw, sh, accs, accFocus,
        D2DColor{t.accent.r, t.accent.g, t.accent.b, t.accent.a},
        D2DColor{t.text.r,   t.text.g,   t.text.b,   t.text.a},
        time2);
    break;
}
                case UIMode::ADD_APP:        HandleAddAppOverlay(sw,sh,input); break;
                default: break;
            }
            UpdateAndDrawNotifications(sw,dt); D2D().EndFrame();
            continue;
        }

        // ─── MAIN UI INPUT ────────────────────────────────────────────────────
        if(PM().IsSkinPickerOpen()) goto skip_main_input;
        if(input.IsBG()&&!s.showDeleteWarning) ChangeBackground();
        if(input.IsView()){RefreshTaskList();s.taskFocusIdx=0;s.taskSlideIn=s.taskAnimTime=0;s.currentMode=UIMode::TASK_SWITCHER;PlayConfirmSound();}
        if(input.IsMenu()&&s.isShellMode){s.shellMenuFocus=0;s.shellMenuSlide=0;s.currentMode=UIMode::SHELL_MENU;PlayConfirmSound();}
        if(!s.showDeleteWarning){
            if(input.IsLB()){s.barFocused=(s.barFocused+MENU_COUNT-1)%MENU_COUNT;s.ResetTabFocus();PlayMoveSound();}
            if(input.IsRB()){s.barFocused=(s.barFocused+1)%MENU_COUNT;s.ResetTabFocus();PlayMoveSound();}
        }
        // Delete warning
        if(s.showDeleteWarning){
            if(input.IsConfirm()){
                if(s.isFullUninstall&&s.library[s.focused].info.platform=="Steam"&&!s.library[s.focused].info.appId.empty())
                    ShellExecuteA(nullptr,"open",("steam://uninstall/"+s.library[s.focused].info.appId).c_str(),nullptr,nullptr,SW_SHOWNORMAL);
                if(s.library[s.focused].hasPoster)D2D().UnloadBitmap(s.library[s.focused].poster);
                auto nm=s.library[s.focused].info.name;
                s.library.erase(s.library.begin()+s.focused);
                SaveProfile(); s.showDeleteWarning=false;
                s.focused=Clamp(s.focused-1,0,std::max(0,(int)s.library.size()-1));
                PlayConfirmSound(); ShowNotification("Removed",nm,3);
            }
            if(input.IsBack()){s.showDeleteWarning=false;PlayBackSound();}
        } else if(s.inTopBar){
            if(input.IsMoveDown()){s.inTopBar=false;if(s.barFocused==0)RefreshLibrary();PlayMoveSound();}
            if(input.IsMoveRight()){s.barFocused=(s.barFocused+1)%MENU_COUNT;s.ResetTabFocus();s.inTopBar=true;PlayMoveSound();}
            if(input.IsMoveLeft()){s.barFocused=(s.barFocused+MENU_COUNT-1)%MENU_COUNT;s.ResetTabFocus();s.inTopBar=true;PlayMoveSound();}
        } else {
            if(s.barFocused==0){
                if(input.IsMoveDown()){s.focused++;s.showDetails=false;PlayMoveSound();}
                if(input.IsMoveUp()){if(s.focused==0)s.inTopBar=true;else{s.focused--;s.showDetails=false;}PlayMoveSound();}
                s.focused=Clamp(s.focused,0,totalItems-1);
                if(input.IsMoveRight()&&s.focused<(int)s.library.size()){s.showDetails=true;PlayMoveSound();}
                if(input.IsMoveLeft()){s.showDetails=false;PlayMoveSound();}
                if(s.focused<(int)s.library.size()){
                    if(input.IsChangeArt()){
                        auto img=OpenFilePicker(false); if(!img.empty()){
                            auto tgt=GetFullPath("img\\"+s.library[s.focused].info.name+".png");
                            if(s.library[s.focused].hasPoster)D2D().UnloadBitmap(s.library[s.focused].poster);
                            try{fs::copy_file(img,tgt,fs::copy_options::overwrite_existing);}catch(...){}
                            s.library[s.focused].poster=D2D().LoadBitmapA(tgt.c_str());
                            s.library[s.focused].hasPoster=s.library[s.focused].poster.Valid();
                            SaveProfile();ShowNotification("Art Updated",s.library[s.focused].info.name,1);
                        }
                    }
                    if(input.IsDeleteDown()){s.holdTimer+=dt;if(s.holdTimer>=HOLD_THRESHOLD){s.showDeleteWarning=true;s.isFullUninstall=true;s.holdTimer=0;PlayErrorSound();}}
                    if(input.IsDeleteReleased()){if(s.holdTimer>0.1f&&s.holdTimer<HOLD_THRESHOLD){s.showDeleteWarning=true;s.isFullUninstall=false;PlayBackSound();}s.holdTimer=0;}
                }
                if(input.IsConfirm()&&!s.showDeleteWarning){
                    PlayConfirmSound();
                    if(s.focused<(int)s.library.size()){ShowNotification("Launching",s.library[s.focused].info.name,0);LaunchApp(s.library[s.focused].info.exePath);}
                    else{auto p2=OpenFilePicker(true);if(!p2.empty()){auto nm=fs::path(p2).stem().string();s.library.push_back({{nm,p2,"Manual",""}});SaveProfile();s.focused=(int)s.library.size()-1;ShowNotification("Added",nm,1);}}
                }
            } else if(s.barFocused==3){
                if(input.IsMoveUp()){if(s.settingsFocusY==0)s.inTopBar=true;else s.settingsFocusY--;PlayMoveSound();}
                if(input.IsMoveDown()){s.settingsFocusY=std::min(s.settingsFocusY+1,2);PlayMoveSound();}
                if(input.IsMoveLeft()){s.settingsFocusX=std::max(s.settingsFocusX-1,0);PlayMoveSound();}
                if(input.IsMoveRight()){s.settingsFocusX=std::min(s.settingsFocusX+1,2);PlayMoveSound();}
                if(input.IsConfirm()){PlayConfirmSound();
                    int idx=s.settingsFocusY*3+s.settingsFocusX;
                    switch(idx){
                        case 0: ChangeBackground(); break;
                        case 1: s.profileEditFocus=0;s.profileEditSlide=0;s.currentMode=UIMode::PROFILE_EDIT; break;
                        case 2: RefreshLibrary(); break;
                        case 3: UploadThemeSong(); break;
                        case 4: RemoveThemeSong(); break;
                        case 5: s.themeSelectFocus=s.currentThemeIdx;s.themeSelectSlide=0;s.currentMode=UIMode::THEME_SELECT; break;
                        case 6: PM().OpenSkinPicker(); break;
                        case 7: ShowNotification("Q-Shell v3.0","Gaming Hub",0); break;
                        case 8: shouldExit=true; break;
                    }
                }
            }
        }

        skip_main_input:;

        // Smooth scroll
        s.scrollY=LerpF(s.scrollY,(float)(-s.focused*320)+sh/2.f-135,0.12f);
        s.transAlpha=LerpF(s.transAlpha,0.f,0.3f);
        for(int i=0;i<(int)s.library.size();i++){
            float tgt=(!s.inTopBar&&s.showDetails&&i==s.focused&&s.barFocused==0)?1.f:0.f;
            s.library[i].detailAlpha=LerpF(s.library[i].detailAlpha,tgt,0.15f);
        }

        // ─── DRAWING ──────────────────────────────────────────────────────────
        D2D().BeginFrame(t.primary);
        DrawBackground(sw,sh);
        float contentTop=120;

        // TAB 0: LIBRARY
        if(s.barFocused==0){
            bool skinHandled=PM().DrawLibraryTab(sw,sh,s.focused,time2);
            if(!skinHandled) for(int i=0;i<totalItems;i++){
                float iy=s.scrollY+i*320; if(iy<-300||iy>sh)continue;
                bool iF=(!s.inTopBar&&i==s.focused);
                float al=iF?1.f:(s.inTopBar?0.15f:0.25f);
                QRect_t card={120,iy,480,270};
                bool skinCard=PM().HasActiveCardSkin();
                if(i<(int)s.library.size()){
                    auto& g2=s.library[i];
                    if(!skinCard&&g2.detailAlpha>0.01f){
                        float da=g2.detailAlpha;
                        D2D().FillRoundRect(card.x+card.width+40,card.y,600*da,card.height,5,5,CA(t.secondary,da*0.9f));
                        if(da>0.8f){
                            float dx=card.x+card.width+80;
                            D2D().DrawTextA("READY TO PLAY",dx,card.y+55,24,CA(t.success,da));
                            D2D().DrawTextA(g2.info.platform.c_str(),dx,card.y+135,22,CA(t.text,da));
                            D2D().DrawTextA("[A] LAUNCH",dx,card.y+200,18,CA(t.accent,da));
                        }
                    }
                    DrawGameCard(card,g2,iF,time2);
                    if(!skinCard&&iF&&!s.showDetails){
                        D2D().DrawTextA(g2.info.name.c_str(),card.x+card.width+50,iy+90,40,CA(t.text,al),(DWRITE_FONT_WEIGHT)700);
                        D2D().DrawTextA(g2.info.platform.c_str(),card.x+card.width+50,iy+140,18,CA(t.textDim,al*0.7f));
                        if(s.holdTimer>0)D2D().FillRect(card.x+card.width+50,iy+170,(s.holdTimer/HOLD_THRESHOLD)*200,4,t.danger);
                    }
                } else {
                    if(!skinCard){
                        D2D().FillRoundRect(card.x,card.y,card.width,card.height,5,5,CA(t.cardBg,al));
                        float pw3=D2D().MeasureTextA("+",80,(DWRITE_FONT_WEIGHT)700); D2D().DrawTextA("+",card.x+card.width/2-pw3/2,card.y+card.height/2-40,80,CA(t.text,al),(DWRITE_FONT_WEIGHT)700);
                        float aw2=D2D().MeasureTextA("Add Game",16); D2D().DrawTextA("Add Game",card.x+card.width/2-aw2/2,card.y+card.height/2+35,16,CA(t.textDim,al*0.6f));
                    }
                }
                if(iF&&!skinCard)D2D().StrokeRoundRect(card.x,card.y,card.width,card.height,5,5,4.f,CA(t.accent,0.4f+pulse*0.4f));
            }
        }
        // TAB 1: MEDIA
        else if(s.barFocused==1){ DrawMediaTab(sw,sh,contentTop,input,dt); }
        // TAB 2: SHARE
        else if(s.barFocused==2){ DrawShareTab(sw,sh,contentTop,input,dt); }
        // TAB 3: SETTINGS
        else if(s.barFocused==3){
            float tsx=sw/2.f-490,tsy=contentTop+60,tw2=270,th2=165,tgap=18;
            struct SI{const char* i,*t2;D2D1_COLOR_F c;};
            SI items[]={{"B","Background",t.accent},{"P","Profile",PURPLE_COL},{"R","Refresh",t.success},{"M","Upload Music",C(100,200,255)},{"x","Remove Music",C(255,100,100)},{"T","Theme",ORANGE_COL},{"S","Skin/Plugin",C(200,100,255)},{"?","About",GRAY_COL},{"Q","Exit",t.danger}};
            int fi=s.settingsFocusY*3+s.settingsFocusX;
            for(int r=0;r<3;r++) for(int c2=0;c2<3;c2++){
                int idx=r*3+c2;
                QRect_t tile={tsx+c2*(tw2+tgap),tsy+r*(th2+tgap),tw2,th2};
                DrawSettingsTile(tile,items[idx].i,items[idx].t2,items[idx].c,(!s.inTopBar&&fi==idx),time2);
            }
        }

        DrawTopBar(sw,60); DrawBottomBar(sw,sh,time2);

        // Delete warning overlay
        if(s.showDeleteWarning){
            D2D().FillRect(0,0,(float)sw,(float)sh,CA(BLACK_COL,0.8f));
            float bx3=(float)(sw/2-300),by3=(float)(sh/2-150);
            D2D().FillRoundRect(bx3,by3,600,300,8,8,t.secondary);
            D2D().StrokeRoundRect(bx3,by3,600,300,8,8,2.f,s.isFullUninstall?t.danger:t.warning);
            D2D().DrawTextA(s.isFullUninstall?"FULL UNINSTALL":"REMOVE FROM LIST",bx3+150,by3+50,28,s.isFullUninstall?t.danger:t.warning,(DWRITE_FONT_WEIGHT)700);
            D2D().DrawTextA(s.library[s.focused].info.name.c_str(),bx3+100,by3+100,20,t.text);
            D2D().DrawTextA("Confirm [A] or [B] cancel",bx3+180,by3+180,18,t.textDim);
        }
        if(s.transAlpha>0.01f)D2D().FillRect(0,110,(float)sw,(float)(sh-180),CA(t.primary,s.transAlpha));

        UpdateAndDrawNotifications(sw,dt);
        DrawSkinPickerOverlay(sw,sh,input);
        D2D().EndFrame();
    }

    // ─── CLEANUP ──────────────────────────────────────────────────────────────
    if(g_app.bgTexture.Valid())D2D().UnloadBitmap(g_app.bgTexture);
    if(g_app.steamAvatarTex.Valid())D2D().UnloadBitmap(g_app.steamAvatarTex);
    for(auto& app:g_app.customApps)if(app.hasIcon)D2D().UnloadBitmap(app.icon);
    for(auto& g2:g_app.library)if(g2.hasPoster)D2D().UnloadBitmap(g2.poster);
    if(g_app.profile.hasAvatar)D2D().UnloadBitmap(g_app.profile.avatar);
    for(int i=0;i<3;i++)if(g_app.hubSlider.artCovers[i].Valid())D2D().UnloadBitmap(g_app.hubSlider.artCovers[i]);

    g_audio.Cleanup(); UnloadSkinPlugins(); D2D().Shutdown(); StopInputMonitoring();

    if(g_app.isShellMode)LaunchExplorer();
    if(g_app.shouldRestart){char exe[MAX_PATH];GetModuleFileNameA(nullptr,exe,MAX_PATH);ShellExecuteA(nullptr,"open",exe,nullptr,nullptr,SW_SHOWNORMAL);}

    CoUninitialize();
    return 0;
}
