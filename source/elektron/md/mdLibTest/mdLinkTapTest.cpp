// Diagnostic: boots a Machinedrum, starts the sequencer and records
//  - every word the producer DSP (DSP2) sends to the mixer DSP (DSP1) over ESSI0
//  - the six codec output channels
// so the link frame layout can be analysed offline (is there per-track audio before the mixer?).
//
//   mdLinkTapTest <firmware.bin> <factory cache> <out prefix> [seconds]
//
// Writes <prefix>_link.bin (int32 little endian, sign-extended 24-bit words, in transmit order),
// <prefix>_link_idx.bin (uint32: codec frame counter at each word) and <prefix>_out.bin
// (float32, 6 channels interleaved).

#include "mdLib/mdhardware.h"
#include "mdLib/mdpanel.h"
#include "mdLib/mdrom.h"
#include "mdLib/mdromloader.h"
#include "mdLib/mdstate.h"
#include "mdLib/mdtypes.h"

#include "baseLib/filesystem.h"
#include "dsp56kEmu/audio.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
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

	bool tap(md::Hardware& _hardware, const md::PanelControl _control)
	{
		const auto packet = md::panelPacket(md::MachineModel::Machinedrum, _control);
		if(!packet)
			return false;
		_hardware.sendPanelEvent(packet->row, packet->mask);
		advance(_hardware, 2048);
		_hardware.sendPanelEvent(packet->row, 0);
		advance(_hardware, 4096);
		return true;
	}

	int32_t signExtend24(const dsp56k::TWord _w)
	{
		return static_cast<int32_t>(_w << 8) >> 8;
	}
}

int main(const int argc, char* argv[])
{
	if(argc < 4)
	{
		std::cerr << "usage: mdLinkTapTest <firmware.bin> <factory cache|-> <out prefix> [seconds]\n";
		return 2;
	}
	const std::string firmwarePath = argv[1];
	const std::string cachePath = argv[2];
	const std::string prefix = argv[3];
	const uint32_t seconds = argc > 4 ? static_cast<uint32_t>(std::atoi(argv[4])) : 4;

	std::vector<uint8_t> rom;
	if(!baseLib::filesystem::readFile(rom, firmwarePath) || !md::RomLoader::isRomForModel(rom, md::MachineModel::Machinedrum))
	{
		std::cerr << "firmware image not accepted\n";
		return 1;
	}
	std::vector<uint8_t> cache, flash;
	if(cachePath != "-" && baseLib::filesystem::readFile(cache, cachePath))
	{
		if(!md::decodeFactoryFlashCache(flash, cache, rom))
		{
			std::cerr << "factory cache does not match this firmware, booting without it\n";
			cache.clear(); flash.clear();
		}
	}

	auto hardwareStorage = std::make_unique<md::Hardware>(rom, firmwarePath,
		md::MachineModel::Machinedrum, std::vector<uint8_t>{},
		std::shared_ptr<md::FrontPanelPublisher>{}, flash, cache);
	auto& hardware = *hardwareStorage;

	std::vector<int32_t> linkWords;
	std::vector<uint32_t> linkFrame;
	std::vector<uint32_t> slotCounts(64, 0);
	bool recording = false;
	uint32_t codecFrames = 0;
	hardware.setLinkTap([&](const uint32_t _dsp, const dsp56k::Audio::TxFrame& _frame)
	{
		if(_dsp != 1)
			return;
		const auto slots = _frame.size();
		if(slots < slotCounts.size())
			++slotCounts[slots];
		if(!recording)
			return;
		for(size_t i = 0; i < slots; ++i)
		{
			linkWords.push_back(signExtend24(_frame[i][0]));
			linkFrame.push_back(codecFrames);
		}
	});

	std::cerr << "booting...\n";
	advance(hardware, md::g_samplerate * 20);
	std::cerr << "audio ready " << hardware.isAudioReady() << " midi ready " << hardware.isFirmwareMidiReady() << "\n";
	// let the first-run flash initialisation settle when there is no cache
	advance(hardware, md::g_samplerate * 5);

	std::cerr << "PLAY\n";
	tap(hardware, md::PanelControl::Play);
	advance(hardware, md::g_samplerate / 4);

	// record: pull audio through processAudio so the codec output is captured too
	std::vector<float> out(6 * 256, 0.0f);
	std::vector<float> outAll;
	synthLib::TAudioOutputs outputs{};
	std::vector<std::vector<float>> chan(6, std::vector<float>(256, 0.0f));
	for(size_t c = 0; c < 6; ++c)
		outputs[c] = chan[c].data();

	recording = true;
	const uint32_t total = md::g_samplerate * seconds;
	for(uint32_t done = 0; done < total; done += 256)
	{
		hardware.processAudio(outputs, 256, 0);
		codecFrames += 256;
		for(uint32_t i = 0; i < 256; ++i)
			for(size_t c = 0; c < 6; ++c)
				outAll.push_back(chan[c][i]);
	}
	recording = false;
	tap(hardware, md::PanelControl::Stop);

	std::cerr << "link words recorded: " << linkWords.size() << " over " << codecFrames << " codec frames ("
		<< static_cast<double>(linkWords.size()) / codecFrames << " words/frame)\n";
	for(size_t i = 0; i < slotCounts.size(); ++i)
		if(slotCounts[i])
			std::cerr << "  frames with " << i << " slot(s): " << slotCounts[i] << "\n";

	std::ofstream(prefix + "_link.bin", std::ios::binary).write(reinterpret_cast<const char*>(linkWords.data()), static_cast<std::streamsize>(linkWords.size() * 4));
	std::ofstream(prefix + "_link_idx.bin", std::ios::binary).write(reinterpret_cast<const char*>(linkFrame.data()), static_cast<std::streamsize>(linkFrame.size() * 4));
	std::ofstream(prefix + "_out.bin", std::ios::binary).write(reinterpret_cast<const char*>(outAll.data()), static_cast<std::streamsize>(outAll.size() * 4));
	return 0;
}
