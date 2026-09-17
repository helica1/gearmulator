// Diagnostic: looks for the causes of audible clicks while a Machinedrum plays and all eight DATA ENTRY
// encoders are turned quickly, with and without FUNCTION held (CTRL-ALL). For each phase it reports the
// per-callback CPU time against the real-time budget, DSP JIT compilations inside callbacks, and (in a
// build with MD_TRANSPORT_DIAGNOSTICS) inter-DSP link drops and purges. Audio of each phase is written as
// float32 stereo for listening.
//
//   mdClickProbe <md|mm> <firmware.bin> <factory cache|patch ram|-> <out prefix> [seconds per phase] [block frames]

#include "mdLib/mdhardware.h"
#include "mdLib/mdpanel.h"
#include "mdLib/mdromloader.h"
#include "mdLib/mdstate.h"
#include "mdLib/mdtypes.h"

#include "synthLib/realtimeInstrumentation.h"

#include "baseLib/filesystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
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

	uint64_t linkLosses(const md::LinkDirectionScore& _s)
	{
		return (_s.dispositionTotal() - _s.acceptedFrames) + _s.purgedFrames();
	}

	enum class Phase { Baseline, Encoders, CtrlAll, After };

	const char* phaseName(const Phase _p)
	{
		switch(_p)
		{
		case Phase::Baseline: return "baseline (playing)";
		case Phase::Encoders: return "8 encoders turning";
		case Phase::CtrlAll: return "FUNCTION + 8 encoders (CTRL-ALL)";
		case Phase::After: return "baseline again";
		}
		return "";
	}
}

int main(const int _argc, char* _argv[])
{
	if(_argc < 5)
	{
		std::fprintf(stderr, "usage: mdClickProbe <md|mm> <firmware.bin> <factory cache|patch ram|-> <out prefix> [seconds per phase] [block frames]\n");
		return 2;
	}
	const auto model = std::string(_argv[1]) == "mm" ? md::MachineModel::Monomachine : md::MachineModel::Machinedrum;
	const std::string prefix = _argv[4];
	const uint32_t seconds = _argc > 5 ? static_cast<uint32_t>(std::atoi(_argv[5])) : 10;
	const uint32_t block = _argc > 6 ? static_cast<uint32_t>(std::atoi(_argv[6])) : 470;	// ~512 host frames at 48 kHz

	std::vector<uint8_t> rom, extra, cache, flash, patchRam;
	if(!baseLib::filesystem::readFile(rom, _argv[2]) || !md::RomLoader::isRomForModel(rom, model))
		return 1;
	if(std::string(_argv[3]) != "-" && baseLib::filesystem::readFile(extra, _argv[3]))
	{
		if(model == md::MachineModel::Monomachine)
			patchRam = extra;
		else if(md::decodeFactoryFlashCache(flash, extra, rom))
			cache = extra;
	}
	auto publisher = std::make_shared<md::FrontPanelPublisher>();
	auto hardware = std::make_unique<md::Hardware>(rom, _argv[2], model, patchRam, publisher, flash, cache);

	advance(*hardware, md::g_samplerate * 25);
	tap(*hardware, model, md::PanelControl::Play);
	advance(*hardware, md::g_samplerate * 3);

	synthLib::RealtimeInstrumentation instrumentation;
	instrumentation.setEnabled(true);

	std::vector<std::vector<float>> chan(6, std::vector<float>(block, 0.0f));
	synthLib::TAudioOutputs outputs{};
	for(size_t c = 0; c < 6; ++c)
		outputs[c] = chan[c].data();

	const double budgetMs = 1000.0 * block / md::g_samplerate;
	const auto functionPacket = md::panelPacket(model, md::PanelControl::Function);

	std::printf("block %u frames, real-time budget %.2f ms, transport diagnostics %s\n", block, budgetMs,
		hardware->getTransportScorecard().enabled ? "on" : "off");
	std::printf("%-34s %8s %8s %8s %6s %6s %6s %9s %10s %8s %8s\n", "phase", "mean ms", "p99 ms", "max ms", ">50%", ">80%", ">100%",
		"JIT comp", "cb w/ JIT", "link0", "link1");

	for(const auto phase : {Phase::Baseline, Phase::Encoders, Phase::CtrlAll, Phase::After})
	{
		if(phase == Phase::CtrlAll && functionPacket)
		{
			hardware->sendPanelEvent(functionPacket->row, functionPacket->mask);
			advance(*hardware, 2048);
		}

		const auto scoreBefore = hardware->getTransportScorecard();
		const auto instBefore = instrumentation.snapshot();
		std::vector<double> ms;
		std::vector<float> audio;
		uint32_t over50 = 0, over80 = 0, over100 = 0;
		const uint32_t total = md::g_samplerate * seconds;
		uint32_t blockIndex = 0;
		for(uint32_t done = 0; done < total; done += block, ++blockIndex)
		{
			if(phase == Phase::Encoders || phase == Phase::CtrlAll)
			{
				// one detent on every encoder per callback, direction flips every half second
				const bool up = ((done / (md::g_samplerate / 2)) & 1) == 0;
				for(uint8_t e = 0; e < 8; ++e)
				{
					if(const auto cmd = md::panelEncoderCommand(model, static_cast<md::PanelEncoder>(e)))
						hardware->sendPanelEvent(*cmd, up ? 0x01 : 0xff);
				}
			}
			const auto t0 = threadCpuSeconds();
			{
				synthLib::RealtimeInstrumentation::CallbackScope scope(instrumentation, block, md::g_samplerate);
				hardware->processAudio(outputs, block, 0);
			}
			const auto v = (threadCpuSeconds() - t0) * 1000.0;
			ms.push_back(v);
			if(v > budgetMs * 0.5) ++over50;
			if(v > budgetMs * 0.8) ++over80;
			if(v > budgetMs) ++over100;
			for(uint32_t i = 0; i < block; ++i)
			{
				audio.push_back(chan[0][i]);
				audio.push_back(chan[1][i]);
			}
		}

		if(phase == Phase::CtrlAll && functionPacket)
		{
			hardware->sendPanelEvent(functionPacket->row, 0);
			advance(*hardware, 2048);
		}

		const auto instAfter = instrumentation.snapshot();
		const auto scoreAfter = hardware->getTransportScorecard();
		double sum = 0;
		for(const auto v : ms)
			sum += v;
		const auto maxMs = *std::max_element(ms.begin(), ms.end());
		auto sorted = ms;
		std::sort(sorted.begin(), sorted.end());
		const auto p99 = sorted[sorted.size() * 99 / 100];

		std::printf("%-34s %8.3f %8.3f %8.3f %6u %6u %6u %9llu %10llu %8llu %8llu\n", phaseName(phase),
			sum / static_cast<double>(ms.size()), p99, maxMs, over50, over80, over100,
			static_cast<unsigned long long>(instAfter.jitCompilationCount - instBefore.jitCompilationCount),
			static_cast<unsigned long long>(instAfter.callbacksWithJitCompilation - instBefore.callbacksWithJitCompilation),
			static_cast<unsigned long long>(linkLosses(scoreAfter.link[0]) - linkLosses(scoreBefore.link[0])),
			static_cast<unsigned long long>(linkLosses(scoreAfter.link[1]) - linkLosses(scoreBefore.link[1])));
		if(scoreAfter.enabled)
		{
			for(size_t d = 0; d < 2; ++d)
			{
				const auto& a = scoreAfter.link[d];
				const auto& b = scoreBefore.link[d];
				std::printf("    link[%zu] transmit %llu accepted %llu emptyReads %llu stallPurged %llu windowPurged %llu overrun %llu postFlush %llu ringFull %llu\n", d,
					static_cast<unsigned long long>(a.transmitFrames - b.transmitFrames),
					static_cast<unsigned long long>(a.acceptedFrames - b.acceptedFrames),
					static_cast<unsigned long long>(a.emptyReads - b.emptyReads),
					static_cast<unsigned long long>(a.stallPurgedFrames - b.stallPurgedFrames),
					static_cast<unsigned long long>(a.mdWindowPurgedFrames - b.mdWindowPurgedFrames),
					static_cast<unsigned long long>(a.mdReceiverOverrunDrops - b.mdReceiverOverrunDrops),
					static_cast<unsigned long long>(a.mdPostFlushRetainedDrops - b.mdPostFlushRetainedDrops),
					static_cast<unsigned long long>((a.ringFullDrops + a.mdRendezvousRingFullDrops) - (b.ringFullDrops + b.mdRendezvousRingFullDrops)));
			}
		}
		std::fflush(stdout);

		const auto file = prefix + "_" + std::to_string(static_cast<int>(phase)) + ".f32";
		std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(audio.data()), static_cast<std::streamsize>(audio.size() * sizeof(float)));
	}
	std::printf("host audio overflow %llu\n", static_cast<unsigned long long>(hardware->hostAudioOverflowCount()));
	return 0;
}
