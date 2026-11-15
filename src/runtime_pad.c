/* libScePad on SDL3 gamepads, with a keyboard and mouse fallback. SDL events are pumped
 * by the window thread (gpu/shim/window.cpp); here state is only sampled.
 *
 * Default keyboard layout (SF_KEY_MAP changes the buttons, see apply_button_map):
 *   WASD left stick (walk), Q L1 and E R1 (turn), arrow keys or the mouse right stick (camera),
 *   left click Square, right click Triangle, Space Cross, LShift Circle, R L2, F R2, Z L3, C R3,
 *   Enter Options, Tab and Backspace the touchpad click,
 *   IJKL d-pad (I up, K down, J left, L right).
 * SF_KEY_MAP also takes the mouse: Mouse Left, Mouse Right, Mouse Middle, Mouse Back,
 * Mouse Forward, Wheel Up and Wheel Down (see mouse_names). */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/mtgpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#include <SDL3/SDL.h>
#include <sys/stat.h>

#define ERR_INVALID_ARG ((int32_t)0x80920001)
#define ERR_INVALID_HANDLE ((int32_t)0x80920003)
#define ERR_ALREADY_OPENED ((int32_t)0x80920004)
#define ERR_NOT_INITIALIZED ((int32_t)0x80920005)
#define PAD_HANDLE 1
/* Guitar Hero Live's guitar: the "special" pad port, opened with the USB id of the game's
 * wireless receiver (scePadOpenExt). See the guitar section below. */
#define GUITAR_HANDLE 2
#define ERR_DEVICE_NOT_CONNECTED ((int32_t)0x80920007)
#define GUITAR_VENDOR 0x1430
#define GUITAR_PRODUCT 0x07bb
#define DEVICE_CLASS_GUITAR 1

enum {
    BTN_L3=0x2, BTN_R3=0x4, BTN_OPTIONS=0x8, BTN_UP=0x10, BTN_RIGHT=0x20, BTN_DOWN=0x40, BTN_LEFT=0x80,
    BTN_L2=0x100, BTN_R2=0x200, BTN_L1=0x400, BTN_R1=0x800, BTN_TRIANGLE=0x1000, BTN_CIRCLE=0x2000,
    BTN_CROSS=0x4000, BTN_SQUARE=0x8000, BTN_TOUCHPAD=0x100000,
    /* Host-only sources, never shown to the game: a click on one side of the touchpad from a
     * button or key without a touch surface (play_click turns them into a touch and a click). */
    CLICK_LEFT=0x40000000, CLICK_RIGHT=0x80000000,
    /* Host-only stick directions of the keyboard map. */
    MOVE_UP=0x00010000, MOVE_DOWN=0x00020000, MOVE_LEFT=0x00040000, MOVE_RIGHT=0x00080000,
    LOOK_UP=0x00200000, LOOK_DOWN=0x00400000, LOOK_LEFT=0x00800000, LOOK_RIGHT=0x01000000,
    STICK_DIRECTIONS=MOVE_UP|MOVE_DOWN|MOVE_LEFT|MOVE_RIGHT|LOOK_UP|LOOK_DOWN|LOOK_LEFT|LOOK_RIGHT,
};
typedef struct { uint16_t x, y; uint8_t id, reserve[3]; } PadTouch;
typedef struct {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t l2, r2, analog_padding[2];
    float orientation[4], acceleration[3], angular_velocity[3];
    uint8_t touch_count, touch_reserve[3];
    uint32_t touch_held_time;
    PadTouch touches[2];
    uint8_t connected, pad0[3];
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t connected_count, reserve[2], unique_length, unique[12];
} PadData;
typedef struct {
    float pixel_density; uint16_t resolution_x, resolution_y;
    uint8_t dead_zone_left, dead_zone_right, connection_type, connected_count;
    uint8_t connected, pad[3];
    int32_t device_class;
    uint8_t reserve[8];
} ControllerInfo;
_Static_assert(sizeof(PadData)==120,"OrbisPadData layout");
_Static_assert(sizeof(PadTouch)==8,"OrbisPadTouch layout");
_Static_assert(__builtin_offsetof(PadData,touches)==60,"OrbisPadData touch offset");
_Static_assert(__builtin_offsetof(PadData,timestamp)==80,"OrbisPadData timestamp offset");
_Static_assert(sizeof(ControllerInfo)==28,"OrbisPadControllerInformation layout");

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized, opened, guitar_opened, sdl_ready;
static SDL_Gamepad *gamepad;
static size_t reads;
static uint8_t connected_count;

static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u; }
static uint8_t axis(int16_t v) { int x=(v+32768)>>8; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint8_t trigger(int16_t v) { int x=v>>7; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint16_t touch_axis(float v, int max) {
    return (uint16_t)(v<=0.0f ? 0 : v>=1.0f ? max : (int)(v*max+0.5f));
}

/* A click without a touch surface (Back/View, Tab, Backspace, the pad file) is played like a
 * finger: the touch is reported CLICK_LEAD_US before the button, the button stays at least
 * CLICK_HOLD_US, and the touch lingers CLICK_TAIL_US after the release. SixFret opens
 * photo mode on the release of a left click with the finger still down, and ignores a touch
 * that comes and goes with the button. The side is chosen when the click starts and kept: the
 * right source with L1 held is a left click (photo mode on pads without a touchpad). */
enum { CLICK_LEAD_US=50000, CLICK_HOLD_US=50000, CLICK_TAIL_US=100000 };
static struct { int side; uint64_t pressed, released; } click={-1,0,0};
static void play_click(PadData *d) {
    const uint64_t now=d->timestamp;
    uint32_t sources=d->buttons & (CLICK_LEFT|CLICK_RIGHT);
    d->buttons&=~(CLICK_LEFT|CLICK_RIGHT);
    if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) { /* a replay, or a pad without fingers */
        d->buttons&=~BTN_TOUCHPAD;
        sources|=CLICK_LEFT;
    }
    const int side=(sources & CLICK_LEFT) ? 0 : (sources & CLICK_RIGHT) ? !(d->buttons & BTN_L1) : -1;
    if (side>=0 && (click.side<0 || click.released)) { click.side=side; click.pressed=now; click.released=0; }
    else if (side<0 && click.side>=0 && !click.released) click.released=now;
    if (click.side<0) return;
    const uint64_t down=click.pressed+CLICK_LEAD_US;
    uint64_t up=down+CLICK_HOLD_US;
    if (!click.released) up=UINT64_MAX;
    else if (click.released>up) up=click.released;
    if (now>=up && now-up>=CLICK_TAIL_US) { click.side=-1; return; }
    if (d->touch_count<2) {
        d->touches[d->touch_count]=(PadTouch){.x=click.side ? 1440 : 480,.y=471,.id=d->touch_count};
        ++d->touch_count;
    }
    if (now>=down && now<up) d->buttons|=BTN_TOUCHPAD;
}

/* Opens the first gamepad SDL knows about; called under lock. */
static SDL_Gamepad *current_gamepad(void) {
    if (!sdl_ready) sdl_ready = SDL_WasInit(SDL_INIT_GAMEPAD) ? 1 : SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : -1;
    if (sdl_ready<0) return NULL;
    if (gamepad && !SDL_GamepadConnected(gamepad)) { SDL_CloseGamepad(gamepad); gamepad=NULL; }
    if (!gamepad) {
        int count=0;
        SDL_JoystickID *ids=SDL_GetGamepads(&count);
        if (ids && count>0) {
            gamepad=SDL_OpenGamepad(ids[0]);
            if (gamepad) { ++connected_count; printf("Runtime: gamepad connected: %s\n",SDL_GetGamepadName(gamepad)); }
        }
        SDL_free(ids);
    }
    return gamepad;
}
/* Button sources: a host button or key and the PS4 button it presses. The defaults below can be
 * changed per PS4 button with SF_PAD_MAP (gamepad) and SF_KEY_MAP (keyboard), e.g.
 * SF_KEY_MAP="cross=Q,square=Left Shift" or SF_PAD_MAP="cross=b,circle=a": comma-separated
 * <ps4 button>=<SDL key or gamepad button name>. A mapped PS4 button loses its default sources;
 * <ps4 button>=None leaves it without any.
 * touchpad_left (alias touchpad) and touchpad_right are the two sides of the touchpad click:
 * photo mode and the pause menu. Back/View (Share on a DualSense) is the right click, and with
 * L1 held the left one. Keyboard stick directions are mapped too; gamepad axes, the real touchpad
 * click and the overlay shortcuts are unchanged. */
static const struct { const char *name; uint32_t ps; } ps_names[]={
    {"cross",BTN_CROSS}, {"circle",BTN_CIRCLE}, {"square",BTN_SQUARE}, {"triangle",BTN_TRIANGLE},
    {"l1",BTN_L1}, {"r1",BTN_R1}, {"l2",BTN_L2}, {"r2",BTN_R2}, {"l3",BTN_L3}, {"r3",BTN_R3},
    {"options",BTN_OPTIONS}, {"touchpad",CLICK_LEFT}, {"touchpad_left",CLICK_LEFT}, {"touchpad_right",CLICK_RIGHT},
    {"up",BTN_UP}, {"down",BTN_DOWN}, {"left",BTN_LEFT}, {"right",BTN_RIGHT},
    {"move_up",MOVE_UP}, {"move_down",MOVE_DOWN}, {"move_left",MOVE_LEFT}, {"move_right",MOVE_RIGHT},
    {"look_up",LOOK_UP}, {"look_down",LOOK_DOWN}, {"look_left",LOOK_LEFT}, {"look_right",LOOK_RIGHT},
};
typedef struct { int source; uint32_t ps; } ButtonSource;
typedef struct { ButtonSource entries[48]; size_t count; } ButtonMap;
static ButtonMap pad_map={{
    {SDL_GAMEPAD_BUTTON_SOUTH,BTN_CROSS}, {SDL_GAMEPAD_BUTTON_EAST,BTN_CIRCLE},
    {SDL_GAMEPAD_BUTTON_WEST,BTN_SQUARE}, {SDL_GAMEPAD_BUTTON_NORTH,BTN_TRIANGLE},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,BTN_L1}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,BTN_R1},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK,BTN_L3}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK,BTN_R3},
    {SDL_GAMEPAD_BUTTON_START,BTN_OPTIONS}, {SDL_GAMEPAD_BUTTON_BACK,CLICK_RIGHT},
    {SDL_GAMEPAD_BUTTON_TOUCHPAD,BTN_TOUCHPAD},
    {SDL_GAMEPAD_BUTTON_DPAD_UP,BTN_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN,BTN_DOWN},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT,BTN_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT,BTN_RIGHT},
},15};
/* Mouse sources of the keyboard map, numbered above every SDL scancode. Buttons are sampled like
 * keys; the wheel has no held state, so each notch becomes a short press (WHEEL_PRESS_US down,
 * then WHEEL_GAP_US up) that the game sees across several reads. */
enum { MOUSE_LEFT=0x10000, MOUSE_RIGHT, MOUSE_MIDDLE, MOUSE_BACK, MOUSE_FORWARD, WHEEL_UP, WHEEL_DOWN };
/* Guitar Hero Live's guitar on the keyboard (see the guitar section below for the game's bits):
 * A S D are the black frets 1-3 (Cross, Circle, Triangle), Z X C the white frets under them
 * (Square, L1, R1); the Up and Down arrows or Right Shift strum (left stick Y), Left and Right
 * bend the whammy bar; I J K L are the guitar's direction knob for menus; Return pauses, Space is
 * the Hero Power button (L3) and G the GHTV button (R3). */
static ButtonMap key_map={{
    {SDL_SCANCODE_A,BTN_CROSS}, {SDL_SCANCODE_S,BTN_CIRCLE}, {SDL_SCANCODE_D,BTN_TRIANGLE},
    {SDL_SCANCODE_Z,BTN_SQUARE}, {SDL_SCANCODE_X,BTN_L1}, {SDL_SCANCODE_C,BTN_R1},
    {SDL_SCANCODE_SPACE,BTN_L3}, {SDL_SCANCODE_G,BTN_R3},
    {SDL_SCANCODE_RETURN,BTN_OPTIONS}, {SDL_SCANCODE_TAB,CLICK_LEFT}, {SDL_SCANCODE_BACKSPACE,CLICK_RIGHT},
    {SDL_SCANCODE_I,BTN_UP}, {SDL_SCANCODE_K,BTN_DOWN}, {SDL_SCANCODE_J,BTN_LEFT}, {SDL_SCANCODE_L,BTN_RIGHT},
    {SDL_SCANCODE_UP,MOVE_UP}, {SDL_SCANCODE_DOWN,MOVE_DOWN}, {SDL_SCANCODE_RSHIFT,MOVE_DOWN},
    {SDL_SCANCODE_LEFT,LOOK_LEFT}, {SDL_SCANCODE_RIGHT,LOOK_RIGHT},
},20};

static char *trim(char *text) {
    while (*text==' ' || *text=='\t') ++text;
    char *end=text+strlen(text);
    while (end>text && (end[-1]==' ' || end[-1]=='\t' || end[-1]=='\r' || end[-1]=='\n')) *--end=0;
    return text;
}

enum { WHEEL_PRESS_US=60000, WHEEL_GAP_US=40000, WHEEL_MAX_PENDING=4 };
static const struct { const char *name; int source; } mouse_names[]={
    {"Mouse Left",MOUSE_LEFT}, {"Mouse Right",MOUSE_RIGHT}, {"Mouse Middle",MOUSE_MIDDLE},
    {"Mouse Back",MOUSE_BACK}, {"Mouse Forward",MOUSE_FORWARD}, {"Wheel Up",WHEEL_UP}, {"Wheel Down",WHEEL_DOWN},
};
static int mouse_source(const char *name) {
    for (size_t i=0;i<sizeof(mouse_names)/sizeof(*mouse_names);++i)
        if (!SDL_strcasecmp(name,mouse_names[i].name)) return mouse_names[i].source;
    return 0;
}

static SDL_AtomicInt wheel_ticks[2]; /* notches not yet played: 0 up, 1 down */
static struct { int phase; uint64_t until; } wheel_pulse[2]; /* phase 0 idle, 1 pressed, 2 gap */
static SDL_AtomicInt mouse_motion[2]; /* x, y moved since the last read, in 1/16 px (mouse look) */
/* Runs on the window thread for every queued event (SDL_AddEventWatch). */
static bool SDLCALL mouse_watch(void *userdata, SDL_Event *event) {
    (void)userdata;
    if (event->type==SDL_EVENT_MOUSE_MOTION) {
        SDL_AddAtomicInt(&mouse_motion[0],(int)(event->motion.xrel*16.0f));
        SDL_AddAtomicInt(&mouse_motion[1],(int)(event->motion.yrel*16.0f));
        return true;
    }
    if (event->type!=SDL_EVENT_MOUSE_WHEEL) return true;
    int steps=event->wheel.integer_y;
    if (event->wheel.direction==SDL_MOUSEWHEEL_FLIPPED) steps=-steps;
    SDL_AtomicInt *ticks=&wheel_ticks[steps<0];
    for (int n=steps<0 ? -steps : steps; n>0 && SDL_GetAtomicInt(ticks)<WHEEL_MAX_PENDING; --n) SDL_AddAtomicInt(ticks,1);
    return true;
}
static void watch_wheel(void) {
    static bool watching;
    if (watching || !SDL_WasInit(SDL_INIT_VIDEO)) return;
    watching=SDL_AddEventWatch(mouse_watch,NULL);
}
static void drop_wheel(void) {
    for (int dir=0;dir<2;++dir) {
        SDL_SetAtomicInt(&wheel_ticks[dir],0); wheel_pulse[dir].phase=0;
        SDL_SetAtomicInt(&mouse_motion[dir],0);
    }
}
/* Whether the wheel's direction `dir` is held at `now`; call once per read for each direction. */
static bool wheel_held(int dir, uint64_t now) {
    if (wheel_pulse[dir].phase==1 && now>=wheel_pulse[dir].until) {
        wheel_pulse[dir].phase=2; wheel_pulse[dir].until=now+WHEEL_GAP_US;
    }
    if (wheel_pulse[dir].phase==2 && now>=wheel_pulse[dir].until) wheel_pulse[dir].phase=0;
    if (wheel_pulse[dir].phase==0 && SDL_GetAtomicInt(&wheel_ticks[dir])>0) {
        SDL_AddAtomicInt(&wheel_ticks[dir],-1);
        wheel_pulse[dir].phase=1; wheel_pulse[dir].until=now+WHEEL_PRESS_US;
    }
    return wheel_pulse[dir].phase==1;
}
/* Whether a keyboard-map source (a scancode or a mouse source) is down. */
static bool source_down(int source, const bool *keys, SDL_MouseButtonFlags mouse, const bool wheel[2]) {
    switch (source) {
    case MOUSE_LEFT: return mouse & SDL_BUTTON_LMASK;
    case MOUSE_RIGHT: return mouse & SDL_BUTTON_RMASK;
    case MOUSE_MIDDLE: return mouse & SDL_BUTTON_MMASK;
    case MOUSE_BACK: return mouse & SDL_BUTTON_X1MASK;
    case MOUSE_FORWARD: return mouse & SDL_BUTTON_X2MASK;
    case WHEEL_UP: return wheel[0];
    case WHEEL_DOWN: return wheel[1];
    default: return source>0 && source<SDL_SCANCODE_COUNT && keys[source];
    }
}

/* Scancode of a key name: SDL's names, plus Comma and Equals for the two keys whose own names
 * (',' and '=') are separators of SF_KEY_MAP. */
static int key_source(const char *name) {
    if (!SDL_strcasecmp(name,"Comma")) return SDL_SCANCODE_COMMA;
    if (!SDL_strcasecmp(name,"Equals")) return SDL_SCANCODE_EQUALS;
    return (int)SDL_GetScancodeFromName(name);
}

/* Applies a SF_PAD_MAP/SF_KEY_MAP value; returns how many entries were used. Bad entries are
 * logged and skipped, so their PS4 button keeps its default. */
static int apply_button_map(ButtonMap *map, const char *spec, bool keyboard) {
    if (!spec || !*spec) return 0;
    char copy[1024];
    snprintf(copy,sizeof(copy),"%s",spec);
    int used=0;
    char *entry=copy;
    while (entry) {
        char *next=strchr(entry,',');
        if (next) *next++=0;
        char *eq=strchr(entry,'=');
        if (!eq) {
            if (*trim(entry)) printf("Runtime: pad map: ignored '%s' (expected button=name)\n",trim(entry));
            entry=next;
            continue;
        }
        *eq=0;
        const char *button=trim(entry), *name=trim(eq+1);
        entry=next;
        uint32_t ps=0;
        for (size_t i=0;i<sizeof(ps_names)/sizeof(*ps_names);++i) if (!SDL_strcasecmp(button,ps_names[i].name)) ps=ps_names[i].ps;
        if (!keyboard && (ps & STICK_DIRECTIONS)) ps=0;
        if (ps && !SDL_strcasecmp(name,"None")) { /* the button gets no source at all */
            size_t kept=0;
            for (size_t i=0;i<map->count;++i) if (map->entries[i].ps!=ps) map->entries[kept++]=map->entries[i];
            map->count=kept;
            ++used;
            continue;
        }
        const int mouse=keyboard && *name ? mouse_source(name) : 0;
        const int source=!*name ? -1 : mouse ? mouse : keyboard ? key_source(name)
                                                         : (int)SDL_GetGamepadButtonFromString(name);
        if (!ps || source<=(keyboard ? (int)SDL_SCANCODE_UNKNOWN : (int)SDL_GAMEPAD_BUTTON_INVALID)) {
            printf("Runtime: pad map: ignored '%s=%s'\n",button,name);
            continue;
        }
        size_t kept=0;
        for (size_t i=0;i<map->count;++i) if (map->entries[i].ps!=ps) map->entries[kept++]=map->entries[i];
        map->count=kept;
        if (map->count<sizeof(map->entries)/sizeof(*map->entries)) map->entries[map->count++]=(ButtonSource){source,ps};
        ++used;
    }
    return used;
}

static void load_button_maps(void) {
    static bool loaded;
    if (loaded) return;
    loaded=true;
    const int pad=apply_button_map(&pad_map,getenv("SF_PAD_MAP"),false);
    const int keys=apply_button_map(&key_map,getenv("SF_KEY_MAP"),true);
    if (pad || keys) printf("Runtime: pad map: %d gamepad and %d keyboard buttons remapped\n",pad,keys);
}

static void key_axis(uint8_t *axis, bool negative, bool positive) {
    if (negative || positive) *axis=(uint8_t)(128-(negative ? 128 : 0)+(positive ? 127 : 0));
}
static void sample_keyboard(PadData *d, const bool *keys, SDL_MouseButtonFlags mouse, const bool wheel[2]) {
    uint32_t pressed=0;
    for (size_t i=0;i<key_map.count;++i)
        if (source_down(key_map.entries[i].source,keys,mouse,wheel)) pressed|=key_map.entries[i].ps;
    d->buttons|=pressed & ~STICK_DIRECTIONS;
    if (d->buttons & BTN_L2) d->l2=255;
    if (d->buttons & BTN_R2) d->r2=255;
    key_axis(&d->left_x,pressed & MOVE_LEFT,pressed & MOVE_RIGHT);
    key_axis(&d->left_y,pressed & MOVE_UP,pressed & MOVE_DOWN);
    key_axis(&d->right_x,pressed & LOOK_LEFT,pressed & LOOK_RIGHT);
    key_axis(&d->right_y,pressed & LOOK_UP,pressed & LOOK_DOWN);
}

static void sample_host(PadData *d) {
    memset(d,0,sizeof(*d));
    d->left_x=d->left_y=d->right_x=d->right_y=128;
    d->orientation[3]=1.0f;
    d->connected=1; d->connected_count=connected_count ? connected_count : 1;
    d->timestamp=now_us();
    SDL_Gamepad *g=current_gamepad();
    watch_wheel();
    if (mtgpu_overlay_captures_input()) { drop_wheel(); return; } /* settings menu open: neutral input */
    const bool *k=SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(NULL) : NULL;
    load_button_maps();
    if (g) {
        for (size_t i=0;i<pad_map.count;++i)
            if (SDL_GetGamepadButton(g,(SDL_GamepadButton)pad_map.entries[i].source)) d->buttons|=pad_map.entries[i].ps;
        const uint32_t mapped=d->buttons;
        d->left_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTX)); d->left_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTY));
        d->right_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTX)); d->right_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTY));
        d->l2=trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFT_TRIGGER)); d->r2=trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
        if (d->l2>30) d->buttons|=BTN_L2;
        if (d->r2>30) d->buttons|=BTN_R2;
        if (mapped & BTN_L2) d->l2=255;
        if (mapped & BTN_R2) d->r2=255;
        if (SDL_GetNumGamepadTouchpads(g)>0) {
            const int fingers=SDL_GetNumGamepadTouchpadFingers(g,0);
            for (int finger=0;finger<fingers && d->touch_count<2;++finger) {
                bool down=false;
                float x=0, y=0;
                if (SDL_GetGamepadTouchpadFinger(g,0,finger,&down,&x,&y,NULL) && down) {
                    d->touches[d->touch_count++]=(PadTouch){.x=touch_axis(x,1919),
                        .y=touch_axis(y,942),.id=(uint8_t)finger};
                }
            }
        }
    }
    /* The keyboard works with or without a gamepad: its buttons add to the pad's, and a stick
     * follows the keys only while one of them is held. */
    if (!k) return;
    const SDL_MouseButtonFlags mouse=SDL_GetMouseState(NULL,NULL);
    const bool wheel[2]={wheel_held(0,d->timestamp),wheel_held(1,d->timestamp)};
    sample_keyboard(d,k,mouse,wheel);
}

/* SF_PAD_FILE=<file>: scripted input for automated runs. The file holds whitespace-separated
 * tokens, re-read when it changes: button names (cross circle square triangle l1 r1 l2 r2 l3 r3
 * options touchpad touchpad_left touchpad_right up down left right) are held while listed;
 * touchpad is the left side; lx= ly= rx= ry= (0..255) override the sticks. An empty file
 * releases everything. */
static struct { uint32_t buttons; int stick[4]; } injected={0,{-1,-1,-1,-1}};
static int replay_armed;      /* 1 while a SF_PAD_REPLAY recording plays, 2 once it ended */
static uint64_t replay_start; /* 0: (re)start at the next sample */
static void read_inject(void) {
    static const char *path; static int checked; static uint64_t last_check; static struct timespec mtime;
    if (!checked) { path=getenv("SF_PAD_FILE"); checked=1; }
    if (!path || !*path) return;
    uint64_t now=now_us();
    if (now-last_check<20000) return;
    last_check=now;
    struct stat st;
    if (stat(path,&st)!=0) return;
#ifdef _WIN32
    /* Whole-second mtimes: the size tells two edits within a second apart. */
    if (st.st_mtime==mtime.tv_sec && st.st_size==mtime.tv_nsec) return;
    mtime=(struct timespec){st.st_mtime,(long)st.st_size};
#else
    if (st.st_mtim.tv_sec==mtime.tv_sec && st.st_mtim.tv_nsec==mtime.tv_nsec) return;
    mtime=st.st_mtim;
#endif
    FILE *f=fopen(path,"r");
    if (!f) return;
    static const char *sticks[]={"lx=","ly=","rx=","ry="};
    injected.buttons=0;
    for (int i=0;i<4;++i) injected.stick[i]=-1;
    char token[64];
    while (fscanf(f,"%63s",token)==1) {
        if (!strcmp(token,"replay") && replay_armed!=1) { replay_armed=1; replay_start=0; } /* SF_PAD_REPLAY */
        for (size_t i=0;i<sizeof(ps_names)/sizeof(*ps_names);++i) if (!strcmp(token,ps_names[i].name)) injected.buttons|=ps_names[i].ps;
        for (int i=0;i<4;++i) if (!strncmp(token,sticks[i],3)) { int v=atoi(token+3); injected.stick[i]=v<0 ? 0 : v>255 ? 255 : v; }
    }
    fclose(f);
    printf("Runtime: pad file: buttons 0x%x sticks %d %d %d %d\n",injected.buttons,
           injected.stick[0],injected.stick[1],injected.stick[2],injected.stick[3]);
}
/* SF_PAD_RECORD=<file>: F9 starts and stops recording the pad state (gamepad or keyboard) with
 * the time since F9; SF_PAD_REPLAY=<file> plays such a recording back, started by the token
 * "replay" in SF_PAD_FILE (scripted tests repeat a route the player ran once). Lines: ms buttons
 * lx ly rx ry l2 r2, written when the state changes. */
typedef struct { uint32_t ms, buttons; uint8_t axes[4], l2, r2; } PadSample;
static FILE *record_file;
static uint64_t record_start;
static PadSample record_last;
static void record_sample(const PadData *d) {
    static const char *path; static int checked, f9_was_down;
    if (!checked) { path=getenv("SF_PAD_RECORD"); checked=1; }
    if (!path || !*path || !sdl_ready) return;
    const bool *k=SDL_GetKeyboardState(NULL);
    const int f9=k && k[SDL_SCANCODE_F9];
    if (f9 && !f9_was_down) {
        if (record_file) {
            fclose(record_file); record_file=NULL;
            printf("Runtime: pad recording stopped (%s)\n",path);
        } else if ((record_file=fopen(path,"w"))) {
            record_start=now_us();
            memset(&record_last,0xff,sizeof(record_last));
            printf("Runtime: pad recording started (%s, F9 stops)\n",path);
        }
    }
    f9_was_down=f9;
    if (!record_file) return;
    PadSample s={(uint32_t)((now_us()-record_start)/1000),d->buttons,
                 {d->left_x,d->left_y,d->right_x,d->right_y},d->l2,d->r2};
    if (s.buttons==record_last.buttons && !memcmp(s.axes,record_last.axes,4) &&
        s.l2==record_last.l2 && s.r2==record_last.r2) return;
    record_last=s;
    fprintf(record_file,"%u %u %u %u %u %u %u %u\n",s.ms,s.buttons,s.axes[0],s.axes[1],s.axes[2],
            s.axes[3],s.l2,s.r2);
    fflush(record_file);
}
static PadSample *replay; static size_t replay_count, replay_next;
static void replay_sample(PadData *d) {
    if (!replay_armed) return;
    if (!replay_start) {
        static int loaded;
        if (!loaded) {
            loaded=1;
            const char *path=getenv("SF_PAD_REPLAY");
            FILE *f=path ? fopen(path,"r") : NULL;
            PadSample s; unsigned v[8]; size_t cap=0;
            while (f && fscanf(f,"%u %u %u %u %u %u %u %u",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7])==8) {
                s=(PadSample){v[0],v[1],{(uint8_t)v[2],(uint8_t)v[3],(uint8_t)v[4],(uint8_t)v[5]},(uint8_t)v[6],(uint8_t)v[7]};
                if (replay_count==cap && !(replay=realloc(replay,(cap=cap ? cap*2 : 1024)*sizeof(*replay)))) break;
                replay[replay_count++]=s;
            }
            if (f) fclose(f);
            printf("Runtime: pad replay of %zu samples from %s\n",replay_count,path ? path : "(unset)");
        }
        replay_start=now_us();
        replay_next=0;
    }
    const uint32_t ms=(uint32_t)((now_us()-replay_start)/1000);
    while (replay_next<replay_count && replay[replay_next].ms<=ms) ++replay_next;
    if (!replay_next) return;
    if (replay_next==replay_count && ms>replay[replay_count-1].ms+500) {
        if (replay_armed==1) { puts("Runtime: pad replay finished"); replay_armed=2; }
        return;
    }
    const PadSample *s=&replay[replay_next-1];
    d->buttons=s->buttons;
    d->left_x=s->axes[0]; d->left_y=s->axes[1]; d->right_x=s->axes[2]; d->right_y=s->axes[3];
    d->l2=s->l2; d->r2=s->r2;
}
/* Mouse look: moving the mouse pushes the right stick (the camera) the way it moves, as far as it
 * moves fast (full tilt at LOOK_FULL_PX_PER_MS); when the mouse stops the stick is left to the pad
 * and the keys again. A direction holds LOOK_HOLD_US after the last movement, over the gaps between
 * mouse reports. The menu's sensitivity scales the speed. Absolute style points the stick from where
 * the mouse was when it started moving instead (full tilt LOOK_RADIUS_PX away at 100 %). */
enum { LOOK_HOLD_US=40000, LOOK_STEP_MIN_US=1000, LOOK_STEP_MAX_US=50000, LOOK_RADIUS_PX=200 };
static const float LOOK_FULL_PX_PER_MS=1.2f;
static struct { uint64_t last, moved; int x, y; float offset_x, offset_y; } look;
static void apply_mouse_look(PadData *d) {
    const float dx=SDL_SetAtomicInt(&mouse_motion[0],0)/16.0f, dy=SDL_SetAtomicInt(&mouse_motion[1],0)/16.0f;
    uint64_t step=d->timestamp-look.last;
    look.last=d->timestamp;
    const float sensitivity=mtgpu_mouse_aim_sensitivity()/100.0f;
    if (mtgpu_mouse_aim_style()==1) {
        const float radius=LOOK_RADIUS_PX/sensitivity;
        if (dx==0 && dy==0 && look.moved && d->timestamp-look.moved>=LOOK_HOLD_US) look.offset_x=look.offset_y=0;
        look.offset_x+=dx; look.offset_y+=dy;
        const float length=sqrtf(look.offset_x*look.offset_x+look.offset_y*look.offset_y);
        if (length>radius) { look.offset_x*=radius/length; look.offset_y*=radius/length; }
        if (dx!=0 || dy!=0) look.moved=d->timestamp;
        if (look.offset_x!=0 || look.offset_y!=0) {
            d->right_x=(uint8_t)(128+(int)(look.offset_x*127/radius));
            d->right_y=(uint8_t)(128+(int)(look.offset_y*127/radius));
        }
        return;
    }
    if (dx!=0 || dy!=0) {
        step=step<LOOK_STEP_MIN_US ? LOOK_STEP_MIN_US : step>LOOK_STEP_MAX_US ? LOOK_STEP_MAX_US : step;
        const float scale=127.0f*1000.0f*sensitivity/(LOOK_FULL_PX_PER_MS*(float)step);
        const float x=dx*scale, y=dy*scale;
        look.x=x>127 ? 127 : x<-128 ? -128 : (int)x;
        look.y=y>127 ? 127 : y<-128 ? -128 : (int)y;
        look.moved=d->timestamp;
    }
    if (look.moved && d->timestamp-look.moved<LOOK_HOLD_US) {
        d->right_x=(uint8_t)(128+look.x);
        d->right_y=(uint8_t)(128+look.y);
    }
}

/* The pause key (touchpad right without L1, which is photo mode) lets go of the window's mouse
 * lock while the game's pause menu is up, and takes it back (gpu/shim/window.cpp). */
static void note_pause_key(const PadData *d) {
    static bool held;
    const bool pause=(d->buttons & CLICK_RIGHT) && !(d->buttons & BTN_L1);
    if (pause && !held) mtgpu_mouse_pause_key();
    held=pause;
}

/* After the menu or the text dialog closes, buttons still held (the Cross that accepted a
 * name) stay hidden until released: the game would take them as a new press. */
static int hold_after_capture;
static void sample(PadData *d) {
    sample_host(d);
    if (mtgpu_overlay_captures_input()) { hold_after_capture=1; click.side=-1; return; }
    record_sample(d); /* the click sources as they are: a replay plays them again */
    read_inject();
    replay_sample(d);
    d->buttons|=injected.buttons;
    if (injected.buttons & BTN_L2) d->l2=255;
    if (injected.buttons & BTN_R2) d->r2=255;
    uint8_t *axes[4]={&d->left_x,&d->left_y,&d->right_x,&d->right_y};
    for (int i=0;i<4;++i) if (injected.stick[i]>=0) *axes[i]=(uint8_t)injected.stick[i];
    apply_mouse_look(d);
    note_pause_key(d);
    play_click(d);
    if (hold_after_capture) {
        if (d->buttons) d->buttons=0;
        else hold_after_capture=0;
    }
}

static ABI int32_t pad_init(void) { pthread_mutex_lock(&lock); initialized=1; pthread_mutex_unlock(&lock); return 0; }
static ABI int32_t pad_open(int32_t user, int32_t type, int32_t index, const void *param) {
    (void)param;
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (user!=1) return ERR_INVALID_ARG;
    if (type!=0) return ERR_INVALID_ARG; /* standard port; the special port is the guitar's (scePadOpenExt) */
    if (index) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    int already=opened; opened=1;
    pthread_mutex_unlock(&lock);
    if (already) return ERR_ALREADY_OPENED;
    puts("Runtime: pad opened for user 1 (SDL gamepad or keyboard)");
    return PAD_HANDLE;
}
static ABI int32_t pad_close(int32_t handle) {
    if (handle==GUITAR_HANDLE && guitar_opened) { guitar_opened=0; return 0; }
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    opened=0; return 0;
}

/* ---- The Guitar Hero Live guitar ----
 * The game opens it with scePadOpenExt(user, 2 (the special port), 0, {vendor 0x1430, product
 * 0x07bb}), requires scePadGetControllerInformation to report device class 1 (guitar) while it is
 * connected, and reads it with scePadReadState (its update function is at 0xcb6c80 in 01.08):
 *   frets   black 1-3: Cross, Circle, Triangle; white 1-3: Square, L1, R1
 *   strum   left stick Y at exactly 0x00 (up) or 0xff (down), 0x80 at rest
 *   d-pad   the guitar's direction knob (menus); Options: pause; L3: Hero Power button;
 *           R3: the GHTV button
 *   whammy  right stick X, tilt: right stick Y (0x80 at rest)
 * A real receiver is read directly (above); without one, or beside it, the guitar is played from
 * the keyboard or a gamepad through the same mapping as the standard pad: the buttons are the game's own bits, the left stick (or the
 * d-pad's up and down, with SF_GUITAR_DPAD_STRUM=1) strums. */
/* ---- A real guitar: the game's wireless receiver, read over USB HID (SDL's hidapi) ----
 * The PS4 receiver (1430:07bb) and the PS3 / Wii U one (12ba:074b) send the same report:
 *   byte 0  frets: 01 white 1, 02 black 1, 04 black 2, 08 black 3, 10 white 2, 20 white 3
 *   byte 1  01 Hero Power, 02 pause, 04 GHTV, 10 the console button
 *   byte 2  the direction knob as a hat (0 up, clockwise to 7; 8 or 15 at rest)
 *   byte 4  strum bar (00 up, 80 rest, ff down), byte 6 whammy bar (80 rest to ff)
 *   byte 19 tilt
 * Both stop sending unless they are poked every few seconds: the PS4 one with the report the
 * game itself sends through scePadOutputReport (30 02 08 ..), which is passed on, the PS3 one
 * with 02 08 20 00... The layout and the pokes are those of Linux's hid-sony driver and the
 * Clone Hero wiki. SF_GUITAR_HID=0 leaves the receiver alone; SF_GUITAR_TRACE=1 prints each
 * report that differs from the last. */
enum { GHL_NONE, GHL_PS4, GHL_PS3 };
static struct {
    SDL_hid_device *device;
    int kind, tried_init, said_missing;
    uint64_t next_open, next_poke;
    uint8_t report[64];
    int size;
} receiver;
static int guitar_env(const char *name, int otherwise) {
    const char *t=getenv(name);
    return t && *t ? *t!='0' : otherwise;
}
static void receiver_close(const char *why) {
    if (!receiver.device) return;
    printf("Runtime: guitar receiver %s\n",why);
    SDL_hid_close(receiver.device);
    receiver.device=NULL; receiver.kind=GHL_NONE; receiver.size=0;
}
static void receiver_poke(uint64_t now) {
    static const uint8_t ps4[9]={0x30,0x02,0x08,0x0a,0,0,0,0,0};
    static const uint8_t ps3[9]={0x00,0x02,0x08,0x20,0,0,0,0,0}; /* no report number: hidapi's leading 0 */
    if (!receiver.device || now<receiver.next_poke) return;
    receiver.next_poke=now+4000000;
    const uint8_t *data=receiver.kind==GHL_PS4 ? ps4 : ps3;
    if (SDL_hid_write(receiver.device,data,9)<0 && receiver.kind==GHL_PS3) {
        uint8_t feature[9]; memcpy(feature,ps3,9);
        SDL_hid_send_feature_report(receiver.device,feature,9); /* drivers that take it as a control transfer only */
    }
}
static void receiver_open(uint64_t now) {
    static const struct { uint16_t vendor, product; int kind; const char *name; } known[]={
        {0x1430,0x07bb,GHL_PS4,"PS4"}, {0x12ba,0x074b,GHL_PS3,"PS3 / Wii U"},
    };
    if (receiver.device || now<receiver.next_open || !guitar_env("SF_GUITAR_HID",1)) return;
    receiver.next_open=now+2000000;
    if (!receiver.tried_init) { receiver.tried_init=1; if (SDL_hid_init()) { receiver.next_open=UINT64_MAX; return; } }
    for (size_t i=0;i<sizeof(known)/sizeof(*known);++i) {
        SDL_hid_device *device=SDL_hid_open(known[i].vendor,known[i].product,NULL);
        if (!device) continue;
        SDL_hid_set_nonblocking(device,1);
        receiver.device=device; receiver.kind=known[i].kind; receiver.next_poke=0; receiver.size=0;
        printf("Runtime: Guitar Hero Live receiver found (%s, %04x:%04x): the guitar plays the game\n",
               known[i].name,known[i].vendor,known[i].product);
        receiver_poke(now);
        return;
    }
    if (!receiver.said_missing) {
        receiver.said_missing=1;
        puts("Runtime: no Guitar Hero Live receiver plugged in (keyboard or gamepad plays the guitar; it is picked up when plugged in)");
    }
}
/* The newest report of the receiver over the keyboard's and gamepad's sample. */
static void receiver_sample(PadData *d, uint8_t *strum) {
    const uint64_t now=now_us();
    receiver_open(now);
    if (!receiver.device) return;
    receiver_poke(now);
    uint8_t report[64];
    for (;;) {
        int n=SDL_hid_read(receiver.device,report,sizeof(report));
        if (n<0) { receiver_close("unplugged"); return; }
        if (n==0) break;
        if (n<7) continue;
        if (guitar_env("SF_GUITAR_TRACE",0) && (n!=receiver.size || memcmp(report,receiver.report,(size_t)n))) {
            printf("Guitar trace:");
            for (int i=0;i<n && i<27;++i) printf(" %02x",report[i]);
            putchar('\n');
        }
        memcpy(receiver.report,report,(size_t)n); receiver.size=n;
    }
    if (receiver.size<7) return;
    const uint8_t *r=receiver.report;
    if (r[0]&0x01) d->buttons|=BTN_SQUARE;
    if (r[0]&0x02) d->buttons|=BTN_CROSS;
    if (r[0]&0x04) d->buttons|=BTN_CIRCLE;
    if (r[0]&0x08) d->buttons|=BTN_TRIANGLE;
    if (r[0]&0x10) d->buttons|=BTN_L1;
    if (r[0]&0x20) d->buttons|=BTN_R1;
    if (r[1]&0x01) d->buttons|=BTN_L3;      /* Hero Power */
    if (r[1]&0x02) d->buttons|=BTN_OPTIONS; /* pause */
    if (r[1]&0x04) d->buttons|=BTN_R3;      /* GHTV */
    static const uint32_t hat[8]={BTN_UP,BTN_UP|BTN_RIGHT,BTN_RIGHT,BTN_RIGHT|BTN_DOWN,BTN_DOWN,BTN_DOWN|BTN_LEFT,
                                  BTN_LEFT,BTN_LEFT|BTN_UP};
    if ((r[2]&0x0f)<8) d->buttons|=hat[r[2]&0x0f];
    if (r[4]<0x40) *strum=0x00; else if (r[4]>0xc0) *strum=0xff;
    d->right_x=r[6];
    if (receiver.size>19) d->right_y=r[19];
}
/* An Xbox 360 Guitar Hero Live guitar is an ordinary XInput pad whose strum bar is the d-pad. */
static int gamepad_is_guitar(void) {
    const char *name=gamepad ? SDL_GetGamepadName(gamepad) : NULL;
    return name && (SDL_strcasestr(name,"guitar")!=NULL);
}
static int guitar_dpad_strum(void) {
    static int on=-1;
    if (on<0) { const char *t=getenv("SF_GUITAR_DPAD_STRUM"); on=t && *t=='1'; }
    return on;
}
static void guitar_sample(PadData *d) {
    sample(d);
    uint8_t strum=0x80;
    if (d->left_y<0x40) strum=0x00; else if (d->left_y>0xc0) strum=0xff;
    if (guitar_dpad_strum() || gamepad_is_guitar()) {
        if (d->buttons & BTN_UP) strum=0x00; else if (d->buttons & BTN_DOWN) strum=0xff;
        d->buttons&=~(uint32_t)(BTN_UP|BTN_DOWN);
    }
    receiver_sample(d,&strum);
    d->left_x=0x80; d->left_y=strum;
    d->l2=d->r2=0; d->buttons&=~(uint32_t)(BTN_L2|BTN_R2|BTN_TOUCHPAD);
    d->touch_count=0;
    d->connected=1;
}
static ABI int32_t pad_read_state(int32_t handle, PadData *data) {
    if (handle==GUITAR_HANDLE && guitar_opened) {
        if (!data) return ERR_INVALID_ARG;
        pthread_mutex_lock(&lock);
        guitar_sample(data); ++reads;
        pthread_mutex_unlock(&lock);
        return 0;
    }
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!data) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    sample(data); ++reads;
    pthread_mutex_unlock(&lock);
    return 0;
}
typedef struct { uint16_t vendor, product, product2; uint8_t reserve[10]; } PadOpenExtParam;
static ABI int32_t pad_open_ext(int32_t user, int32_t type, int32_t index, const PadOpenExtParam *param) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (user!=1 || index) return ERR_INVALID_ARG;
    if (type!=2 || !param) return ERR_INVALID_ARG;
    if (param->vendor!=GUITAR_VENDOR || param->product!=GUITAR_PRODUCT) {
        printf("Runtime: special pad %04x:%04x is not a device this port provides\n",param->vendor,param->product);
        return ERR_DEVICE_NOT_CONNECTED;
    }
    /* SF_GUITAR=0: no guitar, as on a console with only a DualShock 4 (the game then asks for one). */
    const char *wanted=getenv("SF_GUITAR");
    if (wanted && *wanted=='0') { puts("Runtime: guitar not connected (SF_GUITAR=0)"); return ERR_DEVICE_NOT_CONNECTED; }
    pthread_mutex_lock(&lock);
    int already=guitar_opened; guitar_opened=1;
    pthread_mutex_unlock(&lock);
    if (already) return ERR_ALREADY_OPENED;
    puts("Runtime: guitar opened for user 1 (special pad 1430:07bb; keyboard or gamepad plays it)");
    return GUITAR_HANDLE;
}
/* Reports to the receiver: 30 02 08 <n> ... and 30 01 08 <n> ... (9 bytes), the game's keep-alive
 * and player light. A PS4 receiver gets them (the keep-alive it needs). */
static ABI int32_t pad_output_report(int32_t handle, int32_t report, const uint8_t *data, int32_t size) {
    if (handle!=GUITAR_HANDLE || !guitar_opened) return ERR_INVALID_HANDLE;
    if (!data || size<1) return ERR_INVALID_ARG;
    (void)report;
    /* The PS4 receiver takes the game's reports as they are (the report number leads the data). */
    pthread_mutex_lock(&lock);
    if (receiver.device && receiver.kind==GHL_PS4 && size<=64) {
        if (SDL_hid_write(receiver.device,data,(size_t)size)<0) receiver_close("stopped answering");
        else receiver.next_poke=now_us()+4000000;
    }
    pthread_mutex_unlock(&lock);
    return 0;
}
/* Buffered read: the port samples once per call, so one entry is returned. */
static ABI int32_t pad_read(int32_t handle, PadData *data, int32_t count) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!data || count<1 || count>64) return ERR_INVALID_ARG;
    pad_read_state(handle,data);
    return 1;
}
static ABI int32_t pad_info(int32_t handle, ControllerInfo *info) {
    if (handle==GUITAR_HANDLE && guitar_opened) {
        if (!info) return ERR_INVALID_ARG;
        memset(info,0,sizeof(*info));
        info->connection_type=2; /* through a USB receiver */
        info->connected=1; info->connected_count=1; info->device_class=DEVICE_CLASS_GUITAR;
        return 0;
    }
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!info) return ERR_INVALID_ARG;
    memset(info,0,sizeof(*info));
    info->pixel_density=44.86f; info->resolution_x=1920; info->resolution_y=943;
    info->dead_zone_left=info->dead_zone_right=2;
    info->connection_type=0; info->connected=1; info->device_class=0;
    pthread_mutex_lock(&lock);
    current_gamepad();
    info->connected_count=connected_count ? connected_count : 1;
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t pad_vibration(int32_t handle, const uint8_t *param) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!param) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    SDL_Gamepad *g=current_gamepad();
    /* The menu's vibration strength scales the game's motors (0: off). */
    const unsigned percent=(unsigned)mtgpu_vibration_percent();
    if (g) SDL_RumbleGamepad(g,(uint16_t)(param[0]*257u*percent/100u),(uint16_t)(param[1]*257u*percent/100u),1000);
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t pad_ok_handle(int32_t handle) { return handle==PAD_HANDLE && opened ? 0 : ERR_INVALID_HANDLE; }
static ABI int32_t pad_ok_handle_flag(int32_t handle, uint8_t flag) { (void)flag; return pad_ok_handle(handle); }

static ABI int32_t pad_light_bar(int32_t handle, const uint8_t *rgb) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    (void)rgb; return 0;
}
static const RuntimeExport exports[]={
    {"scePadSetLightBar",pad_light_bar}, {"scePadResetLightBar",pad_ok_handle},
    {"scePadInit",pad_init}, {"scePadOpen",pad_open}, {"scePadClose",pad_close},
    {"scePadOpenExt",pad_open_ext}, {"scePadOutputReport",pad_output_report},
    {"scePadReadState",pad_read_state}, {"scePadRead",pad_read},
    {"scePadGetControllerInformation",pad_info}, {"scePadSetVibration",pad_vibration},
    {"scePadResetOrientation",pad_ok_handle},
    {"scePadSetAngularVelocityDeadbandState",pad_ok_handle_flag}, {"scePadSetTiltCorrectionState",pad_ok_handle_flag},
    {"scePadSetMotionSensorState",pad_ok_handle_flag},
};
uintptr_t runtime_pad_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_pad_report(void) { printf("Runtime: pad reads=%zu, gamepad=%s\n",reads,gamepad ? SDL_GetGamepadName(gamepad) : "none"); }

/* probe --gamepad: runs before any game state exists, so it uses its own SDL gamepad and leaves
 * the lock and statics above alone. run.py parses this line; keep the format. */
static const char *label_name(SDL_GamepadButtonLabel label) {
    switch (label) {
    case SDL_GAMEPAD_BUTTON_LABEL_A: return "a";
    case SDL_GAMEPAD_BUTTON_LABEL_B: return "b";
    case SDL_GAMEPAD_BUTTON_LABEL_X: return "x";
    case SDL_GAMEPAD_BUTTON_LABEL_Y: return "y";
    case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return "cross";
    case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return "circle";
    case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return "square";
    case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return "triangle";
    default: return "unknown";
    }
}
int runtime_pad_describe(void) {
    SDL_Gamepad *g=NULL;
    const bool ready=SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    /* No event loop yet, so pads show up only after a pump: poll for about 300 ms. */
    for (int waited=0; ready && !g && waited<30; ++waited) {
        SDL_PumpEvents();
        int count=0;
        SDL_JoystickID *ids=SDL_GetGamepads(&count);
        if (ids && count>0) g=SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
        if (!g) SDL_Delay(10);
    }
    if (!g) puts("Gamepad: none");
    else {
        const char *type=SDL_GetGamepadStringForType(SDL_GetGamepadType(g));
        const char *name=SDL_GetGamepadName(g);
        printf("Gamepad: type=%s south=%s east=%s west=%s north=%s name=%s\n",
               type ? type : "unknown",
               label_name(SDL_GetGamepadButtonLabel(g,SDL_GAMEPAD_BUTTON_SOUTH)),
               label_name(SDL_GetGamepadButtonLabel(g,SDL_GAMEPAD_BUTTON_EAST)),
               label_name(SDL_GetGamepadButtonLabel(g,SDL_GAMEPAD_BUTTON_WEST)),
               label_name(SDL_GetGamepadButtonLabel(g,SDL_GAMEPAD_BUTTON_NORTH)),
               name ? name : "unknown");
        SDL_CloseGamepad(g);
    }
    if (ready) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    return g!=NULL;
}
