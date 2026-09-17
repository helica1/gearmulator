// Rendering ahead on a worker thread must not change what the machine does, only when the host hears it.
//
// Two Machinedrums run side by side through synthLib::Plugin with identical host blocks, audio input and
// MIDI, both with one block of extra MIDI latency. One renders synchronously, the other renders two blocks
// ahead. The render-ahead output must equal the synchronous output delayed by exactly the additional
// latency it reports, sample for sample, and the machine's MIDI output must match with the same delay.
// Access from outside the audio callback (withDeviceLocked) is exercised while the worker runs, and
// turning render ahead off must stop the worker cleanly.
//
// Needs GEARMULATOR_MD_FIRMWARE_BIN; GEARMULATOR_MD_FACTORY_CACHE makes the boot faster. Skips (77) without.

#include "mdLib/mddevice.h"
#include "mdLib/mdromloader.h"
#include "mdLib/mdtypes.h"

#include "synthLib/plugin.h"

#include "baseLib/filesystem.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <memory>
#include <string>
#include <vector>

namespace
{
	[[noreturn]] void fail(const std::string& _message)
	{
		std::fprintf(stderr, "FAILED: %s\n", _message.c_str());
		std::exit(1);
	}

	void require(const bool _condition, const std::string& _message)
	{
		if(!_condition)
			fail(_message);
	}

	constexpr uint32_t g_block = 256;
	constexpr uint32_t g_renderAheadFrames = 2 * g_block;
	constexpr uint32_t g_midiLatencyBlocks = 1;

	struct Instance
	{
		std::unique_ptr<md::Device> device;
		std::unique_ptr<synthLib::Plugin> plugin;
		std::vector<std::vector<float>> recorded = std::vector<std::vector<float>>(6);
		std::vector<std::pair<uint64_t, std::vector<uint8_t>>> midiOut;	// host frame, bytes
		std::vector<std::vector<float>> out = std::vector<std::vector<float>>(6, std::vector<float>(g_block));
	};

	void setRenderAhead(Instance& _instance, const uint32_t _frames)
	{
		_instance.plugin->withDeviceLocked([_frames](synthLib::Device* _device)
		{
			static_cast<md::Device*>(_device)->setRenderAheadFrames(_frames);
		});
		_instance.plugin->refreshDeviceLatency();
	}

	void create(Instance& _instance, const std::vector<uint8_t>& _rom, const std::string& _romName, const std::string& _home, const uint32_t _renderAheadFrames)
	{
		synthLib::DeviceCreateParams params;
		params.romData = _rom;
		params.romName = _romName;
		params.homePath = _home;
		params.customData = md::deviceCustomData(md::MachineModel::Machinedrum);
		_instance.device = std::make_unique<md::Device>(params);
		require(_instance.device->isValid(), "device not valid");
		_instance.plugin = std::make_unique<synthLib::Plugin>(_instance.device.get(), [](synthLib::Device*) {});
		_instance.plugin->reserveMidiEventCapacity();
		_instance.plugin->setHostSamplerate(44100.0f, 44100.0f);
		_instance.plugin->setBlockSize(g_block);
		_instance.plugin->setLatencyBlocks(g_midiLatencyBlocks);
		_instance.plugin->setNonRealtime(true);	// deterministic: the host waits for the worker
		setRenderAhead(_instance, _renderAheadFrames);
	}

	void run(Instance& _instance, const uint64_t _frame, const std::vector<synthLib::SMidiEvent>& _midi)
	{
		std::vector<float> left(g_block), right(g_block);
		for(uint32_t i = 0; i < g_block; ++i)
		{
			const auto t = static_cast<double>(_frame + i);
			left[i] = static_cast<float>(0.25 * std::sin(t * 0.021));
			right[i] = static_cast<float>(0.2 * std::cos(t * 0.017));
		}
		const synthLib::TAudioInputs inputs{left.data(), right.data(), nullptr, nullptr};
		synthLib::TAudioOutputs outputs{};
		for(size_t c = 0; c < 6; ++c)
			outputs[c] = _instance.out[c].data();

		for(const auto& ev : _midi)
			_instance.plugin->addMidiEvent(ev);
		_instance.plugin->process(inputs, outputs, g_block, 0.0f, 0.0f, false);

		for(size_t c = 0; c < 6; ++c)
			_instance.recorded[c].insert(_instance.recorded[c].end(), _instance.out[c].begin(), _instance.out[c].end());

		std::vector<synthLib::SMidiEvent> midiOut;
		_instance.plugin->getMidiOut(midiOut);
		for(const auto& ev : midiOut)
		{
			std::vector<uint8_t> bytes(ev.sysex.begin(), ev.sysex.end());
			if(bytes.empty())
				bytes = {ev.a, ev.b, ev.c};
			_instance.midiOut.emplace_back(_frame + ev.offset, std::move(bytes));
		}
	}
}

int main()
{
	const auto* const firmware = std::getenv("GEARMULATOR_MD_FIRMWARE_BIN");
	if(!firmware || !*firmware)
	{
		std::puts("GEARMULATOR_MD_FIRMWARE_BIN not set, skipping");
		return 77;
	}
	std::vector<uint8_t> rom;
	require(baseLib::filesystem::readFile(rom, firmware) && md::RomLoader::isRomForModel(rom, md::MachineModel::Machinedrum),
		"firmware not accepted");

	// private data folders, seeded with the factory cache when available
	char baseTemplate[] = "/tmp/mdRenderAheadTest-XXXXXX";
	require(mkdtemp(baseTemplate) != nullptr, "could not create a temporary folder");
	const std::string base = std::string(baseTemplate) + "/";
	std::vector<uint8_t> cacheBytes;
	if(const auto* const cache = std::getenv("GEARMULATOR_MD_FACTORY_CACHE"); cache && *cache)
		(void)baseLib::filesystem::readFile(cacheBytes, cache);
	std::string homes[2];
	for(int i = 0; i < 2; ++i)
	{
		homes[i] = base + (i ? "ahead/" : "sync/");
		require(baseLib::filesystem::createDirectory(homes[i] + "nvram"), "could not create a data folder");
		if(!cacheBytes.empty())
			require(baseLib::filesystem::writeFile(homes[i] + "nvram/md-uw-1.63-factory-v2.cache", cacheBytes), "could not seed the factory cache");
	}

	Instance sync, ahead;
	create(sync, rom, firmware, homes[0], 0);
	create(ahead, rom, firmware, homes[1], g_renderAheadFrames);

	require(!sync.device->isRenderingAhead(), "render ahead off must render synchronously");
	require(ahead.device->isRenderingAhead(), "render ahead on must use the worker");

	const auto syncLatency = sync.plugin->getLatencyMidiToOutput();
	const auto aheadLatency = ahead.plugin->getLatencyMidiToOutput();
	require(syncLatency == g_midiLatencyBlocks * g_block, "the MIDI latency setting must be unchanged");
	const auto prefill = aheadLatency - syncLatency;
	require(prefill == g_renderAheadFrames + md::Device::g_renderAheadSlackFrames,
		"additional reported latency " + std::to_string(prefill) + " does not match the render-ahead prefill");
	require(ahead.plugin->getLatencyInputToOutput() - sync.plugin->getLatencyInputToOutput() == prefill,
		"input latency report does not include the render-ahead prefill");

	constexpr uint64_t bootFrames = 44100ull * 30;
	constexpr uint64_t playFrames = 44100ull * 8;
	uint64_t frame = 0;
	uint32_t blockIndex = 0;
	std::vector<synthLib::SMidiEvent> midi;
	while(frame < bootFrames + playFrames)
	{
		midi.clear();
		if(frame >= bootFrames)
		{
			// pads 36..51 on the base channel at varying positions inside the block, released two blocks later
			const auto step = blockIndex % 6;
			const uint8_t note = static_cast<uint8_t>(36 + (blockIndex / 6) % 16);
			const auto offset = static_cast<uint32_t>((blockIndex * 37) % g_block);
			if(step == 0)
				midi.emplace_back(synthLib::MidiEventSource::Host, 0x90, note, 100, offset);
			else if(step == 2)
				midi.emplace_back(synthLib::MidiEventSource::Host, 0x80, note, 0, offset);
			else if(step == 4)
				midi.emplace_back(synthLib::MidiEventSource::Host, 0xB0, 1, static_cast<uint8_t>(blockIndex % 128), offset);
		}

		run(sync, frame, midi);
		run(ahead, frame, midi);

		// control-plane access while the worker renders
		if(blockIndex % 50 == 25)
		{
			for(auto* instance : {&sync, &ahead})
			{
				const auto bytes = instance->plugin->withDeviceLocked([](synthLib::Device* _device)
				{
					return static_cast<md::Device*>(_device)->getHardware().copyPatchRam().size();
				});
				require(bytes > 0, "patch RAM not readable under exclusive access");
			}
		}

		frame += g_block;
		++blockIndex;
	}

	const auto stats = ahead.device->getRenderAheadStats();
	std::printf("render ahead: prefill %u frames, rendered %llu frames, underruns %llu frames, dropped chunks %llu\n", prefill,
		static_cast<unsigned long long>(stats.renderedFrames), static_cast<unsigned long long>(stats.underrunFrames),
		static_cast<unsigned long long>(stats.droppedChunks));
	require(stats.underrunFrames == 0 && stats.droppedChunks == 0, "offline render ahead must not underrun or drop");

	// audio: ahead[t + prefill] == sync[t]
	size_t compared = 0, mismatches = 0, firstMismatch = 0;
	double peak = 0;
	for(size_t c = 0; c < 6; ++c)
	{
		const auto& s = sync.recorded[c];
		const auto& a = ahead.recorded[c];
		for(size_t t = 0; t + prefill < a.size() && t < s.size(); ++t)
		{
			peak = std::max(peak, static_cast<double>(std::abs(s[t])));
			++compared;
			if(s[t] != a[t + prefill])
			{
				if(!mismatches)
					firstMismatch = t;
				++mismatches;
			}
		}
		for(size_t t = 0; t < prefill && t < a.size(); ++t)
			require(a[t] == 0.0f, "render-ahead prefill must be silent");
	}
	std::printf("audio: %zu samples compared, peak %.3f, %zu mismatches (first at frame %zu)\n", compared, peak, mismatches, firstMismatch);
	require(peak > 0.01, "the machine produced no audio, the comparison would be meaningless");
	require(mismatches == 0, "render-ahead audio differs from the synchronous audio");

	// machine MIDI output: same bytes, delayed by the prefill (events still in flight at the end are ignored)
	const auto limit = frame - prefill;
	std::vector<std::pair<uint64_t, std::vector<uint8_t>>> expected, actual;
	for(const auto& e : sync.midiOut)
		if(e.first < limit)
			expected.push_back(e);
	for(const auto& e : ahead.midiOut)
		if(e.first >= prefill && e.first - prefill < limit)
			actual.emplace_back(e.first - prefill, e.second);
	std::printf("midi out: %zu events synchronous, %zu render ahead\n", expected.size(), actual.size());
	require(expected == actual, "render-ahead MIDI output differs from the synchronous MIDI output");

	// switching off stops the worker and keeps running synchronously
	setRenderAhead(ahead, 0);
	require(!ahead.device->isRenderingAhead(), "turning render ahead off must stop the worker");
	require(ahead.plugin->getLatencyMidiToOutput() == syncLatency, "latency report not restored");
	for(int i = 0; i < 20; ++i)
		run(ahead, frame + static_cast<uint64_t>(i) * g_block, {});

	ahead.plugin.reset();
	sync.plugin.reset();
	ahead.device.reset();
	sync.device.reset();
	(void)std::system(("rm -rf '" + base + "'").c_str());
	std::puts("mdRenderAheadTest passed");
	return 0;
}
