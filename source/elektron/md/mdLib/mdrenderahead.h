#pragma once

#include "synthLib/audioTypes.h"
#include "synthLib/midiTypes.h"

#include "dsp56kBase/semaphore.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace md
{
	// Runs the machine on its own thread, a fixed number of frames ahead of the host.
	//
	// The host callback hands over what it has (MIDI with sample offsets, audio input) as one chunk of
	// the size it asked for, and takes finished audio that the worker rendered earlier. The worker
	// renders the chunks in the same order and with the same sizes as the synchronous path would, so
	// the machine sees exactly the same sequence of blocks: the output equals the synchronous output
	// delayed by the prefill. Slow moments on the worker are absorbed by that prefill instead of
	// making the host callback late.
	//
	// Threads: process() is called by the host audio thread only. The worker is the only other user
	// of the queues. The machine itself is only touched inside the render callback, which the worker
	// calls with the machine mutex held; everything else that touches the machine takes the same
	// mutex, so it pauses the worker between chunks.
	class RenderAhead
	{
	public:
		// Renders one chunk. Runs on the worker with the machine mutex held. _midiIn offsets are relative
		// to the chunk start; _midiOut offsets must be relative to the chunk start as well.
		using RenderChunk = std::function<void(std::vector<synthLib::SMidiEvent>& _midiIn,
			const synthLib::TAudioInputs& _inputs, const synthLib::TAudioOutputs& _outputs, uint32_t _frames,
			std::vector<synthLib::SMidiEvent>& _midiOut)>;

		struct Stats
		{
			uint64_t renderedFrames = 0;
			uint64_t underrunFrames = 0;
			uint64_t underrunCallbacks = 0;
			uint64_t droppedChunks = 0;
			uint64_t droppedMidiOut = 0;
		};

		RenderAhead(std::recursive_timed_mutex& _machineMutex, RenderChunk _render, uint32_t _channelsIn,
			uint32_t _channelsOut, uint32_t _prefillFrames, float _samplerate);
		~RenderAhead();

		RenderAhead(const RenderAhead&) = delete;
		RenderAhead& operator=(const RenderAhead&) = delete;

		uint32_t getPrefillFrames() const { return m_prefill; }

		// Host audio thread. _midiIn offsets are relative to this callback. _nonRealtime waits for the
		// worker as long as needed (offline bounce) instead of outputting silence when it falls behind.
		void process(const synthLib::TAudioInputs& _inputs, const synthLib::TAudioOutputs& _outputs, uint32_t _frames,
			const std::vector<synthLib::SMidiEvent>& _midiIn, std::vector<synthLib::SMidiEvent>& _midiOut, bool _nonRealtime);

		// Stops the worker. MIDI that was handed over but not rendered yet is appended to _pendingMidi
		// (offsets reset to zero) so the caller can deliver it synchronously; nothing is lost.
		void stop(std::vector<synthLib::SMidiEvent>& _pendingMidi);

		Stats getStats() const;

	private:
		template<typename T>
		class SpscQueue
		{
		public:
			explicit SpscQueue(size_t _capacity) : m_items(roundUp(_capacity)), m_mask(m_items.size() - 1) {}

			size_t size() const { return m_write.load(std::memory_order_acquire) - m_read.load(std::memory_order_acquire); }
			size_t capacity() const { return m_items.size(); }
			size_t freeSpace() const { return capacity() - size(); }

			bool push(const T& _item)
			{
				const auto w = m_write.load(std::memory_order_relaxed);
				if(w - m_read.load(std::memory_order_acquire) >= m_items.size())
					return false;
				m_items[w & m_mask] = _item;
				m_write.store(w + 1, std::memory_order_release);
				return true;
			}

			T* front()
			{
				const auto r = m_read.load(std::memory_order_relaxed);
				if(r == m_write.load(std::memory_order_acquire))
					return nullptr;
				return &m_items[r & m_mask];
			}

			void pop() { m_read.store(m_read.load(std::memory_order_relaxed) + 1, std::memory_order_release); }

		private:
			static size_t roundUp(size_t _v)
			{
				size_t p = 1;
				while(p < _v)
					p <<= 1;
				return p;
			}

			std::vector<T> m_items;
			size_t m_mask;
			std::atomic<size_t> m_write{0};
			std::atomic<size_t> m_read{0};
		};

		// Interleaved sample ring for a fixed channel count, single producer and single consumer.
		class FrameRing
		{
		public:
			FrameRing(uint32_t _channels, size_t _frames);

			size_t available() const { return m_write.load(std::memory_order_acquire) - m_read.load(std::memory_order_acquire); }
			size_t freeFrames() const { return m_frames - available(); }

			// Channels with a null pointer write zeros.
			void write(const float* const* _channels, uint32_t _frames);
			void writeSilence(uint32_t _frames);
			// Channels with a null pointer are discarded.
			void read(float* const* _channels, uint32_t _frames);
			void discard(uint32_t _frames);

		private:
			const uint32_t m_channels;
			size_t m_frames;
			size_t m_mask;
			std::vector<float> m_data;
			std::atomic<size_t> m_write{0};
			std::atomic<size_t> m_read{0};
		};

		struct Chunk
		{
			uint32_t frames = 0;
			uint32_t midiCount = 0;
		};

		struct TimedMidi
		{
			synthLib::SMidiEvent event;
			uint64_t frame = 0;		// worker frame index
		};

		void threadFunc();
		bool lockMachine();

		std::recursive_timed_mutex& m_machineMutex;
		RenderChunk m_render;
		const uint32_t m_channelsIn;
		const uint32_t m_channelsOut;
		const uint32_t m_prefill;
		const float m_samplerate;

		SpscQueue<Chunk> m_chunks;
		SpscQueue<synthLib::SMidiEvent> m_midiIn;
		SpscQueue<TimedMidi> m_midiOut;
		FrameRing m_inputRing;
		FrameRing m_outputRing;

		// host thread only
		uint64_t m_hostFrames = 0;			// frames delivered to the host, including the prefill
		uint32_t m_skipFrames = 0;			// frames given up on in an underrun, discarded when they arrive
		std::vector<float> m_scratch;		// sink for outputs the host did not request

		// worker thread only
		uint64_t m_workerFrames = 0;
		std::vector<std::vector<float>> m_workerIn;
		std::vector<std::vector<float>> m_workerOut;
		std::vector<synthLib::SMidiEvent> m_workerMidiIn;
		std::vector<synthLib::SMidiEvent> m_workerMidiOut;
		std::vector<synthLib::SMidiEvent> m_inFlightMidi;	// popped for the chunk being rendered, for stop()

		dsp56k::SpscSemaphore m_wake;
		std::atomic<bool> m_exit{false};
		std::atomic<uint32_t> m_lastBlock{0};

		std::atomic<uint64_t> m_renderedFrames{0};
		std::atomic<uint64_t> m_underrunFrames{0};
		std::atomic<uint64_t> m_underrunCallbacks{0};
		std::atomic<uint64_t> m_droppedChunks{0};
		std::atomic<uint64_t> m_droppedMidiOut{0};

		std::thread m_thread;
	};
}
