// The VR menu and overlay pixels. Forked from the Vice City VR port's
// vrdebug.cpp: the software glyph font, the full-screen menu pages and the
// settings load/save helpers are carried over; the pages for the tracked-hand
// weapon layer, ragdolls, deformation, model sets, traffic tuning, cheats and
// missions are not part of the GTA III port and are gone. Delivery is the same:
// the pixels reach the compositor through the Android session's quad layer.

#include "common.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

#include "android.h"
#include "platform_android.h"
#include "QuestProfiler.h"
#include "xr_vulkan_session.h"
#include "vulkan/rwvk.h"
#include "skeleton.h"
#include "Game.h"
#include "Frontend.h"
#include "CutsceneMgr.h"
#include "Camera.h"
#include "PlayerInfo.h"
#include "PlayerPed.h"
#include "Timer.h"
#include "crossplatform.h"
#include <android/log.h>

#define GTA3VR_VERSION_TEXT "0.1.0"

enum {
	VR_DEBUG_WIDTH = 512,
	VR_DEBUG_HEIGHT = 437,
	VR_FPS_HEIGHT = 128,
	VR_MENU_WIDTH = 1024,
	VR_MENU_HEIGHT = 768,
};

// --- the software glyph font, from the desktop overlay -------------------------------------------
struct DebugGlyph { char character; uint8 rows[7]; };
static const DebugGlyph gDebugGlyphs[] = {
	{' ',{0x00,0x00,0x00,0x00,0x00,0x00,0x00}}, {'%',{0x11,0x12,0x02,0x04,0x08,0x09,0x11}},
	{'+',{0x00,0x04,0x04,0x1F,0x04,0x04,0x00}},
	{'.',{0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}},
	{'/',{0x01,0x02,0x02,0x04,0x08,0x08,0x10}},
	{'-',{0x00,0x00,0x00,0x1F,0x00,0x00,0x00}}, {'<',{0x02,0x04,0x08,0x10,0x08,0x04,0x02}},
	{'>',{0x08,0x04,0x02,0x01,0x02,0x04,0x08}},
	{'[',{0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}},
	{']',{0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}},
	{':',{0x00,0x04,0x04,0x00,0x04,0x04,0x00}}, {'0',{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
	{'1',{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}}, {'2',{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}},
	{'3',{0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}}, {'4',{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
	{'5',{0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E}}, {'6',{0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E}},
	{'7',{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}}, {'8',{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
	{'9',{0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E}}, {'A',{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
	{'B',{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
	{'C',{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}}, {'D',{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}},
	{'E',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}}, {'F',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
	{'G',{0x0E,0x11,0x10,0x17,0x11,0x11,0x0E}}, {'H',{0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
	{'I',{0x1F,0x04,0x04,0x04,0x04,0x04,0x1F}}, {'J',{0x01,0x01,0x01,0x01,0x11,0x11,0x0E}},
	{'K',{0x11,0x12,0x14,0x18,0x14,0x12,0x11}}, {'L',{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
	{'M',{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}}, {'N',{0x11,0x19,0x19,0x15,0x13,0x13,0x11}},
	{'O',{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}}, {'P',{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
	{'Q',{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}},
	{'R',{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}}, {'S',{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},
	{'T',{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}}, {'U',{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
	{'V',{0x11,0x11,0x11,0x11,0x11,0x0A,0x04}}, {'W',{0x11,0x11,0x11,0x15,0x15,0x15,0x0A}},
	{'X',{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}}, {'Y',{0x11,0x11,0x0A,0x04,0x04,0x04,0x04}},
	{'Z',{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}}
};

// The headset named in menu text.
#if MIAMIVR_STEAM_FRAME
#define VR_PLATFORM_NAME "STEAM FRAME"
#else
#define VR_PLATFORM_NAME "QUEST"
#endif

static bool gDebugVisible;
static uint8 gDebugPixels[VR_DEBUG_WIDTH*VR_DEBUG_HEIGHT*4];
static uint8 gVrMenuPixels[VR_MENU_WIDTH*VR_MENU_HEIGHT*4];
static int gDebugFps;
static double gDebugPreviousFrameMs;
static float gDebugSmoothedFrameMs;
static bool gTouchRecenterShortcutDown;
// A pad looks behind on R3. That is off on foot in first person in VR: the view
// is the player's own head, so looking behind turns the world around him.
// CONTROLS hands it back for anyone who wants it. (GTA III has no crouch, so
// the Vice City crouch row and its setting do nothing here.)
static int gVrStickLookBehind;
static int gVrStickCrouch;
// In a vehicle the view inherits the vehicle's whole basis instead of yaw.
static bool gVehicleFollowRotation;
// First person on foot: shots go where the head looks, and the sniper, M16 and
// rocket cameras follow the headset. vr_settings.ini [VR] HeadAim, no menu row.
static bool gHeadAim = true;

bool
androidgame::VrStickLookBehindEnabled(void)
{
	return gVrStickLookBehind != 0;
}

bool
androidgame::VrStickCrouchEnabled(void)
{
	return gVrStickCrouch != 0;
}

// Rebuild the reference space around the pose being held now.
void
androidgame::VrRecenterView(void)
{
	rw::vulkan::setFirstPersonAnchor(nil, 0.0f, 0, 0);
}
static bool gVrMenuVisible;
static bool gVrMenuShortcutDown;
static bool gVrMenuSelectDown;
static bool gVrMenuBackDown;
static int gVrMenuNavigateDirection;
static double gVrMenuNavigateRepeatAt;
static bool gVrMenuIncreaseDown;
static bool gVrMenuDecreaseDown;
static double gVrMenuIncreaseRepeatAt;
static double gVrMenuDecreaseRepeatAt;
static double gVrMenuIncreaseHoldStart;
static double gVrMenuDecreaseHoldStart;
static bool gVrSettingsLoaded;
// The Vice City colour filter is off by default in this port.
static bool gViceCityColorEnabled = false;
// Stable single-frame FXAA. The stronger 3x3 experiment produced no visible
// headset improvement over this path, so do not expose duplicate choices.
static int gSpatialAaMode = 1;
// Vice City ships single-level world textures, so distant fences and
// signs are minified straight off the full-size image and crawl. The
// chain is built on a worker thread, so the only cost the player pays
// is about a third more texture memory.
static int gGenerateMipmaps = 1;
// The PS2 two-pass alpha rule the desktop backends have always run: the
// solid half of a blended mesh writes depth, the faint half only blends.
// Without it the invisible border around a railing cuts its own outline
// out of the wall behind it.
// Off by default all the same: every masked mesh goes twice, and a
// street of railings, fences and foliage is most of what a Vice City
// block is made of. The fringe it removes is faint and daylight-only;
// the draws are paid everywhere. On for anyone who would rather have
// it the other way round.
static int gPs2AlphaTest = 0;
// Renderer counters in logcat. A shipped build has no business
// writing to a player log every ten seconds, so this stays off
// unless it is asked for.
static int gRenderDiagnostics;
// How far down the mip chain masked geometry is sampled, in half
// levels. A leaf mask is finer than a pixel at any distance worth
// looking at, so it sparkles; nothing spatial can resolve it, and
// softening it is what a temporal filter buys on desktop.
static int gFoliageSoftness;
// A cutscene runs its own director camera. CINEMA puts that camera on
// the flat theater screen, which is where it has always gone; the other
// modes let the headset render it in stereo instead, so the scene has
// depth and the player can look around inside the shot.
static int gCutsceneMode;
// The Frame adds 3D cinema: the same flat screen, but each eye gets its own
// view of the director's shot, so the scene has depth behind the frame.
// Three depth levels share the CUTSCENES row (the page has no room for a row
// of its own); 0 and 1 keep their meaning, so a Quest settings file reads the
// same here.
enum
{
	VR_CUTSCENE_CINEMA = 0,
	VR_CUTSCENE_STEREO = 1,
	VR_CUTSCENE_CINEMA_3D_SUBTLE = 2,
	VR_CUTSCENE_CINEMA_3D = 3,
	VR_CUTSCENE_CINEMA_3D_DEEP = 4,
};
#if MIAMIVR_STEAM_FRAME
// The order the row steps through.
static const int kCutsceneModeOrder[] = {
	VR_CUTSCENE_CINEMA, VR_CUTSCENE_CINEMA_3D_SUBTLE, VR_CUTSCENE_CINEMA_3D,
	VR_CUTSCENE_CINEMA_3D_DEEP, VR_CUTSCENE_STEREO
};
// A third person view for the player who finds first person too intense.
// FIRST PERSON is the shipped view; the other two both play from Vice City's
// own camera, either on the cinema screen (the whole game as a flat picture
// with 3D depth) or in stereo from behind the shoulder. The style is the
// last third person one chosen, so a short press of View swings between it
// and first person.
enum
{
	VR_VIEW_FIRST_PERSON = 0,
	VR_VIEW_THIRD_SCREEN = 1,
	VR_VIEW_THIRD_SHOULDER = 2,
};
static int gViewMode;
static int gThirdPersonStyle = VR_VIEW_THIRD_SCREEN;
#endif
// Which camera this cutscene is being watched from: 0 is the director's
// own, 1 and up are the staged actors in the order the scene put them
// there. Picking the right one automatically never worked -- what the
// shot is about changes from scene to scene -- so the player cycles it
// and keeps the one they liked, per scene, for next time.
static int gCutsceneCamera;
static char gCutsceneCameraScene[32];
static bool gCutsceneCycleDown;
static bool gCutsceneStoreDown;
#if defined(MIAMIVR_DEV_TOOLS) && MIAMIVR_DEV_TOOLS
static bool gQuestQuickTestStart;
#endif
static int gQuestRenderScalePercent = 100;
static int gQuestSgsrMode = rw::vulkan::SGSR_OFF;
static int gQuestMsaaSamples = 1;
static bool gGameplayHudEnabled = true;
// Distance fog, as the desktop game draws it. Off shows the bare far clip;
// it is here to compare the two.
static bool gDistanceFog = true;
// Weapon icon, ammo counter and clock. Part of the classic interface, which is
// what a new player gets: the immersive layout is what switches them off.
static bool gHudWeaponPanel = true;
static bool gHudClock = true;
static int gHudWidthPercent = 100;
static int gHudScalePercent = 130;
static int gHudOffsetXCm;
static int gHudOffsetYCm;
static int gQuestMovementOrientation = 1;
// 0.4.1 PC parity: walking head bob is off by default; the anchor holds a
// stable eye height while the legs animate.
static int gQuestHeadBobbing = 0;
// 72 is the safe default; the Quest 3 display also offers 80/90/120 and the
// game holds 90 with headroom after the traffic director work.
static int gQuestRefreshRateHz = 72;
static int gQuestTurnMode;
static int gQuestTurnSensitivityPercent = 100;
static int gQuestHeadSteeringSensitivityPercent = 50;
static int gQuestSnapTurnAngleDegrees = 30;
// Button assignment, one entry per physical input, filled in by LoadVrSettings
// and read through androidgame::VrPadBinding. The defaults reproduce the layout
// the port shipped with. The triggers are in the table because the pad assembly
// walks all of it, not because the CONTROLS page offers them: they are the
// accelerator and the brake, and the weapon trigger on foot.
static const int kVrPadBindingDefault[androidgame::VR_PAD_SOURCE_COUNT] = {
	androidgame::VR_PAD_TARGET_CROSS,     // A
	androidgame::VR_PAD_TARGET_CIRCLE,    // B
	androidgame::VR_PAD_TARGET_SQUARE,    // X
	androidgame::VR_PAD_TARGET_TRIANGLE,  // Y
	androidgame::VR_PAD_TARGET_SQUARE,    // left trigger
	androidgame::VR_PAD_TARGET_CROSS,     // right trigger
	androidgame::VR_PAD_TARGET_L1,        // left grip
	androidgame::VR_PAD_TARGET_R1,        // right grip
	androidgame::VR_PAD_TARGET_L3,        // left stick click
	androidgame::VR_PAD_TARGET_R3         // right stick click
};
static const char *const kVrPadBindingKey[androidgame::VR_PAD_SOURCE_COUNT] = {
	"BindA", "BindB", "BindX", "BindY",
	"BindLeftTrigger", "BindRightTrigger",
	"BindLeftGrip", "BindRightGrip",
	"BindLeftStickClick", "BindRightStickClick"
};
static int gVrPadBinding[androidgame::VR_PAD_SOURCE_COUNT];
static int gVrMenuPage;
static int gVrMenuSelection;
static int gVrGraphicsSelection;
static int gVrHudSelection;
static int gVrLocomotionSelection;
static int gVrControlsSelection;
static bool gVrWelcomePending;
static bool gVrWelcomeFirstRun;
static bool gVrAboutDismissArmed;
static bool gVrAboutReleaseGate;
static bool gVrWelcomeBaselineValid;
static CVector gVrWelcomeBaseline;
static int gQuestCpuPerformanceMode;
static int gQuestCpuPerformanceSavedMode;
static int gQuestGpuPerformanceMode = 1; // SUSTAINED; enum is declared below.

enum {
	VR_MENU_PAGE_SETTINGS,
	VR_MENU_PAGE_GRAPHICS,
	VR_MENU_PAGE_HUD,
	VR_MENU_PAGE_LOCOMOTION,
	VR_MENU_PAGE_CONTROLS,
	VR_MENU_PAGE_ABOUT,
};

enum eVrHudMenuItem {
	VR_HUD_ENABLED = 0,
	VR_HUD_HORIZONTAL_SCALE,
	VR_HUD_SCALE,
	VR_HUD_OFFSET_X,
	VR_HUD_OFFSET_Y,
	VR_HUD_WEAPON_PANEL,
	VR_HUD_CLOCK,
	VR_HUD_BACK,
	VR_HUD_ITEM_COUNT
};

// Only settings backed by the Vulkan/OpenXR runtime and the game as it is.
enum eVrMainMenuItem {
	VR_MAIN_GRAPHICS = 0,
	VR_MAIN_HUD,
	VR_MAIN_LOCOMOTION_SETTINGS,
	VR_MAIN_CONTROLS,
	VR_MAIN_ABOUT,
	VR_MAIN_ITEM_COUNT
};

enum eVrGraphicsMenuItem {
	VR_GRAPHICS_RENDER_SCALE = 0,
	VR_GRAPHICS_SGSR,
	VR_GRAPHICS_MSAA,
	VR_GRAPHICS_FXAA,
	VR_GRAPHICS_MIPMAPS,
	VR_GRAPHICS_PS2_ALPHA,
	VR_GRAPHICS_FOLIAGE,
	VR_GRAPHICS_DIAGNOSTICS,
	VR_GRAPHICS_COLOR,
	VR_GRAPHICS_CPU_PERFORMANCE,
	VR_GRAPHICS_GPU_PERFORMANCE,
	VR_GRAPHICS_FOG,
#if defined(MIAMIVR_DEV_TOOLS) && MIAMIVR_DEV_TOOLS
	VR_GRAPHICS_QUICK_START,
#endif
	VR_GRAPHICS_BACK,
	VR_GRAPHICS_ITEM_COUNT
};

enum eQuestCpuPerformanceMode {
	QUEST_CPU_PERFORMANCE_AUTO = 0,
	QUEST_CPU_PERFORMANCE_SUSTAINED,
	QUEST_CPU_PERFORMANCE_BOOST,
	QUEST_CPU_PERFORMANCE_COUNT
};

enum eVrLocomotionMenuItem {
#if MIAMIVR_STEAM_FRAME
	VR_LOCOMOTION_VIEW = 0,
	VR_LOCOMOTION_MOVEMENT_ORIENTATION,
#else
	VR_LOCOMOTION_MOVEMENT_ORIENTATION = 0,
#endif
	VR_LOCOMOTION_TURN_MODE,
	VR_LOCOMOTION_TURN_SENSITIVITY,
	VR_LOCOMOTION_SNAP_ANGLE,
	VR_LOCOMOTION_HEAD_BOBBING,
	VR_LOCOMOTION_CUTSCENES,
#if MIAMIVR_STEAM_FRAME
	VR_LOCOMOTION_VEHICLE_VIEW,
#endif
	VR_LOCOMOTION_REFRESH_RATE,
	VR_LOCOMOTION_RECENTER,
	VR_LOCOMOTION_BACK,
	VR_LOCOMOTION_ITEM_COUNT
};

// Which physical input each row of the CONTROLS page edits; the triggers are
// deliberately absent. VrControlsRowSource maps a row back to its input.
enum eVrControlsMenuItem {
	VR_CONTROLS_LAYOUT = 0,
	VR_CONTROLS_A,
	VR_CONTROLS_B,
	VR_CONTROLS_X,
	VR_CONTROLS_Y,
	VR_CONTROLS_LEFT_GRIP,
	VR_CONTROLS_RIGHT_GRIP,
	VR_CONTROLS_LEFT_STICK_CLICK,
	VR_CONTROLS_RIGHT_STICK_CLICK,
	VR_CONTROLS_LOOK_BEHIND,
	VR_CONTROLS_RESET,
	VR_CONTROLS_BACK,
	VR_CONTROLS_ITEM_COUNT
};

// SWAPPED HANDS is the reason the page exists: it moves jump and enter/exit to
// the right controller and attack and sprint to the left, for players who read
// the shipped layout as inverted.
enum eVrControlsLayout {
	VR_CONTROLS_LAYOUT_DEFAULT = 0,
	VR_CONTROLS_LAYOUT_SWAPPED_HANDS,
	VR_CONTROLS_LAYOUT_CUSTOM,
	VR_CONTROLS_LAYOUT_COUNT
};

enum eQuestMovementOrientation {
	QUEST_MOVEMENT_ORIENTATION_BODY = 0,
	QUEST_MOVEMENT_ORIENTATION_HEAD,
	QUEST_MOVEMENT_ORIENTATION_HEAD_TURN_EXPERIMENTAL,
	QUEST_MOVEMENT_ORIENTATION_COUNT
};

enum eQuestTurnMode {
	QUEST_TURN_SMOOTH = 0,
	QUEST_TURN_SNAP,
	QUEST_TURN_MODE_COUNT
};

static const char *
QuestCpuPerformanceModeName(int mode)
{
	static const char *const names[QUEST_CPU_PERFORMANCE_COUNT] = {
		"AUTO", "SUSTAINED", "BOOST (TEST)"
	};
	return mode >= 0 && mode < QUEST_CPU_PERFORMANCE_COUNT ?
		names[mode] : "UNKNOWN";
}

static const uint8 *
FindDebugGlyph(char character)
{
	if(character >= 'a' && character <= 'z')
		character = character-'a'+'A';
	for(uint32 i = 0; i < ARRAY_SIZE(gDebugGlyphs); i++)
		if(gDebugGlyphs[i].character == character)
			return gDebugGlyphs[i].rows;
	return gDebugGlyphs[0].rows;
}

static void
PutDebugPixel(int x, int y, uint8 red, uint8 green, uint8 blue, uint8 alpha)
{
	if(x < 0 || y < 0 || x >= VR_DEBUG_WIDTH || y >= VR_DEBUG_HEIGHT) return;
	// Vulkan images are top-down, same as the desktop's D3D12 branch.
	const int offset = (y*VR_DEBUG_WIDTH+x)*4;
	gDebugPixels[offset+0] = red; gDebugPixels[offset+1] = green;
	gDebugPixels[offset+2] = blue; gDebugPixels[offset+3] = alpha;
}

static void
DrawDebugText(const char *value, int centreX, int y, int scale,
              uint8 red, uint8 green, uint8 blue)
{
	const int advance = scale*6;
	int x = centreX-(int)strlen(value)*advance/2;
	for(const char *ch = value; *ch; ch++, x += advance){
		const uint8 *rows = FindDebugGlyph(*ch);
		for(int row = 0; row < 7; row++) for(int column = 0; column < 5; column++)
			if(rows[row]&(1<<(4-column)))
				for(int py = 0; py < scale; py++) for(int px = 0; px < scale; px++)
					PutDebugPixel(x+column*scale+px, y+row*scale+py, red, green, blue, 255);
	}
}

static void
PutVrMenuPixel(int x, int y, uint8 red, uint8 green, uint8 blue, uint8 alpha)
{
	if(x < 0 || y < 0 || x >= VR_MENU_WIDTH || y >= VR_MENU_HEIGHT)
		return;
	const int offset = (y*VR_MENU_WIDTH+x)*4;
	gVrMenuPixels[offset+0] = red;
	gVrMenuPixels[offset+1] = green;
	gVrMenuPixels[offset+2] = blue;
	gVrMenuPixels[offset+3] = alpha;
}

static void
FillVrMenuRect(int left, int top, int right, int bottom,
               uint8 red, uint8 green, uint8 blue, uint8 alpha)
{
	left = Max(0, left);
	top = Max(0, top);
	right = Min(VR_MENU_WIDTH, right);
	bottom = Min(VR_MENU_HEIGHT, bottom);
	for(int y = top; y < bottom; y++)
		for(int x = left; x < right; x++)
			PutVrMenuPixel(x, y, red, green, blue, alpha);
}

static void
DrawVrMenuText(const char *value, int centreX, int y, int scale,
               uint8 red, uint8 green, uint8 blue)
{
	const int advance = scale*6;
	int x = centreX-(int)strlen(value)*advance/2;
	for(const char *ch = value; *ch; ch++, x += advance){
		const uint8 *rows = FindDebugGlyph(*ch);
		for(int row = 0; row < 7; row++)
			for(int column = 0; column < 5; column++)
				if(rows[row]&(1<<(4-column)))
					for(int py = 0; py < scale; py++)
						for(int px = 0; px < scale; px++)
							PutVrMenuPixel(
								x+column*scale+px,
								y+row*scale+py,
								red, green, blue, 255);
	}
}
// --------------------------------------------------------------------------

namespace androidgame {

static void SaveVrInteger(const char *key, int value);

static void
LoadVrSettings(void)
{
	if(gVrSettingsLoaded)
		return;
	gViceCityColorEnabled =
		GetPrivateProfileIntA("VR", "ViceCityColor", 0,
			".\\vr_settings.ini") != 0;
	gGenerateMipmaps = GetPrivateProfileIntA("VR", "GenerateMipmaps", 1,
		".\\vr_settings.ini") != 0 ? 1 : 0;
	gPs2AlphaTest = GetPrivateProfileIntA("VR", "Ps2AlphaTest", 0,
		".\\vr_settings.ini") != 0 ? 1 : 0;
	gRenderDiagnostics = GetPrivateProfileIntA("VR",
		"RenderDiagnostics", 0, ".\\vr_settings.ini") != 0 ? 1 : 0;
	gFoliageSoftness = Clamp((int)(int32)GetPrivateProfileIntA("VR",
		"FoliageSoftness", 0, ".\\vr_settings.ini"), 0, 3);
#if MIAMIVR_STEAM_FRAME
	gCutsceneMode = Clamp((int)(int32)GetPrivateProfileIntA("VR",
		"CutsceneMode", VR_CUTSCENE_CINEMA_3D, ".\\vr_settings.ini"),
		VR_CUTSCENE_CINEMA, VR_CUTSCENE_CINEMA_3D_DEEP);
	gViewMode = Clamp((int)(int32)GetPrivateProfileIntA("VR",
		"ViewMode", VR_VIEW_THIRD_SCREEN, ".\\vr_settings.ini"),
		VR_VIEW_FIRST_PERSON, VR_VIEW_THIRD_SHOULDER);
	gThirdPersonStyle = Clamp((int)(int32)GetPrivateProfileIntA("VR",
		"ThirdPersonStyle", VR_VIEW_THIRD_SCREEN, ".\\vr_settings.ini"),
		VR_VIEW_THIRD_SCREEN, VR_VIEW_THIRD_SHOULDER);
#else
	gCutsceneMode = Clamp((int)(int32)GetPrivateProfileIntA("VR",
		"CutsceneMode", 0, ".\\vr_settings.ini"), 0, 1);
#endif
	gSpatialAaMode = GetPrivateProfileIntA("VR", "AntiAliasing", 1,
		".\\vr_settings.ini") != 0;
#if defined(MIAMIVR_DEV_TOOLS) && MIAMIVR_DEV_TOOLS
	gQuestQuickTestStart =
		GetPrivateProfileIntA("VR", "QuickTestStart", 0,
			".\\vr_settings.ini") != 0;
#endif
	{
		static const int scales[] = { 100, 125, 150, 175 };
		const int savedScale = GetPrivateProfileIntA("VR", "RenderScalePercent",
			100, ".\\vr_settings.ini");
		int best = 0;
		for(int i = 1; i < (int)ARRAY_SIZE(scales); i++)
			if(Abs(scales[i]-savedScale) < Abs(scales[best]-savedScale))
				best = i;
	gQuestRenderScalePercent = scales[best];
	}
	// Temporal AA is intentionally unavailable until its stereo instability is
	// solved. Preserve an exact native fallback instead of letting an old INI
	// silently reactivate an eye-straining mode.
	gQuestSgsrMode = rw::vulkan::SGSR_OFF;
	SaveVrInteger("Sgsr2Mode", gQuestSgsrMode);
	gQuestMsaaSamples = GetPrivateProfileIntA("VR", "MsaaSamples", 1,
		".\\vr_settings.ini");
	// Off, 2x or 4x and nothing else: the swapchain is built from
	// this and an ini carrying anything else has to land somewhere.
	gQuestMsaaSamples = gQuestMsaaSamples >= 4 ? 4 :
		gQuestMsaaSamples >= 2 ? 2 : 1;
	gDistanceFog = GetPrivateProfileIntA("VR", "DistanceFog", 1,
		".\\vr_settings.ini") != 0;
	// The classic interface: weapon icon, ammo and clock on the HUD plane.
	gHudWeaponPanel = GetPrivateProfileIntA("VR", "HudWeaponPanel", 1,
		".\\vr_settings.ini") != 0;
	gHudClock = GetPrivateProfileIntA("VR", "HudClock", 1,
		".\\vr_settings.ini") != 0;
	gGameplayHudEnabled =
		GetPrivateProfileIntA("VR", "GameplayHud", 1,
			".\\vr_settings.ini") != 0;
	gHudWidthPercent = Min(Max(GetPrivateProfileIntA("VR",
		"HudWidthPercent", 100, ".\\vr_settings.ini"), 50), 200);
	gHudScalePercent = Min(Max(GetPrivateProfileIntA("VR",
		"HudScalePercent", 130, ".\\vr_settings.ini"), 50), 200);
	// Win32-style profile reads return an unsigned value. Cast back to signed
	// before clamping so a saved negative offset cannot become +100.
	gHudOffsetXCm = Min(Max((int)(int32)GetPrivateProfileIntA("VR",
		"HudOffsetXCm", 0, ".\\vr_settings.ini"), -100), 100);
	gHudOffsetYCm = Min(Max((int)(int32)GetPrivateProfileIntA("VR",
		"HudOffsetYCm", 0, ".\\vr_settings.ini"), -100), 100);
	gQuestMovementOrientation = Min(Max(GetPrivateProfileIntA("VR",
		"MovementOrientation", QUEST_MOVEMENT_ORIENTATION_HEAD,
		".\\vr_settings.ini"),
		(int)QUEST_MOVEMENT_ORIENTATION_BODY),
		(int)QUEST_MOVEMENT_ORIENTATION_COUNT-1);
	gQuestHeadBobbing = GetPrivateProfileIntA("VR", "HeadBobbing", 0,
		".\\vr_settings.ini") != 0;
	gQuestRefreshRateHz = Min(Max(GetPrivateProfileIntA("VR",
		"RefreshRate", 72, ".\\vr_settings.ini"), 72), 120);
	androidgame::SetPreferredRefreshRate(gQuestRefreshRateHz);
	const int persistedCpuPerformanceMode = GetPrivateProfileIntA("VR",
		"CpuPerformanceMode", QUEST_CPU_PERFORMANCE_SUSTAINED,
		".\\vr_settings.ini");
	// Boost is deliberately session-only. Old test builds may have persisted
	// value 2; treat that as Auto rather than re-entering Boost on startup.
	gQuestCpuPerformanceSavedMode =
		persistedCpuPerformanceMode >= QUEST_CPU_PERFORMANCE_AUTO &&
		persistedCpuPerformanceMode <= QUEST_CPU_PERFORMANCE_SUSTAINED ?
		persistedCpuPerformanceMode : QUEST_CPU_PERFORMANCE_SUSTAINED;
	gQuestCpuPerformanceMode = gQuestCpuPerformanceSavedMode;
	xrvk::setPerformanceMode(gQuestCpuPerformanceMode);
	gQuestGpuPerformanceMode = Min(Max(GetPrivateProfileIntA("VR",
		"GpuPerformanceMode", QUEST_CPU_PERFORMANCE_SUSTAINED,
		".\\vr_settings.ini"), (int)QUEST_CPU_PERFORMANCE_AUTO),
		(int)QUEST_CPU_PERFORMANCE_BOOST);
	xrvk::setGpuPerformanceMode(gQuestGpuPerformanceMode);
	gQuestTurnMode = Min(Max(GetPrivateProfileIntA("VR", "TurnMode",
		QUEST_TURN_SMOOTH, ".\\vr_settings.ini"),
		(int)QUEST_TURN_SMOOTH), (int)QUEST_TURN_MODE_COUNT-1);
	gQuestTurnSensitivityPercent = Min(Max(GetPrivateProfileIntA("VR",
		"TurnSensitivityPercent", 100, ".\\vr_settings.ini"), 25), 300);
	gQuestHeadSteeringSensitivityPercent = Min(Max(
		GetPrivateProfileIntA("VR", "HeadSteeringSensitivityPercent",
			50, ".\\vr_settings.ini"), 25), 300);
	gQuestSnapTurnAngleDegrees = Min(Max(GetPrivateProfileIntA("VR",
		"SnapTurnAngleDegrees", 30, ".\\vr_settings.ini"), 15), 90);
	gVrStickLookBehind = GetPrivateProfileIntA("VR", "StickLookBehind",
		0, ".\\vr_settings.ini") != 0 ? 1 : 0;
	gVrStickCrouch = GetPrivateProfileIntA("VR", "StickCrouch",
		0, ".\\vr_settings.ini") != 0 ? 1 : 0;
	gVrWelcomePending = GetPrivateProfileIntA("VR", "WelcomeShown", 0,
		".\\vr_settings.ini") == 0;
	gVrWelcomeFirstRun = gVrWelcomePending;
	// A binding out of range is an INI written by a different build; fall back
	// to the shipped assignment for that input rather than to no button at all.
	for(int source = 0; source < VR_PAD_SOURCE_COUNT; source++){
		const int target = GetPrivateProfileIntA("VR",
			kVrPadBindingKey[source], kVrPadBindingDefault[source],
			".\\vr_settings.ini");
		gVrPadBinding[source] =
			target >= VR_PAD_TARGET_NONE && target < VR_PAD_TARGET_COUNT ?
				target : kVrPadBindingDefault[source];
	}
	gVehicleFollowRotation = GetPrivateProfileIntA("VR",
		"VehicleFollowRotation", 0, ".\\vr_settings.ini") != 0;
	gHeadAim = GetPrivateProfileIntA("VR", "HeadAim", 1,
		".\\vr_settings.ini") != 0;
	gVrSettingsLoaded = true;
}

static void
SaveViceCityColor(void)
{
	WritePrivateProfileStringA("VR", "ViceCityColor",
		gViceCityColorEnabled ? "1" : "0", ".\\vr_settings.ini");
}

static void
SaveFxaa(void)
{
	char value[8];
	snprintf(value, sizeof(value), "%d", gSpatialAaMode);
	WritePrivateProfileStringA("VR", "AntiAliasing", value,
	                         ".\\vr_settings.ini");
}

static void
SaveGameplayHud(void)
{
	WritePrivateProfileStringA("VR", "GameplayHud",
		gGameplayHudEnabled ? "1" : "0", ".\\vr_settings.ini");
}

// One key per cutscene name under its own section, so the list grows with
// whatever the player actually watches instead of shipping a table.
static void
LoadCutsceneCameraForScene(const char *scene)
{
	gCutsceneCamera = GetPrivateProfileIntA("VRCutsceneCamera",
		scene, 0, ".\\vr_settings.ini");
	if(gCutsceneCamera < 0)
		gCutsceneCamera = 0;
}

static void
SaveCutsceneCameraForScene(const char *scene)
{
	char value[16];
	snprintf(value, sizeof(value), "%d", gCutsceneCamera);
	WritePrivateProfileStringA("VRCutsceneCamera", scene, value,
		".\\vr_settings.ini");
}


// IMMERSIVE puts what has to be read on the arms and drops the corner
// clutter; CLASSIC is the desktop game's interface, all of it, in front of
// the face. Classic is what a new player gets.
enum {
	VR_HUD_PRESET_IMMERSIVE = 0,
	VR_HUD_PRESET_CLASSIC,
	VR_HUD_PRESET_CUSTOM
};

static void
SaveVrInteger(const char *key, int value)
{
	char text[16];
	snprintf(text, sizeof(text), "%d", value);
	WritePrivateProfileStringA("VR", key, text, ".\\vr_settings.ini");
}

#if MIAMIVR_STEAM_FRAME
static void
SetViewMode(int mode)
{
	mode = Clamp(mode, (int)VR_VIEW_FIRST_PERSON, (int)VR_VIEW_THIRD_SHOULDER);
	if(mode == gViewMode)
		return;
	gViewMode = mode;
	if(mode != VR_VIEW_FIRST_PERSON){
		gThirdPersonStyle = mode;
		SaveVrInteger("ThirdPersonStyle", gThirdPersonStyle);
	}
	SaveVrInteger("ViewMode", gViewMode);
	// The anchor was latched against the old view's camera; relatching now
	// keeps the first frame of the new one from turning the world.
	androidgame::VrRecenterView();
	__android_log_print(ANDROID_LOG_INFO, "GTA3VR", "[vr] view mode -> %s",
		mode == VR_VIEW_THIRD_SCREEN ? "third person, 3D screen" :
		mode == VR_VIEW_THIRD_SHOULDER ? "third person, over shoulder" :
		"first person");
}
#endif

static int
VrControlsRowSource(int item)
{
	switch(item){
	case VR_CONTROLS_A: return androidgame::VR_PAD_SOURCE_A;
	case VR_CONTROLS_B: return androidgame::VR_PAD_SOURCE_B;
	case VR_CONTROLS_X: return androidgame::VR_PAD_SOURCE_X;
	case VR_CONTROLS_Y: return androidgame::VR_PAD_SOURCE_Y;
	case VR_CONTROLS_LEFT_GRIP: return androidgame::VR_PAD_SOURCE_LEFT_GRIP;
	case VR_CONTROLS_RIGHT_GRIP: return androidgame::VR_PAD_SOURCE_RIGHT_GRIP;
	case VR_CONTROLS_LEFT_STICK_CLICK:
		return androidgame::VR_PAD_SOURCE_LEFT_STICK_CLICK;
	case VR_CONTROLS_RIGHT_STICK_CLICK:
		return androidgame::VR_PAD_SOURCE_RIGHT_STICK_CLICK;
	}
	return -1;
}

static const char *
VrPadSourceName(int source)
{
	switch(source){
	case androidgame::VR_PAD_SOURCE_A: return "A BUTTON";
	case androidgame::VR_PAD_SOURCE_B: return "B BUTTON";
	case androidgame::VR_PAD_SOURCE_X: return "X BUTTON";
	case androidgame::VR_PAD_SOURCE_Y: return "Y BUTTON";
	case androidgame::VR_PAD_SOURCE_LEFT_GRIP: return "LEFT GRIP";
	case androidgame::VR_PAD_SOURCE_RIGHT_GRIP: return "RIGHT GRIP";
	case androidgame::VR_PAD_SOURCE_LEFT_STICK_CLICK: return "LEFT STICK CLICK";
	case androidgame::VR_PAD_SOURCE_RIGHT_STICK_CLICK:
		return "RIGHT STICK CLICK";
	}
	return "UNKNOWN";
}

// The action is what the default controller setup does with that pad button on
// foot; the pad button itself is spelled out beside it because the frontend can
// still change what it means.
static const char *
VrPadTargetName(int target)
{
	switch(target){
	case androidgame::VR_PAD_TARGET_SQUARE: return "JUMP [SQUARE]";
	case androidgame::VR_PAD_TARGET_CROSS: return "SPRINT [CROSS]";
	case androidgame::VR_PAD_TARGET_CIRCLE: return "ATTACK [CIRCLE]";
	case androidgame::VR_PAD_TARGET_TRIANGLE: return "ENTER / EXIT [TRIANGLE]";
	case androidgame::VR_PAD_TARGET_L1: return "PICK UP / RADIO [L1]";
	case androidgame::VR_PAD_TARGET_R1: return "TARGET [R1]";
	case androidgame::VR_PAD_TARGET_L2: return "PREVIOUS WEAPON [L2]";
	case androidgame::VR_PAD_TARGET_R2: return "NEXT WEAPON [R2]";
	case androidgame::VR_PAD_TARGET_L3: return "HORN / CROUCH [L3]";
	case androidgame::VR_PAD_TARGET_R3: return "LOOK BEHIND [R3]";
	}
	return "UNUSED";
}

static void
SavePadBinding(int source)
{
	SaveVrInteger(kVrPadBindingKey[source], gVrPadBinding[source]);
}

static const int kVrSwappedHandsBinding[androidgame::VR_PAD_SOURCE_COUNT] = {
	androidgame::VR_PAD_TARGET_SQUARE,    // A
	androidgame::VR_PAD_TARGET_TRIANGLE,  // B
	androidgame::VR_PAD_TARGET_CROSS,     // X
	androidgame::VR_PAD_TARGET_CIRCLE,    // Y
	androidgame::VR_PAD_TARGET_SQUARE,    // left trigger
	androidgame::VR_PAD_TARGET_CROSS,     // right trigger
	androidgame::VR_PAD_TARGET_L1,        // left grip
	androidgame::VR_PAD_TARGET_R1,        // right grip
	androidgame::VR_PAD_TARGET_L3,        // left stick click
	androidgame::VR_PAD_TARGET_R3         // right stick click
};

static bool
PadBindingsMatch(const int *layout)
{
	for(int source = 0; source < androidgame::VR_PAD_SOURCE_COUNT; source++)
		if(gVrPadBinding[source] != layout[source])
			return false;
	return true;
}

static int
CurrentControlsLayout(void)
{
	if(PadBindingsMatch(kVrPadBindingDefault))
		return VR_CONTROLS_LAYOUT_DEFAULT;
	if(PadBindingsMatch(kVrSwappedHandsBinding))
		return VR_CONTROLS_LAYOUT_SWAPPED_HANDS;
	return VR_CONTROLS_LAYOUT_CUSTOM;
}

static void
ApplyControlsLayout(const int *layout)
{
	for(int source = 0; source < androidgame::VR_PAD_SOURCE_COUNT; source++){
		gVrPadBinding[source] = layout[source];
		SavePadBinding(source);
	}
}

static double
MonotonicMilliseconds(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec*1000.0+ts.tv_nsec/1e6;
}

static bool
MenuRepeatPulse(bool down, bool &wasDown, double &repeatAt,
	double &holdStart, double now,
                bool allowRepeat)
{
	if(!down){
		wasDown = false;
		repeatAt = 0.0;
		holdStart = 0.0;
		return false;
	}
	if(!wasDown){
		wasDown = true;
		holdStart = now;
		repeatAt = now+420.0;
		return true;
	}
	if(!allowRepeat)
		return false;
	if(now >= repeatAt){
		repeatAt = now+85.0;
		return true;
	}
	return false;
}

// Navigation needs two things at once: a deliberate tap must always move one
// row, while a held stick must eventually scroll long lists such as CHEATS.
// Separate engage/release thresholds prevent Touch-stick noise around a single
// threshold from manufacturing extra taps.
static int
MenuNavigationPulse(float axis, double now)
{
	const float engage = 0.68f;
	const float release = 0.34f;
	int requested = axis >= engage ? -1 : (axis <= -engage ? 1 : 0);

	if(gVrMenuNavigateDirection == 0){
		if(requested == 0)
			return 0;
		gVrMenuNavigateDirection = requested;
		gVrMenuNavigateRepeatAt = now+430.0;
		return requested;
	}

	if(Abs(axis) <= release){
		gVrMenuNavigateDirection = 0;
		gVrMenuNavigateRepeatAt = 0.0;
		return 0;
	}

	// A deliberate reversal is a new navigation edge even if the stick crossed
	// the centre too quickly for a rendered frame to observe the release band.
	if(requested != 0 && requested != gVrMenuNavigateDirection){
		gVrMenuNavigateDirection = requested;
		gVrMenuNavigateRepeatAt = now+430.0;
		return requested;
	}

	if(now >= gVrMenuNavigateRepeatAt){
		gVrMenuNavigateRepeatAt = now+110.0;
		return gVrMenuNavigateDirection;
	}
	return 0;
}

static void
ResetMenuNavigationRepeat(void)
{
	gVrMenuNavigateDirection = 0;
	gVrMenuNavigateRepeatAt = 0.0;
}

static int *
CurrentMenuSelection(void)
{
	switch(gVrMenuPage){
	case VR_MENU_PAGE_GRAPHICS: return &gVrGraphicsSelection;
	case VR_MENU_PAGE_HUD: return &gVrHudSelection;
	case VR_MENU_PAGE_LOCOMOTION: return &gVrLocomotionSelection;
	case VR_MENU_PAGE_CONTROLS: return &gVrControlsSelection;
	default: return &gVrMenuSelection;
	}
}

static int
CurrentMenuItemCount(void)
{
	switch(gVrMenuPage){
	case VR_MENU_PAGE_SETTINGS: return VR_MAIN_ITEM_COUNT;
	case VR_MENU_PAGE_GRAPHICS: return VR_GRAPHICS_ITEM_COUNT;
	case VR_MENU_PAGE_HUD: return VR_HUD_ITEM_COUNT;
	case VR_MENU_PAGE_LOCOMOTION: return VR_LOCOMOTION_ITEM_COUNT;
	case VR_MENU_PAGE_CONTROLS: return VR_CONTROLS_ITEM_COUNT;
	default: return 1;
	}
}

// Every row of the pages that are left is usable in any configuration.
static bool
IsMenuItemVisible(int page, int item)
{
	(void)page;
	(void)item;
	return true;
}

static bool
CurrentMenuValueRepeats(void)
{
	if(gVrMenuPage == VR_MENU_PAGE_HUD)
		return gVrHudSelection >= VR_HUD_HORIZONTAL_SCALE &&
			gVrHudSelection <= VR_HUD_OFFSET_Y;
	if(gVrMenuPage == VR_MENU_PAGE_LOCOMOTION)
		return gVrLocomotionSelection ==
			VR_LOCOMOTION_TURN_SENSITIVITY;
	return false;
}

static void
ReturnFromCurrentMenuPage(void)
{
	if(gVrMenuPage == VR_MENU_PAGE_SETTINGS){
		gVrMenuVisible = false;
		return;
	}
	gVrMenuPage = VR_MENU_PAGE_SETTINGS;
}

static bool
AnyVrAboutButtonDown(const PadInput &in)
{
	return in.a || in.b || in.x || in.y || in.menu ||
		in.leftStickClick || in.rightStickClick ||
		in.leftTrigger >= 0.55f || in.rightTrigger >= 0.55f ||
		in.leftGrip >= 0.65f || in.rightGrip >= 0.65f;
}

static bool
DidPlayerTakeWelcomeStep(const PadInput &in)
{
	CPlayerPed *player = FindPlayerPed();
	if(!player || gGameState != GS_PLAYING_GAME ||
	   FrontEndMenuManager.m_bMenuActive || CCutsceneMgr::IsRunning() ||
	   FindPlayerVehicle()){
		gVrWelcomeBaselineValid = false;
		return false;
	}
	const CVector position = player->GetPosition();
	if(!gVrWelcomeBaselineValid){
		gVrWelcomeBaseline = position;
		gVrWelcomeBaselineValid = true;
		return false;
	}
	const bool movementInput = Abs(in.leftStickX) >= 0.20f ||
		Abs(in.leftStickY) >= 0.20f;
	const CVector moved = position-gVrWelcomeBaseline;
	return movementInput && moved.x*moved.x+moved.y*moved.y >= 0.0025f;
}

// Runs once per rendered frame from the pad path. All menu state changes are
// edge/repeat driven so a tap remains one exact step while a held trigger can
// traverse large calibration ranges without hundreds of clicks.
void
VrDebugUpdate(const PadInput &in)
{
	LoadVrSettings();
	gQuestCpuPerformanceMode = Min(Max(xrvk::getPerformanceMode(),
		(int)QUEST_CPU_PERFORMANCE_AUTO),
		(int)QUEST_CPU_PERFORMANCE_COUNT-1);
	const double now = MonotonicMilliseconds();
	if(gVrWelcomePending && DidPlayerTakeWelcomeStep(in)){
		gVrWelcomePending = false;
		gVrMenuVisible = true;
		gVrMenuPage = VR_MENU_PAGE_ABOUT;
		gVrWelcomeFirstRun = true;
		gVrAboutDismissArmed = false;
		gVrAboutReleaseGate = false;
	}
	if(gVrMenuVisible && gVrMenuPage == VR_MENU_PAGE_ABOUT){
		const bool button = AnyVrAboutButtonDown(in);
		if(!button)
			gVrAboutDismissArmed = true;
		else if(gVrAboutDismissArmed){
			gVrMenuVisible = false;
			gVrMenuPage = VR_MENU_PAGE_SETTINGS;
			gVrAboutDismissArmed = false;
			gVrAboutReleaseGate = true;
			if(gVrWelcomeFirstRun){
				WritePrivateProfileStringA("VR", "WelcomeShown", "1",
					".\\vr_settings.ini");
				gVrWelcomeFirstRun = false;
			}
		}
		return;
	}
	if(gVrAboutReleaseGate){
		if(!AnyVrAboutButtonDown(in))
			gVrAboutReleaseGate = false;
		return;
	}
	const bool bothGrips =
		in.leftGrip >= 0.75f && in.rightGrip >= 0.75f;
	// Match the desktop service chords.  Menu remains the primary shortcut,
	// while both triggers + X is the fallback for controllers which do not
	// expose a usable Menu action to the application.
	const bool alternateMenuShortcut = bothGrips &&
		in.leftTrigger >= 0.75f && in.rightTrigger >= 0.75f &&
		(in.x || in.leftStickClick);
	const bool menuShortcut =
		(bothGrips && in.menu) || alternateMenuShortcut;
	if(menuShortcut && !gVrMenuShortcutDown){
		gVrMenuVisible = !gVrMenuVisible;
		gVrMenuPage = VR_MENU_PAGE_SETTINGS;
		gVrMenuSelection = 0;
		gVrMenuSelectDown = false;
		gVrMenuBackDown = in.b || in.leftStickClick;
		ResetMenuNavigationRepeat();
		// The fallback chord holds both triggers.  Seed their latches so
		// opening the panel cannot also change its first value.
		gVrMenuIncreaseDown = in.rightTrigger >= 0.55f;
		gVrMenuDecreaseDown = in.leftTrigger >= 0.55f;
		gVrMenuIncreaseRepeatAt = 0.0;
		gVrMenuDecreaseRepeatAt = 0.0;
	}
	gVrMenuShortcutDown = menuShortcut;

	// A stereo cutscene owns the stick clicks while it runs: R3 steps to
	// the next camera, L3 keeps the one on screen for this scene. The
	// scene name is the key, so the choice comes back the next time the
	// same cutscene plays.
	const bool cutsceneCameras = !gVrMenuVisible &&
		gCutsceneMode == VR_CUTSCENE_STEREO &&
		(CCutsceneMgr::IsRunning() || TheCamera.m_WideScreenOn);
	if(cutsceneCameras){
		// A scripted sequence has no name of its own, so every one of
		// them shares a key. They are rare and they are all the same
		// kind of shot.
		const char *scene = CCutsceneMgr::IsRunning() ?
			CCutsceneMgr::GetCutsceneName() : "scripted";
		if(scene != nil &&
		   strncmp(scene, gCutsceneCameraScene,
		     sizeof(gCutsceneCameraScene)-1) != 0){
			strncpy(gCutsceneCameraScene, scene,
				sizeof(gCutsceneCameraScene)-1);
			gCutsceneCameraScene[sizeof(gCutsceneCameraScene)-1] = '\0';
			LoadCutsceneCameraForScene(gCutsceneCameraScene);
		}
		const bool cycle = in.rightStickClick && !in.leftStickClick;
		const bool store = in.leftStickClick && !in.rightStickClick;
		if(cycle && !gCutsceneCycleDown){
			// One past the last actor wraps back to the director.
			const int cameras = androidgame::VrCutsceneCameraCount();
			gCutsceneCamera = cameras > 0 ?
				(gCutsceneCamera+1)%cameras : 0;
		}
		if(store && !gCutsceneStoreDown &&
		   gCutsceneCameraScene[0] != '\0')
			SaveCutsceneCameraForScene(gCutsceneCameraScene);
		gCutsceneCycleDown = cycle;
		gCutsceneStoreDown = store;
	}else{
		gCutsceneCycleDown = false;
		gCutsceneStoreDown = false;
		gCutsceneCameraScene[0] = '\0';
	}

	// Both stick clicks rebuild the gameplay reference space, the desktop
	// chord's job. No grips in it: reaching for one picks a weapon up. The
	// clicks are free for it because R3 and L3 hand their pad behaviour to
	// CONTROLS, off by default.
#if MIAMIVR_STEAM_FRAME
	// The Frame recentres on a View hold (android_main.cpp). Stick clicks
	// stay game actions there, so the chord is off.
	const bool recenterShortcut = false;
#else
	const bool recenterShortcut =
		!gVrMenuVisible && !VrShouldUseTheaterMode() && !cutsceneCameras &&
		in.leftStickClick && in.rightStickClick;
#endif
	if(recenterShortcut && !gTouchRecenterShortcutDown)
		androidgame::VrRecenterView();
	gTouchRecenterShortcutDown = recenterShortcut;

#if MIAMIVR_STEAM_FRAME
	// View's short press swaps first person and the chosen third person style
	// during play. The menu, the frontend and cutscenes already own that press
	// as "back" (or have no use for it), so it only counts when none of them
	// is up. gVrMenuVisible is read before this frame's menu code can close
	// the menu with the same press.
	if(in.viewBack && !gVrMenuVisible && gGameState == GS_PLAYING_GAME &&
	   !FrontEndMenuManager.m_bMenuActive &&
	   !FrontEndMenuManager.m_bGameNotLoaded &&
	   !FrontEndMenuManager.m_bWantToRestart &&
	   !FrontEndMenuManager.m_bWantToLoad &&
	   !CGame::playingIntro && FindPlayerPed() != nil &&
	   !CCutsceneMgr::IsRunning() && !CCutsceneMgr::IsCutsceneProcessing() &&
	   !TheCamera.m_WideScreenOn)
		androidgame::VrToggleThirdPerson();
#endif

	// The profiler had a chord of its own, both grips and Y. Both grips are
	// how a weapon is picked up, so players kept summoning the counters by
	// accident and could not read the game through them. It is a developer
	// tool with a row on the GRAPHICS page; that is route enough.

	if(gVrMenuVisible){
		const int itemCount = CurrentMenuItemCount();
		const int navigationPulse = MenuNavigationPulse(in.leftStickY, now);
		if(navigationPulse != 0 && itemCount > 0){
			int *selection = CurrentMenuSelection();
			int guard = itemCount;
			do{
				*selection =
					(*selection+navigationPulse+itemCount)%itemCount;
			}while(!IsMenuItemVisible(gVrMenuPage, *selection) &&
			       --guard > 0);
		}

		const bool select = in.a || in.rightStickClick;
		const bool selectPulse = select && !gVrMenuSelectDown;
		gVrMenuSelectDown = select;
		const bool repeatValue = CurrentMenuValueRepeats();
		const bool increasePulse = MenuRepeatPulse(
			in.rightTrigger >= 0.55f, gVrMenuIncreaseDown,
			gVrMenuIncreaseRepeatAt, gVrMenuIncreaseHoldStart,
			now, repeatValue);
		const bool decreasePulse = MenuRepeatPulse(
			in.leftTrigger >= 0.55f, gVrMenuDecreaseDown,
			gVrMenuDecreaseRepeatAt, gVrMenuDecreaseHoldStart,
			now, repeatValue);
		const bool positivePulse = selectPulse || increasePulse;

		if(gVrMenuPage == VR_MENU_PAGE_SETTINGS &&
		   (positivePulse || decreasePulse)){
			if(gVrMenuSelection == VR_MAIN_GRAPHICS){
				gVrMenuPage = VR_MENU_PAGE_GRAPHICS;
				gVrGraphicsSelection = 0;
			}else if(gVrMenuSelection == VR_MAIN_HUD){
				gVrMenuPage = VR_MENU_PAGE_HUD;
				gVrHudSelection = 0;
			}else if(gVrMenuSelection == VR_MAIN_LOCOMOTION_SETTINGS){
				gVrMenuPage = VR_MENU_PAGE_LOCOMOTION;
				gVrLocomotionSelection = 0;
			}else if(gVrMenuSelection == VR_MAIN_CONTROLS){
				gVrMenuPage = VR_MENU_PAGE_CONTROLS;
				gVrControlsSelection = 0;
			}else if(gVrMenuSelection == VR_MAIN_ABOUT){
				gVrMenuPage = VR_MENU_PAGE_ABOUT;
				gVrWelcomeFirstRun = false;
				gVrWelcomePending = false;
				gVrAboutDismissArmed = false;
			}
		}else if(gVrMenuPage == VR_MENU_PAGE_GRAPHICS &&
		         (positivePulse || decreasePulse)){
			if(gVrGraphicsSelection == VR_GRAPHICS_RENDER_SCALE){
				static const int scales[] = { 100, 125, 150, 175 };
				int index = 0;
				for(int i = 0; i < (int)ARRAY_SIZE(scales); i++)
					if(scales[i] == gQuestRenderScalePercent)
						index = i;
				const int direction = decreasePulse ? -1 : 1;
				index = (index+direction+(int)ARRAY_SIZE(scales)) %
					(int)ARRAY_SIZE(scales);
				gQuestRenderScalePercent = scales[index];
				SaveVrInteger("RenderScalePercent", gQuestRenderScalePercent);
			}else if(gVrGraphicsSelection == VR_GRAPHICS_SGSR){
				// Reserved for a future stereo-stable temporal AA path.
			}else if(gVrGraphicsSelection == VR_GRAPHICS_MSAA){
				// Off, 2x, 4x. It smooths the geometry it was made
				// for -- railings, poles, wires. Masks are left to the
				// mip chain; turning them into coverage instead put a
				// dither lattice across every leaf and neon tube.
				gQuestMsaaSamples = gQuestMsaaSamples >= 4 ? 1 :
					gQuestMsaaSamples >= 2 ? 4 : 2;
				SaveVrInteger("MsaaSamples", gQuestMsaaSamples);
			}else if(gVrGraphicsSelection == VR_GRAPHICS_FOLIAGE){
				gFoliageSoftness = (gFoliageSoftness+4+
					(decreasePulse ? -1 : 1))%4;
				SaveVrInteger("FoliageSoftness", gFoliageSoftness);
			}else if(gVrGraphicsSelection == VR_GRAPHICS_DIAGNOSTICS){
				gRenderDiagnostics = !gRenderDiagnostics;
				SaveVrInteger("RenderDiagnostics", gRenderDiagnostics);
				rw::vulkan::setRenderDiagnostics(gRenderDiagnostics);
			}else if(gVrGraphicsSelection == VR_GRAPHICS_PS2_ALPHA){
				gPs2AlphaTest = !gPs2AlphaTest;
				SaveVrInteger("Ps2AlphaTest", gPs2AlphaTest);
				rw::vulkan::setPs2AlphaTestEnabled(gPs2AlphaTest);
			}else if(gVrGraphicsSelection == VR_GRAPHICS_MIPMAPS){
				gGenerateMipmaps = !gGenerateMipmaps;
				SaveVrInteger("GenerateMipmaps", gGenerateMipmaps);
				rw::vulkan::setGenerateMipmaps(gGenerateMipmaps);
			}else if(gVrGraphicsSelection == VR_GRAPHICS_FXAA){
				gSpatialAaMode = !gSpatialAaMode;
				SaveFxaa();
			}else if(gVrGraphicsSelection == VR_GRAPHICS_COLOR){
				gViceCityColorEnabled = !gViceCityColorEnabled;
				SaveViceCityColor();
			}else if(gVrGraphicsSelection == VR_GRAPHICS_CPU_PERFORMANCE){
				if(xrvk::isPerformanceModeSupported()){
					const int direction = decreasePulse ? -1 : 1;
					gQuestCpuPerformanceMode =
						(gQuestCpuPerformanceMode+direction+
						 QUEST_CPU_PERFORMANCE_COUNT)%
						QUEST_CPU_PERFORMANCE_COUNT;
					if(gQuestCpuPerformanceMode <=
					   QUEST_CPU_PERFORMANCE_SUSTAINED){
						gQuestCpuPerformanceSavedMode =
							gQuestCpuPerformanceMode;
						SaveVrInteger("CpuPerformanceMode",
							gQuestCpuPerformanceSavedMode);
					}
					xrvk::setPerformanceMode(gQuestCpuPerformanceMode);
				}
			}else if(gVrGraphicsSelection == VR_GRAPHICS_GPU_PERFORMANCE){
				if(xrvk::isPerformanceModeSupported()){
					const int direction = decreasePulse ? -1 : 1;
					gQuestGpuPerformanceMode =
						(gQuestGpuPerformanceMode+direction+
						 QUEST_CPU_PERFORMANCE_COUNT)%
						QUEST_CPU_PERFORMANCE_COUNT;
					SaveVrInteger("GpuPerformanceMode",
					              gQuestGpuPerformanceMode);
					xrvk::setGpuPerformanceMode(gQuestGpuPerformanceMode);
				}
			}else if(gVrGraphicsSelection == VR_GRAPHICS_FOG){
				gDistanceFog = !gDistanceFog;
				SaveVrInteger("DistanceFog", gDistanceFog ? 1 : 0);
#if defined(MIAMIVR_DEV_TOOLS) && MIAMIVR_DEV_TOOLS
			}else if(gVrGraphicsSelection == VR_GRAPHICS_QUICK_START){
				gQuestQuickTestStart = !gQuestQuickTestStart;
				SaveVrInteger("QuickTestStart",
					gQuestQuickTestStart ? 1 : 0);
#endif
			}else if(gVrGraphicsSelection == VR_GRAPHICS_BACK){
				gVrMenuPage = VR_MENU_PAGE_SETTINGS;
			}
		}else if(gVrMenuPage == VR_MENU_PAGE_HUD &&
		         (positivePulse || decreasePulse)){
			const int direction = decreasePulse ? -1 : 1;
			switch(gVrHudSelection){
			case VR_HUD_ENABLED:
				gGameplayHudEnabled = !gGameplayHudEnabled;
				SaveGameplayHud();
				break;
			case VR_HUD_HORIZONTAL_SCALE:
				gHudWidthPercent = Min(Max(
					gHudWidthPercent+direction*5, 50), 200);
				SaveVrInteger("HudWidthPercent", gHudWidthPercent);
				break;
			case VR_HUD_SCALE:
				gHudScalePercent = Min(Max(
					gHudScalePercent+direction*5, 50), 200);
				SaveVrInteger("HudScalePercent", gHudScalePercent);
				break;
			case VR_HUD_OFFSET_X:
				gHudOffsetXCm = Min(Max(
					gHudOffsetXCm+direction, -100), 100);
				SaveVrInteger("HudOffsetXCm", gHudOffsetXCm);
				break;
			case VR_HUD_OFFSET_Y:
				gHudOffsetYCm = Min(Max(
					gHudOffsetYCm+direction, -100), 100);
				SaveVrInteger("HudOffsetYCm", gHudOffsetYCm);
				break;
			case VR_HUD_WEAPON_PANEL:
				gHudWeaponPanel = !gHudWeaponPanel;
				SaveVrInteger("HudWeaponPanel",
					gHudWeaponPanel ? 1 : 0);
				break;
			case VR_HUD_CLOCK:
				gHudClock = !gHudClock;
				SaveVrInteger("HudClock", gHudClock ? 1 : 0);
				break;
			case VR_HUD_BACK:
				gVrMenuPage = VR_MENU_PAGE_SETTINGS;
				break;
			}
		}else if(gVrMenuPage == VR_MENU_PAGE_LOCOMOTION &&
		         (positivePulse || decreasePulse)){
			const int direction = decreasePulse ? -1 : 1;
			switch(gVrLocomotionSelection){
#if MIAMIVR_STEAM_FRAME
			case VR_LOCOMOTION_VIEW:
				SetViewMode((gViewMode+3+direction)%3);
				break;
#endif
			case VR_LOCOMOTION_MOVEMENT_ORIENTATION:
				// BODY ignores physical rotation: a standing player who turns
				// around and pushes forward walks the way they used to face.
				// It stays on the page for anyone who wants it, but a brushed
				// stick or trigger steps over it -- only a deliberate press
				// selects it. Leaving BODY works either way.
				if(selectPulse)
					gQuestMovementOrientation =
						(gQuestMovementOrientation+1) %
						QUEST_MOVEMENT_ORIENTATION_COUNT;
				else{
					int next = gQuestMovementOrientation;
					do
						next = (next+
							QUEST_MOVEMENT_ORIENTATION_COUNT+
							direction) %
							QUEST_MOVEMENT_ORIENTATION_COUNT;
					while(next == QUEST_MOVEMENT_ORIENTATION_BODY);
					gQuestMovementOrientation = next;
				}
				SaveVrInteger("MovementOrientation",
					gQuestMovementOrientation);
				break;
			case VR_LOCOMOTION_TURN_MODE:
				gQuestTurnMode =
					(gQuestTurnMode+QUEST_TURN_MODE_COUNT+
					 direction) % QUEST_TURN_MODE_COUNT;
				SaveVrInteger("TurnMode", gQuestTurnMode);
				break;
			case VR_LOCOMOTION_TURN_SENSITIVITY:
				if(gQuestMovementOrientation ==
				   QUEST_MOVEMENT_ORIENTATION_HEAD_TURN_EXPERIMENTAL){
					gQuestHeadSteeringSensitivityPercent = Min(Max(
						gQuestHeadSteeringSensitivityPercent+
						 direction*5, 25), 300);
					SaveVrInteger("HeadSteeringSensitivityPercent",
						gQuestHeadSteeringSensitivityPercent);
				}else{
					gQuestTurnSensitivityPercent = Min(Max(
						gQuestTurnSensitivityPercent+
						 direction*5, 25), 300);
					SaveVrInteger("TurnSensitivityPercent",
						gQuestTurnSensitivityPercent);
				}
				break;
			case VR_LOCOMOTION_CUTSCENES:
#if MIAMIVR_STEAM_FRAME
			{
				const int count = (int)(sizeof(kCutsceneModeOrder)/
					sizeof(kCutsceneModeOrder[0]));
				int index = 0;
				while(index < count &&
				      kCutsceneModeOrder[index] != gCutsceneMode)
					index++;
				index = (index%count+count+direction)%count;
				gCutsceneMode = kCutsceneModeOrder[index];
				SaveVrInteger("CutsceneMode", gCutsceneMode);
				break;
			}
#else
				gCutsceneMode = (gCutsceneMode+2+
					(decreasePulse ? -1 : 1))%2;
				SaveVrInteger("CutsceneMode", gCutsceneMode);
				break;
#endif
			case VR_LOCOMOTION_HEAD_BOBBING:
				gQuestHeadBobbing = !gQuestHeadBobbing;
				SaveVrInteger("HeadBobbing", gQuestHeadBobbing);
				break;
#if MIAMIVR_STEAM_FRAME
			case VR_LOCOMOTION_VEHICLE_VIEW:
				gVehicleFollowRotation = !gVehicleFollowRotation;
				SaveVrInteger("VehicleFollowRotation",
					gVehicleFollowRotation ? 1 : 0);
				break;
#endif
			case VR_LOCOMOTION_REFRESH_RATE: {
				static const int rates[] = { 72, 80, 90, 120 };
				const int count =
					(int)(sizeof(rates)/sizeof(rates[0]));
				int index = 0;
				for(int i = 0; i < count; i++)
					if(rates[i] == gQuestRefreshRateHz)
						index = i;
				index = (index + direction + count) % count;
				gQuestRefreshRateHz = rates[index];
				SaveVrInteger("RefreshRate", gQuestRefreshRateHz);
				androidgame::SetPreferredRefreshRate(
					gQuestRefreshRateHz);
				break;
			}
			case VR_LOCOMOTION_SNAP_ANGLE:
				gQuestSnapTurnAngleDegrees += direction*15;
				if(gQuestSnapTurnAngleDegrees < 15)
					gQuestSnapTurnAngleDegrees = 90;
				if(gQuestSnapTurnAngleDegrees > 90)
					gQuestSnapTurnAngleDegrees = 15;
				SaveVrInteger("SnapTurnAngleDegrees",
					gQuestSnapTurnAngleDegrees);
				break;
			case VR_LOCOMOTION_RECENTER:
				// Same operation as the both-grips chord: drop the anchor and
				// let VrUpdateFirstPersonAnchor latch the pose being held now.
				// A player who sat down to start and then stood up is riding an
				// eye height taken while seated until this runs.
				androidgame::VrRecenterView();
				gVrMenuVisible = false;
				break;
			case VR_LOCOMOTION_BACK:
				gVrMenuPage = VR_MENU_PAGE_SETTINGS;
				break;
			}
		}else if(gVrMenuPage == VR_MENU_PAGE_CONTROLS &&
		         (positivePulse || decreasePulse)){
			const int direction = decreasePulse ? -1 : 1;
			const int source = VrControlsRowSource(gVrControlsSelection);
			if(source >= 0){
				// NONE is a real choice here: it is how a player switches an
				// input off, and the row it belongs to says UNUSED.
				int target = gVrPadBinding[source]+direction;
				if(target < VR_PAD_TARGET_NONE)
					target = VR_PAD_TARGET_COUNT-1;
				if(target >= VR_PAD_TARGET_COUNT)
					target = VR_PAD_TARGET_NONE;
				gVrPadBinding[source] = target;
				SavePadBinding(source);
			}else if(gVrControlsSelection == VR_CONTROLS_LAYOUT){
				// CUSTOM describes an assignment, it is not one to step into:
				// the row cycles between the two layouts that are.
				ApplyControlsLayout(
					CurrentControlsLayout() ==
						VR_CONTROLS_LAYOUT_DEFAULT ?
						kVrSwappedHandsBinding :
						kVrPadBindingDefault);
			}else if(gVrControlsSelection == VR_CONTROLS_LOOK_BEHIND){
				gVrStickLookBehind = !gVrStickLookBehind;
				SaveVrInteger("StickLookBehind", gVrStickLookBehind);
			}else if(gVrControlsSelection == VR_CONTROLS_RESET){
				ApplyControlsLayout(kVrPadBindingDefault);
			}else if(gVrControlsSelection == VR_CONTROLS_BACK){
				gVrMenuPage = VR_MENU_PAGE_SETTINGS;
			}
		}

#if MIAMIVR_STEAM_FRAME
		// View's short press is the Frame's "back" (standard.md section 1).
		const bool back = in.b || in.leftStickClick || in.viewBack;
#else
		const bool back = in.b || in.leftStickClick;
#endif
		if(back && !gVrMenuBackDown)
			ReturnFromCurrentMenuPage();
		gVrMenuBackDown = back;
	}else{
		gVrMenuIncreaseDown = false;
		gVrMenuDecreaseDown = false;
	}

	if(gDebugPreviousFrameMs > 0.0){
		const float frameMs = (float)(now-gDebugPreviousFrameMs);
		gDebugSmoothedFrameMs = gDebugSmoothedFrameMs > 0.0f ?
			gDebugSmoothedFrameMs*0.9f+frameMs*0.1f : frameMs;
		gDebugFps = gDebugSmoothedFrameMs > 0.0f ?
			(int)(1000.0f/gDebugSmoothedFrameMs+0.5f) : 0;
	}
	gDebugPreviousFrameMs = now;
}

static void
QuestMainCategoryColour(int item, uint8 *red, uint8 *green, uint8 *blue)
{
	static const uint8 colours[VR_MAIN_ITEM_COUNT][3] = {
		{100,225,255}, {255,205,90}, {90,220,210},
		{145,200,255}, {170,190,210}
	};
	*red = colours[item][0]; *green = colours[item][1];
	*blue = colours[item][2];
}

// A submenu wears the colour its category has on the settings page, and the
// pages opened from a submenu keep their parent's, so the palette says where
// the player is without reading the heading.
static void
QuestMenuPageColour(uint8 *red, uint8 *green, uint8 *blue)
{
	int category = -1;
	switch(gVrMenuPage){
	case VR_MENU_PAGE_GRAPHICS: category = VR_MAIN_GRAPHICS; break;
	case VR_MENU_PAGE_HUD: category = VR_MAIN_HUD; break;
	case VR_MENU_PAGE_LOCOMOTION:
		category = VR_MAIN_LOCOMOTION_SETTINGS; break;
	case VR_MENU_PAGE_CONTROLS: category = VR_MAIN_CONTROLS; break;
	case VR_MENU_PAGE_ABOUT: category = VR_MAIN_ABOUT; break;
	}
	if(category < 0){
		*red = 100; *green = 225; *blue = 255;
		return;
	}
	QuestMainCategoryColour(category, red, green, blue);
}

static void
BeginFullVrMenuPage(const char *heading, const char *subtitle = nil)
{
	FillVrMenuRect(0, 0, VR_MENU_WIDTH, VR_MENU_HEIGHT,
		5, 10, 20, 238);
	FillVrMenuRect(28, 28, VR_MENU_WIDTH-28, VR_MENU_HEIGHT-28,
		10, 22, 38, 245);
	DrawVrMenuText("GTA III VR", VR_MENU_WIDTH/2, 55, 7,
		255, 120, 205);
	uint8 red, green, blue;
	QuestMenuPageColour(&red, &green, &blue);
	DrawVrMenuText(heading, VR_MENU_WIDTH/2, 112, 4, red, green, blue);
	if(subtitle && subtitle[0] != '\0')
		DrawVrMenuText(subtitle, VR_MENU_WIDTH/2, 146, 2,
			170, 190, 210);
}

static void
DrawFullVrMenuRow(const char *text, int y, int scale, bool selected,
                  bool available = true, bool warning = false,
                  bool positive = false, bool highlight = false)
{
	// A row worth finding on a page where every line reads alike, in
	// the title pink because nothing else on a page uses it.
	if(highlight){
		if(selected)
			FillVrMenuRect(85, y-5, VR_MENU_WIDTH-85,
				y+scale*7+4, 95, 32, 78, 245);
		DrawVrMenuText(text, VR_MENU_WIDTH/2, y, scale,
			255, selected ? 165 : 120, selected ? 228 : 205);
		return;
	}
	if(selected)
		FillVrMenuRect(85, y-5, VR_MENU_WIDTH-85,
			y+scale*7+4,
			warning ? 125 : (positive ? 20 : (available ? 25 : 55)),
			warning ? 35 : (positive ? 105 : (available ? 95 : 62)),
			warning ? 30 : (positive ? 60 : (available ? 135 : 72)), 245);
	if(!warning && !positive && !selected && available){
		uint8 red, green, blue;
		QuestMenuPageColour(&red, &green, &blue);
		DrawVrMenuText(text, VR_MENU_WIDTH/2, y, scale, red, green, blue);
		return;
	}
	DrawVrMenuText(text, VR_MENU_WIDTH/2, y, scale,
		warning ? 255 : (positive ? (selected ? 210 : 105) :
			(selected ? 255 : (available ? 205 : 120))),
		warning ? (selected ? 225 : 105) :
			(positive ? 245 : (selected ? (available ? 245 : 180) :
			 (available ? 215 : 135))),
		warning ? (selected ? 120 : 95) :
			(positive ? 145 : (selected ? (available ? 110 : 130) :
			 (available ? 225 : 145))));
}

static void
DrawQuestSettingsPage(void)
{
	gQuestCpuPerformanceMode = Min(Max(xrvk::getPerformanceMode(),
		(int)QUEST_CPU_PERFORMANCE_AUTO),
		(int)QUEST_CPU_PERFORMANCE_COUNT-1);
	BeginFullVrMenuPage("SETTINGS");
	char rows[VR_MAIN_ITEM_COUNT][112];
	strcpy(rows[VR_MAIN_GRAPHICS], "GRAPHICS  < OPEN >");
	strcpy(rows[VR_MAIN_HUD], "HUD  < OPEN >");
	strcpy(rows[VR_MAIN_LOCOMOTION_SETTINGS], "LOCOMOTION  < OPEN >");
	strcpy(rows[VR_MAIN_CONTROLS], "CONTROLS  < OPEN >");
	strcpy(rows[VR_MAIN_ABOUT], "ABOUT GTA III VR  < OPEN >");

	for(int item = 0; item < VR_MAIN_ITEM_COUNT; item++){
		DrawFullVrMenuRow(rows[item], 190+item*60, 3,
			item == gVrMenuSelection, true, false);
		uint8 red, green, blue;
		QuestMainCategoryColour(item, &red, &green, &blue);
		if(item != gVrMenuSelection)
			DrawVrMenuText(rows[item], VR_MENU_WIDTH/2,
				190+item*60, 3, red, green, blue);
	}
	DrawVrMenuText(
		"LEFT STICK SELECT   L2 PREVIOUS   R2 OR A NEXT   B CLOSE",
		VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
}

static void
DrawQuestGraphicsPage(void)
{
	BeginFullVrMenuPage("GRAPHICS", VR_PLATFORM_NAME " RENDERING AND PERFORMANCE");
	xrvk::RenderScaleStatus scaleStatus = {};
	const bool scaleStatusValid = xrvk::getRenderScaleStatus(&scaleStatus);
	char rows[VR_GRAPHICS_ITEM_COUNT][112];
	if(scaleStatusValid)
		snprintf(rows[VR_GRAPHICS_RENDER_SCALE], sizeof(rows[0]),
			"OUTPUT REQUEST < %d%% >  ACTIVE %.0f%% - RESTART",
			gQuestRenderScalePercent, scaleStatus.effectivePercent);
	else
		snprintf(rows[VR_GRAPHICS_RENDER_SCALE], sizeof(rows[0]),
			"OUTPUT REQUEST  < %d%% - RESTART >",
			gQuestRenderScalePercent);
	snprintf(rows[VR_GRAPHICS_SGSR], sizeof(rows[0]),
		"TEMPORAL AA < DISABLED - UNSTABLE >");
	snprintf(rows[VR_GRAPHICS_MSAA], sizeof(rows[0]),
		"* STEREO MSAA  < %s - RESTART >",
		gQuestMsaaSamples >= 4 ? "4X" :
		gQuestMsaaSamples >= 2 ? "2X" : "OFF");
	snprintf(rows[VR_GRAPHICS_FXAA], sizeof(rows[0]),
		"SPATIAL AA  < %s >", gSpatialAaMode ? "ON" : "OFF");
	snprintf(rows[VR_GRAPHICS_PS2_ALPHA], sizeof(rows[0]),
		"* PS2 ALPHA (FENCE FIX)  < %s >",
		gPs2AlphaTest ? "ON" : "OFF");
	snprintf(rows[VR_GRAPHICS_MIPMAPS], sizeof(rows[0]),
		"* GENERATE MIPMAPS  < %s >",
		gGenerateMipmaps ? "ON (RESTART)" : "OFF");
	snprintf(rows[VR_GRAPHICS_FOLIAGE], sizeof(rows[0]),
		"* FOLIAGE SOFTNESS  < %s >",
		gFoliageSoftness == 3 ? "1.5 LEVELS" :
		gFoliageSoftness == 2 ? "1.0 LEVEL" :
		gFoliageSoftness == 1 ? "0.5 LEVEL" : "OFF");
	snprintf(rows[VR_GRAPHICS_DIAGNOSTICS], sizeof(rows[0]),
		"* RENDERER LOG  < %s >",
		gRenderDiagnostics ? "ON" : "OFF");
	snprintf(rows[VR_GRAPHICS_COLOR], sizeof(rows[0]),
		"COLOR FILTER  < %s >", gViceCityColorEnabled ? "ON" : "OFF");
	snprintf(rows[VR_GRAPHICS_CPU_PERFORMANCE], sizeof(rows[0]),
		"CPU PERFORMANCE < %s > ACTIVE %s",
		QuestCpuPerformanceModeName(gQuestCpuPerformanceMode),
		xrvk::getActivePerformanceMode() >= 0 ?
			QuestCpuPerformanceModeName(xrvk::getActivePerformanceMode()) :
			"PENDING");
	snprintf(rows[VR_GRAPHICS_GPU_PERFORMANCE], sizeof(rows[0]),
		"GPU PERFORMANCE < %s > ACTIVE %s",
		QuestCpuPerformanceModeName(gQuestGpuPerformanceMode),
		xrvk::getActiveGpuPerformanceMode() >= 0 ?
			QuestCpuPerformanceModeName(xrvk::getActiveGpuPerformanceMode()) :
			"PENDING");
	snprintf(rows[VR_GRAPHICS_FOG], sizeof(rows[0]),
		"DISTANCE FOG  < %s >", gDistanceFog ? "ON" : "OFF");
#if defined(MIAMIVR_DEV_TOOLS) && MIAMIVR_DEV_TOOLS
	snprintf(rows[VR_GRAPHICS_QUICK_START], sizeof(rows[0]),
		"QUICK TEST START  < %s - NEXT LAUNCH >",
		gQuestQuickTestStart ? "ON" : "OFF");
#endif
	strcpy(rows[VR_GRAPHICS_BACK], "BACK TO SETTINGS");
	for(int item = 0; item < VR_GRAPHICS_ITEM_COUNT; item++)
		// Compact rows leave room for the status footer.
		DrawFullVrMenuRow(rows[item], 174+item*25, 2,
			item == gVrGraphicsSelection, true, false,
			item == VR_GRAPHICS_MIPMAPS ||
				item == VR_GRAPHICS_PS2_ALPHA ||
				item == VR_GRAPHICS_MSAA ||
				item == VR_GRAPHICS_DIAGNOSTICS ||
				item == VR_GRAPHICS_FOLIAGE);
	if(scaleStatusValid){
		char activeScale[192];
		snprintf(activeScale, sizeof(activeScale),
			"OUTPUT ACTIVE %.0f%%  %u X %u  BASE %u X %u%s%s",
			scaleStatus.effectivePercent,
			scaleStatus.actualWidth, scaleStatus.actualHeight,
			scaleStatus.recommendedWidth, scaleStatus.recommendedHeight,
			scaleStatus.fallbackReason !=
				xrvk::RENDER_SCALE_FALLBACK_NONE ? "  FALLBACK: " : "",
			scaleStatus.fallbackReason !=
				xrvk::RENDER_SCALE_FALLBACK_NONE ?
				xrvk::getRenderScaleFallbackReasonName(
					scaleStatus.fallbackReason) : "");
		const bool fallback = scaleStatus.fallbackReason !=
			xrvk::RENDER_SCALE_FALLBACK_NONE;
		DrawVrMenuText(activeScale, VR_MENU_WIDTH/2, 674, 2,
			fallback ? 255 : 120, fallback ? 95 : 220,
			fallback ? 85 : 255);
		DrawVrMenuText("NATIVE SCENE  SPATIAL AA SINGLE-FRAME",
			VR_MENU_WIDTH/2, 692, 2, 170, 190, 210);
		if(scaleStatus.previousFallbackReason !=
		   xrvk::RENDER_SCALE_FALLBACK_NONE){
			char recoveredScale[192];
			snprintf(recoveredScale, sizeof(recoveredScale),
				"LAST FALLBACK %d%% TO %d%%: %s",
				scaleStatus.previousFallbackRequestedPercent,
				scaleStatus.previousFallbackPercent,
				xrvk::getRenderScaleFallbackReasonName(
					scaleStatus.previousFallbackReason));
			DrawVrMenuText(recoveredScale, VR_MENU_WIDTH/2, 710, 2,
				255, 105, 85);
		}
	}
	if(gQuestRenderScalePercent >= 150)
		DrawVrMenuText("150/175% IS EXPERIMENTAL: HIGH GPU AND MEMORY LOAD",
			VR_MENU_WIDTH/2, 746, 2, 255, 120, 95);
	else if(gQuestRenderScalePercent > 100)
		DrawVrMenuText("HIGHER SCALE SHARPENS THE IMAGE BUT INCREASES GPU LOAD",
			VR_MENU_WIDTH/2, 746, 2, 255, 175, 95);
}

static void
DrawQuestHudPage(void)
{
	BeginFullVrMenuPage("HUD SETTINGS",
		"HEAD-LOCKED GAMEPLAY INTERFACE");
	char rows[VR_HUD_ITEM_COUNT][112];
	snprintf(rows[VR_HUD_ENABLED], sizeof(rows[0]),
		"GAMEPLAY HUD  < %s >", gGameplayHudEnabled ? "ON" : "OFF");
	snprintf(rows[VR_HUD_HORIZONTAL_SCALE], sizeof(rows[0]),
		"HORIZONTAL WIDTH  < %d%% >", gHudWidthPercent);
	snprintf(rows[VR_HUD_SCALE], sizeof(rows[0]),
		"UNIFORM SCALE  < %d%% >", gHudScalePercent);
	snprintf(rows[VR_HUD_OFFSET_X], sizeof(rows[0]),
		"HORIZONTAL OFFSET  < %+d CM >", gHudOffsetXCm);
	snprintf(rows[VR_HUD_OFFSET_Y], sizeof(rows[0]),
		"VERTICAL OFFSET  < %+d CM >", gHudOffsetYCm);
	snprintf(rows[VR_HUD_WEAPON_PANEL], sizeof(rows[0]),
		"WEAPON ICON AND AMMO  < %s >", gHudWeaponPanel ? "ON" : "OFF");
	snprintf(rows[VR_HUD_CLOCK], sizeof(rows[0]),
		"CLOCK  < %s >", gHudClock ? "ON" : "OFF");
	strcpy(rows[VR_HUD_BACK], "BACK TO SETTINGS");
	for(int item = 0; item < VR_HUD_ITEM_COUNT; item++)
		DrawFullVrMenuRow(rows[item], 190+item*60, 3,
			item == gVrHudSelection);
	DrawVrMenuText(
		"LEFT STICK SELECT   L2 MINUS   R2 OR A PLUS   B BACK",
		VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
}

static void
DrawQuestLocomotionPage(void)
{
	BeginFullVrMenuPage("LOCOMOTION",
		"MOVEMENT AND COMFORT TURNING");
	char rows[VR_LOCOMOTION_ITEM_COUNT][112];
#if MIAMIVR_STEAM_FRAME
	snprintf(rows[VR_LOCOMOTION_VIEW], sizeof(rows[0]),
		"VIEW  < %s >",
		gViewMode == VR_VIEW_THIRD_SCREEN ? "THIRD: 3D SCREEN" :
		gViewMode == VR_VIEW_THIRD_SHOULDER ? "THIRD: OVER SHOULDER" :
		"FIRST PERSON");
#endif
	snprintf(rows[VR_LOCOMOTION_MOVEMENT_ORIENTATION], sizeof(rows[0]),
		"MOVEMENT DIRECTION  < %s >",
		gQuestMovementOrientation ==
			QUEST_MOVEMENT_ORIENTATION_HEAD_TURN_EXPERIMENTAL ?
				"HEAD TURN EXP" :
		gQuestMovementOrientation == QUEST_MOVEMENT_ORIENTATION_HEAD ?
			"HEAD" : "BODY");
	snprintf(rows[VR_LOCOMOTION_TURN_MODE], sizeof(rows[0]),
		"TURNING  < %s >",
		gQuestTurnMode == QUEST_TURN_SNAP ? "SNAP" : "SMOOTH");
	if(gQuestMovementOrientation ==
	   QUEST_MOVEMENT_ORIENTATION_HEAD_TURN_EXPERIMENTAL)
		snprintf(rows[VR_LOCOMOTION_TURN_SENSITIVITY], sizeof(rows[0]),
			"HEAD TURN SENSITIVITY  < %d%% >",
			gQuestHeadSteeringSensitivityPercent);
	else
		snprintf(rows[VR_LOCOMOTION_TURN_SENSITIVITY], sizeof(rows[0]),
			"SMOOTH TURN SENSITIVITY  < %d%% >",
			gQuestTurnSensitivityPercent);
	snprintf(rows[VR_LOCOMOTION_SNAP_ANGLE], sizeof(rows[0]),
		"SNAP TURN ANGLE  < %d DEG >", gQuestSnapTurnAngleDegrees);
	snprintf(rows[VR_LOCOMOTION_HEAD_BOBBING], sizeof(rows[0]),
		"WALKING HEAD BOB  < %s >", gQuestHeadBobbing ? "ON" : "OFF");
#if MIAMIVR_STEAM_FRAME
	snprintf(rows[VR_LOCOMOTION_CUTSCENES], sizeof(rows[0]),
		"CUTSCENES  < %s >",
		gCutsceneMode == VR_CUTSCENE_STEREO ? "STEREO  R3 CAMERA  L3 KEEP" :
		gCutsceneMode == VR_CUTSCENE_CINEMA_3D_SUBTLE ? "CINEMA 3D  SUBTLE" :
		gCutsceneMode == VR_CUTSCENE_CINEMA_3D ? "CINEMA 3D" :
		gCutsceneMode == VR_CUTSCENE_CINEMA_3D_DEEP ? "CINEMA 3D  DEEP" :
		"CINEMA SCREEN");
#else
	snprintf(rows[VR_LOCOMOTION_CUTSCENES], sizeof(rows[0]),
		"CUTSCENES  < %s >",
		gCutsceneMode ? "STEREO  R3 CAMERA  L3 KEEP" : "CINEMA SCREEN");
#endif
#if MIAMIVR_STEAM_FRAME
	snprintf(rows[VR_LOCOMOTION_VEHICLE_VIEW], sizeof(rows[0]),
		"CAR VIEW  < %s >",
		gVehicleFollowRotation ? "FOLLOWS TILT" : "LEVEL HORIZON");
#endif
	snprintf(rows[VR_LOCOMOTION_REFRESH_RATE], sizeof(rows[0]),
		"REFRESH RATE  < %d HZ >", gQuestRefreshRateHz);
	strcpy(rows[VR_LOCOMOTION_RECENTER], "RECENTER VIEW");
	strcpy(rows[VR_LOCOMOTION_BACK], "BACK TO SETTINGS");
#if MIAMIVR_STEAM_FRAME
	// Eleven rows: a tighter pitch, with the hint and footer pushed down.
	const int rowPitch = 48, hintY = 704, footerY = 726;
#else
	const int rowPitch = 68, hintY = 668, footerY = 718;
#endif
	for(int item = 0; item < VR_LOCOMOTION_ITEM_COUNT; item++)
		DrawFullVrMenuRow(rows[item], 190+item*rowPitch, 3,
			item == gVrLocomotionSelection);
	// The one setting a standing player can be hurt by without knowing why.
	if(gVrLocomotionSelection == VR_LOCOMOTION_MOVEMENT_ORIENTATION)
		DrawVrMenuText(
			"BODY IGNORES PHYSICAL TURNS - PRESS A TO REACH IT",
			VR_MENU_WIDTH/2, hintY, 2, 255, 205, 90);
#if MIAMIVR_STEAM_FRAME
	else if(gVrLocomotionSelection == VR_LOCOMOTION_VIEW)
		DrawVrMenuText("SHORT PRESS VIEW TO SWITCH IN PLAY",
			VR_MENU_WIDTH/2, hintY, 2, 255, 205, 90);
	else if(gVrLocomotionSelection == VR_LOCOMOTION_VEHICLE_VIEW)
		DrawVrMenuText("FIRST PERSON ONLY - FOLLOWS PITCH AND ROLL",
			VR_MENU_WIDTH/2, hintY, 2, 255, 205, 90);
#endif
	DrawVrMenuText(
		"LEFT STICK SELECT   L2 PREVIOUS   R2 OR A NEXT   B BACK",
		VR_MENU_WIDTH/2, footerY, 2, 170, 190, 210);
}

static void
DrawQuestControlsPage(void)
{
	static const char *const layoutNames[VR_CONTROLS_LAYOUT_COUNT] = {
		"DEFAULT", "SWAPPED HANDS", "CUSTOM"
	};
	BeginFullVrMenuPage("CONTROLS",
		"ON FOOT BUTTONS - DRIVING AND WEAPON GESTURES ARE FIXED");
	char rows[VR_CONTROLS_ITEM_COUNT][112];
	snprintf(rows[VR_CONTROLS_LAYOUT], sizeof(rows[0]),
		"LAYOUT  < %s >", layoutNames[CurrentControlsLayout()]);
	for(int item = VR_CONTROLS_A; item <= VR_CONTROLS_RIGHT_STICK_CLICK;
	    item++){
		const int source = VrControlsRowSource(item);
		snprintf(rows[item], sizeof(rows[0]), "%s  < %s >",
			VrPadSourceName(source),
			VrPadTargetName(gVrPadBinding[source]));
	}
	snprintf(rows[VR_CONTROLS_LOOK_BEHIND], sizeof(rows[0]),
		"R3 LOOKS BEHIND  < %s >", gVrStickLookBehind ? "ON" : "OFF");
	strcpy(rows[VR_CONTROLS_RESET], "RESET TO DEFAULTS");
	strcpy(rows[VR_CONTROLS_BACK], "BACK TO SETTINGS");
	for(int item = 0; item < VR_CONTROLS_ITEM_COUNT; item++)
		DrawFullVrMenuRow(rows[item], 185+item*46, 3,
			item == gVrControlsSelection);
	DrawVrMenuText(
		"LEFT STICK SELECT   L2 PREVIOUS   R2 OR A NEXT   B BACK",
		VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
}

static void
DrawQuestAboutPage(void)
{
	BeginFullVrMenuPage(gVrWelcomeFirstRun ?
		"WELCOME TO GTA III VR" : "ABOUT GTA III VR",
		"VERSION " GTA3VR_VERSION_TEXT " - NOT FOR SALE");
	static const char *lines[] = {
		"OPEN THE VR MENU: HOLD BOTH GRIPS + MENU",
		"MENU ON ITS OWN IS START: PAUSE",
		"VIEW, SHORT PRESS: BACK, OR SWITCH FIRST / THIRD PERSON IN PLAY",
		"VIEW, HOLD: RECENTRE",
		"THE GAME STARTS IN THIRD PERSON ON THE 3D SCREEN",
		"IF THAT FEELS FLAT, TRY SETTINGS > LOCOMOTION > VIEW",
		"CLASSIC VEGETATION AND MSAA OFF ARE THE LIGHTEST ON " VR_PLATFORM_NAME,
		"THIS PORT IS STILL IN DEVELOPMENT",
		"PRESS ANY BUTTON TO CLOSE"
	};
	for(int index = 0; index < (int)ARRAY_SIZE(lines); index++){
		const bool warning = index >= 6 && index <= 7;
		const bool close = index == (int)ARRAY_SIZE(lines)-1;
		DrawVrMenuText(lines[index], VR_MENU_WIDTH/2, 190+index*40,
			2, close ? 255 : (warning ? 255 : 215),
			close ? 205 : (warning ? 145 : 225),
			close ? 80 : (warning ? 70 : 235));
	}
}


// Fills the same 1024x768 menu surface as desktop. The FPS line uses a
// 512x128 surface.
const unsigned char *
VrDebugPixels(int *width, int *height)
{
	*width = gVrMenuVisible ? VR_MENU_WIDTH : VR_DEBUG_WIDTH;
	*height = gVrMenuVisible ? VR_MENU_HEIGHT : VR_FPS_HEIGHT;
	if(!gDebugVisible && !gVrMenuVisible)
		return nil;

	if(gVrMenuVisible){
		switch(gVrMenuPage){
		case VR_MENU_PAGE_GRAPHICS:
			DrawQuestGraphicsPage();
			break;
		case VR_MENU_PAGE_HUD:
			DrawQuestHudPage();
			break;
		case VR_MENU_PAGE_LOCOMOTION:
			DrawQuestLocomotionPage();
			break;
		case VR_MENU_PAGE_CONTROLS:
			DrawQuestControlsPage();
			break;
		case VR_MENU_PAGE_ABOUT:
			DrawQuestAboutPage();
			break;
		default:
			DrawQuestSettingsPage();
			break;
		}
		return gVrMenuPixels;
	}

	for(int pixel = 0; pixel < VR_DEBUG_WIDTH*(*height); pixel++){
		gDebugPixels[pixel*4+0] = 5; gDebugPixels[pixel*4+1] = 4;
		gDebugPixels[pixel*4+2] = 12; gDebugPixels[pixel*4+3] = 225;
	}
	char value[128];
	sprintf(value, "OPENXR FPS:%d", gDebugFps);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 10, 3, 255, 230, 64);
	return gDebugPixels;
}

bool
VrMenuConsumesInput(void)
{
	// Keep the service chord from leaking through as Pause when the menu is
	// opened or closed. Gameplay resumes only after Menu has been released.
	return gVrMenuVisible || gVrMenuShortcutDown;
}

bool
VrViceCityColorEnabled(void)
{
	LoadVrSettings();
	return gViceCityColorEnabled;
}

int
VrCutsceneCamera(void)
{
	return gCutsceneCamera;
}

// 1 when cutscenes are rendered immersively (STEREO), else 0: every cinema
// mode, 3D or not, is the theater screen as far as the callers are concerned.
int
VrCutsceneMode(void)
{
	LoadVrSettings();
	return gCutsceneMode == VR_CUTSCENE_STEREO ? 1 : 0;
}

#if MIAMIVR_STEAM_FRAME
// How deep the 3D cinema picture goes: the separation of the two images at
// infinity, as a fraction of the viewer's IPD (0 = flat). Under 1 keeps the
// eyes from diverging; the far background lands at screen distance / (1-k).
float
VrCinema3DDepth(void)
{
	LoadVrSettings();
	switch(gCutsceneMode){
	case VR_CUTSCENE_CINEMA_3D_SUBTLE: return 0.35f;
	case VR_CUTSCENE_CINEMA_3D:        return 0.55f;
	case VR_CUTSCENE_CINEMA_3D_DEEP:   return 0.75f;
	}
	return 0.0f;
}

int
VrViewMode(void)
{
	LoadVrSettings();
	return gViewMode;
}

bool
VrThirdPersonScreen(void)
{
	return VrViewMode() == VR_VIEW_THIRD_SCREEN;
}

bool
VrThirdPersonShoulder(void)
{
	return VrViewMode() == VR_VIEW_THIRD_SHOULDER;
}

void
VrToggleThirdPerson(void)
{
	LoadVrSettings();
	SetViewMode(gViewMode == VR_VIEW_FIRST_PERSON ?
		gThirdPersonStyle : (int)VR_VIEW_FIRST_PERSON);
}
#endif

int
VrFoliageSoftness(void)
{
	LoadVrSettings();
	return gFoliageSoftness;
}

bool
VrRenderDiagnostics(void)
{
	LoadVrSettings();
	return gRenderDiagnostics != 0;
}

bool
VrPs2AlphaTest(void)
{
	LoadVrSettings();
	return gPs2AlphaTest != 0;
}

bool
VrGenerateMipmaps(void)
{
	LoadVrSettings();
	return gGenerateMipmaps != 0;
}


bool
VrFxaaEnabled(void)
{
	LoadVrSettings();
	return gSpatialAaMode != 0;
}

int
VrSpatialAaMode(void)
{
	LoadVrSettings();
	return gSpatialAaMode;
}

bool
VrDistanceFogEnabled(void)
{
	LoadVrSettings();
	return gDistanceFog;
}

bool
VrHudWeaponPanelEnabled(void)
{
	LoadVrSettings();
#if MIAMIVR_STEAM_FRAME
	// With the wrist ammo counter gone, the corner readout is the only place
	// the round count is shown.
	if(gViewMode != VR_VIEW_FIRST_PERSON)
		return true;
#endif
	return gHudWeaponPanel;
}

bool
VrHudClockEnabled(void)
{
	LoadVrSettings();
	return gHudClock;
}

bool
VrGameplayHudEnabled(void)
{
	LoadVrSettings();
#if MIAMIVR_STEAM_FRAME
	// The interface has to be there in third person whatever the first
	// person preference says; nothing else carries it.
	if(gViewMode != VR_VIEW_FIRST_PERSON)
		return true;
#endif
	return gGameplayHudEnabled;
}

void
VrGetGameplayHudSettings(int *widthPercent, int *scalePercent,
                         int *offsetXCm, int *offsetYCm)
{
	LoadVrSettings();
	if(widthPercent) *widthPercent = gHudWidthPercent;
	if(scalePercent) *scalePercent = gHudScalePercent;
	if(offsetXCm) *offsetXCm = gHudOffsetXCm;
	if(offsetYCm) *offsetYCm = gHudOffsetYCm;
}

bool
VrHeadBobbingEnabled(void)
{
	LoadVrSettings();
	return gQuestHeadBobbing != 0;
}

bool
VrUsesHeadRelativeMovement(void)
{
	LoadVrSettings();
	return gQuestMovementOrientation != QUEST_MOVEMENT_ORIENTATION_BODY;
}

bool
VrUsesHeadDirectedMovement(void)
{
	LoadVrSettings();
	return gQuestMovementOrientation ==
		QUEST_MOVEMENT_ORIENTATION_HEAD;
}

bool
VrUsesExperimentalHeadTurning(void)
{
	LoadVrSettings();
	return gQuestMovementOrientation ==
		QUEST_MOVEMENT_ORIENTATION_HEAD_TURN_EXPERIMENTAL;
}

float
VrHeadTurnScale(void)
{
	LoadVrSettings();
	return (float)gQuestHeadSteeringSensitivityPercent/100.0f;
}

bool
VrUsesSnapTurn(void)
{
	LoadVrSettings();
	return gQuestTurnMode == QUEST_TURN_SNAP;
}

float
VrSmoothTurnScale(void)
{
	LoadVrSettings();
	return (float)gQuestTurnSensitivityPercent/100.0f;
}

int
VrSnapTurnAngleDegrees(void)
{
	LoadVrSettings();
	return gQuestSnapTurnAngleDegrees;
}

int
VrPadBinding(int source)
{
	LoadVrSettings();
	if(source < 0 || source >= VR_PAD_SOURCE_COUNT)
		return VR_PAD_TARGET_NONE;
	return gVrPadBinding[source];
}

int
VrPadBindingDefault(int source)
{
	if(source < 0 || source >= VR_PAD_SOURCE_COUNT)
		return VR_PAD_TARGET_NONE;
	return kVrPadBindingDefault[source];
}


// Dynamic point lights, car reflections and modern water belong to the Vice
// City port's LIGHTING and WATER pages, which are not carried. STUB (T3): the
// backend is handed an empty light list, and no setting can turn them on.
void
VrPushDynamicLights(void)
{
	LoadVrSettings();
	rw::vulkan::setPointLights(nil, 0, 0.0f);
}

// No tracked scopes: the sniper, M16 and rocket camera zoom is not applied in
// the headset (the view stays the player's own).
float
VrScopeZoomFactor(void)
{
	return 1.0f;
}

bool
VrVehicleFollowRotation(void)
{
	LoadVrSettings();
	return gVehicleFollowRotation;
}

bool
VrHeadAimEnabled(void)
{
	LoadVrSettings();
	return gHeadAim;
}

}
