// Diagnostic: host callback timing with and without render ahead, paced like a real-time host.
//
// Runs a Machinedrum through synthLib::Plugin at a host rate and block size (default 48 kHz / 512, as in
// Bitwig), calling process() at wall-clock block intervals while the sequencer plays. Reports how long the
// callback takes against its budget and, with render ahead, how many frames the worker failed to deliver.
//
//   mdRenderAheadRealtimeProbe <firmware.bin> <factory cache|-> [render ahead blocks] [seconds] [host rate] [block]

#include "mdLib/mddevice.h"
#include "mdLib/mdpanel.h"
#include "mdLib/mdromloader.h"
#include "mdLib/mdtypes.h"

#include "synthLib/plugin.h"

#include "baseLib/filesystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

int main(const int _argc, char* _argv[])
{
	if(_argc < 3)
	{
		std::fprintf(stderr, "usage: mdRenderAheadRealtimeProbe <firmware.bin> <factory cache|-> [blocks] [seconds] [host rate] [block]\n");
		return 2;
	}
	const uint32_t aheadBlocks = _argc > 3 ? static_cast<uint32_t>(std::atoi(_argv[3])) : 0;
	const uint32_t seconds = _argc > 4 ? static_cast<uint32_t>(std::atoi(_argv[4])) : 20;
	const float hostRate = _argc > 5 ? static_cast<float>(std::atof(_argv[5])) : 48000.0f;
	const uint32_t block = _argc > 6 ? static_cast<uint32_t>(std::atoi(_argv[6])) : 512;

	std::vector<uint8_t> rom;
	if(!baseLib::filesystem::readFile(rom, _argv[1]) || !md::RomLoader::isRomForModel(rom, md::MachineModel::Machinedrum))
		return 1;

	char home[] = "/tmp/mdRenderAheadRealtimeProbe-XXXXXX";
	if(!mkdtemp(home))
		return 1;
	const std::string homePath = std::string(home) + "/";
	baseLib::filesystem::createDirectory(homePath + "nvram");
	if(std::string(_argv[2]) != "-")
	{
		std::vector<uint8_t> cache;
		if(baseLib::filesystem::readFile(cache, _argv[2]))
			baseLib::filesystem::writeFile(homePath + "nvram/md-uw-1.63-factory-v2.cache", cache);
	}

	synthLib::DeviceCreateParams params;
	params.romData = rom;
	params.romName = _argv[1];
	params.homePath = homePath;
	params.customData = md::deviceCustomData(md::MachineModel::Machinedrum);
	auto device = std::make_unique<md::Device>(params);
	synthLib::Plugin plugin(device.get(), [](synthLib::Device*) {});
	plugin.reserveMidiEventCapacity();
	plugin.setHostSamplerate(hostRate, 44100.0f);
	plugin.setBlockSize(block);

	std::vector<std::vector<float>> out(6, std::vector<float>(block));
	std::vector<float> silence(block, 0.0f);
	const synthLib::TAudioInputs inputs{silence.data(), silence.data(), nullptr, nullptr};
	synthLib::TAudioOutputs outputs{};
	for(size_t c = 0; c < 6; ++c)
		outputs[c] = out[c].data();

	// boot and start the sequencer offline (fast), then switch render ahead on and play in real time
	plugin.setNonRealtime(true);
	const auto blocksPerSecond = static_cast<uint32_t>(hostRate / static_cast<float>(block)) + 1;
	for(uint32_t i = 0; i < blocksPerSecond * 25; ++i)
		plugin.process(inputs, outputs, block, 0.0f, 0.0f, false);
	const auto play = md::panelPacket(md::MachineModel::Machinedrum, md::PanelControl::Play);
	plugin.withDeviceLocked([&](synthLib::Device*) { device->sendPanelEvent(play->row, play->mask); });
	for(uint32_t i = 0; i < 4; ++i)
		plugin.process(inputs, outputs, block, 0.0f, 0.0f, false);
	plugin.withDeviceLocked([&](synthLib::Device*) { device->sendPanelEvent(play->row, 0); });

	const auto frames = static_cast<uint32_t>(std::ceil(static_cast<double>(aheadBlocks) * block * 44100.0 / hostRate));
	plugin.withDeviceLocked([&](synthLib::Device*) { device->setRenderAheadFrames(frames); });
	plugin.refreshDeviceLatency();
	plugin.setNonRealtime(false);

	using Clock = std::chrono::steady_clock;
	const auto period = std::chrono::duration<double>(static_cast<double>(block) / hostRate);
	const double budgetMs = period.count() * 1000.0;
	std::vector<double> ms;
	auto next = Clock::now();
	const auto total = blocksPerSecond * seconds;
	for(uint32_t i = 0; i < total; ++i)
	{
		next += std::chrono::duration_cast<Clock::duration>(period);
		const auto t0 = Clock::now();
		plugin.process(inputs, outputs, block, 0.0f, 0.0f, false);
		ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
		std::this_thread::sleep_until(next);
	}

	double sum = 0;
	uint32_t over50 = 0, over100 = 0;
	for(const auto v : ms)
	{
		sum += v;
		if(v > budgetMs * 0.5) ++over50;
		if(v > budgetMs) ++over100;
	}
	auto sorted = ms;
	std::sort(sorted.begin(), sorted.end());
	const auto stats = device->getRenderAheadStats();
	std::printf("render ahead %u blocks (%u frames, reported latency %u samples) | %u callbacks of %u samples at %.0f Hz, budget %.2f ms\n",
		aheadBlocks, frames, plugin.getLatencyMidiToOutput(), total, block, hostRate, budgetMs);
	std::printf("  callback: mean %.3f ms, p99 %.3f ms, max %.3f ms, over half budget %u, over budget %u\n",
		sum / static_cast<double>(ms.size()), sorted[sorted.size() * 99 / 100], sorted.back(), over50, over100);
	if(aheadBlocks)
		std::printf("  worker: rendered %llu frames, missing %llu frames in %llu callbacks\n",
			static_cast<unsigned long long>(stats.renderedFrames), static_cast<unsigned long long>(stats.underrunFrames),
			static_cast<unsigned long long>(stats.underrunCallbacks));

	device->setRenderAheadFrames(0);
	(void)std::system(("rm -rf '" + homePath + "'").c_str());
	return 0;
}
