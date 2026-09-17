#include "mdrenderahead.h"

#include "dsp56kBase/threadtools.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace md
{
	namespace
	{
		constexpr size_t g_chunkCapacity = 512;
		constexpr size_t g_midiCapacity = 4096;
		constexpr size_t g_ringFrames = 1 << 17;		// ~3 s at 44.1 kHz, far beyond the largest prefill
		constexpr uint32_t g_maxChannels = 12;

		size_t roundUpPow2(size_t _v)
		{
			size_t p = 1;
			while(p < _v)
				p <<= 1;
			return p;
		}
	}

	// ---------------------------------------------------------------------------------------------

	RenderAhead::FrameRing::FrameRing(const uint32_t _channels, const size_t _frames)
		: m_channels(std::max<uint32_t>(_channels, 1))
		, m_frames(roundUpPow2(_frames))
		, m_mask(m_frames - 1)
		, m_data(m_frames * m_channels, 0.0f)
	{
	}

	void RenderAhead::FrameRing::write(const float* const* _channels, const uint32_t _frames)
	{
		auto w = m_write.load(std::memory_order_relaxed);
		for(uint32_t i = 0; i < _frames; ++i, ++w)
		{
			const auto base = (w & m_mask) * m_channels;
			for(uint32_t c = 0; c < m_channels; ++c)
				m_data[base + c] = _channels[c] ? _channels[c][i] : 0.0f;
		}
		m_write.store(w, std::memory_order_release);
	}

	void RenderAhead::FrameRing::writeSilence(const uint32_t _frames)
	{
		auto w = m_write.load(std::memory_order_relaxed);
		for(uint32_t i = 0; i < _frames; ++i, ++w)
		{
			const auto base = (w & m_mask) * m_channels;
			std::fill_n(m_data.begin() + static_cast<std::ptrdiff_t>(base), m_channels, 0.0f);
		}
		m_write.store(w, std::memory_order_release);
	}

	void RenderAhead::FrameRing::read(float* const* _channels, const uint32_t _frames)
	{
		auto r = m_read.load(std::memory_order_relaxed);
		for(uint32_t i = 0; i < _frames; ++i, ++r)
		{
			const auto base = (r & m_mask) * m_channels;
			for(uint32_t c = 0; c < m_channels; ++c)
			{
				if(_channels[c])
					_channels[c][i] = m_data[base + c];
			}
		}
		m_read.store(r, std::memory_order_release);
	}

	void RenderAhead::FrameRing::discard(const uint32_t _frames)
	{
		m_read.store(m_read.load(std::memory_order_relaxed) + _frames, std::memory_order_release);
	}

	// ---------------------------------------------------------------------------------------------

	RenderAhead::RenderAhead(std::recursive_timed_mutex& _machineMutex, RenderChunk _render, const uint32_t _channelsIn,
		const uint32_t _channelsOut, const uint32_t _prefillFrames, const float _samplerate)
		: m_machineMutex(_machineMutex)
		, m_render(std::move(_render))
		, m_channelsIn(std::min(_channelsIn, static_cast<uint32_t>(std::tuple_size_v<synthLib::TAudioInputs>)))
		, m_channelsOut(std::min(_channelsOut, g_maxChannels))
		, m_prefill(_prefillFrames)
		, m_samplerate(_samplerate)
		, m_chunks(g_chunkCapacity)
		, m_midiIn(g_midiCapacity)
		, m_midiOut(g_midiCapacity)
		, m_inputRing(m_channelsIn, g_ringFrames)
		, m_outputRing(m_channelsOut, g_ringFrames)
	{
		m_workerIn.resize(m_channelsIn);
		m_workerOut.resize(m_channelsOut);
		m_workerMidiIn.reserve(g_midiCapacity);
		m_workerMidiOut.reserve(g_midiCapacity);
		m_inFlightMidi.reserve(g_midiCapacity);

		// The host hears this much silence first; from then on every chunk it hands over is heard
		// exactly m_prefill frames after the synchronous path would have played it.
		m_outputRing.writeSilence(m_prefill);
		m_hostFrames = 0;

		m_thread = std::thread([this] { threadFunc(); });
	}

	RenderAhead::~RenderAhead()
	{
		std::vector<synthLib::SMidiEvent> ignored;
		stop(ignored);
	}

	void RenderAhead::process(const synthLib::TAudioInputs& _inputs, const synthLib::TAudioOutputs& _outputs,
		const uint32_t _frames, const std::vector<synthLib::SMidiEvent>& _midiIn,
		std::vector<synthLib::SMidiEvent>& _midiOut, const bool _nonRealtime)
	{
		if(_frames == 0)
		{
			// MIDI admitted without audio still reaches the machine, in order, before the next block
			if(!_midiIn.empty() && m_thread.joinable() && m_chunks.freeSpace() > 0 && m_midiIn.freeSpace() >= _midiIn.size())
			{
				for(const auto& ev : _midiIn)
					m_midiIn.push(ev);
				m_chunks.push(Chunk{0, static_cast<uint32_t>(_midiIn.size())});
				m_wake.notify();
			}
			return;
		}

		m_lastBlock.store(_frames, std::memory_order_relaxed);

		// 1. hand over this callback as one chunk: MIDI, input audio, size
		const bool worker = m_thread.joinable() && !m_exit.load(std::memory_order_relaxed);
		if(worker && m_chunks.freeSpace() > 0 && m_inputRing.freeFrames() >= _frames && m_midiIn.freeSpace() >= _midiIn.size())
		{
			for(const auto& ev : _midiIn)
				m_midiIn.push(ev);

			std::array<const float*, g_maxChannels> in{};
			for(uint32_t c = 0; c < m_channelsIn; ++c)
				in[c] = _inputs[c];
			m_inputRing.write(in.data(), _frames);

			m_chunks.push(Chunk{_frames, static_cast<uint32_t>(_midiIn.size())});
			m_wake.notify();
		}
		else
		{
			m_droppedChunks.fetch_add(1, std::memory_order_relaxed);
		}

		// 2. take finished audio. Normally it is already there because the worker had the prefill's
		// worth of time to render it. If the worker is behind, give it a bounded moment.
		const auto needed = static_cast<size_t>(m_skipFrames) + _frames;
		if(m_outputRing.available() < needed)
		{
			using Clock = std::chrono::steady_clock;
			const auto blockDuration = std::chrono::duration<double>(static_cast<double>(_frames) / std::max(1.0f, m_samplerate));
			const auto limit = _nonRealtime
				? std::chrono::duration<double>(30.0)
				: std::min(std::chrono::duration<double>(0.004), blockDuration * 0.5);
			const auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(limit);
			while(m_outputRing.available() < needed && Clock::now() < deadline && worker)
			{
				if(_nonRealtime)
					std::this_thread::sleep_for(std::chrono::microseconds(50));
				else
					std::this_thread::yield();
			}
		}

		auto available = m_outputRing.available();
		if(m_skipFrames)
		{
			const auto discard = static_cast<uint32_t>(std::min<size_t>(m_skipFrames, available));
			m_outputRing.discard(discard);
			m_skipFrames -= discard;
			available -= discard;
		}

		const auto got = static_cast<uint32_t>(std::min<size_t>(available, _frames));
		std::array<float*, g_maxChannels> out{};
		for(uint32_t c = 0; c < m_channelsOut; ++c)
			out[c] = _outputs[c];
		m_outputRing.read(out.data(), got);

		if(got < _frames)
		{
			for(uint32_t c = 0; c < m_channelsOut; ++c)
			{
				if(_outputs[c])
					std::fill_n(_outputs[c] + got, _frames - got, 0.0f);
			}
			// those frames will still be rendered; drop them when they arrive to stay in time
			m_skipFrames += _frames - got;
			m_underrunFrames.fetch_add(_frames - got, std::memory_order_relaxed);
			m_underrunCallbacks.fetch_add(1, std::memory_order_relaxed);
		}

		// 3. MIDI the machine sent during frames that are now being heard
		const auto hostStart = m_hostFrames;
		m_hostFrames += _frames;
		while(auto* timed = m_midiOut.front())
		{
			const auto hostFrame = timed->frame + m_prefill;
			if(hostFrame >= m_hostFrames)
				break;
			auto ev = timed->event;
			ev.offset = hostFrame > hostStart ? static_cast<uint32_t>(hostFrame - hostStart) : 0;
			_midiOut.push_back(std::move(ev));
			m_midiOut.pop();
		}
	}

	bool RenderAhead::lockMachine()
	{
		while(!m_exit.load(std::memory_order_relaxed))
		{
			if(m_machineMutex.try_lock_for(std::chrono::milliseconds(2)))
				return true;
		}
		return false;
	}

	void RenderAhead::threadFunc()
	{
		dsp56k::ThreadTools::setCurrentThreadName("MD render ahead");
		const auto block = std::max<uint32_t>(m_prefill, 64);
		dsp56k::ThreadTools::setCurrentThreadRealtimeParameters(static_cast<int>(m_samplerate), static_cast<int>(block));

		while(true)
		{
			m_wake.wait();
			if(m_exit.load(std::memory_order_relaxed))
				break;

			auto* chunk = m_chunks.front();
			if(!chunk)
				continue;
			const auto frames = chunk->frames;
			const auto midiCount = chunk->midiCount;

			// gather everything for this chunk before touching the machine
			m_inFlightMidi.clear();
			for(uint32_t i = 0; i < midiCount; ++i)
			{
				auto* ev = m_midiIn.front();
				if(!ev)
					break;
				m_inFlightMidi.push_back(*ev);
				m_midiIn.pop();
			}

			std::array<float*, g_maxChannels> in{};
			for(uint32_t c = 0; c < m_channelsIn; ++c)
			{
				if(m_workerIn[c].size() < frames)
					m_workerIn[c].resize(frames);
				in[c] = m_workerIn[c].data();
			}
			m_inputRing.read(in.data(), frames);

			synthLib::TAudioInputs inputs{};
			for(uint32_t c = 0; c < m_channelsIn; ++c)
				inputs[c] = m_workerIn[c].data();
			synthLib::TAudioOutputs outputs{};
			for(uint32_t c = 0; c < m_channelsOut; ++c)
			{
				if(m_workerOut[c].size() < frames)
					m_workerOut[c].resize(frames);
				outputs[c] = m_workerOut[c].data();
			}

			if(!lockMachine())
				break;

			m_workerMidiIn = m_inFlightMidi;
			m_workerMidiOut.clear();
			m_render(m_workerMidiIn, inputs, outputs, frames, m_workerMidiOut);
			m_inFlightMidi.clear();
			m_machineMutex.unlock();

			std::array<const float*, g_maxChannels> produced{};
			for(uint32_t c = 0; c < m_channelsOut; ++c)
				produced[c] = m_workerOut[c].data();
			if(frames && m_outputRing.freeFrames() >= frames)
				m_outputRing.write(produced.data(), frames);

			for(auto& ev : m_workerMidiOut)
			{
				if(!m_midiOut.push(TimedMidi{ev, m_workerFrames + ev.offset}))
					m_droppedMidiOut.fetch_add(1, std::memory_order_relaxed);
			}

			m_workerFrames += frames;
			m_renderedFrames.store(m_workerFrames, std::memory_order_relaxed);
			m_chunks.pop();
		}
	}

	void RenderAhead::stop(std::vector<synthLib::SMidiEvent>& _pendingMidi)
	{
		if(!m_thread.joinable())
			return;

		m_exit.store(true, std::memory_order_release);
		m_wake.notify();
		m_thread.join();

		// MIDI that was taken for a chunk that never rendered, then everything still queued
		for(auto& ev : m_inFlightMidi)
		{
			ev.offset = 0;
			_pendingMidi.push_back(ev);
		}
		m_inFlightMidi.clear();
		while(auto* ev = m_midiIn.front())
		{
			auto e = *ev;
			e.offset = 0;
			_pendingMidi.push_back(std::move(e));
			m_midiIn.pop();
		}
	}

	RenderAhead::Stats RenderAhead::getStats() const
	{
		Stats s;
		s.renderedFrames = m_renderedFrames.load(std::memory_order_relaxed);
		s.underrunFrames = m_underrunFrames.load(std::memory_order_relaxed);
		s.underrunCallbacks = m_underrunCallbacks.load(std::memory_order_relaxed);
		s.droppedChunks = m_droppedChunks.load(std::memory_order_relaxed);
		s.droppedMidiOut = m_droppedMidiOut.load(std::memory_order_relaxed);
		return s;
	}
}
