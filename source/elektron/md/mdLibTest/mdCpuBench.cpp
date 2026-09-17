// CPU cost of the emulation core: boots a Machinedrum or Monomachine, then measures the thread CPU time
// needed to render idle audio and a playing pattern, in percent of one core at real time.
// Uses only md::Hardware API that also exists in the base version, so both builds can be compared.
//
//   mdCpuBench <md|mm> <firmware.bin> <factory cache|patch ram|-> [seconds] [block frames]

#include "mdLib/mdhardware.h"
#include "mdLib/mdpanel.h"
#include "mdLib/mdromloader.h"
#include "mdLib/mdstate.h"
#include "mdLib/mdtypes.h"

#include "baseLib/filesystem.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

namespace
{
	double threadCpuSeconds()
	{
		timespec ts{};
		clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
		return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
	}

	void advance(md::Hardware& _hardware, const uint32_t _frames)
	{
		constexpr uint32_t block = 128;
		for(uint32_t frames = 0; frames < _frames; frames += block)
			_hardware.advance(std::min(block, _frames - frames));
	}

	void tap(md::Hardware& _hardware, const md::MachineModel _model, const md::PanelControl _control)
	{
		const auto packet = md::panelPacket(_model, _control);
		if(!packet)
			return;
		_hardware.sendPanelEvent(packet->row, packet->mask);
		advance(_hardware, 2048);
		_hardware.sendPanelEvent(packet->row, 0);
		advance(_hardware, 4096);
	}

	struct Result
	{
		double cpuPercent = 0;
		double meanBlockMs = 0;
		double maxBlockMs = 0;
		double p99BlockMs = 0;
	};

	Result measure(md::Hardware& _hardware, const uint32_t _seconds, const uint32_t _block)
	{
		std::vector<std::vector<float>> chan(6, std::vector<float>(_block, 0.0f));
		synthLib::TAudioOutputs outputs{};
		for(size_t c = 0; c < 6; ++c)
			outputs[c] = chan[c].data();

		std::vector<double> blockMs;
		const uint32_t total = md::g_samplerate * _seconds;
		blockMs.reserve(total / _block + 1);
		const auto cpu0 = threadCpuSeconds();
		for(uint32_t done = 0; done < total; done += _block)
		{
			const auto t0 = threadCpuSeconds();
			_hardware.processAudio(outputs, _block, 0);
			blockMs.push_back((threadCpuSeconds() - t0) * 1000.0);
		}
		const auto cpu = threadCpuSeconds() - cpu0;

		Result r;
		r.cpuPercent = 100.0 * cpu / static_cast<double>(_seconds);
		double sum = 0;
		for(const auto v : blockMs)
			sum += v;
		r.meanBlockMs = sum / static_cast<double>(blockMs.size());
		std::sort(blockMs.begin(), blockMs.end());
		r.maxBlockMs = blockMs.back();
		r.p99BlockMs = blockMs[blockMs.size() * 99 / 100];
		return r;
	}
}

int main(const int _argc, char* _argv[])
{
	if(_argc < 4)
	{
		std::fprintf(stderr, "usage: mdCpuBench <md|mm> <firmware.bin> <factory cache|patch ram|-> [seconds] [block frames]\n");
		return 2;
	}
	const auto model = std::string(_argv[1]) == "mm" ? md::MachineModel::Monomachine : md::MachineModel::Machinedrum;
	const uint32_t seconds = _argc > 4 ? static_cast<uint32_t>(std::atoi(_argv[4])) : 20;
	const uint32_t block = _argc > 5 ? static_cast<uint32_t>(std::atoi(_argv[5])) : 256;

	std::vector<uint8_t> rom, extra, cache, flash, patchRam;
	if(!baseLib::filesystem::readFile(rom, _argv[2]) || !md::RomLoader::isRomForModel(rom, model))
	{
		std::fprintf(stderr, "firmware image not accepted\n");
		return 1;
	}
	if(std::string(_argv[3]) != "-" && baseLib::filesystem::readFile(extra, _argv[3]))
	{
		if(model == md::MachineModel::Monomachine)
			patchRam = extra;
		else if(md::decodeFactoryFlashCache(flash, extra, rom))
			cache = extra;
		else
			std::fprintf(stderr, "factory cache does not match, booting without it\n");
	}

	auto publisher = std::make_shared<md::FrontPanelPublisher>();
	auto hardware = std::make_unique<md::Hardware>(rom, _argv[2], model, patchRam, publisher, flash, cache);

	const auto bootCpu0 = threadCpuSeconds();
	advance(*hardware, md::g_samplerate * 25);
	std::printf("boot 25 s emulated: %.2f s cpu, midi ready %d\n", threadCpuSeconds() - bootCpu0, hardware->isFirmwareMidiReady() ? 1 : 0);

	// warm up the JIT on the idle code path, then measure idle
	measure(*hardware, 3, block);
	const auto idle = measure(*hardware, seconds, block);

	tap(*hardware, model, md::PanelControl::Play);
	measure(*hardware, 3, block);
	const auto play = measure(*hardware, seconds, block);
	tap(*hardware, model, md::PanelControl::Stop);

	const double budgetMs = 1000.0 * block / md::g_samplerate;
	const auto print = [&](const char* _name, const Result& _r)
	{
		std::printf("%-5s cpu %.1f%% of one core | block %u frames (budget %.2f ms): mean %.3f ms, p99 %.3f ms, max %.3f ms\n",
			_name, _r.cpuPercent, block, budgetMs, _r.meanBlockMs, _r.p99BlockMs, _r.maxBlockMs);
	};
	print("idle", idle);
	print("play", play);
	return 0;
}
