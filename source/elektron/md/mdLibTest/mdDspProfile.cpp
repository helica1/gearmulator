// Diagnostic: statistical PC profile of both DSPs and the UC while a Machinedrum or Monomachine runs.
// Samples the program counters after every emulated codec frame and prints the hottest addresses of each
// DSP with a disassembly around them, to find idle loops that execute without doing work.
//
//   mdDspProfile <md|mm> <firmware.bin> <factory cache|patch ram|-> [seconds] [play 0|1]

#include "mdLib/mdhardware.h"
#include "mdLib/mdpanel.h"
#include "mdLib/mdromloader.h"
#include "mdLib/mdstate.h"
#include "mdLib/mdtypes.h"

#include "dsp56kEmu/disasm.h"

#include "baseLib/filesystem.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace
{
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

	void report(const char* _name, dsp56k::DSP& _dsp, const std::map<uint32_t, uint64_t>& _hist, const uint64_t _total)
	{
		std::vector<std::pair<uint32_t, uint64_t>> sorted(_hist.begin(), _hist.end());
		std::sort(sorted.begin(), sorted.end(), [](const auto& _a, const auto& _b) { return _a.second > _b.second; });

		// group samples into 64-word windows to show code regions
		std::map<uint32_t, uint64_t> regions;
		for(const auto& [pc, n] : _hist)
			regions[pc & ~0x3fu] += n;
		std::vector<std::pair<uint32_t, uint64_t>> sortedRegions(regions.begin(), regions.end());
		std::sort(sortedRegions.begin(), sortedRegions.end(), [](const auto& _a, const auto& _b) { return _a.second > _b.second; });

		std::printf("\n===== %s: %llu samples, %zu distinct PCs\n", _name, static_cast<unsigned long long>(_total), _hist.size());
		std::printf("hottest 64-word regions:\n");
		for(size_t i = 0; i < std::min<size_t>(10, sortedRegions.size()); ++i)
			std::printf("  %06x-%06x %5.1f%%\n", sortedRegions[i].first, sortedRegions[i].first + 63, 100.0 * static_cast<double>(sortedRegions[i].second) / static_cast<double>(_total));
		std::printf("hottest PCs:\n");
		for(size_t i = 0; i < std::min<size_t>(16, sorted.size()); ++i)
			std::printf("  %06x %5.1f%%\n", sorted[i].first, 100.0 * static_cast<double>(sorted[i].second) / static_cast<double>(_total));

		dsp56k::Disassembler disasm(_dsp.opcodes());
		const auto& mem = _dsp.memory();
		for(size_t i = 0; i < std::min<size_t>(4, sortedRegions.size()); ++i)
		{
			const auto start = sortedRegions[i].first;
			std::printf("disassembly of region %06x (sample counts per PC on the left):\n", start);
			for(uint32_t pc = start; pc < start + 64;)
			{
				std::string line;
				const auto op = mem.get(dsp56k::MemArea_P, pc);
				const auto opB = mem.get(dsp56k::MemArea_P, pc + 1);
				auto len = disasm.disassemble(line, op, opB, 0, 0, pc);
				if(len == 0)
					len = 1;
				const auto it = _hist.find(pc);
				const auto count = it == _hist.end() ? 0ull : static_cast<unsigned long long>(it->second);
				std::printf("  %8llu  %06x  %06x  %s\n", count, pc, op, line.c_str());
				pc += len;
			}
		}
	}
}

int main(const int _argc, char* _argv[])
{
	if(_argc < 4)
	{
		std::fprintf(stderr, "usage: mdDspProfile <md|mm> <firmware.bin> <factory cache|patch ram|-> [seconds] [play 0|1]\n");
		return 2;
	}
	const auto model = std::string(_argv[1]) == "mm" ? md::MachineModel::Monomachine : md::MachineModel::Machinedrum;
	const uint32_t seconds = _argc > 4 ? static_cast<uint32_t>(std::atoi(_argv[4])) : 10;
	const bool play = _argc > 5 ? std::atoi(_argv[5]) != 0 : true;

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
	if(play)
		tap(*hardware, model, md::PanelControl::Play);
	advance(*hardware, md::g_samplerate * 2);

	std::map<uint32_t, uint64_t> hist[2];
	uint64_t total = 0;
	auto& mixer = hardware->getDspMixer().dsp();
	auto& producer = hardware->getDspProducer().dsp();
	const uint64_t mixerCycles0 = mixer.getCycles();
	const uint64_t producerCycles0 = producer.getCycles();
	const auto frames = md::g_samplerate * seconds;
	for(uint32_t f = 0; f < frames; ++f)
	{
		hardware->advance(1);
		++hist[0][mixer.getPC().toWord()];
		++hist[1][producer.getPC().toWord()];
		++total;
	}
	std::printf("cycles per frame: mixer %.1f, producer %.1f\n",
		static_cast<double>(mixer.getCycles() - mixerCycles0) / frames,
		static_cast<double>(producer.getCycles() - producerCycles0) / frames);
	report("DSP1 mixer", mixer, hist[0], total);
	report("DSP2 producer", producer, hist[1], total);
	return 0;
}
