#include <time.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "platform_android.h"

#include "common.h"
#include "crossplatform.h"
#include "platform.h"
#include "skeleton.h"
#include "android.h"
#include "VrVehicleSeatRecenter.h"
#include "xr_vulkan_session.h"

#include "main.h"
#include "FileMgr.h"
#include "Frontend.h"
#include "Game.h"
#include "Pad.h"
#include "Text.h"
#include "Timer.h"
#include "Camera.h"
#include "General.h"
#include "Timecycle.h"
#include "DMAudio.h"
#include "MemoryMgr.h"
#include "Lists.h"	// CPtrList, needed by PlayerInfo.h
#include "PlayerInfo.h"
#include "PlayerPed.h"
#include "CutsceneMgr.h"
#include "CutsceneObject.h"
#include "RpAnimBlend.h"
#include "AnimBlendClumpData.h"
#include "Bones.h"
#include "ModelIndices.h"
#include "Vehicle.h"
#include "World.h"
#include "Draw.h"
#include "Renderer.h"
#include "Streaming.h"
#if MIAMIVR_STEAM_FRAME
#include "frame_diag.h"
#endif
#include "vulkan/rwvk.h"

#include <android/log.h>
#define ALOG(...) __android_log_print(ANDROID_LOG_INFO, "GTA3VR", __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, "GTA3VR", __VA_ARGS__)

// ---------------------------------------------------------------------------
// Platform globals
// ---------------------------------------------------------------------------

psGlobalType PsGlobal;

extern RsGlobalType RsGlobal;

// Owned by the skeleton on every platform; the desktop builds define these in
// win.cpp / glfw.cpp.
RwUInt32 gGameState;
long _dwOperatingSystemVersion;

// CStreaming sizes its budget as (_dwMemAvailPhys - 10MB)/2. Quest 3 has 8 GB,
// but an Android app never gets to use all of it and the OpenXR compositor
// needs headroom, so the figure reported here is capped well below the
// physical total.
size_t _dwMemAvailPhys;

static bool gRwInitialised = false;
static bool gForegroundApp = true;
static VrVehicleSeatRecenter gVehicleSeatRecenter;
#if defined(MIAMIVR_DEV_TOOLS) && MIAMIVR_DEV_TOOLS
static uint32 gQuestQuickStartSkipFrames;
static uint32 gQuestQuickStartPlayableFrames;
static bool gQuestQuickStartCutsceneSkipIssued;
static bool gQuestQuickStartTimeScaleOwned;

static bool
QuestQuickTestStartEnabled(void)
{
	return GetPrivateProfileIntA("VR", "QuickTestStart", 0,
	                             ".\\vr_settings.ini") != 0;
}

static void
SkipQuestQuickStartCutsceneIfNeeded(void)
{
	if(gQuestQuickStartSkipFrames == 0)
		return;

	if(FindPlayerPed() != nil &&
	   (CCutsceneMgr::IsRunning() || TheCamera.m_WideScreenOn)){
		gQuestQuickStartPlayableFrames = 0;
		if(!gQuestQuickStartCutsceneSkipIssued){
			ALOG("quick test start: skipping startup cinema %s",
			     CCutsceneMgr::IsRunning() ?
			     CCutsceneMgr::GetCutsceneName() : "SCRIPTED WIDESCREEN");
			if(CCutsceneMgr::IsRunning())
				CCutsceneMgr::FinishCutscene();
			else
				TheCamera.FinishCutscene();
			// FinishCutscene marks the cutscene as skipped, but the script may
			// leave IsRunning true until its cleanup has completed. Do not
			// repeatedly finish the same cutscene while that happens.
			gQuestQuickStartCutsceneSkipIssued = true;
		}
		return;
	}

	// Re-arm for the next formal cutscene in the stock intro chain.
	gQuestQuickStartCutsceneSkipIssued = false;
}

static void
AdvanceQuestQuickStartIntroSkip(void)
{
	if(gQuestQuickStartSkipFrames == 0){
		if(gQuestQuickStartTimeScaleOwned){
			CTimer::SetTimeScale(1.0f);
			gQuestQuickStartTimeScaleOwned = false;
		}
		return;
	}

	--gQuestQuickStartSkipFrames;
	CPlayerPed *player = FindPlayerPed();
	const bool genuinelyPlayable =
		!CGame::playingIntro &&
		!CCutsceneMgr::IsRunning() &&
		!CCutsceneMgr::IsCutsceneProcessing() &&
		player != nil &&
		!CPad::GetPad(0)->ArePlayerControlsDisabled();

	if(!genuinelyPlayable){
		gQuestQuickStartPlayableFrames = 0;
		// Some opening shots are main.scm-controlled cameras rather than formal
		// CCutsceneMgr sequences. Speed only this armed startup interval so their
		// script timers finish quickly even when no skippable spline exists.
		if(CTimer::GetTimeScale() < 6.0f){
			CTimer::SetTimeScale(6.0f);
			gQuestQuickStartTimeScaleOwned = true;
		}
		return;
	}
	if(gQuestQuickStartTimeScaleOwned){
		CTimer::SetTimeScale(1.0f);
		gQuestQuickStartTimeScaleOwned = false;
	}

	// Require stable player control for a little over a second at 72 Hz.
	// This keeps the skip armed across the gaps between the several formal
	// startup cutscenes, but disarms it before later gameplay cutscenes.
	if(++gQuestQuickStartPlayableFrames >= 90){
		ALOG("quick test start: intro chain complete; cutscene skip disarmed");
		gQuestQuickStartSkipFrames = 0;
		gQuestQuickStartPlayableFrames = 0;
		gQuestQuickStartCutsceneSkipIssued = false;
		gQuestQuickStartTimeScaleOwned = false;
	}
}

#endif

// ---------------------------------------------------------------------------
// ps* interface
// ---------------------------------------------------------------------------

#ifdef USE_CUSTOM_ALLOCATOR
extern RwMemoryFunctions memFuncs;
#endif

RwMemoryFunctions *
psGetMemoryFunctions(void)
{
#ifdef USE_CUSTOM_ALLOCATOR
	return &memFuncs;
#else
	return nil;
#endif
}

RwBool
psInstallFileSystem(void)
{
	return TRUE;
}

RwBool
psNativeTextureSupport(void)
{
	return TRUE;
}

double
psTimer(void)
{
	// While frames are flowing, game time follows the compositor's predicted
	// display time: it advances in exact display intervals, where the wall
	// clock jitters with thread scheduling -- that jitter was directly
	// visible as vehicles pulsing forward.
	//
	// When frames STOP (blocking loads, paused session) the display time
	// freezes, and a frozen clock hangs anything that waits for time to pass
	// -- New Game stalled forever in exactly that way. So after a short
	// grace the clock switches to advancing by wall time from the frozen
	// value, and a monotonicity clamp bridges the switch back when frames
	// resume.
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	const double wallNow = ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;

	extern long long gVrFrameTimeNs;
	static double maxReturned;
	double result;
	if(gVrFrameTimeNs != 0){
		static long long lastSeenNs;
		static double wallAtChange;
		if(gVrFrameTimeNs != lastSeenNs){
			lastSeenNs = gVrFrameTimeNs;
			wallAtChange = wallNow;
		}
		const double frozenFor = wallNow - wallAtChange;
		result = gVrFrameTimeNs / 1000000.0;
		if(frozenFor > 50.0)
			result += frozenFor - 50.0;
	}else
		result = wallNow;

	if(result < maxReturned)
		result = maxReturned;
	maxReturned = result;
	return result;
}

void
psMouseSetPos(RwV2d *pos)
{
	// No pointer device on a headset.
	(void)pos;
}

RwBool
psCameraBeginUpdate(RwCamera *camera)
{
	// The OpenXR layer has already opened the multiview render pass by the time
	// the game reaches here, so the camera update is unconditional.
	if(!RwCameraBeginUpdate(camera)){
		gForegroundApp = FALSE;
		return FALSE;
	}
	return TRUE;
}

void
psCameraShowRaster(RwCamera *camera)
{
	// Presentation belongs to xrEndFrame; nothing to flip here.
	(void)camera;
}

RwImage *
psGrabScreen(RwCamera *camera)
{
	(void)camera;
	return nil;
}

RwBool
psSelectDevice(void)
{
	RwVideoMode videoMode;
	RwEngineGetVideoModeInfo(&videoMode, RwEngineGetCurrentVideoMode());
	if(videoMode.flags & rwVIDEOMODEEXCLUSIVE)
		PsGlobal.fullScreen = TRUE;
	return TRUE;
}

RwInt32
_psGetNumVideModes(void)
{
	// The headset owns the resolution; there is exactly one mode.
	return 1;
}

RwChar **
_psGetVideoModeList(void)
{
	static RwChar *modeName = nil;
	static RwChar *modeList[1];
	if(modeName == nil){
		modeName = (RwChar*)malloc(32);
		snprintf(modeName, 32, "%dx%d", RsGlobal.maximumWidth, RsGlobal.maximumHeight);
	}
	modeList[0] = modeName;
	return modeList;
}

void
_psSelectScreenVM(RwInt32 videoMode)
{
	(void)videoMode;
}

RwBool
_psSetVideoMode(RwInt32 subSystem, RwInt32 videoMode)
{
	(void)subSystem;
	(void)videoMode;
	return TRUE;
}

void
_InputTranslateShiftKeyUpDown(RsKeyCodes *rs)
{
	(void)rs;
}

long
_InputInitialiseMouse(bool exclusive)
{
	(void)exclusive;
	return 0;
}

void _InputShutdownMouse(void) {}
bool _InputMouseNeedsExclusive(void) { return false; }
void _InputInitialiseJoys(void) {}

void
HandleExit(void)
{
	RsGlobal.quit = TRUE;
}

// Saves and settings live beside the staged game data, in the app-specific
// external directory, so they survive an app update and are reachable over adb
// without any storage permission.
const char *
_psGetUserFilesFolder(void)
{
	static char path[512] = "";
	if(path[0] == '\0'){
		const char *root = platform::storageRoot();
		snprintf(path, sizeof(path), "%s/userfiles",
		         root != nil && root[0] != '\0' ? root : ".");
		mkdir(path, 0770);
	}
	return path;
}

static androidgame::PadInput gPadInput;

// Read by psTimer; written once per frame from the XR layer.
long long gVrFrameTimeNs;

namespace androidgame {
void
SetPadInput(const PadInput &input)
{
	gPadInput = input;
}

const PadInput &
GetPadInput(void)
{
	return gPadInput;
}

// The pad button a binding target names. Anything the remapping page cannot
// reach -- the sticks, Start, the D-pad -- has no entry and is never touched by
// VrApplyPadBindings.
static int16 *
PadTargetField(CControllerState *state, int target)
{
	switch(target){
	case VR_PAD_TARGET_SQUARE:   return &state->Square;
	case VR_PAD_TARGET_CROSS:    return &state->Cross;
	case VR_PAD_TARGET_CIRCLE:   return &state->Circle;
	case VR_PAD_TARGET_TRIANGLE: return &state->Triangle;
	case VR_PAD_TARGET_L1:       return &state->LeftShoulder1;
	case VR_PAD_TARGET_R1:       return &state->RightShoulder1;
	case VR_PAD_TARGET_L2:       return &state->LeftShoulder2;
	case VR_PAD_TARGET_R2:       return &state->RightShoulder2;
	case VR_PAD_TARGET_L3:       return &state->LeftShock;
	case VR_PAD_TARGET_R3:       return &state->RightShock;
	}
	return nil;
}

// The pad buttons a vehicle action lives on, for the controller configuration
// the frontend has chosen (CPad::Mode, the CONFIG 1..4 of the controls page).
// This is the same table CPad itself reads in GetHandBrake, GetExitVehicle,
// GetHorn, ChangeStationJustDown and GetCarGunFired; each action has a button
// of its own in every configuration, so no two of them can collide.
enum eVehicleAction
{
	VEHICLE_ACTION_HANDBRAKE,
	VEHICLE_ACTION_EXIT,
	VEHICLE_ACTION_HORN,
	VEHICLE_ACTION_RADIO,
	VEHICLE_ACTION_FIRE
};

static int16 *
VehicleActionField(CControllerState *state, int action, int mode)
{
	switch(action){
	case VEHICLE_ACTION_HANDBRAKE:
		return mode == 2 ? &state->Triangle :
		       mode == 3 ? &state->LeftShoulder1 : &state->RightShoulder1;
	case VEHICLE_ACTION_EXIT:
		return mode == 2 ? &state->LeftShoulder1 : &state->Triangle;
	case VEHICLE_ACTION_HORN:
		return mode == 1 ? &state->LeftShoulder1 :
		       mode == 2 ? &state->RightShoulder1 : &state->LeftShock;
	case VEHICLE_ACTION_RADIO:
		return mode == 1 ? &state->Select :
		       mode == 2 ? &state->LeftShock :
		       mode == 3 ? &state->Circle : &state->LeftShoulder1;
	case VEHICLE_ACTION_FIRE:
		return mode == 3 ? &state->RightShoulder1 : &state->Circle;
	}
	return nil;
}

// The in-car layout (docs/STEAM_FRAME.md has the table):
//   right trigger accelerate, left trigger brake and reverse,
//   A handbrake, Y get in and out (and nothing else), X next radio station,
//   B fire the vehicle's weapon or the driver's gun, left stick click horn,
//   left bumper look left, right bumper look right, both look behind.
// The grips, the right stick click and the camera-mode button do nothing here.
static void
ApplyVehiclePadLayout(CControllerState *state, const PadInput &input,
                      bool includeTriggers)
{
	const int mode = CPad::GetPad(0)->GetMode();
	const int16 leftTrigger = includeTriggers ?
		(int16)(Clamp(input.leftTrigger, 0.0f, 1.0f)*255.0f) : (int16)0;
	const int16 rightTrigger = includeTriggers ?
		(int16)(Clamp(input.rightTrigger, 0.0f, 1.0f)*255.0f) : (int16)0;

	if(mode == 3){
		// Configuration 4 drives with the right stick: forward is the
		// accelerator, back is the brake.
		const int16 axis = (int16)((leftTrigger-rightTrigger)/2);
		if(axis != 0)
			state->RightStickY = axis;
	}else{
		state->Cross = Max(state->Cross, rightTrigger);
		state->Square = Max(state->Square, leftTrigger);
	}

	const struct { int action; bool pressed; } actions[] = {
		{ VEHICLE_ACTION_HANDBRAKE, input.a },
		{ VEHICLE_ACTION_EXIT,      input.y },
		{ VEHICLE_ACTION_RADIO,     input.x },
		{ VEHICLE_ACTION_FIRE,      input.b },
		{ VEHICLE_ACTION_HORN,      input.leftStickClick },
	};
	for(size_t i = 0; i < sizeof(actions)/sizeof(actions[0]); i++){
		int16 *field = VehicleActionField(state, actions[i].action, mode);
		if(field && actions[i].pressed)
			*field = 255;
	}
#if MIAMIVR_STEAM_FRAME
	// The pad's own look buttons: L2 left, R2 right, both behind. Drive-by
	// with the Uzi is a look plus B.
	if(input.leftBumper)
		state->LeftShoulder2 = 255;
	if(input.rightBumper)
		state->RightShoulder2 = 255;
#endif
}

void
VrApplyPadBindings(CControllerState *state, const PadInput &input,
                   bool includeTriggers, bool vehicleLayout)
{
	if(!state)
		return;
	// Grips and triggers keep their analogue value: the game reads the
	// shoulder buttons as a 0..255 scale, the same way the desktop VR layer
	// hands them over.
	const int16 leftTrigger =
		(int16)(Clamp(input.leftTrigger, 0.0f, 1.0f)*255.0f);
	const int16 rightTrigger =
		(int16)(Clamp(input.rightTrigger, 0.0f, 1.0f)*255.0f);
	const int16 values[VR_PAD_SOURCE_COUNT] = {
		(int16)(input.a ? 255 : 0),
		(int16)(input.b ? 255 : 0),
		(int16)(input.x ? 255 : 0),
		(int16)(input.y ? 255 : 0),
		includeTriggers ? leftTrigger : (int16)0,
		includeTriggers ? rightTrigger : (int16)0,
		(int16)(Clamp(input.leftGrip,  0.0f, 1.0f)*255.0f),
		(int16)(Clamp(input.rightGrip, 0.0f, 1.0f)*255.0f),
		(int16)(input.leftStickClick  ? 255 : 0),
		(int16)(input.rightStickClick ? 255 : 0)
	};

	for(int target = VR_PAD_TARGET_NONE+1; target < VR_PAD_TARGET_COUNT;
	    target++){
		int16 *field = PadTargetField(state, target);
		if(field)
			*field = 0;
	}
	// A vehicle has its own layout (see ApplyVehiclePadLayout): remapping is
	// an on-foot feature, and the shipped on-foot table would put the
	// accelerator on A as well as on the trigger.
	if(vehicleLayout){
		ApplyVehiclePadLayout(state, input, includeTriggers);
		return;
	}
	// Remapping is an on-foot feature; see the enum in android.h for why a
	// vehicle keeps its own assignment. In the frontend, or in a vehicle with
	// a menu open, the shipped table applies so A still confirms.
	const bool onFoot = FindPlayerVehicle() == nil &&
		!CWorld::Players[CWorld::PlayerInFocus].IsPlayerInRemoteMode();

	// Two sources may share a button; the stronger press wins rather than the
	// later one in the table.
	for(int source = 0; source < VR_PAD_SOURCE_COUNT; source++){
		const int target = onFoot ?
			VrPadBinding(source) : VrPadBindingDefault(source);
		int16 *field = PadTargetField(state, target);
		if(field)
			*field = Max(*field, values[source]);
	}
}

void
TriggerWeaponHaptic(int hand, float strength)
{
	// 120Hz for a short, punchy report; strength maps to OpenXR amplitude.
	xrvk::triggerHaptic(hand, strength, 120.0f, 60.0f);
}

void
SetPreferredRefreshRate(int hz)
{
	xrvk::setPreferredDisplayRefreshRate((float)hz);
}

void
SetFrameTimeNs(long long ns)
{
	gVrFrameTimeNs = ns;
}

void SetEyeFovDeg(float fov);
}

float gVrEyeFovDeg;

void
androidgame::SetEyeFovDeg(float fov)
{
	gVrEyeFovDeg = fov;
}

// Updated once the camera's view window has been derived for the frame.
static float gVrViewWindowX = 0.6f, gVrViewWindowY = 0.6f;

void
androidgame::GetIm2DViewWindow(float *x, float *y)
{
	*x = gVrViewWindowX;
	*y = gVrViewWindowY;
}

void
VrCaptureViewWindow(void)
{
	const RwV2d *window = RwCameraGetViewWindow(Scene.camera);
	if(window != nil && window->x > 0.01f && window->y > 0.01f){
		gVrViewWindowX = window->x;
		gVrViewWindowY = window->y;
	}
}
static float gVrSavedFov = -1.0f;

// Called at the top of DoRWStuffStartOfFrame_Horizon, before CameraSize
// derives the view window: during first person rendering the game runs on
// the eye's real field of view, exactly as the desktop BeginEye does, which
// is what sizes coronas and other screen-space sprites correctly.
void
VrApplyEyeFov(void)
{
	extern bool gVrFirstPersonActive;
	if(gVrFirstPersonActive && gVrEyeFovDeg > 1.0f){
		if(gVrSavedFov < 0.0f)
			gVrSavedFov = CDraw::GetFOV();
		CDraw::SetFOV(gVrEyeFovDeg);
	}
}

// Called when the frame's game step is done: game logic outside rendering
// keeps the original fov, mirroring the desktop's save/restore.
void
VrRestoreFov(void)
{
	if(gVrSavedFov > 0.0f){
		CDraw::SetFOV(gVrSavedFov);
		gVrSavedFov = -1.0f;
	}
}

// First person state, consumed by CRenderer to hide the player's own body.
// The body is replaced by tracked hands on foot and during physical vehicle
// driving, but DEFAULT driving retains Vice City's animated in-car Tommy.
// Set every frame below, next to the pad capture, which runs before rendering.
bool gVrFirstPersonActive;
CEntity *gVrPlayerEntity;
// First-person in a vehicle, third-person chase excluded. CPed::PreRender
// reads it to drop the occupant head the camera is sitting inside.
bool gVrInVehicle;
bool gVrHidePlayerBody = true;
#if MIAMIVR_STEAM_FRAME
// Third person over the shoulder, on foot: the chase camera is the view, so
// the tracked hands and the physical weapon layer stand down and Vice City's
// own weapon handling takes over. Read by QuestWeaponVR and vrhands_quest.
bool gVrThirdPersonOnFoot;
// The over-the-shoulder view through vehicle enter/exit. The stock camera
// slides between its vehicle and on-foot positions in that window, so it is not
// followed; instead the view is built from the player ped itself: the last
// chase pose, remembered as a heading plus a distance behind and a height above
// the ped, laid out again from wherever the ped is each frame (smoothed). It
// therefore stays on Claude while he walks round the car, climbs in or out, and
// while the car keeps rolling under him. When the window ends the view blends
// back onto the stock camera over a fraction of a second.
static CVector gVrChaseHoldHead;      // The pose last shown, world space.
static CVector gVrChaseHoldForward;   // Its heading as a level unit vector.
static float gVrChaseHoldBack;        // Metres behind the ped along that heading.
static float gVrChaseHoldHeight;      // Metres above the ped.
static bool gVrChaseHoldValid;
static CVector gVrChaseFollow;        // The smoothed follow position.
static bool gVrChaseFollowing;        // A transition is running.
static float gVrChaseBlend = 1.0f;    // 0 at the end of a hold, 1 once back on the stock camera.
static bool gVrChaseLogged;
// First person on foot with the head modes (MOVEMENT DIRECTION HEAD or HEAD
// TURN): the anchor frame is the gameplay camera and the stick is turned by the
// head. Set by VrUpdateFirstPersonAnchor.
static bool gVrHeadModeOnFoot;
// First person on foot, whichever movement direction is chosen.
static bool gVrFirstPersonWalking;
// The same, with head aim switched on: shots go where the head looks, and the
// weapon cameras follow the headset.
static bool gVrHeadAimActive;
// A weapon camera takes its Beta/Alpha from the headset. The headset's world
// yaw is the anchor's heading plus its own turn, so if the anchor followed that
// camera the view would spin up on its own. While one of these cameras is
// active the anchor therefore holds the heading the view had when it started.
static bool gVrScopeLatched;
static CVector gVrScopeBaseForward;
static CVector gVrLastAnchorForward;
static bool gVrLastAnchorForwardValid;

// The camera modes whose processors take Beta/Alpha from VrHeadAimCamera:
// Process_Sniper, Process_Rocket, Process_M16_1stPerson and
// Process_1rstPersonPedOnPC (the *_RUNABOUT modes and the PC first person).
static bool
IsHeadDrivenCameraMode(int mode)
{
	switch(mode){
	case CCam::MODE_SNIPER:
	case CCam::MODE_ROCKETLAUNCHER:
	case CCam::MODE_M16_1STPERSON:
	case CCam::MODE_HELICANNON_1STPERSON:
	case CCam::MODE_SNIPER_RUNABOUT:
	case CCam::MODE_ROCKETLAUNCHER_RUNABOUT:
	case CCam::MODE_M16_1STPERSON_RUNABOUT:
	case CCam::MODE_1STPERSON_RUNABOUT:
	case CCam::MODE_FIGHT_CAM_RUNABOUT:
		return true;
	}
	return false;
}
#endif

// The shape of the cinema screen theater mode draws on. The game lays its
// interface out for this ratio while theater mode is up, so the plane in
// android_main and CDraw both take it from here.
float
androidgame::VrTheaterAspectRatio(void)
{
	return 16.0f/9.0f;
}

bool
androidgame::VrGetViewBasis(float position[3], float right[3], float up[3],
                            float forward[3])
{
	if(!gVrFirstPersonActive)
		return false;
	rw::float32 vr[3], vu[3], va[3], vp[3];
	if(!rw::vulkan::getFirstPersonViewFrame(vr, vu, va, vp))
		return false;
	for(int i = 0; i < 3; i++){
		// getFirstPersonViewFrame answers in the RenderWare camera
		// convention, whose right column points left.
		if(right != nil) right[i] = -vr[i];
		if(up != nil) up[i] = vu[i];
		if(forward != nil) forward[i] = va[i];
		if(position != nil) position[i] = vp[i];
	}
	return true;
}

#if MIAMIVR_STEAM_FRAME
bool
androidgame::VrFirstPersonOnFoot(void)
{
	return gVrFirstPersonWalking;
}

bool
androidgame::VrHeadMovementActive(void)
{
	// The setting, in any view: first person has turned the stick by the head
	// already, and in third person the stick is relative to the stock camera;
	// either way the ped should walk the direction asked, as in the Vice City
	// build, rather than face the camera and strafe.
	return androidgame::VrUsesHeadRelativeMovement();
}

bool
androidgame::VrThirdPersonFaceMove(void)
{
	if(!androidgame::VrThirdPersonFacesMovement() ||
	   !(androidgame::VrThirdPersonScreen() ||
	     androidgame::VrThirdPersonShoulder()))
		return false;
	CPlayerPed *player = FindPlayerPed();
	if(player == nil || player->InVehicle() ||
	   CWorld::Players[CWorld::PlayerInFocus].IsPlayerInRemoteMode() ||
	   gGameState != GS_PLAYING_GAME || CCutsceneMgr::IsRunning() ||
	   CCutsceneMgr::IsCutsceneProcessing() || TheCamera.m_WideScreenOn)
		return false;
	// Aiming and firing a ranged weapon keep the stock behaviour of the PC
	// "standard" controls: the body faces where the camera looks (so shots go
	// there) and the stick strafes. Fists and melee weapons turn with the stick.
	const CWeaponInfo *info = CWeaponInfo::GetWeaponInfo(
		player->GetWeapon()->m_eWeaponType);
	if(info != nil && info->m_eWeaponFire != WEAPON_FIRE_MELEE &&
	   player->GetWeapon()->m_eWeaponType != WEAPONTYPE_UNARMED){
		CPad *pad = CPad::GetPad(0);
		if(pad->GetWeapon() || pad->GetTarget() ||
		   player->GetPedState() == PED_AIM_GUN ||
		   player->GetPedState() == PED_ATTACK)
			return false;
	}
	return true;
}

bool
androidgame::VrHeadAimDirection(float direction[3])
{
	return gVrHeadAimActive &&
		androidgame::VrGetViewBasis(nil, nil, nil, direction);
}

bool
androidgame::VrHeadAimHeading(float *heading)
{
	float direction[3];
	if(!androidgame::VrHeadAimDirection(direction))
		return false;
	if(direction[0]*direction[0]+direction[1]*direction[1] < 0.0001f)
		return false;
	// The ped's own convention: it faces (-sin, cos) of this angle.
	*heading = Atan2(-direction[0], direction[1]);
	return true;
}

bool
androidgame::VrHeadAimCamera(float *beta, float *alpha)
{
	float direction[3];
	if(!androidgame::VrHeadAimDirection(direction))
		return false;
	const float length = sqrtf(direction[0]*direction[0]+
		direction[1]*direction[1]+direction[2]*direction[2]);
	if(length < 0.5f)
		return false;
	// Beta/Alpha as the weapon camera processors build their Front:
	// (cos(a)cos(b), cos(a)sin(b), sin(a)), with the limits they apply.
	*beta = Atan2(direction[1], direction[0]);
	*alpha = Clamp(Asin(Clamp(direction[2]/length, -1.0f, 1.0f)),
		-DEGTORAD(89.5f), DEGTORAD(60.0f));
	return true;
}

// THIRD: 3D SCREEN. Gameplay itself plays on the theater screen from the
// stock camera, on foot and in vehicles. A remote-controlled vehicle counts:
// its camera is the stock chase camera too and the screen is the steadier way
// to watch it. Cutscenes are left to their own setting, so a stereo cutscene
// still comes out of the screen; menus and loading already use it.
bool
androidgame::VrScreenGameplayActive(void)
{
	return androidgame::VrThirdPersonScreen() &&
		gGameState == GS_PLAYING_GAME &&
		FindPlayerPed() != nil &&
		!FrontEndMenuManager.m_bGameNotLoaded &&
		!FrontEndMenuManager.m_bMenuActive &&
		!FrontEndMenuManager.m_bWantToRestart &&
		!FrontEndMenuManager.m_bWantToLoad &&
		!CGame::playingIntro &&
		!CCutsceneMgr::IsCutsceneProcessing() &&
		!CCutsceneMgr::IsRunning() &&
		!TheCamera.m_WideScreenOn;
}
#endif

bool
androidgame::VrShouldUseTheaterMode(void)
{
	// This is the Android equivalent of the desktop build's showGameplay
	// predicate. IsRunning covers formal cutscenes, while WideScreenOn covers
	// scripted cinematics such as the opening drive to the Ocean View hotel.
	// The original scripts also use playingIntro while they remove/rebuild the
	// world between opening scenes.  That interval has no valid immersive
	// camera or world to display, so it belongs to the same static theater
	// path instead of exposing the clear-colour sky for one or more frames.
	// A cutscene has a camera of its own, and in stereo that camera is
	// simply the view: the shot keeps its framing and gains depth, and
	// the player can look around inside it. CINEMA is still the default
	// and still puts the same camera on the flat screen. The letterbox
	// flag rides along with the cutscene and was forcing the flat screen
	// by itself, so both belong to the same choice.
	const bool cinematic = androidgame::VrCutsceneMode() != 0;
	const bool cutsceneOnScreen = !cinematic &&
		(CCutsceneMgr::IsCutsceneProcessing() ||
		 CCutsceneMgr::IsRunning() ||
		 TheCamera.m_WideScreenOn);
	return gGameState != GS_PLAYING_GAME ||
		FrontEndMenuManager.m_bGameNotLoaded ||
		FrontEndMenuManager.m_bMenuActive ||
		FrontEndMenuManager.m_bWantToRestart ||
		FrontEndMenuManager.m_bWantToLoad ||
		CGame::playingIntro ||
		FindPlayerPed() == nil ||
		cutsceneOnScreen
#if MIAMIVR_STEAM_FRAME
		|| androidgame::VrScreenGameplayActive()
#endif
		;
}

#if MIAMIVR_STEAM_FRAME
float
androidgame::VrCinema3DDepthThisFrame(void)
{
	// Gameplay on the screen carries the cutscene setting's depth, and a
	// player who chose the flat cinema for cutscenes gets it flat here too.
	// STEREO cutscenes have no depth of their own to borrow, so gameplay
	// takes the middle 3D level.
	if(androidgame::VrScreenGameplayActive()){
		const float depth = androidgame::VrCinema3DDepth();
		if(depth > 0.0f)
			return depth;
		return androidgame::VrCutsceneMode() != 0 ? 0.55f : 0.0f;
	}
	// Only a cutscene's own camera gets depth. Menus, loading screens and the
	// intro's world rebuild stay flat on the same screen.
	const bool cutscene = CCutsceneMgr::IsCutsceneProcessing() ||
		CCutsceneMgr::IsRunning() || TheCamera.m_WideScreenOn;
	if(!cutscene || gGameState != GS_PLAYING_GAME ||
	   FrontEndMenuManager.m_bGameNotLoaded ||
	   FrontEndMenuManager.m_bMenuActive ||
	   FrontEndMenuManager.m_bWantToRestart ||
	   FrontEndMenuManager.m_bWantToLoad ||
	   // GTA III's script holds playingIntro for the whole opening, cutscenes
	   // and its letterboxed scripted scenes included. Only the gaps between
	   // them have no camera worth depth.
	   (CGame::playingIntro && !CCutsceneMgr::IsRunning() &&
	    !TheCamera.m_WideScreenOn) ||
	   FindPlayerPed() == nil){
		// Say once per cutscene why it stays flat, so a missing 3D cinema
		// can be traced without a debugger.
		static bool reported;
		if(cutscene && !reported){
			reported = true;
			ALOG("[vr] cinema 3D held flat: state=%d notLoaded=%d menu=%d "
			     "restart=%d load=%d intro=%d player=%d",
			     (int)gGameState,
			     (int)FrontEndMenuManager.m_bGameNotLoaded,
			     (int)FrontEndMenuManager.m_bMenuActive,
			     (int)FrontEndMenuManager.m_bWantToRestart,
			     (int)FrontEndMenuManager.m_bWantToLoad,
			     (int)CGame::playingIntro, FindPlayerPed() != nil);
		}
		if(!cutscene)
			reported = false;
		return 0.0f;
	}
	return androidgame::VrCinema3DDepth();
}
#endif

// Camera 0 is the director's own. Camera 1 and up are the staged actors, in
// the order the scene created them; the player steps through them with R3
// and keeps one with L3. Choosing for them never worked -- which actor the
// shot is about changes from line to line -- so the choice is theirs and it
// is remembered per scene.
// The actor the eye is inside this frame, so its head can be kept out of
// the way. Nothing else may touch it.
static CEntity *gCutsceneCameraActor;

// The actor whose head the eye is sitting in, or null. CCutsceneObject
// asks so it can collapse that one head out of the shot.
void *
androidgame::VrCutsceneCameraActor(void)
{
	return gCutsceneCameraActor;
}

// Filming an actor from behind their own eyes needs a posed head bone, and
// that means a skinned clump: the head does not live in an RwFrame at all,
// the matrix comes out of the hanim hierarchy the same way CPedIK reads a
// ped's bones. Cutscene actors and ordinary peds both qualify.
static RpHAnimHierarchy*
GetPosedHeadHierarchy(CEntity *entity)
{
	if(entity == nil || entity->m_rwObject == nil ||
	   RwObjectGetType(entity->m_rwObject) != rpCLUMP)
		return nil;
	RpClump *clump = (RpClump*)entity->m_rwObject;
	if(!IsClumpSkinned(clump))
		return nil;
	RpHAnimHierarchy *hierarchy = GetAnimHierarchyFromSkinClump(clump);
	if(hierarchy == nil || RpHAnimIDGetIndex(hierarchy, BONE_head) < 0 ||
	   RpHAnimHierarchyGetMatrixArray(hierarchy) == nil)
		return nil;
	return hierarchy;
}

// The heads the player can step through, in a fixed order so a remembered
// choice means the same thing the next time the scene plays.
static int
StageCutsceneCameras(CEntity **actors, int maxActors)
{
	int staged = 0;
	const int count = Min(CCutsceneMgr::GetNumCutsceneObjects(),
		(int)NUMCUTSCENEOBJECTS);
	for(int i = 0; i < count && staged < maxActors; i++){
		CEntity *actor = CCutsceneMgr::GetCutsceneObject(i);
		if(GetPosedHeadHierarchy(actor) != nil)
			actors[staged++] = actor;
	}
	// A real cutscene has no one else in it. The player ped is parked
	// under the world while one runs, so falling back to it there would
	// drop the camera through the floor for the frames before the actors
	// finish streaming in.
	if(staged > 0 || CCutsceneMgr::IsRunning() ||
	   CCutsceneMgr::IsCutsceneProcessing())
		return staged;
	// Not every scene is a cutscene. The drive to the hotel plays out in
	// the real world with the real car and the real peds, and only the
	// letterbox marks it as a scene, so CCutsceneMgr has nothing staged.
	// The people in that shot are the player and whoever shares the car.
	CPlayerPed *player = FindPlayerPed();
	if(player == nil)
		return staged;
	if(GetPosedHeadHierarchy(player) != nil && staged < maxActors)
		actors[staged++] = player;
	CVehicle *vehicle = player->bInVehicle ? player->m_pMyVehicle : nil;
	if(vehicle == nil)
		return staged;
	if(vehicle->pDriver != nil && vehicle->pDriver != player &&
	   GetPosedHeadHierarchy(vehicle->pDriver) != nil && staged < maxActors)
		actors[staged++] = vehicle->pDriver;
	for(int i = 0; i < (int)ARRAY_SIZE(vehicle->pPassengers) &&
	    staged < maxActors; i++){
		CPed *passenger = vehicle->pPassengers[i];
		if(passenger != nil && passenger != player &&
		   GetPosedHeadHierarchy(passenger) != nil)
			actors[staged++] = passenger;
	}
	return staged;
}

// One for the director's own camera, then one per staged head.
int
androidgame::VrCutsceneCameraCount(void)
{
	CEntity *actors[NUMCUTSCENEOBJECTS];
	return StageCutsceneCameras(actors, ARRAY_SIZE(actors))+1;
}

static bool
GetCutsceneActorHead(int camera, CVector *head, CVector *forward)
{
	gCutsceneCameraActor = nil;
	if(camera <= 0)
		return false;
	CEntity *actors[NUMCUTSCENEOBJECTS];
	const int staged = StageCutsceneCameras(actors, ARRAY_SIZE(actors));
	if(camera > staged)
		return false;
	CEntity *actor = actors[camera-1];
	RpHAnimHierarchy *hierarchy = GetPosedHeadHierarchy(actor);
	if(hierarchy == nil)
		return false;
	RwMatrix *bonePose = &RpHAnimHierarchyGetMatrixArray(hierarchy)[
		RpHAnimIDGetIndex(hierarchy, BONE_head)];
	*head = CVector(RwMatrixGetPos(bonePose)->x,
		RwMatrixGetPos(bonePose)->y,
		RwMatrixGetPos(bonePose)->z);
	// Where the actor is actually looking, taken off the bone. The root is
	// no use: an actor is staged once and only its skeleton is animated
	// after that, so the root still faces wherever the scene dropped it --
	// which is how the view kept ending up behind the character's head.
	// CPedIK yaws a head bone about its local X and pitches it about local
	// Z, so the look axis is the remaining one, local Y: the up column of
	// the posed matrix.
	*forward = CVector(RwMatrixGetUp(bonePose)->x,
		RwMatrixGetUp(bonePose)->y,
		RwMatrixGetUp(bonePose)->z);
	forward->z = 0.0f;
	if(forward->MagnitudeSqr() < 0.0001f){
		*forward = actor->GetForward();
		forward->z = 0.0f;
	}
	if(forward->MagnitudeSqr() < 0.0001f)
		*forward = CVector(0.0f, 1.0f, 0.0f);
	forward->Normalise();
	gCutsceneCameraActor = actor;
	return true;
}


// The view the eyes actually use, written into the RenderWare camera so
// everything that projects against it lands where the player is looking:
// coronas, headlights, the sky and every sprite. Gameplay and cinematic
// cutscenes both need it, so it lives here rather than inside either.
static void
SyncRenderWareCameraToVrView(void)
{
	rw::float32 vr[3], vu[3], va[3], vp[3];
	if(rw::vulkan::getFirstPersonViewFrame(vr, vu, va, vp)){
		RwMatrix *m = RwFrameGetMatrix(RwCameraGetFrame(Scene.camera));
		RwMatrixGetRight(m)->x = vr[0];
		RwMatrixGetRight(m)->y = vr[1];
		RwMatrixGetRight(m)->z = vr[2];
		RwMatrixGetUp(m)->x = vu[0];
		RwMatrixGetUp(m)->y = vu[1];
		RwMatrixGetUp(m)->z = vu[2];
		RwMatrixGetAt(m)->x = va[0];
		RwMatrixGetAt(m)->y = va[1];
		RwMatrixGetAt(m)->z = va[2];
		RwMatrixGetPos(m)->x = vp[0];
		RwMatrixGetPos(m)->y = vp[1];
		RwMatrixGetPos(m)->z = vp[2];
		RwMatrixUpdate(m);
		RwFrameUpdateObjects(RwCameraGetFrame(Scene.camera));
		RwFrameOrthoNormalize(RwCameraGetFrame(Scene.camera));
		const CVector viewPosition(vp[0], vp[1], vp[2]);
		// Put the game camera on the same frame. Position alone was not
		// enough: m_cameraMatrix is the inverse of this matrix and every
		// IsSphereVisible in the game tests against it, so with a stale
		// orientation the frustum belonged to the chase camera. That is
		// what cut holes in the sea for anyone looking off the nose of a
		// helicopter. CCamera::Process rebuilds this every frame from the
		// active cam, so writing it here only affects rendering.
		TheCamera.GetMatrix().GetRight() =
			CVector(-vr[0], -vr[1], -vr[2]);
		TheCamera.GetMatrix().GetForward() =
			CVector(va[0], va[1], va[2]);
		TheCamera.GetMatrix().GetUp() =
			CVector(vu[0], vu[1], vu[2]);
		TheCamera.GetMatrix().GetPosition() = viewPosition;
		CRenderer::SetVrViewCameraPosition(viewPosition);
		TheCamera.CalculateDerivedValues();
		TheCamera.m_viewMatrix.Update();
	}
}

static void
UpdateQuestVehicleSeatRecenter(CPlayerPed *player, bool seatReady, bool postPhysics)
{
	CVehicle *vehicle = player != nil && player->InVehicle() ?
		player->m_pMyVehicle : nil;
	int seat = -1;
	if(vehicle != nil){
		if(vehicle->pDriver == player)
			seat = 0;
		else
			for(int i = 0; i < (int)ARRAY_SIZE(vehicle->pPassengers); i++)
				if(vehicle->pPassengers[i] == player){
					seat = i+1;
					break;
				}
	}
	if(gVehicleSeatRecenter.Update(player, vehicle, seat, seatReady,
	   postPhysics, xrvk::hasTrackedGameplayHeadPose())){
		// A basis-mode change alone can latch a stale XR position and misses
		// seat changes that retain that mode. Relatch once at a completed seat
		// with fresh tracking, then apply the ordinary calibrated anchor below.
		// Subsequent leaning and wheel grabs keep their basis.
		androidgame::VrRecenterView();
#if defined(MIAMIVR_DEV_TOOLS) && MIAMIVR_DEV_TOOLS
		ALOG("vehicle seat recentered model=%d seat=%d", vehicle->GetModelIndex(), seat);
#endif
	}
}

// Decides whether this frame plays from inside the character's head and, if
// so, anchors the view there. Menus, cutscenes and widescreen sequences keep
// the regular camera anchor -- they are flat content and the cinema treatment
// comes with the full VR layer port.
//
// Called twice per frame: from CapturePad so the stick rotation has a fresh
// view yaw, and again right before the scene renders (after physics) so the
// anchor position is this frame's, not last frame's -- anchoring on the
// pre-physics position made the view trail a moving car into the seat back.
void
VrUpdateFirstPersonAnchor(bool postPhysics)
{
	// Cleared here rather than where it is set: every ped tests this
	// pointer in PreRender, and a scene that ends without clearing it
	// leaves whoever lands on that address walking around headless.
	gCutsceneCameraActor = nil;
#if MIAMIVR_STEAM_FRAME
	gVrHeadModeOnFoot = false;
	gVrHeadAimActive = false;
	gVrFirstPersonWalking = false;
	bool scopeFrame = false;
#endif
	CPlayerPed *player = FindPlayerPed();
	const bool remoteMode =
		CWorld::Players[CWorld::PlayerInFocus].IsPlayerInRemoteMode();
	// Entry and exit animations keep the reference frame on the ped, as the
	// desktop build does, so the view turns with Tommy. Third person is off
	// for the same window: riding the chase camera while the game slides it
	// between its vehicle and on-foot positions threw the view down from
	// above on every exit.
	const PedState playerVehicleState =
		player != nil ? player->GetPedState() : PED_NONE;
	const bool vehicleTransition = player != nil &&
		(player->m_objective == OBJECTIVE_ENTER_CAR_AS_DRIVER ||
		 player->m_objective == OBJECTIVE_ENTER_CAR_AS_PASSENGER ||
		 player->m_objective == OBJECTIVE_LEAVE_CAR ||
#ifdef VC_PED_PORTS
		 player->m_objective == OBJECTIVE_LEAVE_CAR_AND_DIE ||
#endif
		 playerVehicleState == PED_SEEK_CAR ||
		 playerVehicleState == PED_SEEK_IN_BOAT ||
		 playerVehicleState == PED_OPEN_DOOR ||
		 playerVehicleState == PED_CARJACK ||
		 playerVehicleState == PED_ENTER_CAR ||
		 playerVehicleState == PED_STEAL_CAR ||
		 playerVehicleState == PED_EXIT_CAR ||
		 playerVehicleState == PED_DRAG_FROM_CAR);
	// THIRD PERSON vehicle view: leave the immersive seat entirely and let
	// the stock chase camera drive the stereo view, the same way the RC
	// missions already play. Everything downstream follows: the animated
	// Tommy and the vehicle are drawn (the body is hidden only in first
	// person), IMMERSIVE/MOTION driving deactivates because it requires the
	// first-person environment, so the controls fall back to DEFAULT, and no
	// seat offset applies because there is no seat anchor.
#if MIAMIVR_STEAM_FRAME
	// Third person in a vehicle follows the on-foot over-the-shoulder choice.
	const bool thirdPersonVehicle = androidgame::VrThirdPersonShoulder() &&
		player != nil && player->InVehicle() &&
		player->m_pMyVehicle != nil && !vehicleTransition;
#else
	const bool thirdPersonVehicle = false;
#endif
#if MIAMIVR_STEAM_FRAME
	// THIRD: 3D SCREEN has no first person at all: the stock camera is drawn
	// on the theater screen, so the body is shown, the tracked weapons stay
	// off and the physical-input layer never takes the pad.
	const bool screenGameplay = androidgame::VrScreenGameplayActive();
	// THIRD: OVER SHOULDER keeps the stereo first-person machinery (field of
	// view, anchor, level horizon) but rides the stock chase camera on foot,
	// as a third person vehicle does. The enter/exit window is excluded for
	// the reason given above.
	const bool thirdPersonOnFoot = androidgame::VrThirdPersonShoulder() &&
		player != nil && !player->InVehicle() && !remoteMode &&
		!vehicleTransition;
	// Getting into or out of a vehicle the game slides its camera between
	// the two positions, which is why riding it is off for that window. The
	// first-person fallback the vehicle view uses there is no good either for
	// a player who chose third person to get away from it, so the last chase
	// view is held still until the transition ends.
	const bool thirdPersonHold = androidgame::VrThirdPersonShoulder() &&
		player != nil && !remoteMode && vehicleTransition;
#else
	const bool screenGameplay = false;
	const bool thirdPersonOnFoot = false;
	const bool thirdPersonHold = false;
#endif
	const bool wantFirstPerson =
		gGameState == GS_PLAYING_GAME &&
		player != nil &&
		!remoteMode &&
		!screenGameplay &&
		!FrontEndMenuManager.m_bMenuActive &&
		!CGame::playingIntro &&
		!CCutsceneMgr::IsCutsceneProcessing() &&
		!CCutsceneMgr::IsRunning() &&
		!TheCamera.m_WideScreenOn;

	// A cinematic cutscene is rendered in stereo from the director camera.
	// It has to go through the same first-person machinery as gameplay:
	// coronas, headlights and the sky all project against the RenderWare
	// camera, and with that camera left on the director's frame while the
	// eyes moved with the head, they slid across the view in 2D.
	const bool cinematicCutscene = !wantFirstPerson &&
		androidgame::VrCutsceneMode() != 0 &&
		gGameState == GS_PLAYING_GAME && player != nil &&
		!FrontEndMenuManager.m_bMenuActive &&
		!FrontEndMenuManager.m_bGameNotLoaded &&
		!CGame::playingIntro &&
		(CCutsceneMgr::IsRunning() ||
		 CCutsceneMgr::IsCutsceneProcessing() ||
		 TheCamera.m_WideScreenOn);
	gVrFirstPersonActive = wantFirstPerson || cinematicCutscene;
#if MIAMIVR_STEAM_FRAME
	gVrThirdPersonOnFoot = thirdPersonOnFoot || thirdPersonHold;
	if(!androidgame::VrThirdPersonShoulder() || !vehicleTransition){
		gVrChaseFollowing = false;
		gVrChaseLogged = false;
	}
	if(!androidgame::VrThirdPersonShoulder()){
		gVrChaseHoldValid = false;
		gVrChaseBlend = 1.0f;
	}
#endif
	gVrPlayerEntity = (CEntity*)player;
	gVrInVehicle = wantFirstPerson && player->InVehicle() &&
		player->m_pMyVehicle != nil &&
		!thirdPersonVehicle && !thirdPersonHold;
	UpdateQuestVehicleSeatRecenter(player,
		gVrInVehicle && !vehicleTransition && playerVehicleState == PED_DRIVING &&
		!FrontEndMenuManager.m_bGameNotLoaded && !FrontEndMenuManager.m_bWantToRestart &&
		!FrontEndMenuManager.m_bWantToLoad && !androidgame::VrMenuConsumesInput() &&
		player->m_rwObject != nil && !player->DyingOrDead() &&
		player->m_pFrames[PED_HEAD] != nil, postPhysics);
	// Match the desktop policy: DEFAULT driving uses the original animated
	// vehicle occupant, while on-foot VR and IMMERSIVE/MOTION driving replace
	// Tommy with tracked hands.  CRenderer and CPed both consume this flag, so
	// decide it once here instead of letting their early Vulkan exits hide the
	// DEFAULT occupant unconditionally.
// Dying is the one moment on foot when the camera leaves the head and
// looks back at Tommy. Hiding him then plays the shot over empty
// pavement, so he is drawn from the blow that kills him.
	// No tracked hands and no physical driving in this port, so the body is
	// hidden only on foot in first person: in a vehicle the animated driver
	// stays, and over-the-shoulder shows the character.
	// A scripted scene filmed in stereo (the opening drive) is not first
	// person either: the actors, the player among them, are in the shot.
	gVrHidePlayerBody = wantFirstPerson && !thirdPersonVehicle &&
		!thirdPersonOnFoot && !thirdPersonHold && !gVrInVehicle &&
		!(player != nil && player->DyingOrDead());

	// Verbatim port of the desktop VR base camera (RenderVrStereoFrame in
	// main.cpp): the anchor is the player's HEAD BONE -- animated, seated in
	// vehicles by the sitting animation, updated after physics -- nudged
	// 0.12m forward, oriented by the ped's own basis. The desktop's numbers,
	// nothing reinvented. During death/respawn, or when the clump is gone,
	// the regular game camera takes over, exactly as the desktop does.
	if(wantFirstPerson && player->m_rwObject != nil &&
	   !player->DyingOrDead() && player->m_pFrames[PED_HEAD] != nil){
		CVector forward = player->GetForward();
		forward.Normalise();
		// Head modes turn the hidden ped into the movement direction, so
		// anchoring the VR frame on his body would rotate the world a second
		// time and curve the walk away. Use the gameplay camera, as the
		// desktop build does.
		const bool headModeOnFoot = !gVrInVehicle && !thirdPersonVehicle &&
			!thirdPersonOnFoot && !thirdPersonHold && !vehicleTransition &&
			(androidgame::VrUsesHeadRelativeMovement() ||
			 androidgame::VrUsesExperimentalHeadTurning());
		if(headModeOnFoot){
			CVector cameraUp = player->GetUp();
			cameraUp.Normalise();
			CVector cameraForward =
				TheCamera.Cams[TheCamera.ActiveCam].Front;
			cameraForward -= cameraUp*DotProduct(cameraForward, cameraUp);
			if(cameraForward.MagnitudeSqr() > 0.000001f){
				cameraForward.Normalise();
				forward = cameraForward;
			}
		}
#if MIAMIVR_STEAM_FRAME
		gVrFirstPersonWalking = !gVrInVehicle && !thirdPersonVehicle &&
			!thirdPersonOnFoot && !thirdPersonHold && !vehicleTransition;
		gVrHeadModeOnFoot = headModeOnFoot;
		gVrHeadAimActive = headModeOnFoot && androidgame::VrHeadAimEnabled();
		if(gVrHeadAimActive && IsHeadDrivenCameraMode(
		   TheCamera.Cams[TheCamera.ActiveCam].Mode)){
			// A weapon camera driven by the headset: hold the anchor
			// where the view was when it began (see gVrScopeLatched).
			scopeFrame = true;
			if(!gVrScopeLatched){
				gVrScopeLatched = true;
				gVrScopeBaseForward = gVrLastAnchorForwardValid ?
					gVrLastAnchorForward : forward;
				ALOG("[vr] weapon camera mode %d: head driven, anchor held "
				     "at %.1f deg",
				     (int)TheCamera.Cams[TheCamera.ActiveCam].Mode,
				     RADTODEG(Atan2(gVrScopeBaseForward.y,
				                    gVrScopeBaseForward.x)));
			}
			forward = gVrScopeBaseForward;
		}else if(headModeOnFoot){
			if(gVrScopeLatched){
				// Back to the stock camera: relatch the anchor on its
				// heading, which the weapon camera left on the headset's.
				gVrScopeLatched = false;
				androidgame::VrRecenterView();
				ALOG("[vr] weapon camera over: anchor relatched");
			}
			gVrLastAnchorForward = forward;
			gVrLastAnchorForwardValid = true;
		}
#endif
#if MIAMIVR_STEAM_FRAME
		if(thirdPersonHold)
			forward = gVrChaseHoldForward;
#endif
		if(thirdPersonOnFoot){
			// The chase camera's heading is the frame's heading, and so the
			// reference the stick movement is calibrated on: the Orientation
			// write below would otherwise turn "forward" to where the ped faces
			// rather than where the camera looks.
			CVector chaseForward = TheCamera.Cams[TheCamera.ActiveCam].Front;
			chaseForward.z = 0.0f;
			if(chaseForward.MagnitudeSqr() > 0.0001f){
				chaseForward.Normalise();
				forward = chaseForward;
			}
		}
		CVector horizontalForward = forward;
		horizontalForward.z = 0.0f;
		if(horizontalForward.MagnitudeSqr() > 0.0001f){
			horizontalForward.Normalise();

			// PlayerControlZelda derives on-foot movement from
			// TheCamera.Orientation.  With the frame welded to the body the
			// stock chase-camera heading is the wrong reference (it turns
			// "forward" by 90 degrees), so it is replaced here. In the head
			// modes above the frame IS the gameplay camera, so the stock
			// value is the correct reference and must be left alone -- the
			// desktop build never writes it either.
			// Renderer anchor keeps its standard atan2(y, x) convention;
			// the game camera uses atan2(x, y).
			if(!gVrInVehicle && !headModeOnFoot)
				TheCamera.Orientation =
					Atan2(horizontalForward.x, horizontalForward.y);
		}

		CVector head(0.0f, 0.0f, 0.0f);
		if(!gVrInVehicle && !androidgame::VrHeadBobbingEnabled() &&
		   !player->bIsDucking){
			// 0.4.1 PC parity: hold a stable eye height while walking. 0.59m
			// matches reVC's established first-person no-bob anchor.
			CVector up = player->GetUp();
			up.Normalise();
			head = player->GetPosition()+up*0.59f;
		}else
			player->m_pedIK.GetComponentPosition(head, PED_HEAD);
		CVector nudge = forward;
		if(headModeOnFoot){
			// In the head modes TheCamera follows the CHARACTER: a backward
			// walk turns the ped to face the player, the chase logic swings
			// after it, and for a moment this forward points somewhere the
			// player is not looking. The 0.12m eye nudge rode that swing,
			// felt as being pulled a small step forward after every
			// backward walk. The headset's own view basis cannot be turned
			// by gameplay, so the nudge follows it instead; the anchor
			// heading stays on the camera value the movement code is
			// calibrated against.
			float vrForward[3];
			if(androidgame::VrGetViewBasis(nil, nil, nil, vrForward))
				nudge = CVector(vrForward[0], vrForward[1],
					vrForward[2]);
		}
		head += nudge * 0.12f;
		if(thirdPersonVehicle || thirdPersonOnFoot){
			// Ride the stock chase camera instead of a seat: its position
			// and heading, but through the first-person machinery, so the
			// basis stays yaw-only (level horizon), the eye field of view is
			// applied, and everything projecting against the game camera --
			// coronas, headlights, sprites -- lands where the world is.
			CVector chaseForward = TheCamera.Cams[TheCamera.ActiveCam].Front;
			chaseForward.z = 0.0f;
			if(chaseForward.MagnitudeSqr() > 0.0001f){
				chaseForward.Normalise();
				forward = chaseForward;
			}
			head = TheCamera.GetPosition();
#if MIAMIVR_STEAM_FRAME
			// Coming back from a vehicle transition the view eases from the
			// last held pose onto the stock camera, so nothing jumps when the
			// two disagree.
			if(gVrChaseBlend < 1.0f){
				if(postPhysics)
					gVrChaseBlend = Min(1.0f, gVrChaseBlend+
						CTimer::GetTimeStepInSeconds()/0.5f);
				const float t = gVrChaseBlend*gVrChaseBlend*
					(3.0f-2.0f*gVrChaseBlend);
				const float heldYaw = Atan2(gVrChaseHoldForward.y,
					gVrChaseHoldForward.x);
				const float stockYaw = Atan2(forward.y, forward.x);
				const float yaw = heldYaw+
					CGeneral::LimitRadianAngle(stockYaw-heldYaw)*t;
				head = gVrChaseHoldHead+(head-gVrChaseHoldHead)*t;
				forward = CVector(Cos(yaw), Sin(yaw), 0.0f);
			}
			if(gVrChaseBlend >= 1.0f){
				// The stock view is the shown one again: a transition
				// continues from it.
				gVrChaseHoldHead = head;
				gVrChaseHoldForward = forward;
				const CVector offset = head-player->GetPosition();
				gVrChaseHoldBack = Clamp(-(offset.x*forward.x+
					offset.y*forward.y), 0.5f, 12.0f);
				gVrChaseHoldHeight = Clamp(offset.z, -1.0f, 6.0f);
			}
			gVrChaseHoldValid = true;
#endif
		}
#if MIAMIVR_STEAM_FRAME
		if(thirdPersonHold){
			if(!gVrChaseHoldValid){
				// The view was switched on in the middle of a transition:
				// start behind the ped's own heading.
				CVector pedForward = player->GetForward();
				pedForward.z = 0.0f;
				if(pedForward.MagnitudeSqr() < 0.0001f)
					pedForward = CVector(0.0f, 1.0f, 0.0f);
				pedForward.Normalise();
				gVrChaseHoldForward = pedForward;
				gVrChaseHoldBack = 3.0f;
				gVrChaseHoldHeight = 1.8f;
				gVrChaseHoldHead = player->GetPosition()-
					pedForward*gVrChaseHoldBack+
					CVector(0.0f, 0.0f, gVrChaseHoldHeight);
				gVrChaseFollow = gVrChaseHoldHead;
				gVrChaseHoldValid = true;
			}
			forward = gVrChaseHoldForward;
			const CVector target = player->GetPosition()-
				forward*gVrChaseHoldBack+
				CVector(0.0f, 0.0f, gVrChaseHoldHeight);
			if(!gVrChaseFollowing){
				gVrChaseFollowing = true;
				gVrChaseFollow = gVrChaseHoldHead;
			}
			if(postPhysics)
				gVrChaseFollow += (target-gVrChaseFollow)*
					(1.0f-expf(-CTimer::GetTimeStepInSeconds()/0.12f));
			head = gVrChaseFollow;
			// Where the blend back to the stock camera will start from.
			gVrChaseHoldHead = head;
			gVrChaseBlend = 0.0f;
			if(!gVrChaseLogged){
				gVrChaseLogged = true;
				ALOG("[vr] chase follow during transition: back %.1f m, "
				     "up %.1f m, objective %d state %d",
				     gVrChaseHoldBack, gVrChaseHoldHeight,
				     (int)player->m_objective, (int)playerVehicleState);
			}
		}
#endif
		bool fullVehicleBasis = false;
#if MIAMIVR_STEAM_FRAME
		// In a vehicle the view is yaw-only by default. With
		// VehicleFollowRotation on it inherits the vehicle's whole basis, so
		// the cabin stays still around the player through pitch and roll.
		CVehicle *viewVehicle =
			gVrInVehicle && androidgame::VrVehicleFollowRotation() ?
			player->m_pMyVehicle : nil;
		if(viewVehicle){
			CVector vehicleForward = viewVehicle->GetForward();
			CVector vehicleUp = viewVehicle->GetUp();
			if(vehicleForward.MagnitudeSqr() > 0.0001f &&
			   vehicleUp.MagnitudeSqr() > 0.0001f){
				vehicleForward.Normalise();
				vehicleUp.Normalise();
				CVector vehicleRight =
					CrossProduct(vehicleForward, vehicleUp);
				if(vehicleRight.MagnitudeSqr() > 0.0001f){
					vehicleRight.Normalise();
					vehicleUp = CrossProduct(
						vehicleRight, vehicleForward);
					vehicleUp.Normalise();
					rw::vulkan::setFirstPersonAnchorBasis(
						&head.x, &vehicleRight.x,
						&vehicleUp.x, &vehicleForward.x,
						Atan2(vehicleForward.y,
						      vehicleForward.x), 1);
					fullVehicleBasis = true;
				}
			}
		}
#endif
		if(!fullVehicleBasis)
			rw::vulkan::setFirstPersonAnchor(&head.x,
				Atan2(forward.y, forward.x), 1, 1);

		// Write the actual view into the RenderWare camera frame, as the
		// desktop BeginEye does. Everything that projects against the game
		// camera -- corona placement, vehicle interior visibility angles,
		// sprite screen mathematics -- then sees where the player really
		// looks instead of the chase camera.
		if(postPhysics){
			SyncRenderWareCameraToVrView();
		}
	}else if(cinematicCutscene){
		CVector forward = TheCamera.Cams[TheCamera.ActiveCam].Front;
		forward.z = 0.0f;
		if(forward.MagnitudeSqr() < 0.0001f)
			forward = CVector(0.0f, 1.0f, 0.0f);
		forward.Normalise();
		CVector head = TheCamera.GetPosition();
		CVector actorHead, actorForward;
		if(GetCutsceneActorHead(androidgame::VrCutsceneCamera(),
		  &actorHead, &actorForward)){
			head = actorHead;
			forward = actorForward;
		}
		rw::vulkan::setFirstPersonAnchor(&head.x,
			Atan2(forward.y, forward.x), 1, 1);
		if(postPhysics)
			SyncRenderWareCameraToVrView();
	}else{
		rw::vulkan::setFirstPersonAnchor(nil, 0.0f, 0, 0);
	}
#if MIAMIVR_STEAM_FRAME
	if(!scopeFrame)
		gVrScopeLatched = false;
	// Say when the head modes switch, so a dump or a log can tell which
	// controls the engine was running.
	static int reportedHeadModes = -1;
	const int headModes = (gVrHeadModeOnFoot ? 1 : 0)|(gVrHeadAimActive ? 2 : 0);
	if(headModes != reportedHeadModes){
		reportedHeadModes = headModes;
		ALOG("[vr] head modes: head frame %s, head aim %s",
		     gVrHeadModeOnFoot ? "on" : "off", gVrHeadAimActive ? "on" : "off");
	}
#endif
}

// XINPUT is off on this platform, so CPad routes through CapturePad. This is
// the same seam the desktop build uses to inject tracked-controller state.
static bool gQuestSnapTurnStickLatched;
static int gQuestFrontendVerticalDirection;
static int gQuestFrontendHorizontalDirection;

static int
UpdateQuestFrontendAxisDirection(float axis, int &latchedDirection)
{
	const float engage = 0.68f;
	const float release = 0.34f;
	if(latchedDirection == 0){
		if(axis >= engage)
			latchedDirection = 1;
		else if(axis <= -engage)
			latchedDirection = -1;
	}else if(Abs(axis) <= release){
		latchedDirection = 0;
	}else if(axis >= engage){
		latchedDirection = 1;
	}else if(axis <= -engage){
		latchedDirection = -1;
	}
	return latchedDirection;
}

static void
ApplyQuestSnapTurn(float stickX)
{
	if(!androidgame::VrUsesSnapTurn() || FindPlayerVehicle() ||
#if MIAMIVR_STEAM_FRAME
	   // On the screen the right stick drives the stock camera raw.
	   androidgame::VrThirdPersonScreen() ||
#endif
	   CWorld::Players[CWorld::PlayerInFocus].IsPlayerInRemoteMode() ||
	   gGameState != GS_PLAYING_GAME ||
	   FrontEndMenuManager.m_bGameNotLoaded ||
	   CGame::playingIntro ||
	   CCutsceneMgr::IsRunning() ||
	   CCutsceneMgr::IsCutsceneProcessing() ||
	   TheCamera.m_WideScreenOn){
		gQuestSnapTurnStickLatched = fabsf(stickX) >= 0.35f;
		return;
	}
	if(fabsf(stickX) <= 0.35f){
		gQuestSnapTurnStickLatched = false;
		return;
	}
	if(gQuestSnapTurnStickLatched || fabsf(stickX) < 0.70f)
		return;

	gQuestSnapTurnStickLatched = true;
	const float delta = -copysignf(
		DEGTORAD((float)androidgame::VrSnapTurnAngleDegrees()), stickX);
	CCam &cam = TheCamera.Cams[TheCamera.ActiveCam];
	cam.Beta += delta;
	cam.m_fTrueBeta += delta;
	cam.m_fTargetBeta += delta;
	while(cam.Beta >= PI) cam.Beta -= TWOPI;
	while(cam.Beta < -PI) cam.Beta += TWOPI;
	while(cam.m_fTrueBeta >= PI) cam.m_fTrueBeta -= TWOPI;
	while(cam.m_fTrueBeta < -PI) cam.m_fTrueBeta += TWOPI;
	while(cam.m_fTargetBeta >= PI) cam.m_fTargetBeta -= TWOPI;
	while(cam.m_fTargetBeta < -PI) cam.m_fTargetBeta += TWOPI;

	CPlayerPed *player = FindPlayerPed();
	if(player){
		player->m_fRotationCur += delta;
		player->m_fRotationDest += delta;
		player->SetHeading(player->m_fRotationCur);
	}
}

#if MIAMIVR_STEAM_FRAME
// vr-diag.ini diag_new_game=1: once the title menu has been up for a few
// seconds with no game loaded, start a new game as if the player chose it, so
// the opening cutscenes and the scene loads after them can be run with nobody
// in the headset (with force_render).
static void
DiagStartNewGame(void)
{
	static bool done;
	static int frontendFrames;
	if(done || !frame_diag::config().newGame)
		return;
	if(!FrontEndMenuManager.m_bMenuActive ||
	   !FrontEndMenuManager.m_bGameNotLoaded){
		frontendFrames = 0;
		return;
	}
	if(++frontendFrames < 300)
		return;
	done = true;
	ALOG("[vr] diag: starting a new game");
	FrontEndMenuManager.DoSettingsBeforeStartingAGame();
}
#endif

void
CapturePad(RwInt32 padID)
{
	if(padID != 0)
		return;

	VrUpdateFirstPersonAnchor(false);
#if MIAMIVR_STEAM_FRAME
	DiagStartNewGame();
#endif

	CControllerState &state = CPad::GetPad(0)->PCTempJoyState;
	state.Clear();

	const androidgame::PadInput &in = gPadInput;
	if(androidgame::VrMenuConsumesInput())
		return;

	// Thumbstick Y is up-positive on OpenXR and down-positive in the game.
	//
	// Full deflection is 128, not the int16 range: the game divides these by
	// 128 to get a fraction. Scaling to 32767 made every axis read 256 times
	// too far, which spun the camera a full turn per frame and threw the pitch
	// past vertical -- the world appeared to tumble and the player to walk up
	// walls. The 0.3 deadzone and the leave-alone-when-inside behaviour match
	// the desktop skeletons in glfw.cpp and win.cpp.
	const float stickDeadzone = 0.3f;
	float rightStickX = Clamp(in.rightStickX, -1.0f, 1.0f);
	const bool remoteMode =
		CWorld::Players[CWorld::PlayerInFocus].IsPlayerInRemoteMode();
	ApplyQuestSnapTurn(rightStickX);
	const bool onFootGameplay =
		gGameState == GS_PLAYING_GAME &&
		!FrontEndMenuManager.m_bGameNotLoaded &&
		!FrontEndMenuManager.m_bMenuActive &&
		!CGame::playingIntro &&
		!FindPlayerVehicle() &&
		!remoteMode &&
		!CCutsceneMgr::IsRunning() &&
		!CCutsceneMgr::IsCutsceneProcessing() &&
		!TheCamera.m_WideScreenOn;
#if MIAMIVR_STEAM_FRAME
	// Third person on foot, either style: the stock camera is the reference
	// for movement, and the buttons follow the flat game's pad (see below).
	const bool thirdPersonPad = onFootGameplay &&
		(androidgame::VrThirdPersonScreen() ||
		 androidgame::VrThirdPersonShoulder());
	// The screen has no head to turn with: the stick drives the camera as it
	// does on a pad, with no snap, scaling or head-driven turning.
	const bool screenView = onFootGameplay &&
		androidgame::VrThirdPersonScreen();
#else
	const bool thirdPersonPad = false;
	const bool screenView = false;
#endif
	float localHeadYaw = 0.0f;
	const bool localHeadYawValid =
		onFootGameplay && !thirdPersonPad &&
		rw::vulkan::getFirstPersonLocalHeadYaw(&localHeadYaw);
	if(onFootGameplay && !screenView){
		if(androidgame::VrUsesSnapTurn())
			rightStickX = 0.0f;
		else
			rightStickX = Clamp(
				rightStickX*androidgame::VrSmoothTurnScale(),
				-1.0f, 1.0f);
		if(localHeadYawValid &&
		   androidgame::VrUsesExperimentalHeadTurning()){
			const float moveMagnitudeSqr =
				in.leftStickX*in.leftStickX+
				in.leftStickY*in.leftStickY;
			if(moveMagnitudeSqr >= 0.20f*0.20f){
				const float deadZone = DEGTORAD(10.0f);
				const float fullSpeedAngle = DEGTORAD(55.0f);
				const float absoluteYaw = fabsf(localHeadYaw);
				if(absoluteYaw > deadZone){
					float axis = Clamp(
						(absoluteYaw-deadZone)/
						(fullSpeedAngle-deadZone),
						0.0f, 1.0f);
					axis = axis*axis*(3.0f-2.0f*axis);
					axis = copysignf(axis, -localHeadYaw);
					rightStickX = Clamp(
						rightStickX+
						axis*androidgame::VrHeadTurnScale(),
						-1.0f, 1.0f);
				}
			}
		}
	}
	float moveStickX = Clamp(in.leftStickX, -1.0f, 1.0f);
	float moveStickY = Clamp(in.leftStickY, -1.0f, 1.0f);
	if(localHeadYawValid && androidgame::VrUsesHeadRelativeMovement()){
		// Rotate only by local HMD yaw. Pitch/roll never alter locomotion and
		// the latched facing makes recentering a true new forward direction.
		const float cosine = cosf(localHeadYaw);
		const float sine = sinf(localHeadYaw);
		const float headX = moveStickX*cosine-moveStickY*sine;
		const float headY = moveStickY*cosine+moveStickX*sine;
		moveStickX = headX;
		moveStickY = headY;
	}
	// The stock frontend listens to both analogue-stick edges and D-pad edges.
	// Feeding the same Touch stick through both paths makes one slow deflection
	// cross two thresholds on different frames and skip a menu row. While the
	// frontend is open, give it only the debounced D-pad path below.
	const bool frontendConsumesStick = FrontEndMenuManager.m_bMenuActive ||
		FrontEndMenuManager.m_bGameNotLoaded;
	const float moveMagnitude =
		sqrtf(moveStickX*moveStickX+moveStickY*moveStickY);
	if(!frontendConsumesStick && moveMagnitude > stickDeadzone){
		state.LeftStickX = (int16)(moveStickX*128.0f);
		state.LeftStickY = (int16)(-moveStickY*128.0f);
	}

	const float sticks[2] = {
		rightStickX,
		Clamp(-in.rightStickY, -1.0f, 1.0f)
	};
	int16 *const axes[2] = {
		&state.RightStickX, &state.RightStickY
	};
	for(int i = 0; i < 2; i++)
		if(Abs(sticks[i]) > stickDeadzone)
			*axes[i] = (int16)(sticks[i] * 128.0f);

	// Left movement and vertical camera input remain the classic pad mapping.
	// Horizontal turn is the same configurable smooth/snap path as desktop.

	const int padMode = CPad::GetPad(0)->GetMode();
	// Remote-control missions keep A for the stock vehicle-fire action (the
	// demolition helicopter drops its bomb through that path). The accelerator
	// stays on R2, so A must not also reach its own binding here.
	androidgame::PadInput bound = in;
	if(remoteMode)
		bound.a = false;
	// Every mapped button goes through the binding table, so the CONTROLS page
	// can move it on foot. Trigger and grip routing is the desktop VR layer's
	// default: the right trigger is the accelerator (Cross), the left is
	// brake/reverse (Square), and the grips are analogue shoulder buttons on a
	// 0..255 scale. QuestWeaponVR rebuilds the same buttons on foot without the
	// triggers, which belong to the weapon there.
	// Stage 1 has no tracked weapons, so every on-foot mode plays on the pad's
	// own layout; the triggers are the accelerator and brake only in a vehicle.
	const bool padOnFoot = onFootGameplay;
	// The driving layout is for driving only: a menu, a cutscene or the intro
	// keeps the shipped buttons, so A still confirms and skips.
	const bool vehicleGameplay =
		(FindPlayerVehicle() != nil || remoteMode) &&
		gGameState == GS_PLAYING_GAME &&
		!FrontEndMenuManager.m_bGameNotLoaded &&
		!FrontEndMenuManager.m_bMenuActive &&
		!CGame::playingIntro &&
		!CCutsceneMgr::IsRunning() &&
		!CCutsceneMgr::IsCutsceneProcessing() &&
		!TheCamera.m_WideScreenOn;
	androidgame::VrApplyPadBindings(&state, bound, !padOnFoot,
	                                vehicleGameplay);
#if MIAMIVR_STEAM_FRAME
	if(padOnFoot){
		// A pad's own layout: the right trigger fires and the left aims (R1
		// is the lock-on / free-aim button of the default controller setup).
		// The bumpers cycle weapons through the same L2/R2 pair the on-screen
		// icon uses.
		state.Circle = Max(state.Circle,
			(int16)(Clamp(in.rightTrigger, 0.0f, 1.0f)*255.0f));
		state.RightShoulder1 = Max(state.RightShoulder1,
			(int16)(Clamp(in.leftTrigger, 0.0f, 1.0f)*255.0f));
		if(in.leftBumper)
			state.LeftShoulder2 = Max(state.LeftShoulder2, (int16)255);
		if(in.rightBumper)
			state.RightShoulder2 = Max(state.RightShoulder2, (int16)255);
	}
#endif
	state.Start = in.menu ? 255 : 0;
	if(remoteMode && in.a){
		if(padMode == 3)
			state.RightShoulder1 = 255;
		else
			state.Circle = 255;
	}

	// The frontend walks its menus on the D-pad. Use hysteresis there so small
	// controller noise cannot release and re-press the same direction. Gameplay
	// keeps the historical direct mirror because scripts use D-pad edges too.
	if(frontendConsumesStick){
		const int horizontal = UpdateQuestFrontendAxisDirection(
			in.leftStickX, gQuestFrontendHorizontalDirection);
		const int vertical = UpdateQuestFrontendAxisDirection(
			in.leftStickY, gQuestFrontendVerticalDirection);
		state.DPadLeft  = horizontal < 0 ? 255 : 0;
		state.DPadRight = horizontal > 0 ? 255 : 0;
		state.DPadUp    = vertical > 0 ? 255 : 0;
		state.DPadDown  = vertical < 0 ? 255 : 0;
#if MIAMIVR_STEAM_FRAME
		// View's short press backs out of the frontend, whose back button
		// is Triangle (TRIANGLE_BACK_BUTTON); one frame is one edge.
		if(in.viewBack)
			state.Triangle = 255;
#endif
	}else{
		gQuestFrontendHorizontalDirection = 0;
		gQuestFrontendVerticalDirection = 0;
		// Mirror the same vector the analogue axes carry, not the raw stick.
		// GetPedWalkLeftRight/UpDown take the larger of the analogue axis and
		// the D-pad per axis, so a D-pad built from the physical stick while
		// the axes are built from the head-rotated one hands the game a
		// direction neither of them asked for. Facing forward the two are the
		// same vector and nothing changes; after a physical quarter turn the
		// raw mirror was still reporting "up" while the axes had moved the
		// motion onto X.
		//
		// That same comparison is why the mirror only fires at the rim. A
		// pressed D-pad direction counts as 127 out of the 128 a fully
		// deflected axis is worth, so any lower threshold replaced the
		// analogue value with a full press: half a stick forward walked at
		// running speed and there was no speed control left between the
		// deadzone and the rim. Scripts and the in-car camera still see the
		// D-pad, they now need the stick pushed all the way to get it.
		const float mirror = 127.0f/128.0f;
		state.DPadLeft  = moveStickX <= -mirror ? 255 : 0;
		state.DPadRight = moveStickX >=  mirror ? 255 : 0;
		state.DPadUp    = moveStickY >=  mirror ? 255 : 0;
		state.DPadDown  = moveStickY <= -mirror ? 255 : 0;
	}
#if MIAMIVR_STEAM_FRAME
	// The Frame's own D-pad (left controller) is the game's D-pad, in the
	// frontend and in play, alongside the stick-built one above.
	if(in.dpadLeft)  state.DPadLeft  = 255;
	if(in.dpadRight) state.DPadRight = 255;
	if(in.dpadUp)    state.DPadUp    = 255;
	if(in.dpadDown)  state.DPadDown  = 255;
#endif
	// Controller configuration 2 cycles the car camera on D-pad up and down,
	// and the stick-built D-pad would do it every time the stick reached the
	// rim. In a car the camera mode is not a button on this build.
	if(vehicleGameplay && padMode == 1){
		state.DPadUp = 0;
		state.DPadDown = 0;
	}
}

// GetLocalTime_CP comes from src/skel/crossplatform.cpp, which is already in
// the build for every non-Windows target.

void
InitialiseLanguage(void)
{
	// The headset has no reliable per-title locale, and the shipped TEXT set is
	// American. Anything else can be selected from the in-game menu.
	CGame::nastyGame = true;
	CGame::frenchGame = false;
	CGame::germanGame = false;
	CGame::noProstitutes = false;
	FrontEndMenuManager.m_PrefsLanguage = CMenuManager::LANGUAGE_AMERICAN;
	TheText.Unload();
	TheText.Load();
}

RwBool
psInitialize(void)
{
	RsGlobal.ps = &PsGlobal;

	PsGlobal.fullScreen = TRUE;
	PsGlobal.lastMousePos.x = PsGlobal.lastMousePos.y = 0.0f;
	PsGlobal.mouseWheel = 0.0;
	PsGlobal.cursorIsInWindow = FALSE;
	PsGlobal.joy1id = -1;
	PsGlobal.joy2id = -1;

	const long pages = sysconf(_SC_PHYS_PAGES);
	const long pageSize = sysconf(_SC_PAGESIZE);
	size_t physical = pages > 0 && pageSize > 0 ?
		(size_t)pages * (size_t)pageSize : (size_t)2048 * 1024 * 1024;
	// 2.5GB: full-map residency (ISLAND_LOADING_HIGH) needs the streaming
	// budget headroom, or the streamer starts evicting models in plain
	// sight — hydrants and poles blinking out right in front of the player.
	const size_t cap = (size_t)2560 * 1024 * 1024;
	_dwMemAvailPhys = physical < cap ? physical : cap;
	ALOG("physical memory %zu MB, reporting %zu MB to the streamer",
	     physical / (1024*1024), _dwMemAvailPhys / (1024*1024));

	CFileMgr::Initialise();
	InitialiseLanguage();
	// Every other skeleton reads the saved options here (win.cpp and
	// glfw.cpp both call it from psInitialize); this one never did, so the
	// game started on the constructor defaults and the first SaveSettings
	// of the session wrote them back over the player's file. Language and
	// subtitles came back every launch for that reason. It runs after
	// InitialiseLanguage on purpose: that sets American unconditionally,
	// and LoadSettings reloads the text set when the saved language is a
	// different one.
	FrontEndMenuManager.LoadSettings();
	return TRUE;
}

void
psTerminate(void)
{
}

// ---------------------------------------------------------------------------
// Game lifecycle driven by the OpenXR application layer
// ---------------------------------------------------------------------------

namespace androidgame {

bool
Initialise(const VulkanContext &context, bool *renderTargetStartupFailure)
{
	if(renderTargetStartupFailure != nil)
		*renderTargetStartupFailure = false;
	RsGlobal.appName = "GTA III VR";
	RsGlobal.maximumWidth = (RwInt32)context.width;
	RsGlobal.maximumHeight = (RwInt32)context.height;
	RsGlobal.width = (RwInt32)context.width;
	RsGlobal.height = (RwInt32)context.height;
	RsGlobal.quit = FALSE;
	RsGlobal.maxFPS = 90;

	// The game addresses its data with relative paths throughout, right down
	// to statvfs("models/gta3.img") in CdStreamInit. Rather than rewrite every
	// call site, make the staged data directory the working directory; re3's
	// existing POSIX casepath/fcaseopen then absorbs the DATA\\GTA_VC.DAT
	// spelling against a case-sensitive filesystem.
	const char *dataRoot = platform::gameDataRoot();
	if(dataRoot == nil || dataRoot[0] == '\0' || chdir(dataRoot) != 0){
		ALOGE("cannot enter game data directory '%s'",
		      dataRoot != nil ? dataRoot : "(null)");
		return false;
	}
	ALOG("working directory: %s", dataRoot);

	if(RsEventHandler(rsINITIALIZE, nil) == rsEVENTERROR){
		ALOGE("rsINITIALIZE failed");
		return false;
	}

	// RsRwInitialize takes a raw void* and wraps it in RwEngineOpenParams
	// itself, and RwEngineOpen then does
	//   openParams = *(rw::EngineOpenParams*)initParams->displayID;
	// on every non-D3D platform. So the rsRWINITIALIZE parameter is the
	// backend struct directly -- wrapping it here as well would leave the
	// backend reading the struct one indirection off. It must outlive the call.
	static rw::EngineOpenParams backendParams;
	backendParams.instance = context.instance;
	backendParams.physicalDevice = context.physicalDevice;
	backendParams.device = context.device;
	backendParams.fragmentStoresAndAtomicsEnabled =
		context.fragmentStoresAndAtomicsEnabled;
	backendParams.queue = context.queue;
	backendParams.queueFamilyIndex = context.queueFamilyIndex;
	backendParams.width = context.width;
	backendParams.height = context.height;
	backendParams.renderScaleEffectivePercent =
		context.renderScaleEffectivePercent;
	const int requestedTemporalMode = GetPrivateProfileIntA("VR", "Sgsr2Mode",
		(int)rw::vulkan::SGSR_OFF, ".\\vr_settings.ini");
	// Temporal reconstruction is currently disabled on Quest: headset tests
	// showed per-eye micro-motion even when the image was otherwise stationary.
	// Keep the implementation for research, but never expose an unsafe startup
	// mode. Multisampling is available again and does what it always
	// did: geometry edges. It never touched the masks, and the attempt
	// to make it do so through alpha to coverage only dithered them.
	// What settled the foliage was giving it a mip chain to sample.
	const int requestedMsaa = GetPrivateProfileIntA("VR", "MsaaSamples", 1,
	                                               ".\\vr_settings.ini");
	if(requestedTemporalMode != rw::vulkan::SGSR_OFF){
		WritePrivateProfileStringA("VR", "Sgsr2Mode", "0",
		                           ".\\vr_settings.ini");
		ALOG("disabled unstable temporal mode %d",
		     requestedTemporalMode);
	}
	const int msaaSamples = requestedMsaa >= 4 ? 4 :
		requestedMsaa >= 2 ? 2 : 1;
	if(msaaSamples != requestedMsaa){
		char text[8];
		snprintf(text, sizeof(text), "%d", msaaSamples);
		WritePrivateProfileStringA("VR", "MsaaSamples", text,
			".\\vr_settings.ini");
	}
	backendParams.sgsrMode = rw::vulkan::SGSR_OFF;
	backendParams.sceneSampleCount = msaaSamples;
	// The reflection reads a reduced copy of the previous frame; how far
	// reduced is the player-s, because the right answer depends on the
	// render scale it is paired with. One reads the frame itself.
	backendParams.sceneReflectionDivisor =
		GetPrivateProfileIntA("VR", "CarReflectionScale", 1,
			".\\vr_settings.ini");
	backendParams.sceneWidth = context.width;
	backendParams.sceneHeight = context.height;
	backendParams.viewCount = context.viewCount;
	backendParams.colourFormat = context.colourFormat;
	ALOG("stable native scene=%ux%u output=%ux%u temporal=off "
	     "msaa=%dx",
	     backendParams.sceneWidth, backendParams.sceneHeight,
	     context.width, context.height, msaaSamples);

	if(RsEventHandler(rsRWINITIALIZE, &backendParams) == rsEVENTERROR){
		if(renderTargetStartupFailure != nil)
			*renderTargetStartupFailure =
				rw::vulkan::didLastDeviceOpenFailForRenderTarget() != 0;
		ALOGE("rsRWINITIALIZE failed");
		RsEventHandler(rsTERMINATE, nil);
		return false;
	}
	gRwInitialised = true;
	// Before anything streams. The per-frame push below exists so the
	// menu switch takes effect on what loads next, but by the time the
	// first frame runs the world's textures already exist: pushed only
	// from there, the setting missed almost every texture it was meant
	// to cover.
	rw::vulkan::setGenerateMipmaps(androidgame::VrGenerateMipmaps());
	rw::vulkan::setRenderDiagnostics(androidgame::VrRenderDiagnostics());

	RwRect r;
	r.x = 0;
	r.y = 0;
	r.w = RsGlobal.maximumWidth;
	r.h = RsGlobal.maximumHeight;
	RsEventHandler(rsCAMERASIZE, &r);

	ALOG("game initialised at %ux%u, %u view(s)",
	     context.width, context.height, context.viewCount);
	return true;
}

bool
PrepareFrontendBeforeFrames(void)
{
	if(!gForegroundApp || !gRwInitialised)
		return false;

	// This is deliberately done before setFrameRenderer/renderFrame.  The
	// original state machine performed InitialiseOnceAfterRW from Step, after
	// xrBeginFrame and rw::vulkan::beginFrame.  On Quest that one call has been
	// measured at 357 ms, during which the runtime can only keep reprojecting
	// the previous startup image.  LoadingScreen remains useful for loading its
	// splash texture; with no open librw frame all draw submissions fail open
	// as no-ops, just like the existing non-rendering Step path below.
	if(gGameState == GS_START_UP)
		gGameState = GS_INIT_ONCE;

	if(gGameState == GS_INIT_ONCE){
		ALOG("pre-frame GS_INIT_ONCE: loading screen");
		LoadingScreen(nil, nil, "loadsc0");
		ALOG("pre-frame GS_INIT_ONCE: InitialiseOnceAfterRW");
		if(!CGame::InitialiseOnceAfterRW()){
			ALOGE("pre-frame InitialiseOnceAfterRW failed");
			RsGlobal.quit = TRUE;
			return false;
		}
		ALOG("pre-frame GS_INIT_ONCE: done");
		gGameState = GS_INIT_FRONTEND;
	}

	if(gGameState == GS_INIT_FRONTEND){
#if defined(MIAMIVR_DEV_TOOLS) && MIAMIVR_DEV_TOOLS
		if(QuestQuickTestStartEnabled()){
			ALOG("quick test start: bypassing frontend and starting a new game");
			LoadingScreen(nil, nil, "loadsc0");
			InitialiseGame();
			FrontEndMenuManager.m_bGameNotLoaded = false;
			FrontEndMenuManager.m_bMenuActive = false;
			FrontEndMenuManager.m_bStartUpFrontEndRequested = false;
			FrontEndMenuManager.m_bWantToRestart = false;
			FrontEndMenuManager.m_bWantToLoad = false;
			gGameState = GS_PLAYING_GAME;
			// Keep the one-shot skipper armed through the complete stock intro
			// chain. It disarms only after genuine gameplay control is stable.
			gQuestQuickStartSkipFrames = 3600;
			gQuestQuickStartPlayableFrames = 0;
			gQuestQuickStartCutsceneSkipIssued = false;
			gQuestQuickStartTimeScaleOwned = false;
			return true;
		}
#endif
		LoadingScreen(nil, nil, "loadsc0");
		FrontEndMenuManager.m_bGameNotLoaded = true;
		FrontEndMenuManager.m_bStartUpFrontEndRequested = true;
		gGameState = GS_FRONTEND;
	}

	return gGameState == GS_FRONTEND;
}

// The streamer measures its budget in archive bytes, but this device cannot
// sample DXT, so every texture is resident as full RGBA and costs four to eight
// times what it was billed. Its eviction threshold therefore sits at a figure
// the process cannot reach alive, and memory climbs until Android kills it --
// the crash reported after a few minutes at a high render scale with the larger
// model sets.
//
// Until textures reach the GPU in a format it can sample, this is the floor
// under that: watch what the rasters hold and give the streamer's own recycler
// enough work to stay under a real ceiling. Below the limit it does nothing.
static void
EnforceQuestTextureBudget(void)
{
	static size_t budget;
	if(budget == 0){
		const int megabytes = Max(256, GetPrivateProfileIntA("VR",
			"TextureMemoryBudgetMB", 1024, ".\\vr_settings.ini"));
		budget = (size_t)megabytes*1024*1024;
		ALOG("texture memory ceiling %d MB", megabytes);
	}
	if(rw::vulkan::getTextureMemoryUsed() <= budget)
		return;

	// Bounded per frame: releasing a whole overshoot in one step costs a
	// visible hitch, and the ceiling is not a cliff.
	int removed = 0;
	while(removed < 8 && rw::vulkan::getTextureMemoryUsed() > budget &&
	      CStreaming::RemoveLeastUsedModel())
		removed++;
	static uint32 reportAt;
	if(CTimer::GetTimeInMilliseconds() >= reportAt){
		reportAt = CTimer::GetTimeInMilliseconds()+5000;
		ALOG("texture memory %zu MB over the %zu MB ceiling, released %d",
		     rw::vulkan::getTextureMemoryUsed()/(1024*1024),
		     budget/(1024*1024), removed);
	}
}

void
Step(void)
{
	if(!gForegroundApp || !gRwInitialised)
		return;
	EnforceQuestTextureBudget();
	// Whatever the mip worker finished since the last frame.
	rw::vulkan::uploadFinishedMips();
	{
		static uint32 reportAt;
		if(CTimer::GetTimeInMilliseconds() >= reportAt){
			reportAt = CTimer::GetTimeInMilliseconds()+10000;
			rw::vulkan::reportGeneratedMips();
		}
	}
	// Hand the backend the time cycle planes the desktop renderer takes from
	// the RenderWare camera. Without them the world ends at the far clip with
	// a hard edge, which is plain to see from anything that can gain height.
	if(gGameState == GS_PLAYING_GAME)
		rw::vulkan::setFogParams(
			androidgame::VrDistanceFogEnabled() ?
				CTimeCycle::GetFogStart() : 0.0f,
			androidgame::VrDistanceFogEnabled() ?
				CTimeCycle::GetFarClip() : 0.0f);

	static RwUInt32 reportedState = 0xFFFFFFFF;
	if(gGameState != reportedState){
		ALOG("gGameState -> %u", gGameState);
		reportedState = gGameState;
	}

	// Keeps the stall watchdog honest: without this the checkpoint stops
	// moving whenever the game is somewhere that has none, and an idle-but-
	// healthy frontend looks identical to a hang.
	RE3_CHECKPOINT("game/Step running");

	// Starting a new game or loading a save asks for a restart rather than
	// continuing the current state machine. Mirrors the desktop skeleton's
	// handling of the same flag.
	if(FrontEndMenuManager.m_bWantToRestart){
		gVehicleSeatRecenter.Reset();
		ALOG("restart requested (load=%d, firstTime=%d)",
		     (int)FrontEndMenuManager.m_bWantToLoad,
		     (int)FrontEndMenuManager.m_bFirstTime);

		CPad::ResetCheats();
		CPad::StopPadsShaking();
		DMAudio.ChangeMusicMode(MUSICMODE_DISABLE);
		CTimer::Stop();

		if(FrontEndMenuManager.m_bWantToLoad){
			// The desktop frontend constructs a normal game world before it
			// enters the restart/load path.  The Android frame loop used to
			// service m_bWantToRestart first, while we were still in
			// GS_INIT_PLAYING_GAME, so ShutDownForRestart tried to clear pools
			// which had never been created (first the replay vehicle pool, then
			// the temporary-object pool).  Prime the same baseline world here
			// and only then run the ordinary restart/load sequence.
			if(gGameState != GS_PLAYING_GAME){
				ALOG("initialising baseline world before frontend save load (state=%u)",
				     gGameState);
				InitialiseGame();
				FrontEndMenuManager.m_bGameNotLoaded = false;
				gGameState = GS_PLAYING_GAME;
			}
			CGame::ShutDownForRestart();
			CGame::InitialiseWhenRestarting();
			DMAudio.ChangeMusicMode(MUSICMODE_GAME);
			LoadSplash(GetLevelSplashScreen(CGame::currLevel));
			FrontEndMenuManager.m_bWantToLoad = false;
		}else{
			if(gGameState == GS_PLAYING_GAME)
				CGame::ShutDown();
			CTimer::Stop();
			gGameState = FrontEndMenuManager.m_bFirstTime ?
				GS_INIT_FRONTEND : GS_INIT_PLAYING_GAME;
		}

		FrontEndMenuManager.m_bFirstTime = false;
		FrontEndMenuManager.m_bWantToRestart = false;
		return;
	}

	// Heartbeat: distinguishes "one call never returned" from "the frame loop
	// is spinning and calling Step over and over".
	static unsigned int steps = 0;
	static unsigned int reportedSteps = 0;
	if(++steps - reportedSteps >= 200){
		reportedSteps = steps;
		ALOG("Step heartbeat: %u calls, gGameState=%u", steps, gGameState);
	}

	// Mirrors the desktop skeleton's gGameState machine, minus the startup
	// movies: there is no 2D window to play them into, and the VR build shows
	// its own world-locked theatre instead.
	switch(gGameState){
	case GS_START_UP:
		gGameState = GS_INIT_ONCE;
		break;

	case GS_INIT_ONCE:
		ALOG("GS_INIT_ONCE: loading screen");
		LoadingScreen(nil, nil, "loadsc0");
		ALOG("GS_INIT_ONCE: InitialiseOnceAfterRW");
		if(!CGame::InitialiseOnceAfterRW()){
			ALOGE("InitialiseOnceAfterRW failed");
			RsGlobal.quit = TRUE;
			break;
		}
		ALOG("GS_INIT_ONCE: done");
		gGameState = GS_INIT_FRONTEND;
		break;

	case GS_INIT_FRONTEND:
		LoadingScreen(nil, nil, "loadsc0");
		FrontEndMenuManager.m_bGameNotLoaded = true;
		FrontEndMenuManager.m_bStartUpFrontEndRequested = true;
		gGameState = GS_FRONTEND;
		break;

	case GS_FRONTEND:
		RsEventHandler(rsFRONTENDIDLE, nil);
		if(!FrontEndMenuManager.m_bMenuActive ||
		   FrontEndMenuManager.m_bWantToLoad)
			gGameState = GS_INIT_PLAYING_GAME;
		break;

	case GS_INIT_PLAYING_GAME:
		InitialiseGame();
		FrontEndMenuManager.m_bGameNotLoaded = false;
		gGameState = GS_PLAYING_GAME;
		break;

	case GS_PLAYING_GAME:
#if defined(MIAMIVR_DEV_TOOLS) && MIAMIVR_DEV_TOOLS
		SkipQuestQuickStartCutsceneIfNeeded();
#endif
		// No frame limiter: xrWaitFrame already paces the application to the
		// headset's refresh rate, and a second limiter on top of it only adds
		// judder.
		RsEventHandler(rsIDLE, (void*)TRUE);
#if defined(MIAMIVR_DEV_TOOLS) && MIAMIVR_DEV_TOOLS
		// The script may have created the cutscene inside this very idle step.
		// Mark it skipped before the following submitted frame can present it.
		SkipQuestQuickStartCutsceneIfNeeded();
		AdvanceQuestQuickStartIntroSkip();
#endif
		// Rendering ran on the eye fov; game logic keeps the original.
		::VrRestoreFov();
		break;

	default:
		break;
	}
}

bool
WantsToQuit(void)
{
	// m_bWantToRestart is not a request to quit. It means "tear the session
	// down and come back", which is what the frontend sets when a new game is
	// started or a save is loaded. Treating it as quit ran the full CGame
	// shutdown instead -- the log showed REMOVE frontend and CPedStats
	// shutting down the instant New Game was picked, ending in a double
	// pthread_mutex_destroy. Restarting is handled in Step.
	return RsGlobal.quit;
}

void
Shutdown(void)
{
	if(gRwInitialised){
		RsEventHandler(rsRWTERMINATE, nil);
		gRwInitialised = false;
	}
	RsEventHandler(rsTERMINATE, nil);
}

} // namespace androidgame
