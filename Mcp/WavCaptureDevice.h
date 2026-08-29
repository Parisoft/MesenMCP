//MesenMCP - audio capture device
//
//An IAudioDevice that keeps the most recent audio in a ring buffer instead of
//playing it, so agents can verify "is the music actually playing / not silent /
//not clipping" and capture WAV files - the half of homebrew validation that
//screenshots cannot see. Registered with the SoundMixer the same way the old
//SdlSoundManager was.
#pragma once
#include "Core/Shared/Interfaces/IAudioDevice.h"
#include "Utilities/SimpleLock.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

class WavCaptureDevice : public IAudioDevice
{
public:
	//capacitySeconds of stereo int16 audio kept in the ring buffer
	explicit WavCaptureDevice(uint32_t capacitySeconds = 30)
	{
		_capacity = capacitySeconds * 48000 * 2; //samples (worst case 48kHz stereo)
		_buffer.resize(_capacity);
	}

	//--- IAudioDevice (called on the emulation thread) ---

	void PlayBuffer(int16_t* soundBuffer, uint32_t bufferSize, uint32_t sampleRate, bool isStereo) override
	{
		uint32_t sampleCount = bufferSize * (isStereo ? 2 : 1);
		if(sampleCount == 0) {
			return;
		}

		auto lock = _lock.AcquireSafe();
		_sampleRate = sampleRate;
		_isStereo = isStereo;

		for(uint32_t i = 0; i < sampleCount; i++) {
			int16_t s = soundBuffer[i];
			_buffer[_pos] = s;
			_pos = (_pos + 1) % _capacity;

			uint32_t ch = isStereo ? (i & 1) : 0;
			_sqSum[ch] += (double)s * (double)s;
			_sqCountCh[ch]++;
			int16_t abs = s < 0 ? (int16_t)-s : s;
			if(abs > _peak[ch]) { _peak[ch] = abs; }
			if(abs > _clipThreshold) { _clipCount++; }
		}
		_totalSamples += sampleCount;
		_hasData = true;
	}

	void Stop() override {}
	void Pause() override {}
	void ProcessEndOfFrame() override {}

	string GetAvailableDevices() override { return "wav-capture"; }
	void SetAudioDevice(string deviceName) override {}
	AudioStatistics GetStatistics() override { return {}; }

	//--- MCP-side queries ---

	bool HasData() { return _hasData.load(); }
	uint32_t GetSampleRate() { return _sampleRate.load(); }
	bool IsStereo() { return _isStereo.load(); }

	//RMS/peak over the most recent windowMs of audio, per channel (values 0.0-1.0)
	struct ChannelStats { double Rms; double Peak; };

	void GetRecentStats(uint32_t windowMs, ChannelStats out[2], uint64_t& clipCount, uint32_t& windowSamples)
	{
		auto lock = _lock.AcquireSafe();
		uint64_t rate = _sampleRate ? _sampleRate.load() : 48000;
		uint32_t chCount = _isStereo ? 2 : 1;
		windowSamples = (uint32_t)(rate * chCount * windowMs / 1000);
		if(windowSamples > _totalSamples) {
			windowSamples = (uint32_t)_totalSamples;
		}
		if(windowSamples > _capacity) {
			windowSamples = _capacity;
		}
		if(windowSamples == 0) {
			out[0] = {0, 0};
			out[1] = {0, 0};
			clipCount = 0;
			return;
		}

		uint32_t start = (_pos + _capacity - windowSamples) % _capacity;
		double sq[2] = {0, 0};
		int16_t peak[2] = {0, 0};
		uint64_t clips = 0;
		uint32_t count[2] = {0, 0};
		for(uint32_t i = 0; i < windowSamples; i++) {
			int16_t s = _buffer[(start + i) % _capacity];
			uint32_t ch = _isStereo ? (i & 1) : 0;
			sq[ch] += (double)s * (double)s;
			count[ch]++;
			int16_t abs = s < 0 ? (int16_t)-s : s;
			if(abs > peak[ch]) { peak[ch] = abs; }
			if(abs > _clipThreshold) { clips++; }
		}
		for(uint32_t ch = 0; ch < 2; ch++) {
			out[ch].Rms = count[ch] ? std::sqrt(sq[ch] / count[ch]) / 32768.0 : 0.0;
			out[ch].Peak = peak[ch] / 32768.0;
		}
		clipCount = clips;
	}

	//Totals since the device was created/reset
	void GetTotalStats(double outRms[2], int16_t outPeak[2], uint64_t& totalSamples, uint64_t& clipCount)
	{
		auto lock = _lock.AcquireSafe();
		for(uint32_t ch = 0; ch < 2; ch++) {
			outRms[ch] = _sqCountCh[ch] ? std::sqrt(_sqSum[ch] / (double)_sqCountCh[ch]) / 32768.0 : 0.0;
			outPeak[ch] = _peak[ch];
		}
		totalSamples = _totalSamples;
		clipCount = _clipCount;
	}

	void ResetStats()
	{
		auto lock = _lock.AcquireSafe();
		memset(_buffer.data(), 0, _buffer.size() * sizeof(int16_t));
		_pos = 0;
		_totalSamples = 0;
		_sqSum[0] = _sqSum[1] = 0;
		_sqCountCh[0] = _sqCountCh[1] = 0;
		_peak[0] = _peak[1] = 0;
		_clipCount = 0;
		_hasData = false;
	}

private:
	static constexpr int16_t _clipThreshold = 32000;

	SimpleLock _lock;
	std::vector<int16_t> _buffer;
	uint32_t _capacity = 0;
	uint32_t _pos = 0;

	std::atomic<uint32_t> _sampleRate { 0 };
	std::atomic<bool> _isStereo { true };
	std::atomic<bool> _hasData { false };

	uint64_t _totalSamples = 0;
	double _sqSum[2] = { 0, 0 };
	uint64_t _sqCountCh[2] = { 0, 0 };
	int16_t _peak[2] = { 0, 0 };
	uint64_t _clipCount = 0;
};
