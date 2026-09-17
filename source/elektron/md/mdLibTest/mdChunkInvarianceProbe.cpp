// Diagnostic: does the emulation produce the same audio regardless of how the host splits rendering into
// blocks? Boots two identical machines, starts the sequencer on both at the same machine frame, renders
// one in blocks of A frames and the other in blocks of B frames, and compares all six outputs.
//
//   mdChunkInvarianceProbe <md|mm> <firmware.bin> <factory cache|-> <block A> <block B> [seconds]

#include "mdLib/mdhardware.h"
#include "mdLib/mdpanel.h"
#include "mdLib/mdromloader.h"
#include "mdLib/mdstate.h"
#include "mdLib/mdtypes.h"

#include "baseLib/filesystem.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace
{
	std::vector<float> render(const std::vector<uint8_t>& _rom, const char* _romName, const md::MachineModel _model,
		const std::vector<uint8_t>& _flash, const std::vector<uint8_t>& _cache, const uint32_t _block, const uint32_t _seconds)
	{
		auto publisher = std::make_shared<md::FrontPanelPublisher>();
		auto hw = std::make_unique<md::Hardware>(_rom, _romName, _model, std::vector<uint8_t>{}, publisher, _flash, _cache);

		std::vector<std::vector<float>> chan(6, std::vector<float>(_block, 0.0f));
		synthLib::TAudioOutputs outputs{};
		for(size_t c = 0; c < 6; ++c)
			outputs[c] = chan[c].data();

		std::vector<float> out;
		const uint32_t boot = md::g_samplerate * 25;
		const uint32_t total = boot + md::g_samplerate * _seconds;
		const auto play = md::panelPacket(_model, md::PanelControl::Play);
		for(uint32_t done = 0; done < total; done += _block)
		{
			// press PLAY at a fixed machine frame, independent of the block size
			if(play && done <= boot && boot < done + _block)
				hw->sendPanelEvent(play->row, play->mask);
			if(play && done <= boot + 4096 && boot + 4096 < done + _block)
				hw->sendPanelEvent(play->row, 0);
			const auto n = std::min(_block, total - done);
			hw->processAudio(outputs, n, 0);
			if(done >= boot)
				for(uint32_t i = 0; i < n; ++i)
					for(size_t c = 0; c < 6; ++c)
						out.push_back(chan[c][i]);
		}
		return out;
	}
}

int main(const int _argc, char* _argv[])
{
	if(_argc < 6)
		return 2;
	const auto model = std::string(_argv[1]) == "mm" ? md::MachineModel::Monomachine : md::MachineModel::Machinedrum;
	std::vector<uint8_t> rom, cache, flash;
	if(!baseLib::filesystem::readFile(rom, _argv[2]) || !md::RomLoader::isRomForModel(rom, model))
		return 1;
	if(std::string(_argv[3]) != "-" && baseLib::filesystem::readFile(cache, _argv[3]) && !md::decodeFactoryFlashCache(flash, cache, rom))
		cache.clear(), flash.clear();
	const auto a = static_cast<uint32_t>(std::atoi(_argv[4]));
	const auto b = static_cast<uint32_t>(std::atoi(_argv[5]));
	const uint32_t seconds = _argc > 6 ? static_cast<uint32_t>(std::atoi(_argv[6])) : 5;

	const auto ra = render(rom, _argv[2], model, flash, cache, a, seconds);
	const auto rb = render(rom, _argv[2], model, flash, cache, b, seconds);
	const auto n = std::min(ra.size(), rb.size());
	size_t diffs = 0, first = n;
	double peak = 0, maxDiff = 0;
	for(size_t i = 0; i < n; ++i)
	{
		peak = std::max(peak, static_cast<double>(std::abs(ra[i])));
		const auto d = std::abs(static_cast<double>(ra[i]) - rb[i]);
		if(d > 0)
		{
			if(first == n) first = i;
			++diffs;
			maxDiff = std::max(maxDiff, d);
		}
	}
	std::printf("blocks %u vs %u: %zu samples compared, %zu differ, first at frame %zu, max difference %.6f, peak %.3f\n",
		a, b, n, diffs, first == n ? static_cast<size_t>(0) : first / 6, maxDiff, peak);
	return diffs == 0 ? 0 : 3;
}
