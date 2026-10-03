#include "common.h"

#include "main.h"
#include "RwHelper.h"
#include "Lights.h"
#include "PointLights.h"
#include "RpAnimBlend.h"
#include "AnimBlendClumpData.h"
#include "Bones.h"
#include "Renderer.h"
#include "ModelIndices.h"
#include "Shadows.h"
#include "Timecycle.h"
#include "CutsceneObject.h"

CCutsceneObject::CCutsceneObject(void)
{
	SetStatus(STATUS_SIMPLE);
	bUsesCollision = false;
	bStreamingDontDelete = true;
	ObjectCreatedBy = CUTSCENE_OBJECT;
	m_fMass = 1.0f;
	m_fTurnMass = 1.0f;

#ifdef PED_SKIN
	bRenderHead = true;
	bRenderRightHand = true;
	bRenderLeftHand = true;
#endif
}

void
CCutsceneObject::SetModelIndex(uint32 id)
{
	CEntity::SetModelIndex(id);
	assert(RwObjectGetType(m_rwObject) == rpCLUMP);
	RpAnimBlendClumpInit((RpClump*)m_rwObject);
	(*RPANIMBLENDCLUMPDATA(m_rwObject))->velocity3d = &m_vecMoveSpeed;
	(*RPANIMBLENDCLUMPDATA(m_rwObject))->frames[0].flag |= AnimBlendFrameData::VELOCITY_EXTRACTION_3D;
}

void
CCutsceneObject::ProcessControl(void)
{
	CPhysical::ProcessControl();

	if(CTimer::GetTimeStep() < 1/100.0f)
		m_vecMoveSpeed *= 100.0f;
	else
		m_vecMoveSpeed *= 1.0f/CTimer::GetTimeStep();

	ApplyMoveSpeed();

#ifdef PED_SKIN
	if(IsClumpSkinned(GetClump()))
		UpdateRpHAnim();
#endif
}

#if !(defined(RW_VULKAN) && defined(__ANDROID__))
static RpMaterial*
MaterialSetAlpha(RpMaterial *material, void *data)
{
	((RwRGBA*)RpMaterialGetColor(material))->alpha = (uint8)(uintptr)data;
	return material;
}
#endif

#if defined(RW_VULKAN) && defined(__ANDROID__)
static RpMaterial*
MaterialSetOpaqueWhite(RpMaterial *material, void*)
{
	RwRGBA *colour = (RwRGBA*)RpMaterialGetColor(material);
	colour->red = 255;
	colour->green = 255;
	colour->blue = 255;
	colour->alpha = 255;
	return material;
}

static RpAtomic*
RestoreCutsceneAtomic(RpAtomic *atomic, void*)
{
	RpGeometry *geometry = RpAtomicGetGeometry(atomic);
	if(geometry == nil)
		return atomic;

	// Cutscene character DFFs are lit and have no prelight. Their materials
	// are already white, and they only need to be opaque. Forcing MODULATE on
	// just the first atomic made the Vulkan skin path depend on a zero
	// cutscene-light value and left every actor black. Put every atomic back
	// in the DFF's non-modulated state.
	RpGeometrySetFlags(geometry,
		RpGeometryGetFlags(geometry) & ~rpGEOMETRYMODULATEMATERIALCOLOR);
	RpGeometryForAllMaterials(geometry, MaterialSetOpaqueWhite, nil);
	return atomic;
}

static void
EnsureCutsceneAmbientFloor(void)
{
	if(pAmbient == nil)
		return;

	// Cutscenes can inherit an almost black object ambient. The Vulkan skin
	// path has no prelight term for these lit DFFs, so that value turns the
	// whole textured actor into a silhouette. Raise only genuinely under-lit
	// frames, keeping the colour ratio and the directional light intact.
	const float minChannel = 0.28f;
	RwRGBAReal colour = pAmbient->color;
	const float strongest = Max(colour.red, Max(colour.green, colour.blue));
	if(strongest >= minChannel)
		return;

	if(strongest > 0.001f){
		const float scale = minChannel / strongest;
		colour.red = Min(1.0f, colour.red * scale);
		colour.green = Min(1.0f, colour.green * scale);
		colour.blue = Min(1.0f, colour.blue * scale);
	}else{
		colour.red = minChannel;
		colour.green = minChannel;
		colour.blue = minChannel;
	}
	colour.alpha = 1.0f;
	RpLightSetColor(pAmbient, &colour);
}
#endif

void
CCutsceneObject::PreRender(void)
{
	if(IsPedModel(GetModelIndex())){
#if defined(RW_VULKAN) && defined(__ANDROID__)
		// The projected ped shadow's multipass geometry is recorded into the
		// headset colour target and covers the actor with a black silhouette,
		// so it is left out on this backend.
#else
		CShadows::StoreShadowForPedObject(this,
			CTimeCycle::m_fShadowDisplacementX[CTimeCycle::m_CurrentStoredValue],
			CTimeCycle::m_fShadowDisplacementY[CTimeCycle::m_CurrentStoredValue],
			CTimeCycle::m_fShadowFrontX[CTimeCycle::m_CurrentStoredValue],
			CTimeCycle::m_fShadowFrontY[CTimeCycle::m_CurrentStoredValue],
			CTimeCycle::m_fShadowSideX[CTimeCycle::m_CurrentStoredValue],
			CTimeCycle::m_fShadowSideY[CTimeCycle::m_CurrentStoredValue]);
#endif
		// For some reason xbox/android limbs are transparent here...
#if defined(RW_VULKAN) && defined(__ANDROID__)
		RpClumpForAllAtomics(GetClump(), RestoreCutsceneAtomic, nil);
#else
		RpGeometry *geometry = RpAtomicGetGeometry(GetFirstAtomic(GetClump()));
		RpGeometrySetFlags(geometry, RpGeometryGetFlags(geometry) | rpGEOMETRYMODULATEMATERIALCOLOR);
		RpGeometryForAllMaterials(geometry, MaterialSetAlpha, (void*)255);
#endif
	}
}

void
CCutsceneObject::Render(void)
{
#ifdef PED_SKIN
	if(IsClumpSkinned(GetClump())){
		if(bRenderLeftHand) RenderLimb(BONE_Lhand);
		if(bRenderRightHand) RenderLimb(BONE_Rhand);
		if(bRenderHead) RenderLimb(BONE_head);
	}
#endif
	CObject::Render();
}

#ifdef PED_SKIN
void
CCutsceneObject::RenderLimb(int32 bone)
{
	RpAtomic *atomic;
	CPedModelInfo *mi = (CPedModelInfo *)CModelInfo::GetModelInfo(GetModelIndex());
	switch(bone){
	case BONE_head:
		atomic = mi->getHead();
		break;
	case BONE_Lhand:
		atomic = mi->getLeftHand();
		break;
	case BONE_Rhand:
		atomic = mi->getRightHand();
		break;
	default:
		return;
	}
	if(atomic){
		RpHAnimHierarchy *hier = GetAnimHierarchyFromSkinClump(GetClump());
		int idx = RpHAnimIDGetIndex(hier, bone);
		RwMatrix *mat = &RpHAnimHierarchyGetMatrixArray(hier)[idx];
		RwFrame *frame = RpAtomicGetFrame(atomic);
		*RwFrameGetMatrix(frame) = *mat;
		RwFrameUpdateObjects(frame);
		RpAtomicRender(atomic);
	}
}
#endif

bool
CCutsceneObject::SetupLighting(void)
{
	ActivateDirectional();
	SetAmbientColoursForPedsCarsAndObjects();
#if defined(RW_VULKAN) && defined(__ANDROID__)
	EnsureCutsceneAmbientFloor();
#endif

	if(bRenderScorched){
		WorldReplaceNormalLightsWithScorched(Scene.world, 0.1f);
	}else{
		CVector coors = GetPosition();
		float lighting = CPointLights::GenerateLightsAffectingObject(&coors);
		if(!bHasBlip && lighting != 1.0f){
			SetAmbientAndDirectionalColours(lighting);
#if defined(RW_VULKAN) && defined(__ANDROID__)
			// A darkening point light is applied after the base ambient; clamp
			// again so it cannot undo the fix above.
			EnsureCutsceneAmbientFloor();
#endif
			return true;
		}
	}

	return false;
}

void
CCutsceneObject::RemoveLighting(bool reset)
{
	CRenderer::RemoveVehiclePedLights(this, reset);
}
