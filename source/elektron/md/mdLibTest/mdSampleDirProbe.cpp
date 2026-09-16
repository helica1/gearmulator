// Experiment: where does the Machinedrum UW keep its sample slot directory?
// Boots with the factory cache, uploads two samples with distinctive names into slots 5 and 6,
// then diffs flash and patch RAM and searches both for the names.
//
//   mdSampleDirProbe <firmware.bin> <factory cache> <out prefix>

#include "mdLib/mdhardware.h"
#include "mdLib/mdromloader.h"
#include "mdLib/mdsampledirectory.h"
#include "mdLib/mdsdsencode.h"
#include "mdLib/mdstate.h"
#include "mdLib/mdsysexfile.h"
#include "mdLib/mdsysextransfer.h"

#include "baseLib/filesystem.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace
{
	void advance(md::Hardware& _hardware, const uint32_t _frames)
	{
		for(uint32_t done = 0; done < _frames;)
		{
			const auto n = std::min<uint32_t>(64, _frames - done);
			_hardware.advance(n);
			done += n;
		}
	}

	void findAll(const std::vector<uint8_t>& _data, const std::string& _needle, const char* _label)
	{
		for(size_t i = 0; i + _needle.size() <= _data.size(); ++i)
			if(std::equal(_needle.begin(), _needle.end(), _data.begin() + static_cast<std::ptrdiff_t>(i)))
				std::printf("  %s: '%s' at 0x%zx\n", _label, _needle.c_str(), i);
	}

	void save(const std::string& _path, const std::vector<uint8_t>& _data)
	{
		std::ofstream(_path, std::ios::binary).write(reinterpret_cast<const char*>(_data.data()), static_cast<std::streamsize>(_data.size()));
	}
}

int main(const int argc, char* argv[])
{
	if(argc < 4)
		return 2;
	const std::string prefix = argv[3];
	std::vector<uint8_t> rom, cache, flash;
	if(!baseLib::filesystem::readFile(rom, argv[1]) || !baseLib::filesystem::readFile(cache, argv[2]) || !md::decodeFactoryFlashCache(flash, cache, rom))
		return 1;
	auto hardware = std::make_unique<md::Hardware>(rom, argv[1], md::MachineModel::Machinedrum, std::vector<uint8_t>{},
		std::shared_ptr<md::FrontPanelPublisher>{}, flash, cache);
	const auto bootDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(240);
	while(!hardware->isFirmwareMidiReady() && std::chrono::steady_clock::now() < bootDeadline)
		advance(*hardware, 64);
	advance(*hardware, md::g_samplerate * 20);
	std::printf("booted: %d\n", hardware->isFirmwareMidiReady());

	const auto printDirectory = [&](const char* _label)
	{
		const auto dir = md::sampleDirectory::read(*hardware);
		std::printf("directory %s: valid %d, free sectors %u of %u\n", _label, dir.valid, dir.freeSectors, dir.totalSectors);
		for(uint32_t i = 0; i < md::sampleDirectory::g_slotCount; ++i)
		{
			const auto& s = dir.slots[i];
			if(s.used)
				std::printf("  R%02u %-4s %6u frames %5u Hz %.2f s %u sectors\n", i + 1, s.name.c_str(), s.frames, s.sampleRate, s.seconds(), s.sectors);
			else
				std::printf("  R%02u empty\n", i + 1);
		}
	};
	printDirectory("before");
	const auto flashBefore = hardware->copyFlashData();
	const auto ramBefore = hardware->getUC().copyPatchRam();
	save(prefix + "_flash_before.bin", flashBefore);
	save(prefix + "_ram_before.bin", ramBefore);

	std::vector<int16_t> tone(3000);
	for(size_t i = 0; i < tone.size(); ++i)
		tone[i] = static_cast<int16_t>(std::lround(12000.0 * std::sin(i * 0.1)));
	md::sds::Options a; a.slot = 5; a.name = "QZXA"; a.sampleRateHz = 44100;
	md::sds::Options b; b.slot = 6; b.name = "QZXB"; b.sampleRateHz = 32000;
	auto stream = md::sds::encode(tone, a);
	std::vector<int16_t> tone2(tone.begin(), tone.begin() + 1777);
	const auto second = md::sds::encode(tone2, b);
	stream.insert(stream.end(), second.begin(), second.end());
	md::sds::Options c; c.slot = 32; c.name = "QZXC"; c.sampleRateHz = 44100;
	md::sds::Options d; d.slot = 39; d.name = "QZXD"; d.sampleRateHz = 44100;
	std::vector<int16_t> tone3(tone.begin(), tone.begin() + 999);
	const auto third = md::sds::encode(tone3, c);
	const auto fourth = md::sds::encode(tone2, d);
	stream.insert(stream.end(), third.begin(), third.end());
	stream.insert(stream.end(), fourth.begin(), fourth.end());

	auto prepared = md::prepareMidiSysexTransfer(stream, md::MachineModel::Machinedrum);
	if(!prepared || !hardware->startMidiSysexTransfer(*prepared))
		return 1;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(300);
	while(std::chrono::steady_clock::now() < deadline)
	{
		advance(*hardware, 64);
		const auto state = hardware->getMidiSysexTransferProgress().state;
		if(state == md::MidiSysexTransferState::Complete)
			break;
		if(state == md::MidiSysexTransferState::Failed || state == md::MidiSysexTransferState::Cancelled)
			return 1;
	}
	advance(*hardware, md::g_samplerate * 20);

	printDirectory("after");
	const auto flashAfter = hardware->copyFlashData();
	const auto ramAfter = hardware->getUC().copyPatchRam();
	save(prefix + "_flash_after.bin", flashAfter);
	save(prefix + "_ram_after.bin", ramAfter);

	std::printf("patch RAM size 0x%zx, flash size 0x%zx\n", ramAfter.size(), flashAfter.size());
	findAll(flashAfter, "QZXA", "flash");
	findAll(flashAfter, "QZXB", "flash");
	findAll(ramAfter, "QZXA", "ram");
	findAll(ramAfter, "QZXB", "ram");
	findAll(flashAfter, "QZXC", "flash");
	findAll(flashAfter, "QZXD", "flash");
	findAll(ramAfter, "QZXC", "ram");
	findAll(ramAfter, "QZXD", "ram");

	// changed regions
	for(const auto& [label, before, after] : {std::make_tuple("flash", &flashBefore, &flashAfter), std::make_tuple("ram", &ramBefore, &ramAfter)})
	{
		size_t i = 0;
		while(i < after->size())
		{
			if((*before)[i] == (*after)[i]) { ++i; continue; }
			size_t j = i;
			size_t same = 0;
			while(j < after->size() && same < 64) { if((*before)[j] == (*after)[j]) ++same; else same = 0; ++j; }
			std::printf("  %s changed 0x%zx..0x%zx (%zu bytes)\n", label, i, j - same, j - same - i);
			i = j;
		}
	}
	return 0;
}
