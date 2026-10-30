/* A General MIDI synth behind the MPU-401 (mt32.cpp) when there is no MT-32 (WC_MT32): macOS's own
 * (AudioToolbox's DLS synth with its built-in General MIDI sounds), rendered here on the emulation
 * thread like the other devices, so it needs no files. WC_GM=0: none. Elsewhere (the web page)
 * there is none yet. Samples are made like the AdLib's (opl.cpp). */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#endif

uint64_t pc_now_ns(void);
int pc_warping(void);
int sound_open(unsigned rate);
void sound_out(int16_t *frames, unsigned count, uint64_t t0);

#ifdef __APPLE__

enum { RATE = 44100, SLICE = 512 };

static AudioUnit synth;
static int playing;
static uint64_t start_ns, frames_done;
static Float64 sample_time;
/* the message being received: running status, its data bytes, or a SysEx */
static uint8_t status, data[2];
static unsigned data_len, data_need;
static uint8_t sysex[1024 + 1];
static unsigned sysex_len;
static bool in_sysex;

int gm_active(void)
{
	return synth != NULL;
}

void gm_poll(uint64_t now)
{
	if (!synth) return;
	uint64_t due = (now - start_ns) * RATE / 1000000000u;
	if (due > frames_done + RATE / 10) frames_done = due - RATE / 10; /* stalled: skip */
	int out = playing && !pc_warping();
	float left[SLICE], right[SLICE];
	int16_t buf[2 * SLICE];
	while (frames_done < due) {
		uint32_t n = due - frames_done > SLICE ? SLICE : (uint32_t)(due - frames_done);
		struct { AudioBufferList list; AudioBuffer second; } bufs = {};
		bufs.list.mNumberBuffers = 2;
		bufs.list.mBuffers[0] = { 1, n * 4, left };
		bufs.list.mBuffers[1] = { 1, n * 4, right };
		AudioUnitRenderActionFlags flags = 0;
		AudioTimeStamp ts = {};
		ts.mSampleTime = sample_time;
		ts.mFlags = kAudioTimeStampSampleTimeValid;
		if (AudioUnitRender(synth, &flags, &ts, 0, n, &bufs.list) != noErr) {
			memset(left, 0, sizeof left);
			memset(right, 0, sizeof right);
		}
		sample_time += n;
		for (uint32_t i = 0; i < n; i++) {
			float l = left[i] * 32767.0f, r = right[i] * 32767.0f;
			buf[2 * i] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
			buf[2 * i + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
		}
		if (out) sound_out(buf, n, start_ns + frames_done * 1000000000u / RATE);
		frames_done += n;
	}
}

/* A byte of the MIDI stream the MPU-401 sends on. */
void gm_midi(uint8_t v)
{
	if (!synth) return;
	if (v >= 0xf8) return; /* real time: clock, active sensing, ... */
	if (in_sysex) {
		if (!(v & 0x80)) {
			if (sysex_len < sizeof sysex) sysex[sysex_len++] = v;
			return;
		}
		in_sysex = false;
		if (v == 0xf7) { /* complete; another status byte drops it */
			sysex[sysex_len++] = v;
			gm_poll(pc_now_ns());
			MusicDeviceSysEx(synth, sysex, sysex_len);
			return;
		}
	}
	if (v == 0xf0) {
		in_sysex = true;
		sysex[0] = v;
		sysex_len = 1;
		status = 0;
		return;
	}
	if (v & 0x80) {
		status = v >= 0xf0 ? 0 : v; /* other system messages: ignored, and they end running status */
		data_len = 0;
		data_need = (v & 0xe0) == 0xc0 ? 1 : 2; /* program change, channel pressure: one data byte */
		return;
	}
	if (!status) return;
	data[data_len++] = v;
	if (data_len < data_need) return;
	data_len = 0;
	gm_poll(pc_now_ns()); /* the sound up to this moment, before the message */
	MusicDeviceMIDIEvent(synth, status, data[0], data_need > 1 ? data[1] : 0, 0);
}

void gm_init(void)
{
	const char *off = getenv("WC_GM");
	if (off && !strcmp(off, "0")) return;
	AudioComponentDescription desc = { kAudioUnitType_MusicDevice, kAudioUnitSubType_DLSSynth,
		kAudioUnitManufacturer_Apple, 0, 0 };
	AudioComponent comp = AudioComponentFindNext(NULL, &desc);
	if (!comp || AudioComponentInstanceNew(comp, &synth) != noErr) {
		synth = NULL;
		return;
	}
	AudioStreamBasicDescription fmt = {};
	fmt.mSampleRate = RATE;
	fmt.mFormatID = kAudioFormatLinearPCM;
	fmt.mFormatFlags = (AudioFormatFlags)kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
	fmt.mBytesPerPacket = fmt.mBytesPerFrame = 4;
	fmt.mFramesPerPacket = 1;
	fmt.mChannelsPerFrame = 2;
	fmt.mBitsPerChannel = 32;
	UInt32 slice = SLICE;
	if (AudioUnitSetProperty(synth, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &fmt, sizeof fmt) ||
		AudioUnitSetProperty(synth, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &slice,
			sizeof slice) ||
		AudioUnitInitialize(synth)) {
		fprintf(stderr, "wc: no General MIDI synth (AudioToolbox's DLS synth didn't start)\n");
		AudioComponentInstanceDispose(synth);
		synth = NULL;
		return;
	}
	playing = sound_open(RATE);
	start_ns = pc_now_ns();
}

#else

int gm_active(void) { return 0; }
void gm_poll(uint64_t) {}
void gm_midi(uint8_t) {}
void gm_init(void) {}

#endif
