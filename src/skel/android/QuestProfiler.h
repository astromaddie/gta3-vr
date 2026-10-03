#pragma once

class CEntity;

namespace androidgame {

enum QuestProfilerPhase
{
	QUEST_PROFILER_PHASE_GAME,
	QUEST_PROFILER_PHASE_STREAMING,
	QUEST_PROFILER_PHASE_POPULATION,
	QUEST_PROFILER_PHASE_CAR,
	QUEST_PROFILER_PHASE_AUDIO,
	QUEST_PROFILER_PHASE_WORLD_LIST,
	QUEST_PROFILER_PHASE_PRE_RENDER,
	QUEST_PROFILER_PHASE_SCENE_SETUP,
	QUEST_PROFILER_PHASE_WORLD_RENDER,
	QUEST_PROFILER_PHASE_UI,
	QUEST_PROFILER_PHASE_PARTICLE_UPDATE,
	QUEST_PROFILER_PHASE_PARTICLE_RENDER,
	QUEST_PROFILER_PHASE_SCRIPT,
	QUEST_PROFILER_PHASE_PRE_WORLD_MISC,
	QUEST_PROFILER_PHASE_WORLD_PROCESS,
	QUEST_PROFILER_PHASE_POST_WORLD_CAMERA_FX,
	// Coarse RenderScene/RenderEffects partitions. These deliberately bracket
	// whole logical passes rather than individual atomics so profiling remains
	// cheap enough to leave enabled during headset traffic tests.
	QUEST_PROFILER_PHASE_RENDER_SKY,
	QUEST_PROFILER_PHASE_RENDER_ROADS,
	QUEST_PROFILER_PHASE_RENDER_REFLECTIONS,
	QUEST_PROFILER_PHASE_RENDER_WORLD,
	QUEST_PROFILER_PHASE_RENDER_WATER,
	QUEST_PROFILER_PHASE_RENDER_FADING,
	QUEST_PROFILER_PHASE_RENDER_WEATHER,
	QUEST_PROFILER_PHASE_RENDER_EFFECTS,
	QUEST_PROFILER_PHASE_COUNT
};

enum QuestWorldEntityClass
{
	QUEST_WORLD_ENTITY_PED,
	QUEST_WORLD_ENTITY_VEHICLE,
	QUEST_WORLD_ENTITY_OBJECT,
	QUEST_WORLD_ENTITY_COUNT
};

// These phases are already accumulated by CWorld::Process in one aggregate
// QuestWorldSimSample. Keeping the phase axis explicit here lets the profiler
// expose the existing measurements without adding timers to entity loops.
enum QuestWorldSimPhase
{
	QUEST_WORLD_SIM_PHASE_ANIMATION,
	QUEST_WORLD_SIM_PHASE_CONTROL,
	QUEST_WORLD_SIM_PHASE_POSTPONED_CONTROL,
	QUEST_WORLD_SIM_PHASE_COLLISION,
	QUEST_WORLD_SIM_PHASE_SHIFT,
	QUEST_WORLD_SIM_PHASE_TRANSFORM,
	QUEST_WORLD_SIM_PHASE_COUNT
};

// RenderOneNonRoad is the hot dynamic-entity submission path on Quest. Keep
// its class and vehicle-only phase axes separate from the coarser scene-pass
// timers so a capture can distinguish traffic cost from static world cost.
enum QuestRenderEntityClass
{
	QUEST_RENDER_ENTITY_VEHICLE,
	QUEST_RENDER_ENTITY_PED,
	QUEST_RENDER_ENTITY_STATIC,
	QUEST_RENDER_ENTITY_COUNT
};

enum QuestVehicleRenderPhase
{
	QUEST_VEHICLE_RENDER_PHASE_LIGHTING,
	QUEST_VEHICLE_RENDER_PHASE_OCCUPANTS,
	QUEST_VEHICLE_RENDER_PHASE_BODY_SUBMIT,
	QUEST_VEHICLE_RENDER_PHASE_ALPHA_ATOMICS,
	QUEST_VEHICLE_RENDER_PHASE_COUNT
};

// Filled locally by CWorld::Process only while the profiler is enabled. The
// game submits one aggregate sample per frame, avoiding any live world/pool
// access from the profiler itself.
struct QuestWorldSimSample
{
	uint64 worldTotalNs;
	uint64 animationNs[QUEST_WORLD_ENTITY_COUNT];
	uint64 controlNs[QUEST_WORLD_ENTITY_COUNT];
	uint64 postponedControlNs[QUEST_WORLD_ENTITY_COUNT];
	uint64 collisionNs[QUEST_WORLD_ENTITY_COUNT];
	uint64 shiftNs[QUEST_WORLD_ENTITY_COUNT];
	uint64 transformNs[QUEST_WORLD_ENTITY_COUNT];
	uint64 pedAttachmentNs;
	uint64 cutsceneNs;
	uint32 animationCalls[QUEST_WORLD_ENTITY_COUNT];
	uint32 controlCalls[QUEST_WORLD_ENTITY_COUNT];
	uint32 postponedControlCalls[QUEST_WORLD_ENTITY_COUNT];
	uint32 collisionCalls[QUEST_WORLD_ENTITY_COUNT];
	uint32 shiftCalls[QUEST_WORLD_ENTITY_COUNT];
};

// Produced by one completed RenderOneNonRoad call. Timing is only collected
// by the Android/Vulkan renderer while the profiler is enabled; the profiler
// folds these samples into one aggregate per app frame before smoothing.
struct QuestRenderEntitySample
{
	QuestRenderEntityClass entityClass;
	bool fadingPath;
	uint64 totalNs;
	uint64 vehiclePhaseNs[QUEST_VEHICLE_RENDER_PHASE_COUNT];
	uint32 vehicleOccupantsSubmitted;
};

struct QuestProfilerSnapshot
{
	float cpuAppMs;
	float cpuPreMs;
	float cpuVkBeginMs;
	float cpuStepMs;
	float cpuPostMs;
	float cpuVkEndMs;
	float cpuUnaccountedMs;
	float cpuAppMaxMs;
	float cpuPreMaxMs;
	float cpuVkBeginMaxMs;
	float cpuStepMaxMs;
	float cpuPostMaxMs;
	float cpuVkEndMaxMs;
	float gpuFrameMs;
	float gpuVulkanMs;
	float frameBudgetMs;
	bool cpuAppRuntime;
	bool gpuFrameRuntime;
	bool gpuFrameValid;
	bool gpuVulkanValid;

	// GAME is the complete CGame::Process interval. The named sub-phases below
	// partition its work; SIM_OTHER is the residual left after subtracting them.
	float cpuGameMs;
	float cpuGameMaxMs;
	float cpuStreamingMs;
	float cpuStreamingMaxMs;
	float cpuScriptMs;
	float cpuScriptMaxMs;
	float cpuPreWorldMiscMs;
	float cpuPreWorldMiscMaxMs;
	float cpuWorldProcessMs;
	float cpuWorldProcessMaxMs;
	float cpuPostWorldCameraFxMs;
	float cpuPostWorldCameraFxMaxMs;
	float cpuPopulationMs;
	float cpuPopulationMaxMs;
	float cpuCarMs;
	float cpuCarMaxMs;
	float cpuSimOtherMs;
	float cpuSimOtherMaxMs;
	float cpuAudioMs;
	float cpuAudioMaxMs;
	float cpuWorldListMs;
	float cpuWorldListMaxMs;
	float cpuPreRenderMs;
	float cpuPreRenderMaxMs;
	float cpuSceneSetupMs;
	float cpuSceneSetupMaxMs;
	float cpuWorldRenderMs;
	float cpuWorldRenderMaxMs;
	float cpuRenderSkyMs;
	float cpuRenderSkyMaxMs;
	float cpuRenderRoadsMs;
	float cpuRenderRoadsMaxMs;
	float cpuRenderReflectionsMs;
	float cpuRenderReflectionsMaxMs;
	float cpuRenderWorldMs;
	float cpuRenderWorldMaxMs;
	float cpuRenderWaterMs;
	float cpuRenderWaterMaxMs;
	float cpuRenderFadingMs;
	float cpuRenderFadingMaxMs;
	float cpuRenderWeatherMs;
	float cpuRenderWeatherMaxMs;
	float cpuRenderEffectsMs;
	float cpuRenderEffectsMaxMs;
	float cpuRenderOtherMs;
	float cpuRenderOtherMaxMs;
	float cpuRenderEntityMs[QUEST_RENDER_ENTITY_COUNT];
	float cpuRenderEntityMaxMs[QUEST_RENDER_ENTITY_COUNT];
	int32 renderEntityCalls[QUEST_RENDER_ENTITY_COUNT];
	float cpuRenderFadingEntityMs[QUEST_RENDER_ENTITY_COUNT];
	float cpuRenderFadingEntityMaxMs[QUEST_RENDER_ENTITY_COUNT];
	int32 renderFadingEntityCalls[QUEST_RENDER_ENTITY_COUNT];
	float cpuVehicleRenderPhaseMs[QUEST_VEHICLE_RENDER_PHASE_COUNT];
	float cpuVehicleRenderPhaseMaxMs[QUEST_VEHICLE_RENDER_PHASE_COUNT];
	int32 vehicleOccupantsSubmitted;
	float cpuUiMs;
	float cpuUiMaxMs;
	float cpuWorldSimMs;
	float cpuWorldSimMaxMs;
	float cpuPedSimMs;
	float cpuPedSimMaxMs;
	float cpuVehicleSimMs;
	float cpuVehicleSimMaxMs;
	float cpuObjectSimMs;
	float cpuObjectSimMaxMs;
	float cpuWorldAnimationMs;
	float cpuWorldAnimationMaxMs;
	float cpuWorldCollisionMs;
	float cpuWorldCollisionMaxMs;
	// Exponential per-world-frame averages plus the largest raw frame in the
	// current one-second CSV/log window. PED and VEHICLE are displayed in the
	// headset overlay; OBJECT remains available here for future diagnostics.
	float cpuWorldEntityPhaseMs[QUEST_WORLD_ENTITY_COUNT]
		[QUEST_WORLD_SIM_PHASE_COUNT];
	float cpuWorldEntityPhaseMaxMs[QUEST_WORLD_ENTITY_COUNT]
		[QUEST_WORLD_SIM_PHASE_COUNT];
	float cpuParticleUpdateMs;
	float cpuParticleUpdateMaxMs;
	float cpuParticleRenderMs;
	float cpuParticleRenderMaxMs;
	int32 pedControlCalls;
	int32 vehicleControlCalls;
	int32 collisionCalls;

	// One-second diagnostic snapshot. Pool scans and cumulative traffic-counter
	// deltas are intentionally sampled at log cadence rather than every frame.
	float appFps;
	int32 streamingRequested;
	int32 streamingPriority;
	uint64 streamingMemoryUsed;
	uint64 streamingMemoryAvailable;
	int32 streamingVehiclesLoaded;
	int32 streamingPedsLoaded;
	int32 pedTrafficPercent;
	int32 carTrafficPercent;
	int32 ambientPeds;
	float targetAmbientPeds;
	int32 ambientPedCap;
	int32 pedPoolUsed;
	int32 pedPoolSize;
	int32 vehiclePoolUsed;
	int32 vehiclePoolSize;
	int32 objectPoolUsed;
	int32 objectPoolSize;
	int32 entryInfoPoolUsed;
	int32 entryInfoPoolSize;
	int32 particleActive;
	int32 heliDustActive;
	int32 visibleEntities;
	int32 invisibleEntities;
	int32 visibleBuildings;
	int32 visibleRoads;
	int32 activeOccluders;
	int32 occlusionCullingMode;
	float trafficDesired;
	float trafficServed;
	int32 trafficAmbient;
	int32 trafficPursuit;
	int32 trafficProxies;
	int32 trafficPending;
	int32 trafficDebris;
	int32 trafficStalled;
	int32 trafficScavenged;
	int32 trafficPoolReserveBlocks;
	float trafficDirectorMs;
	float trafficDirectorMaxMs;
	int32 trafficJobHitsPerSecond;
	int32 trafficJobMissesPerSecond;
	float trafficJobMs;
	float trafficJobMaxMs;
	int32 trafficJobDispatchesPerSecond;
	int32 trafficJobSkipsPerSecond;
	float trafficJobBuildMs;
	float trafficJobBuildMaxMs;
	int32 trafficCruiseEligiblePerSecond;
	int32 trafficCruiseHitsPerSecond;
	int32 trafficCruiseMissesPerSecond;
	int32 trafficCruiseStalePerSecond;
	int32 collisionFlatScopesPerSecond;
	int32 collisionFlatBuildsPerSecond;
	int32 collisionFlatReusesPerSecond;
	int32 collisionFlatStalePerSecond;
	int32 collisionFlatOverflowsPerSecond;
	int32 collisionFlatItemsPerSecond;
	int32 collisionFlatSavedNodeVisitsPerSecond;

	// Physics Director: tiers/budget describe the latest frame; work counters are
	// one-second deltas sampled at the same cadence as the persistent CSV.
	int32 physicsDirectorMode;
	int32 physicsDirectorPreset;
	int32 physicsAdaptiveLevel;
	float physicsBudgetMs;
	float physicsManagedFrameMs;
	float physicsManagedAverageMs;
	int32 physicsFull;
	int32 physicsReduced;
	int32 physicsRail;
	int32 physicsProxy;
	int32 physicsTracked;
	int32 physicsOverflow;
	float physicsClassifyMs;
	int32 physicsCheckCollisionPerSecond;
	int32 physicsCheckSimplePerSecond;
	int32 physicsSimpleSkippedPerSecond;
	int32 physicsRailCollisionRunsPerSecond;
	int32 physicsRailCollisionSkipsPerSecond;
	int32 physicsProxyCollisionRunsPerSecond;
	int32 physicsProxyCollisionSkipsPerSecond;
	int32 physicsRailAiRunsPerSecond;
	int32 physicsRailAiSkipsPerSecond;
	int32 physicsProxyAiRunsPerSecond;
	int32 physicsProxyAiSkipsPerSecond;
	int32 physicsSafetyPromotionsPerSecond;
	int32 physicsImminentPromotionsPerSecond;
	int32 physicsContactPromotionsPerSecond;
	int32 physicsAdaptiveEscalationsPerSecond;
	int32 physicsAdaptiveRelaxationsPerSecond;
	float physicsManagedSimpleCollisionMs;
	float physicsManagedAiMs;
	int32 physicsRemoveAndAddSkippedPerSecond;
	int32 physicsSectorNodesPerSecond;
	int32 physicsBroadphaseCandidatesPerSecond;
	int32 physicsPairsAfterFilteringPerSecond;
	int32 physicsProcessColModelsPerSecond;
	int32 physicsWheelLineTestsPerSecond;
	int32 physicsTriangleTestsPerSecond;
	int32 physicsSubstepsPerSecond;
	int32 physicsExtraSubstepsPerSecond;
	int32 physicsRetryPassesPerSecond;
	int32 physicsContactManifoldsPerSecond;
	int32 physicsRemoveAndAddPerSecond;
	float physicsRemoveAndAddMs;

	// Visual Traffic Budget: one-second render-submission deltas. VHI is the
	// original full car path, VLO is the opt-in chassis_vlo-only path.
	int32 vehicleVisualBudgetMode;
	int32 vehicleVisualHighPerSecond;
	int32 vehicleVisualVloPerSecond;
	int32 vehicleVisualAtomicsSkippedPerSecond;
	int32 vehicleVisualOccupantsSkippedPerSecond;
	// Requested menu scale and the target OpenXR actually allocated. These
	// must travel together in captures; GPU comparisons across 100/125/150%
	// are otherwise indistinguishable from renderer regressions.
	int32 renderScaleRequestedPercent;
	int32 renderScaleSelectedPresetPercent;
	float renderScaleEffectivePercent;
	int32 renderScaleRecommendedWidth;
	int32 renderScaleRecommendedHeight;
	int32 renderScaleActualWidth;
	int32 renderScaleActualHeight;
	int32 renderScaleRuntimeMaxWidth;
	int32 renderScaleRuntimeMaxHeight;
	int32 renderScaleFallbackReason;
	int32 renderScalePreviousFallbackRequest;
	int32 renderScalePreviousFallbackPercent;
	int32 renderScalePreviousFallbackReason;
	int32 sgsrMode;
	int32 sgsrSceneWidth;
	int32 sgsrSceneHeight;
	int32 sgsrOutputWidth;
	int32 sgsrOutputHeight;
	int32 spawnAttemptsPerSecond;
	int32 spawnSuccessPerSecond;
	int32 spawnDeniedPerSecond;
	int32 spawnTopDenyReason;
	int32 spawnTopDenyCount;
	int32 spawnReasonPerSecond[16];
};

// The profiler is deliberately opt-in. Vulkan timestamp commands are only
// recorded while enabled, keeping the normal release path untouched.
void QuestProfilerSetLogDirectory(const char *directory);
void QuestProfilerSetEnabled(bool enabled);
bool QuestProfilerIsEnabled(void);

// App frame brackets the native renderer callback, excluding xrWaitFrame
// pacing. Step brackets Vice City's simulation + render command recording.
void QuestProfilerBeginAppFrame(void);
void QuestProfilerCancelAppFrame(void);
void QuestProfilerBeginVkBegin(void);
void QuestProfilerEndVkBegin(void);
void QuestProfilerBeginStep(void);
void QuestProfilerEndStep(void);
void QuestProfilerBeginVkEnd(void);
void QuestProfilerEndVkEnd(void);
void QuestProfilerEndAppFrame(void);

// Named sub-phases are independent timers. This permits STREAMING to be
// measured while the containing GAME timer remains open.
void QuestProfilerBeginPhase(QuestProfilerPhase phase);
void QuestProfilerEndPhase(QuestProfilerPhase phase);

uint64 QuestProfilerNowNanoseconds(void);
void QuestProfilerSubmitWorldSimSample(const QuestWorldSimSample &sample);
void QuestProfilerSubmitRenderEntitySample(
	const QuestRenderEntitySample &sample);
void QuestProfilerCountVisibleBuilding(const CEntity *entity);

QuestProfilerSnapshot QuestProfilerGetSnapshot(void);

} // namespace androidgame
