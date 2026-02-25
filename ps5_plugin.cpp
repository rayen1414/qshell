// ============================================================================
//  PS5Station Plugin  v7.0  —  Q-Shell  (D2D backend)
//
//  Faithful PS5 home screen. Correct card layout:
//  - Hero card centred on screen
//  - Neighbours placed edge-to-edge outward, never overlapping
//  - Correct dimming and scale per distance
//  - Info panel below cards: title left, buttons right
//  - Proper hint bar at very bottom
//
//  COMPILE:
//    g++ -shared -O2 -std=c++17 ps5_plugin.cpp -o PS5Station.dll ^
//        -I"." -static -static-libgcc -static-libstdc++
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "qshell_plugin_api.h"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cctype>

static const D2DPluginAPI*  RL  = nullptr;
static const QShellHostAPI* HST = nullptr;

// ============================================================================
//  PALETTE
// ============================================================================
static const D2DColor K_BLACK    = QRGBA(  0,  0,  0,255);
static const D2DColor K_WHITE    = QRGBA(255,255,255,255);
static const D2DColor K_TOPBAR   = QRGBA(  8, 10, 18,232);
static const D2DColor K_HINTBAR  = QRGBA(  6,  8, 14,220);
static const D2DColor K_TEXT     = QRGBA(232,236,248,255);
static const D2DColor K_TEXTDIM  = QRGBA(138,146,170,255);
static const D2DColor K_ACCENT   = QRGBA(  0,150,255,255);
static const D2DColor K_ACCENT2  = QRGBA(  0,210,248,255);
static const D2DColor K_TAB_ON   = QRGBA(255,255,255,255);
static const D2DColor K_TAB_OFF  = QRGBA(126,134,156,180);
static const D2DColor K_CROSS    = QRGBA( 88,152,255,255);
static const D2DColor K_CIRCLE   = QRGBA(220, 68, 68,255);
static const D2DColor K_SQUARE   = QRGBA(192,108,228,255);
static const D2DColor K_TRIANGLE = QRGBA( 52,192,192,255);
static const D2DColor K_GOLD     = QRGBA(218,176, 36,255);
static const D2DColor K_SILVER   = QRGBA(180,184,194,255);
static const D2DColor K_BRONZE   = QRGBA(166, 92, 36,255);

// ============================================================================
//  HELPERS
// ============================================================================
static inline D2DColor Fa(D2DColor c,float a){return QFADE(c,a);}
static inline float Sf(float x){return RL->sinf_(x);}
static inline float Cl(float v,float lo,float hi){return v<lo?lo:v>hi?hi:v;}
static inline float Lp(float a,float b,float t){return a+(b-a)*t;}
static inline float Ease(float t){t=Cl(t,0,1);return t*t*(3-2*t);}

static D2DColor NameColor(const char* s,float sat,float val)
{
    unsigned h=5381u;
    while(s&&*s)h=((h<<5u)+h)^(unsigned char)*s++;
    float hue=(float)(h%360)/360.f;
    float c2=val*sat, x2=c2*(1.f-fabsf(fmodf(hue*6.f,2.f)-1.f)), m=val-c2;
    float r,g,b;
    switch((int)(hue*6.f)%6){
        case 0:r=c2;g=x2;b=0; break; case 1:r=x2;g=c2;b=0; break;
        case 2:r=0; g=c2;b=x2;break; case 3:r=0; g=x2;b=c2;break;
        case 4:r=x2;g=0; b=c2;break; default:r=c2;g=0; b=x2;break;
    }
    return {r+m,g+m,b+m,1.f};
}

// ============================================================================
//  LAYOUT
// ============================================================================
static const float TOP_H  = 56.f;
static const float HINT_H = 44.f;
static const float INFO_H = 150.f;  // height of the info panel

// Hero card (portrait 3:4)
static const float HERO_W = 380.f;
static const float HERO_H = 506.f;

// Scale and vertical drop for each neighbour distance
struct NLevel { float scale; float yDrop; float dim; };
static const NLevel NLEVELS[4] = {
    {1.000f,  0.f, 1.00f},  // dist 0 = hero
    {0.600f, 28.f, 0.65f},  // dist 1
    {0.390f, 50.f, 0.38f},  // dist 2
    {0.255f, 65.f, 0.20f},  // dist 3+
};

static const float CARD_GAP = 16.f;  // pixel gap between card edges

// Card strip vertical centre (fraction of the space between top bar and info panel)
static float StripCY(float sh)
{
    float availTop = TOP_H;
    float availBot = sh - HINT_H - INFO_H;
    return availTop + (availBot - availTop)*0.46f;
}

// Get pixel X of the LEFT EDGE of card[i], with hero centred at sw/2.
// Scroll is applied externally.
static float CardLeftX(int i, int focused, float sw)
{
    float heroLeft = (sw - HERO_W)/2.f;

    if(i == focused) return heroLeft;

    int dist = i - focused;
    // Walk outward from hero
    if(dist > 0){
        float x = heroLeft + HERO_W + CARD_GAP;
        for(int d=1; d<dist; d++){
            int lv = d>3?3:d;
            x += HERO_W*NLEVELS[lv].scale + CARD_GAP;
        }
        return x;
    } else {
        // dist < 0: left side
        int absDist = -dist;
        int lv0 = absDist>3?3:absDist;
        float w0 = HERO_W*NLEVELS[lv0].scale;
        float x = heroLeft - CARD_GAP - w0;
        for(int d=1; d<absDist; d++){
            int lv = d>3?3:d;
            x -= HERO_W*NLEVELS[lv].scale + CARD_GAP;
        }
        return x;
    }
}

// ============================================================================
//  STATE
// ============================================================================
static float s_scroll      = 0.f;
static float s_bgFadeStart = 0.f;
static float s_bgFadeT     = 0.f;
static float s_titleFade   = 0.f;
static float s_infoSlide   = 0.f;
static int   s_lastFocus   = -999;
static bool  s_pLeft=false, s_pRight=false;
static bool  s_pLB=false,   s_pRB=false;
static bool  s_pConfirm=false;

// ============================================================================
//  LIFECYCLE
// ============================================================================
static void OnLoad(){
    HST->PushNotification("PS5Station v7","Authentic PS5 UI",K_ACCENT,4.f);
    s_scroll=0;s_lastFocus=-999;s_bgFadeT=0;s_titleFade=0;s_infoSlide=0;
}
static void OnUnload(){}
static void OnTick(float dt){
    s_titleFade=Cl(s_titleFade+dt*2.5f,0.f,1.f);
    s_infoSlide=Cl(s_infoSlide+dt*3.0f,0.f,1.f);
}
static void OnLibraryChanged(){s_lastFocus=-999;}

// ============================================================================
//  DRAW ONE CARD  (shared by card strip and library tab)
// ============================================================================
static void DrawOneCard(int i,int focused,float sw,float sh,float time,float scrollOff)
{
    int count=HST->GetGameCount();
    if(i<0||i>=count)return;

    QShellGameInfo gi={};HST->GetGame(i,&gi);
    bool isFoc=(i==focused);
    int dist=i-focused; if(dist<0)dist=-dist;
    int lv=dist>3?3:dist;

    float w=HERO_W*NLEVELS[lv].scale;
    float h=HERO_H*NLEVELS[lv].scale;
    float dim=NLEVELS[lv].dim;

    float stripCY=StripCY(sh);
    float yOff=NLEVELS[lv].yDrop;

    // X position: card's left edge relative to hero-centred layout + scroll
    float x=CardLeftX(i,focused,sw)+scrollOff;
    float y=stripCY-h/2.f+yOff;

    // Cull off-screen
    if(x+w<-80.f||x>(float)sw+80.f)return;

    float rx=w*0.045f;
    float pulse=(Sf(time*2.f)+1.f)*0.5f;

    // Shadow
    RL->FillRoundRect(x+4,y+6,w,h,rx,rx,Fa(K_BLACK,isFoc?0.70f:0.30f*dim));

    // Card gradient body (game-unique colour)
    const char* nm=gi.name?gi.name:"?";
    D2DColor gc=NameColor(nm,0.62f,0.32f);
    D2DColor topC=Fa(gc,dim);
    D2DColor botC=Fa(K_BLACK,isFoc?0.88f:0.94f);
    RL->FillGradientV(x,y,w,h,topC,botC);

    // Initial letter (placeholder art)
    {
        char init[2]={nm[0]?(char)toupper((unsigned char)nm[0]):'?',0};
        float isz=h*0.35f;
        float iw=RL->MeasureTextA(init,isz,700);
        float ia=isFoc?0.65f:dim*0.38f;
        RL->DrawTextA(init,x+w/2.f-iw/2.f,y+h*0.28f,isz,Fa(K_WHITE,ia),700);
    }

    // Platform badge (focused only)
    if(isFoc&&gi.platform&&gi.platform[0]){
        float bw=RL->MeasureTextA(gi.platform,10,400)+16.f;
        RL->FillRoundRect(x+8,y+8,bw,20.f,10.f,10.f,Fa(K_ACCENT,0.90f));
        RL->DrawTextA(gi.platform,x+8+8,y+12,10.f,K_WHITE,400);
    }

    // Border / focus ring
    if(isFoc){
        // Outer glow
        RL->StrokeRoundRect(x-3,y-3,w+6,h+6,rx+3,rx+3,
                             3.f,Fa(K_WHITE,0.09f+pulse*0.06f));
        // Sharp ring
        RL->StrokeRoundRect(x,y,w,h,rx,rx,
                             2.f,Fa(K_WHITE,0.84f+pulse*0.16f));
    } else {
        RL->StrokeRoundRect(x,y,w,h,rx,rx,1.f,Fa(K_WHITE,dim*0.13f));
    }

    // Name label under focused card
    if(isFoc&&gi.name){
        char lbl[36];snprintf(lbl,sizeof(lbl),"%.32s",gi.name);
        float lw=RL->MeasureTextA(lbl,11,400);
        float lx=x+w/2.f-lw/2.f-10.f;
        float ly=y+h+10.f;
        RL->FillRoundRect(lx,ly,lw+20.f,20.f,10.f,10.f,Fa(K_BLACK,0.68f));
        RL->DrawTextA(lbl,lx+10.f,ly+4.f,11.f,Fa(K_WHITE,0.90f),400);
    }
}

// ============================================================================
//  BACKGROUND
// ============================================================================
static bool DrawBackground(int sw,int sh,float time)
{
    int focused=HST->GetFocusedIdx();
    int count=HST->GetGameCount();

    RL->FillRect(0,0,(float)sw,(float)sh,K_BLACK);

    if(count>0&&focused>=0&&focused<count){
        QShellGameInfo gi={};HST->GetGame(focused,&gi);

        if(s_lastFocus!=focused){
            s_lastFocus=focused;
            s_bgFadeStart=time;
            s_bgFadeT=0.f;
            s_titleFade=0.f;
            s_infoSlide=0.f;
        }

        float age=Cl((time-s_bgFadeStart)/0.55f,0.f,1.f);
        s_bgFadeT=Ease(age);

        const char* nm=gi.name?gi.name:"?";
        D2DColor gc1=NameColor(nm,0.68f,0.26f);
        D2DColor gc2=NameColor(nm,0.48f,0.14f);

        for(int i=16;i>=1;i--){
            float r=(float)i/16.f*sw*0.70f;
            float a=s_bgFadeT*0.035f*(1.f-(float)i/17.f);
            RL->FillCircle(sw*0.18f,sh*0.70f,r,Fa(gc1,a));
        }
        for(int i=10;i>=1;i--){
            float r=(float)i/10.f*sw*0.38f;
            float a=s_bgFadeT*0.018f*(1.f-(float)i/11.f);
            RL->FillCircle(sw*0.84f,sh*0.18f,r,Fa(gc2,a));
        }
        for(int i=8;i>=1;i--){
            float r=(float)i/8.f*sw*0.26f;
            float a=s_bgFadeT*0.024f*(1.f-(float)i/9.f);
            RL->FillCircle(sw*0.50f,sh*0.40f,r,Fa(gc1,a));
        }
        if(s_bgFadeT<1.f)
            RL->FillRect(0,0,(float)sw,(float)sh,Fa(K_BLACK,1.f-s_bgFadeT));
    }

    // Shimmer
    float t1=time*0.046f;
    for(int b=0;b<3;b++){
        float frac=fmodf(t1+b*0.333f,1.f);
        float bx=frac*(sw+600.f)-300.f;
        RL->FillGradientH(bx-80,0, 80,(float)sh,Fa(K_WHITE,0.f),Fa(K_WHITE,0.010f));
        RL->FillGradientH(bx,   0,120,(float)sh,Fa(K_WHITE,0.010f),Fa(K_WHITE,0.f));
    }

    // Vignette
    RL->FillGradientV(0,       0,       (float)sw,sh*0.20f,Fa(K_BLACK,0.94f),Fa(K_BLACK,0.f));
    RL->FillGradientV(0,       sh*0.68f,(float)sw,sh*0.32f,Fa(K_BLACK,0.f),  Fa(K_BLACK,0.97f));
    RL->FillGradientH(0,       0,       sw*0.15f,(float)sh,Fa(K_BLACK,0.62f),Fa(K_BLACK,0.f));
    RL->FillGradientH(sw*0.85f,0,       sw*0.15f,(float)sh,Fa(K_BLACK,0.f),  Fa(K_BLACK,0.56f));

    return true;
}

// ============================================================================
//  TOP BAR  (nav + card strip)
// ============================================================================
static bool DrawTopBar(int sw,int sh,float time)
{
    float pulse=(Sf(time*2.2f)+1.f)*0.5f;

    // Glass bar
    RL->FillRect(0,0,(float)sw,TOP_H,K_TOPBAR);
    RL->FillRect(0,TOP_H-1,(float)sw,1,Fa(K_WHITE,0.07f));

    // PS mark
    {
        float lx=14.f,ly=15.f;
        RL->FillRect(lx,   ly,   4,24,Fa(K_WHITE,0.92f));
        RL->FillRect(lx,   ly,  13, 6,Fa(K_WHITE,0.92f));
        RL->FillRect(lx,   ly+6,11, 5,Fa(K_WHITE,0.92f));
        float sx=lx+17.f;
        RL->FillRect(sx,   ly,   12, 5,Fa(K_ACCENT2,0.92f));
        RL->FillRect(sx,   ly+5,  4, 4,Fa(K_ACCENT2,0.92f));
        RL->FillRect(sx,   ly+9, 12, 5,Fa(K_ACCENT2,0.92f));
        RL->FillRect(sx+8, ly+14, 4, 4,Fa(K_ACCENT2,0.92f));
        RL->FillRect(sx,   ly+18,12, 5,Fa(K_ACCENT2,0.92f));
    }

    // Tab labels
    int activeTab=HST->GetActiveTab();
    const char* TABS[]={"Games","Media","Store","Search"};
    float tx=52.f;
    for(int i=0;i<4;i++){
        bool sel=(i==activeTab);
        float fsz=sel?16.f:14.f; int wt=sel?700:400;
        RL->DrawTextA(TABS[i],tx,TOP_H/2-fsz/2,fsz,sel?K_TAB_ON:K_TAB_OFF,wt);
        if(sel){
            float uw=RL->MeasureTextA(TABS[i],fsz,wt);
            RL->FillRoundRect(tx,TOP_H-5.f,uw,3.f,1.5f,1.5f,K_WHITE);
        }
        tx+=RL->MeasureTextA(TABS[i],fsz,wt)+26.f;
    }

    // Right: clock + icons + online dot
    SYSTEMTIME st;GetLocalTime(&st);
    char tbuf[10];snprintf(tbuf,sizeof(tbuf),"%02d:%02d",st.wHour,st.wMinute);
    float clkW=RL->MeasureTextA(tbuf,15,400);
    RL->DrawTextA(tbuf,sw-clkW-14,TOP_H/2-8.f,15.f,Fa(K_WHITE,0.88f),400);
    RL->FillCircle(sw-clkW-26,TOP_H/2,4.5f,Fa(K_ACCENT,0.55f+pulse*0.40f));
    const char* iSym[]={"P","I","="};
    float icx=sw-clkW-42.f;
    for(int i=2;i>=0;i--){
        RL->FillCircle  (icx,TOP_H/2,10.f,Fa(K_WHITE,0.10f));
        RL->StrokeCircle(icx,TOP_H/2,10.f,1.f,Fa(K_WHITE,0.36f));
        float iw2=RL->MeasureTextA(iSym[i],10,400);
        RL->DrawTextA(iSym[i],icx-iw2/2,TOP_H/2-6.f,10.f,Fa(K_WHITE,0.70f),400);
        icx-=26.f;
    }

    // Input handling
    const QShellInput* inp=HST->GetInput();
    int count=HST->GetGameCount();
    int focused=HST->GetFocusedIdx();

    if(inp){
        if(inp->lb&&!s_pLB){int t=HST->GetActiveTab();HST->SetActiveTab((t-1+4)%4);}
        if(inp->rb&&!s_pRB){int t=HST->GetActiveTab();HST->SetActiveTab((t+1)%4);}
        s_pLB=inp->lb;s_pRB=inp->rb;

        if(inp->left &&!s_pLeft &&focused>0)      HST->SetFocusedIdx(focused-1);
        if(inp->right&&!s_pRight&&focused<count-1) HST->SetFocusedIdx(focused+1);
        s_pLeft=inp->left;s_pRight=inp->right;

        if(inp->confirm&&!s_pConfirm&&count>0) HST->LaunchGame(focused);
        s_pConfirm=inp->confirm;
    }

    if(count==0){
        const char* em="No games added yet — go to Settings to add games";
        float ew=RL->MeasureTextA(em,15,400);
        RL->DrawTextA(em,(sw-ew)/2.f,sh/2.f-8.f,15.f,Fa(K_WHITE,0.28f),400);
        return true;
    }

    // Smooth scroll: s_scroll lerps to 0 each frame (cards recentre on focus)
    s_scroll=Lp(s_scroll,0.f,0.14f);

    // Draw cards: unfocused first (back-to-front by distance), focused last
    for(int dist2=4;dist2>=1;dist2--){
        DrawOneCard(focused-dist2,focused,(float)sw,(float)sh,time,s_scroll);
        DrawOneCard(focused+dist2,focused,(float)sw,(float)sh,time,s_scroll);
    }
    // Focused card on top
    DrawOneCard(focused,focused,(float)sw,(float)sh,time,s_scroll);

    // Page dots
    {
        float heroY=StripCY((float)sh)-HERO_H/2.f;
        float dotsY=heroY+HERO_H+36.f;
        int ndots=count>8?8:count;
        float dotR=3.f,dotGap=11.f;
        float dotsW=(float)ndots*dotGap-dotGap+dotR*2;
        float dox=((float)sw-dotsW)/2.f;
        int df=focused>ndots-1?ndots-1:focused;
        for(int i=0;i<ndots;i++)
            RL->FillCircle(dox+i*dotGap+dotR,dotsY+dotR,dotR,
                           i==df?Fa(K_WHITE,0.92f):Fa(K_WHITE,0.22f));
    }

    return true;
}

// ============================================================================
//  BOTTOM BAR  (info panel + hint bar)
// ============================================================================
static bool DrawBottomBar(int sw,int sh,float time)
{
    float pulse=(Sf(time*2.f)+1.f)*0.5f;
    int count=HST->GetGameCount();
    int focused=HST->GetFocusedIdx();

    // Hint bar
    float hintY=(float)sh-HINT_H;
    RL->FillRect(0,hintY,(float)sw,HINT_H,K_HINTBAR);
    RL->FillRect(0,hintY,(float)sw,1.f,Fa(K_WHITE,0.06f));

    struct HB{const char* sym;const char* lbl;D2DColor col;};
    HB hints[]={{"x","Open",K_CROSS},{"o","Back",K_CIRCLE},{"=","Options",K_SQUARE},{"^","Details",K_TRIANGLE}};
    float hx=22.f,hy=hintY+HINT_H/2.f;
    for(auto& h:hints){
        float r=9.f;
        RL->FillCircle  (hx+r,hy,r,Fa(h.col,0.18f));
        RL->StrokeCircle(hx+r,hy,r,1.f,Fa(h.col,0.88f));
        float sw2=RL->MeasureTextA(h.sym,11,700);
        RL->DrawTextA(h.sym,hx+r-sw2/2,hy-7.f,11.f,h.col,700);
        hx+=r*2+5.f;
        RL->DrawTextA(h.lbl,hx,hy-7.f,12.f,Fa(K_WHITE,0.60f),400);
        hx+=RL->MeasureTextA(h.lbl,12,400)+18.f;
    }
    const char* lr="L1 / R1  Switch Tabs";
    float lrW=RL->MeasureTextA(lr,12,400);
    RL->DrawTextA(lr,sw-lrW-18,hy-7.f,12.f,Fa(K_WHITE,0.30f),400);

    if(count==0||focused<0||focused>=count) return true;

    QShellGameInfo gi={};HST->GetGame(focused,&gi);
    float fa=s_titleFade;
    float slide=Ease(s_infoSlide);

    // Panel area
    float panelTop=hintY-INFO_H;
    float slideOff=(1.f-slide)*30.f;

    // Gradient backdrop
    RL->FillGradientV(0,panelTop-50.f,(float)sw,50.f,Fa(K_BLACK,0.f),Fa(K_BLACK,0.68f));
    RL->FillRect(0,panelTop,(float)sw,INFO_H,Fa(K_BLACK,0.68f));

    float lx=44.f;
    float titleY=panelTop+12.f+slideOff;

    // Title
    if(gi.name){
        char nm2[64];snprintf(nm2,sizeof(nm2),"%.56s",gi.name);
        float tsz=34.f;
        if(RL->MeasureTextA(nm2,tsz,700)>(float)sw*0.50f) tsz=26.f;
        RL->DrawTextA(nm2,lx+2,titleY+2,tsz,Fa(K_BLACK,fa*0.78f),700);
        RL->DrawTextA(nm2,lx,  titleY,  tsz,Fa(K_TEXT, fa),      700);
    }

    // Platform + progress bar on same line
    float row2Y=titleY+46.f;
    {
        const char* plat=(gi.platform&&gi.platform[0])?gi.platform:"Unknown";
        RL->DrawTextA(plat,lx,row2Y,13.f,Fa(K_TEXTDIM,fa*0.85f),400);
        float platW=RL->MeasureTextA(plat,13,400);

        RL->DrawTextA("·",lx+platW+6,row2Y,13.f,Fa(K_TEXTDIM,fa*0.45f),400);

        float barX=lx+platW+22.f;
        float barW=180.f,barH=5.f,barY2=row2Y+5.f;
        float prog=0.38f;
        RL->FillRoundRect(barX,barY2,barW,barH,2.5f,2.5f,Fa(K_WHITE,0.12f));
        RL->FillRoundRect(barX,barY2,barW*prog,barH,2.5f,2.5f,Fa(K_ACCENT,fa*0.92f));
        char pct[8];snprintf(pct,sizeof(pct),"%.0f%%",prog*100.f);
        RL->DrawTextA(pct,barX+barW+8,row2Y,12.f,Fa(K_TEXTDIM,fa*0.80f),400);
    }

    // Trophies
    float row3Y=row2Y+24.f;
    {
        struct Troph{const char* l;D2DColor col;int n;} ts[]={
            {"G",K_GOLD,0},{"S",K_SILVER,1},{"B",K_BRONZE,3}
        };
        float tx2=lx;
        for(auto& tr:ts){
            float r2=8.f;
            RL->FillCircle  (tx2+r2,row3Y+r2,r2,Fa(tr.col,fa*0.20f));
            RL->StrokeCircle(tx2+r2,row3Y+r2,r2,1.f,Fa(tr.col,fa*0.80f));
            float lw2=RL->MeasureTextA(tr.l,9,700);
            RL->DrawTextA(tr.l,tx2+r2-lw2/2,row3Y+r2-5.5f,9.f,Fa(tr.col,fa),700);
            char cnt[4];snprintf(cnt,sizeof(cnt),"%d",tr.n);
            RL->DrawTextA(cnt,tx2+r2*2+4,row3Y+r2-5.5f,11.f,Fa(K_TEXT,fa*0.72f),400);
            tx2+=r2*2+4+RL->MeasureTextA(cnt,11,400)+10.f;
        }
    }

    // Buttons (right-aligned, vertically centred in panel)
    float btnH=40.f,playW=148.f,optW=44.f;
    float btnY2=panelTop+(INFO_H-btnH)/2.f+slideOff;
    float btnX=(float)sw-optW-playW-40.f;

    // Play (white pill)
    {
        float prx=btnH/2.f;
        RL->FillRoundRect(btnX,btnY2,playW,btnH,prx,prx,
                          Fa(K_WHITE,fa*(0.90f+pulse*0.08f)));
        float ptw=RL->MeasureTextA("Play Game",15,700);
        RL->DrawTextA("Play Game",btnX+playW/2-ptw/2,btnY2+btnH/2-8.f,15.f,K_BLACK,700);
    }

    // Options (dark pill)
    {
        float ox=btnX+playW+10.f,orx=btnH/2.f;
        RL->FillRoundRect(ox,btnY2,optW,btnH,orx,orx,Fa(K_WHITE,fa*0.12f));
        RL->StrokeRoundRect(ox,btnY2,optW,btnH,orx,orx,1.f,Fa(K_WHITE,fa*0.42f));
        float dw=RL->MeasureTextA("···",14,700);
        RL->DrawTextA("···",ox+optW/2-dw/2,btnY2+btnH/2-8.f,14.f,Fa(K_WHITE,fa*0.80f),700);
    }

    (void)pulse;
    return true;
}

// ============================================================================
//  GAME CARD  (host grid)
// ============================================================================
static bool DrawGameCard(QRect card,const char* name,bool foc,
                          D2DBitmapHandle poster,float time)
{
    float pulse=(Sf(time*2.f)+1.f)*0.5f;
    float rx=card.width*0.045f;
    float dim=foc?1.f:0.58f;

    RL->FillRoundRect(card.x+4,card.y+6,card.width,card.height,rx,rx,
                      Fa(K_BLACK,foc?0.72f:0.36f));

    if(poster.opaque&&poster.w>0){
        float ta=(float)poster.w/(float)poster.h,ca=card.width/card.height;
        float srcX=0,srcY=0,srcW=(float)poster.w,srcH=(float)poster.h;
        if(ta>ca){srcW=poster.h*ca;srcX=(poster.w-srcW)*.5f;}
        else     {srcH=poster.w/ca;srcY=(poster.h-srcH)*.5f;}
        RL->DrawBitmapCropped(poster,srcX,srcY,srcW,srcH,
                               card.x,card.y,card.width,card.height,foc?1.f:0.50f);
    } else {
        D2DColor gc=NameColor(name?name:"?",0.62f,0.32f);
        RL->FillGradientV(card.x,card.y,card.width,card.height,
                          Fa(gc,dim),Fa(K_BLACK,0.92f));
        char init[2]={name&&name[0]?(char)toupper((unsigned char)name[0]):'?',0};
        float isz=card.height*0.37f,iw=RL->MeasureTextA(init,isz,700);
        RL->DrawTextA(init,card.x+card.width/2-iw/2,card.y+card.height*0.26f,
                      isz,Fa(K_WHITE,foc?0.68f:0.20f),700);
    }

    if(foc){
        RL->StrokeRoundRect(card.x-3,card.y-3,card.width+6,card.height+6,
                             rx+3,rx+3,3.f,Fa(K_WHITE,0.09f+pulse*0.05f));
        RL->StrokeRoundRect(card.x,card.y,card.width,card.height,
                             rx,rx,2.f,Fa(K_WHITE,0.84f+pulse*0.16f));
    } else {
        RL->StrokeRoundRect(card.x,card.y,card.width,card.height,
                             rx,rx,1.f,Fa(K_WHITE,0.09f));
    }
    return true;
}

// ============================================================================
//  SETTINGS TILE
// ============================================================================
static bool DrawSettingsTile(QRect r,const char* icon,const char* title,
                              D2DColor,bool foc,float time)
{
    float pulse=(Sf(time*2.f)+1.f)*0.5f;
    float sc=foc?1.04f:1.f;
    QRect s={r.x-r.width*(sc-1)*.5f,r.y-r.height*(sc-1)*.5f,
             r.width*sc,r.height*sc};
    float rx=s.width*0.055f;

    RL->FillRoundRect(s.x+4,s.y+5,s.width,s.height,rx,rx,Fa(K_BLACK,0.55f));
    D2DColor top2=foc?QRGBA(24,38,82,255):QRGBA(14,20,42,255);
    RL->FillGradientV(s.x,s.y,s.width,s.height,top2,K_BLACK);

    if(foc){
        RL->FillRoundRect(s.x+8,s.y+4,s.width-16,3.f,1.5f,1.5f,Fa(K_ACCENT,0.88f));
        RL->StrokeRoundRect(s.x,s.y,s.width,s.height,rx,rx,
                             1.5f,Fa(K_WHITE,0.46f+pulse*0.30f));
    } else {
        RL->StrokeRoundRect(s.x,s.y,s.width,s.height,rx,rx,1.f,Fa(K_WHITE,0.07f));
    }

    float isz=s.height*0.36f,iw=RL->MeasureTextA(icon,isz,400);
    RL->DrawTextA(icon,s.x+s.width/2-iw/2,s.y+s.height*0.18f,
                  isz,Fa(K_WHITE,foc?0.95f:0.38f),400);
    float tw=RL->MeasureTextA(title,12,400);
    RL->DrawTextA(title,s.x+s.width/2-tw/2,s.y+s.height*0.68f,
                  12.f,Fa(K_WHITE,foc?0.90f:0.32f),400);
    return true;
}

// ============================================================================
//  LIBRARY TAB
// ============================================================================
static bool DrawLibraryTab(int sw,int sh,int focusedIdx,float time)
{
    int count=HST->GetGameCount();
    if(count==0){
        const char* msg="No games in library";
        float mw=RL->MeasureTextA(msg,18,400);
        RL->DrawTextA(msg,(sw-mw)/2.f,sh/2.f-9,18.f,Fa(K_WHITE,0.28f),400);
        return true;
    }

    float heroW=(float)sw*0.36f;
    float heroH=heroW*(4.f/3.f);
    float maxH=(float)sh-TOP_H-HINT_H-INFO_H-40.f;
    if(heroH>maxH){heroH=maxH;heroW=heroH*(3.f/4.f);}
    float heroX=((float)sw-heroW)/2.f, heroY=TOP_H+10.f;

    QRect heroRect={heroX,heroY,heroW,heroH};
    D2DBitmapHandle noTex={};
    if(focusedIdx>=0&&focusedIdx<count){
        QShellGameInfo gi={};HST->GetGame(focusedIdx,&gi);
        DrawGameCard(heroRect,gi.name,true,noTex,time);
    }

    float peekW=heroW*0.48f,peekH=heroH*0.68f;
    float peekY=heroY+(heroH-peekH)/2.f;
    float fa2=Cl((time-s_bgFadeStart-0.15f)/0.40f,0.f,0.65f);

    if(focusedIdx>0){
        QShellGameInfo gp={};HST->GetGame(focusedIdx-1,&gp);
        QRect pr={heroX-peekW*0.62f,peekY,peekW,peekH};
        DrawGameCard(pr,gp.name,false,noTex,time);
        RL->FillGradientH(pr.x,pr.y,peekW,peekH,Fa(K_BLACK,fa2*0.88f+0.12f),Fa(K_BLACK,0.f));
    }
    if(focusedIdx<count-1){
        QShellGameInfo gn={};HST->GetGame(focusedIdx+1,&gn);
        QRect nr={heroX+heroW-peekW*0.38f,peekY,peekW,peekH};
        DrawGameCard(nr,gn.name,false,noTex,time);
        RL->FillGradientH(nr.x,nr.y,peekW,peekH,Fa(K_BLACK,0.f),Fa(K_BLACK,fa2*0.88f+0.12f));
    }

    {
        int ndots=count>8?8:count;
        float dotR=3.f,dotGap=11.f;
        float dotsW=(float)ndots*dotGap-dotGap+dotR*2;
        float dox=((float)sw-dotsW)/2.f,doy=heroY+heroH+14.f;
        int df=focusedIdx>ndots-1?ndots-1:focusedIdx;
        for(int i=0;i<ndots;i++)
            RL->FillCircle(dox+i*dotGap+dotR,doy+dotR,dotR,
                           i==df?Fa(K_WHITE,0.92f):Fa(K_WHITE,0.22f));
    }
    return true;
}

// ============================================================================
//  ENTRY POINT
// ============================================================================
QSHELL_PLUGIN_EXPORT
void RegisterPlugin(QShellPluginDesc* desc)
{
    RL =desc->rl; HST=desc->host;

    desc->name        = "PS5Station";
    desc->author      = "QShell";
    desc->version     = "7.0.0";
    desc->description = "Faithful PS5 home screen — correct card layout, no overlap, proper info panel";
    desc->isSkin      = true;

    desc->OnLoad           = OnLoad;
    desc->OnUnload         = OnUnload;
    desc->OnTick           = OnTick;
    desc->OnLibraryChanged = OnLibraryChanged;
    desc->DrawBackground   = DrawBackground;
    desc->DrawTopBar       = DrawTopBar;
    desc->DrawBottomBar    = DrawBottomBar;
    desc->DrawGameCard     = DrawGameCard;
    desc->DrawSettingsTile = DrawSettingsTile;
    desc->DrawLibraryTab   = DrawLibraryTab;
}
