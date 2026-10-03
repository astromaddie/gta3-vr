#include "common.h"

#ifdef AUDIO_ANDROID

#include "sampman.h"
#include "AudioManager.h"
#include "MusicManager.h"
#include "platform_android.h"

#include <aaudio/AAudio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

cSampleManager SampleManager;
bool _bSampmanInitialised = false;
uint32 BankStartOffset[MAX_SFX_BANKS];
uint32 nNumMP3s = 0;
static tSample *gAndroidSampleTable = nullptr;

namespace {

struct AndroidChannel {
	uint32 sample = 0;
	uint32 frameCount = 0;
	uint32 loopStart = 0;
	uint32 loopEnd = 0;
	uint32 loopCount = 1;
	uint32 baseFrequency = DIGITALRATE;
	uint32 frequency = DIGITALRATE;
	uint32 volume = MAX_VOLUME;
	uint32 pan = 63;
	float maxDistance = 100.0f;
	double cursor = 0.0;
	uint32 revision = 0;
	bool valid = false;
	bool playing = false;
};

// The one 2D (stereo) channel follows the 3D channels; in GTA III it is the
// police radio channel.
static const uint32 CHANNEL2D = MAXCHANNELS;
static AndroidChannel gChannels[MAXCHANNELS + MAX2DCHANNELS];
static std::mutex gMixerMutex;
static AAudioStream *gOutputStream = nullptr;
static uint8 *gRawSamples = nullptr;
static size_t gRawSize = 0;
static int gRawFd = -1;
static int32 gOutputRate = 48000;
static bool gBankLoaded[MAX_SFX_BANKS] = {};
static uint8 gStreamVolume[MAX_STREAMS] = { MAX_VOLUME, MAX_VOLUME };
static uint8 gStreamPan[MAX_STREAMS] = { 63, 63 };
static uint8 gStreamEffect[MAX_STREAMS] = {};
static bool gStreamLoop[MAX_STREAMS] = {};
static uint32 gTrackLengths[TOTAL_STREAMED_SOUNDS] = {};

static int
clampSample(int value)
{
	return value < -32768 ? -32768 : value > 32767 ? 32767 : value;
}

static aaudio_data_callback_result_t
audioCallback(AAudioStream *, void *, void *audioData, int32_t numFrames)
{
	int16_t *output = static_cast<int16_t *>(audioData);
	memset(output, 0, size_t(numFrames) * 2 * sizeof(int16_t));

	// Never hold the control mutex while mixing a complete AAudio burst.  The
	// game thread updates several channel properties every frame; keeping this
	// mutex across all channels and samples could stall DMAudio.Service for a
	// whole callback.  Snapshotting makes both critical sections tiny without
	// dropping or repeating an entire burst when an update happens concurrently.
	AndroidChannel channels[MAXCHANNELS + MAX2DCHANNELS];
	{
		std::lock_guard<std::mutex> lock(gMixerMutex);
		memcpy(channels, gChannels, sizeof(channels));
	}

	for(AndroidChannel &channel : channels){
		if(!channel.playing || !channel.valid || channel.frameCount == 0)
			continue;

		const int16_t *samples = reinterpret_cast<const int16_t *>(
			gRawSamples + gAndroidSampleTable[channel.sample].nOffset);

		const float volume = float(channel.volume) / float(MAX_VOLUME);
		const float leftPan = channel.pan <= 63 ? 1.0f :
			float(127 - channel.pan) / 64.0f;
		const float rightPan = channel.pan >= 63 ? 1.0f :
			float(channel.pan) / 63.0f;
		const double step = std::max(1u, channel.frequency) /
			double(std::max(1, gOutputRate));
		uint32 loopEnd = channel.loopEnd == 0 ||
			channel.loopEnd > channel.frameCount ?
			channel.frameCount : channel.loopEnd;
		uint32 loopStart = std::min(channel.loopStart, loopEnd);

		for(int32 frame = 0; frame < numFrames; ++frame){
			uint32 index = uint32(channel.cursor);
			if(index >= loopEnd){
				if(channel.loopCount == 1){
					channel.playing = false;
					break;
				}
				if(channel.loopCount > 1)
					channel.loopCount--;
				channel.cursor = double(loopStart) +
					(channel.cursor - double(loopEnd));
				index = uint32(channel.cursor);
				if(index >= channel.frameCount)
					index = loopStart;
			}

			const int sample = samples[index];
			const int left = int(sample * volume * leftPan);
			const int right = int(sample * volume * rightPan);
			output[frame * 2] = int16_t(clampSample(output[frame * 2] + left));
			output[frame * 2 + 1] =
				int16_t(clampSample(output[frame * 2 + 1] + right));
			channel.cursor += step;
		}
	}

	// Commit only playback progress.  A revision change means the game thread
	// reinitialised, restarted, stopped, or changed loop semantics while this
	// snapshot was being mixed, so its newer state must win.
	{
		std::lock_guard<std::mutex> lock(gMixerMutex);
		for(size_t i = 0; i < ARRAY_SIZE(gChannels); ++i){
			AndroidChannel &live = gChannels[i];
			const AndroidChannel &mixed = channels[i];
			if(live.revision != mixed.revision || !live.valid)
				continue;
			live.cursor = mixed.cursor;
			live.loopCount = mixed.loopCount;
			live.playing = mixed.playing;
		}
	}
	return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static bool
openOutput()
{
	AAudioStreamBuilder *builder = nullptr;
	if(AAudio_createStreamBuilder(&builder) != AAUDIO_OK)
		return false;
	AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
	AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
	AAudioStreamBuilder_setChannelCount(builder, 2);
	AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
	AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
	AAudioStreamBuilder_setDataCallback(builder, audioCallback, nullptr);
	aaudio_result_t result = AAudioStreamBuilder_openStream(builder, &gOutputStream);
	AAudioStreamBuilder_delete(builder);
	if(result != AAUDIO_OK || gOutputStream == nullptr){
		LOGE("AAudio open failed: %s", AAudio_convertResultToText(result));
		gOutputStream = nullptr;
		return false;
	}
	gOutputRate = AAudioStream_getSampleRate(gOutputStream);
	result = AAudioStream_requestStart(gOutputStream);
	if(result != AAUDIO_OK){
		LOGE("AAudio start failed: %s", AAudio_convertResultToText(result));
		AAudioStream_close(gOutputStream);
		gOutputStream = nullptr;
		return false;
	}
	LOGI("AAudio SFX mixer: %d Hz, burst %d", gOutputRate,
		AAudioStream_getFramesPerBurst(gOutputStream));
	return true;
}

static void
closeOutput()
{
	if(gOutputStream != nullptr){
		AAudioStream_requestStop(gOutputStream);
		AAudioStream_close(gOutputStream);
		gOutputStream = nullptr;
	}
}

static bool
hasAdfExtension(const char *path)
{
	const size_t length = strlen(path);
	return length >= 4 && strcasecmp(path + length - 4, ".adf") == 0;
}

static uint32
estimateMp3Length(const char *path)
{
	FILE *file = fopen(path, "rb");
	if(file == nullptr)
		return 0;
	struct stat st{};
	fstat(fileno(file), &st);
	uint8 data[65536];
	const size_t count = fread(data, 1, sizeof(data), file);
	fclose(file);
	const bool encrypted = hasAdfExtension(path);
	if(encrypted)
		for(size_t i = 0; i < count; ++i)
			data[i] ^= 0x22;

	static const int mpeg1Bitrates[16] =
		{ 0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0 };
	static const int mpeg2Bitrates[16] =
		{ 0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0 };
	for(size_t i = 0; i + 4 <= count; ++i){
		const uint32 h = uint32(data[i]) << 24 | uint32(data[i+1]) << 16 |
			uint32(data[i+2]) << 8 | data[i+3];
		if((h & 0xFFE00000u) != 0xFFE00000u)
			continue;
		const int version = (h >> 19) & 3;
		const int layer = (h >> 17) & 3;
		const int bitrateIndex = (h >> 12) & 15;
		if(version == 1 || layer != 1 || bitrateIndex == 0 || bitrateIndex == 15)
			continue;
		const int kbps = version == 3 ?
			mpeg1Bitrates[bitrateIndex] : mpeg2Bitrates[bitrateIndex];
		if(kbps > 0 && st.st_size > 0)
			return uint32((uint64(st.st_size) * 8u) / uint64(kbps));
	}
	return 0;
}

static void
applyStreamVolume(uint8 stream)
{
	if(stream >= MAX_STREAMS)
		return;
	// SetStreamedVolumeAndPan stores an already game-scaled gain below; this
	// helper only turns its 0..127 level and pan into MediaPlayer L/R gains.
	const float gain = float(gStreamVolume[stream]) / float(MAX_VOLUME);
	const float left = gain * (gStreamPan[stream] <= 63 ? 1.0f :
		float(127 - gStreamPan[stream]) / 64.0f);
	const float right = gain * (gStreamPan[stream] >= 63 ? 1.0f :
		float(gStreamPan[stream]) / 63.0f);
	platform::audioSetStreamVolume(stream, left, right);
}

} // namespace

cSampleManager::cSampleManager(void)
{
	memset(m_aSamples, 0, sizeof(m_aSamples));
	memset(m_aAudioProviders, 0, sizeof(m_aAudioProviders));
	m_nEffectsVolume = MAX_VOLUME;
	m_nMusicVolume = MAX_VOLUME;
	m_nEffectsFadeVolume = MAX_VOLUME;
	m_nMusicFadeVolume = MAX_VOLUME;
	m_nMonoMode = 0;
	m_bInitialised = false;
	gAndroidSampleTable = m_aSamples;
}

cSampleManager::~cSampleManager(void) { Terminate(); }

void cSampleManager::SetSpeakerConfig(int32) {}
uint32 cSampleManager::GetMaximumSupportedChannels(void) { return MAXCHANNELS; }
uint32 cSampleManager::GetNum3DProvidersAvailable(void) { return 1; }
void cSampleManager::SetNum3DProvidersAvailable(uint32) {}
char *cSampleManager::Get3DProviderName(uint8)
{
	static char name[] = "Android AAudio";
	return name;
}
void cSampleManager::Set3DProviderName(uint8, char *) {}
int8 cSampleManager::GetCurrent3DProviderIndex(void) { return 0; }
int8 cSampleManager::SetCurrent3DProvider(uint8) { return 0; }
bool8 cSampleManager::IsMP3RadioChannelAvailable(void) { return false; }
void cSampleManager::ReleaseDigitalHandle(void) {}
void cSampleManager::ReacquireDigitalHandle(void) {}

bool8
cSampleManager::Initialise(void)
{
	if(_bSampmanInitialised)
		return true;
	m_nEffectsVolume = m_nMusicVolume = MAX_VOLUME;
	m_nEffectsFadeVolume = m_nMusicFadeVolume = MAX_VOLUME;
	m_nMonoMode = 0;
	for(int i = 0; i < TOTAL_AUDIO_SAMPLES; ++i){
		m_aSamples[i].nFrequency = MAX_FREQ;
		m_aSamples[i].nLoopEnd = -1;
	}
	if(!InitialiseSampleBanks()){
		LOGE("Android audio: sfx.RAW/SDT could not be opened");
		return false;
	}

	for(int i = 0; i < TOTAL_STREAMED_SOUNDS; ++i){
		char path[768];
		if(platform::resolveGamePathCaseInsensitive(path, sizeof(path),
			StreamedNameTable[i]))
			gTrackLengths[i] = estimateMp3Length(path);
		if(gTrackLengths[i] == 0)
			gTrackLengths[i] = i <= STREAMED_SOUND_RADIO_POLICE ?
				60u * 60u * 1000u : 10u * 60u * 1000u;
	}
	if(!openOutput())
		return false;
	gBankLoaded[SFX_BANK_0] = true;
	gBankLoaded[SFX_BANK_PED_COMMENTS] = true;
	m_bInitialised = true;
	_bSampmanInitialised = true;
	LOGI("Android audio backend initialised");
	return true;
}

void
cSampleManager::Terminate(void)
{
	if(!_bSampmanInitialised && gRawSamples == nullptr)
		return;
	for(int i = 0; i < MAX_STREAMS; ++i)
		platform::audioStopStream(i);
	closeOutput();
	if(gRawSamples != nullptr){
		munmap(gRawSamples, gRawSize);
		gRawSamples = nullptr;
		gRawSize = 0;
	}
	if(gRawFd >= 0){
		close(gRawFd);
		gRawFd = -1;
	}
	m_bInitialised = false;
	_bSampmanInitialised = false;
}

bool8 cSampleManager::CheckForAnAudioFileOnCD(void) { return gRawSamples != nullptr; }
char cSampleManager::GetCDAudioDriveLetter(void) { return '\0'; }
void cSampleManager::UpdateEffectsVolume(void) {}
void cSampleManager::SetEffectsMasterVolume(uint8 volume) { m_nEffectsVolume = volume; }
void cSampleManager::SetMusicMasterVolume(uint8 volume) { m_nMusicVolume = volume; }
void cSampleManager::SetEffectsFadeVolume(uint8 volume) { m_nEffectsFadeVolume = volume; }
void cSampleManager::SetMusicFadeVolume(uint8 volume) { m_nMusicFadeVolume = volume; }
void cSampleManager::SetMonoMode(bool8 mode) { m_nMonoMode = mode; }

bool8
cSampleManager::LoadSampleBank(uint8 bank)
{
	if(bank >= MAX_SFX_BANKS || gRawSamples == nullptr)
		return false;
	gBankLoaded[bank] = true;
	return true;
}

void cSampleManager::UnloadSampleBank(uint8 bank)
{
	if(bank < MAX_SFX_BANKS) gBankLoaded[bank] = false;
}
int8 cSampleManager::IsSampleBankLoaded(uint8 bank)
{
	return bank < MAX_SFX_BANKS && gBankLoaded[bank];
}
uint8 cSampleManager::IsPedCommentLoaded(uint32 comment)
{
	return comment < TOTAL_AUDIO_SAMPLES && gRawSamples != nullptr;
}
int32 cSampleManager::_GetPedCommentSlot(uint32) { return 0; }
bool8 cSampleManager::LoadPedComment(uint32 comment)
{
	return IsPedCommentLoaded(comment);
}
int32 cSampleManager::GetBankContainingSound(uint32 offset)
{
	if(offset >= BankStartOffset[SFX_BANK_PED_COMMENTS]) return SFX_BANK_PED_COMMENTS;
	if(offset >= BankStartOffset[SFX_BANK_0]) return SFX_BANK_0;
	return INVALID_SFX_BANK;
}
uint32 cSampleManager::GetSampleBaseFrequency(uint32 sample)
{
	return sample < TOTAL_AUDIO_SAMPLES ? m_aSamples[sample].nFrequency : 0;
}
uint32 cSampleManager::GetSampleLoopStartOffset(uint32 sample)
{
	return sample < TOTAL_AUDIO_SAMPLES ? m_aSamples[sample].nLoopStart : 0;
}
int32 cSampleManager::GetSampleLoopEndOffset(uint32 sample)
{
	return sample < TOTAL_AUDIO_SAMPLES ? m_aSamples[sample].nLoopEnd : -1;
}
uint32 cSampleManager::GetSampleLength(uint32 sample)
{
	return sample < TOTAL_AUDIO_SAMPLES ? m_aSamples[sample].nSize / 2 : 0;
}
bool8 cSampleManager::UpdateReverb(void) { return false; }
void cSampleManager::SetChannelReverbFlag(uint32, bool8) {}

bool8
cSampleManager::InitialiseChannel(uint32 channel, uint32 sfx, uint8 bank)
{
	if(channel >= MAXCHANNELS + MAX2DCHANNELS ||
	   sfx >= TOTAL_AUDIO_SAMPLES || !IsSampleBankLoaded(bank))
		return false;
	const tSample &sample = m_aSamples[sfx];
	if(sample.nOffset < 0 || sample.nSize < 2 ||
	   uint64(sample.nOffset) + sample.nSize > gRawSize)
		return false;
	std::lock_guard<std::mutex> lock(gMixerMutex);
	AndroidChannel &out = gChannels[channel];
	const uint32 revision = out.revision + 1;
	out = AndroidChannel{};
	out.revision = revision;
	out.sample = sfx;
	out.frameCount = sample.nSize / 2;
	out.loopEnd = out.frameCount;
	out.baseFrequency = sample.nFrequency > 0 ? sample.nFrequency : DIGITALRATE;
	out.frequency = out.baseFrequency;
	out.valid = true;
	return true;
}

void
cSampleManager::SetChannelEmittingVolume(uint32 channel, uint32 volume)
{
	if(channel >= MAXCHANNELS) return;
	volume = std::min<uint32>(volume, MAX_VOLUME);
	if(MusicManager.GetMusicMode() == MUSICMODE_CUTSCENE)
		volume >>= 2;
	volume = m_nEffectsFadeVolume * volume * m_nEffectsVolume >> 14;
	std::lock_guard<std::mutex> lock(gMixerMutex);
	gChannels[channel].volume = std::min<uint32>(volume, MAX_VOLUME);
}

void
cSampleManager::SetChannel3DPosition(uint32 channel, float x, float, float)
{
	if(channel >= MAXCHANNELS) return;
	std::lock_guard<std::mutex> lock(gMixerMutex);
	AndroidChannel &out = gChannels[channel];
	const float normal = out.maxDistance > 0.01f ?
		std::max(-1.0f, std::min(1.0f, -x / out.maxDistance)) : 0.0f;
	out.pan = uint32(std::max(0.0f, std::min(127.0f, 63.0f + normal * 64.0f)));
}
void cSampleManager::SetChannel3DDistances(uint32 channel, float maxDist, float)
{
	if(channel >= MAXCHANNELS) return;
	std::lock_guard<std::mutex> lock(gMixerMutex);
	gChannels[channel].maxDistance = std::max(0.01f, maxDist);
}
void cSampleManager::SetChannelVolume(uint32 channel, uint32 volume)
{
	if(channel != CHANNEL2D) return;
	volume = std::min<uint32>(volume, MAX_VOLUME);
	volume = m_nEffectsFadeVolume * volume * m_nEffectsVolume >> 14;
	std::lock_guard<std::mutex> lock(gMixerMutex);
	gChannels[channel].volume = std::min<uint32>(volume, MAX_VOLUME);
}
void cSampleManager::SetChannelPan(uint32 channel, uint32 pan)
{
	if(channel != CHANNEL2D) return;
	std::lock_guard<std::mutex> lock(gMixerMutex);
	gChannels[channel].pan = std::min<uint32>(pan, MAX_VOLUME);
}
void cSampleManager::SetChannelFrequency(uint32 channel, uint32 frequency)
{
	if(channel >= MAXCHANNELS + MAX2DCHANNELS) return;
	std::lock_guard<std::mutex> lock(gMixerMutex);
	gChannels[channel].frequency = std::max(1u, frequency);
}
void cSampleManager::SetChannelLoopPoints(uint32 channel, uint32 start, int32 end)
{
	if(channel >= MAXCHANNELS + MAX2DCHANNELS) return;
	std::lock_guard<std::mutex> lock(gMixerMutex);
	gChannels[channel].loopStart = start / 2;
	gChannels[channel].loopEnd = end < 0 ? gChannels[channel].frameCount :
		std::min<uint32>(uint32(end) / 2, gChannels[channel].frameCount);
	gChannels[channel].revision++;
}
void cSampleManager::SetChannelLoopCount(uint32 channel, uint32 count)
{
	if(channel >= MAXCHANNELS + MAX2DCHANNELS) return;
	std::lock_guard<std::mutex> lock(gMixerMutex);
	gChannels[channel].loopCount = count;
	gChannels[channel].revision++;
}
bool8 cSampleManager::GetChannelUsedFlag(uint32 channel)
{
	if(channel >= MAXCHANNELS + MAX2DCHANNELS) return false;
	std::lock_guard<std::mutex> lock(gMixerMutex);
	return gChannels[channel].playing;
}
void cSampleManager::StartChannel(uint32 channel)
{
	if(channel >= MAXCHANNELS + MAX2DCHANNELS) return;
	std::lock_guard<std::mutex> lock(gMixerMutex);
	if(gChannels[channel].valid){
		gChannels[channel].cursor = 0.0;
		gChannels[channel].playing = true;
		gChannels[channel].revision++;
	}
}
void cSampleManager::StopChannel(uint32 channel)
{
	if(channel >= MAXCHANNELS + MAX2DCHANNELS) return;
	std::lock_guard<std::mutex> lock(gMixerMutex);
	gChannels[channel].playing = false;
	gChannels[channel].revision++;
}

void cSampleManager::PreloadStreamedFile(uint8 file, uint8 stream)
{
	if(file >= TOTAL_STREAMED_SOUNDS || stream >= MAX_STREAMS) return;
	char path[768];
	if(platform::resolveGamePathCaseInsensitive(path, sizeof(path), StreamedNameTable[file]))
		platform::audioLoadStream(stream, path, 0, gStreamLoop[stream], false);
}
void cSampleManager::PauseStream(bool8 pause, uint8 stream)
{
	if(stream < MAX_STREAMS) platform::audioPauseStream(stream, pause != 0);
}
void cSampleManager::StartPreloadedStreamedFile(uint8 stream)
{
	if(stream < MAX_STREAMS) platform::audioPlayStream(stream);
}
bool8 cSampleManager::StartStreamedFile(uint8 file, uint32 position, uint8 stream)
{
	if(stream >= MAX_STREAMS) return false;
	if(file >= TOTAL_STREAMED_SOUNDS) return false;
	char path[768];
	if(!platform::resolveGamePathCaseInsensitive(path, sizeof(path), StreamedNameTable[file])){
		LOGE("stream not found: %s", StreamedNameTable[file]);
		return false;
	}
	const bool ok = platform::audioLoadStream(stream, path, position,
		gStreamLoop[stream], true);
	if(ok) applyStreamVolume(stream);
	return ok;
}
void cSampleManager::StopStreamedFile(uint8 stream)
{
	if(stream < MAX_STREAMS) platform::audioStopStream(stream);
}
int32 cSampleManager::GetStreamedFilePosition(uint8 stream)
{
	return stream < MAX_STREAMS ? platform::audioGetStreamPosition(stream) : 0;
}
void cSampleManager::SetStreamedVolumeAndPan(uint8 volume, uint8 pan,
	bool8 effect, uint8 stream)
{
	if(stream >= MAX_STREAMS) return;
	volume = std::min<uint8>(volume, MAX_VOLUME);
	pan = std::min<uint8>(pan, MAX_VOLUME);
	gStreamPan[stream] = pan;
	gStreamEffect[stream] = effect;
	uint32 effective;
	if(effect){
		// Match the PC backends: mission dialogue lives on streams 1/2 and
		// must not inherit the global effects fade used while a scripted
		// cutscene starts.  Applying that ramp made the first lines nearly
		// silent and only reached normal volume partway through the scene.
		if(stream == 1 || stream == 2)
			effective = 128u * uint32(volume) * m_nEffectsVolume >> 14;
		else
			effective = m_nEffectsFadeVolume * uint32(volume) *
				m_nEffectsVolume >> 14;
	}else
		effective = m_nMusicFadeVolume * uint32(volume) * m_nMusicVolume >> 14;
	gStreamVolume[stream] = std::min<uint32>(effective, MAX_VOLUME);
	applyStreamVolume(stream);
}
int32 cSampleManager::GetStreamedFileLength(uint8 stream)
{
	return gTrackLengths[stream];
}
bool8 cSampleManager::IsStreamPlaying(uint8 stream)
{
	return stream < MAX_STREAMS && platform::audioIsStreamPlaying(stream);
}
void cSampleManager::Service(void) {}

bool8
cSampleManager::InitialiseSampleBanks(void)
{
	char descPath[768];
	char rawPath[768];
	if(!platform::resolveGamePathCaseInsensitive(descPath, sizeof(descPath), "audio/sfx.SDT") ||
	   !platform::resolveGamePathCaseInsensitive(rawPath, sizeof(rawPath), "audio/sfx.RAW"))
		return false;
	FILE *desc = fopen(descPath, "rb");
	if(desc == nullptr) return false;
	const size_t read = fread(m_aSamples, sizeof(tSample), TOTAL_AUDIO_SAMPLES, desc);
	fclose(desc);
	if(read != TOTAL_AUDIO_SAMPLES) return false;

	gRawFd = open(rawPath, O_RDONLY);
	if(gRawFd < 0) return false;
	struct stat st{};
	if(fstat(gRawFd, &st) != 0 || st.st_size <= 0){
		close(gRawFd); gRawFd = -1; return false;
	}
	gRawSize = size_t(st.st_size);
	void *mapping = mmap(nullptr, gRawSize, PROT_READ, MAP_SHARED, gRawFd, 0);
	if(mapping == MAP_FAILED){
		close(gRawFd); gRawFd = -1; gRawSize = 0; return false;
	}
	gRawSamples = static_cast<uint8 *>(mapping);
	return true;
}

#endif // AUDIO_ANDROID
