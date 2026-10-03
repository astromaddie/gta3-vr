// No-op profiler. The Quest profiler (Vulkan timestamp queries, CSV logs and
// an overlay page) is not part of the Steam Frame port; the engine hooks that
// would feed it are not carried either. The interface stays so the
// application layer and the menu keep compiling unchanged.
#include "common.h"
#include "QuestProfiler.h"

namespace androidgame {

void QuestProfilerSetLogDirectory(const char *) {}
void QuestProfilerSetEnabled(bool) {}
bool QuestProfilerIsEnabled(void) { return false; }

void QuestProfilerBeginAppFrame(void) {}
void QuestProfilerCancelAppFrame(void) {}
void QuestProfilerBeginVkBegin(void) {}
void QuestProfilerEndVkBegin(void) {}
void QuestProfilerBeginStep(void) {}
void QuestProfilerEndStep(void) {}
void QuestProfilerBeginVkEnd(void) {}
void QuestProfilerEndVkEnd(void) {}
void QuestProfilerEndAppFrame(void) {}

void QuestProfilerBeginPhase(QuestProfilerPhase) {}
void QuestProfilerEndPhase(QuestProfilerPhase) {}

uint64 QuestProfilerNowNanoseconds(void) { return 0; }
void QuestProfilerSubmitWorldSimSample(const QuestWorldSimSample &) {}
void QuestProfilerSubmitRenderEntitySample(const QuestRenderEntitySample &) {}
void QuestProfilerCountVisibleBuilding(const CEntity *) {}

QuestProfilerSnapshot
QuestProfilerGetSnapshot(void)
{
	QuestProfilerSnapshot snapshot = {};
	return snapshot;
}

} // namespace androidgame
