// Verifies the machine selector protocol against the firmware: the current track status (0x70 0x22),
// selecting a track, and ASSIGN MACHINE (0x5B), observed through kit dumps before and after.
//
//   mdMachineAssignProbe <md|mm> <firmware.bin> <factory cache|->

#include "mdLib/mdhardware.h"
#include "mdLib/mdmachines.h"
#include "mdLib/mdromloader.h"
#include "mdLib/mdstate.h"

#include "baseLib/filesystem.h"

#include <chrono>
#include <cstdio>
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

	std::vector<uint8_t> query(md::Hardware& _hardware, const std::vector<uint8_t>& _request, const uint8_t _replyCommand)
	{
		std::vector<synthLib::SMidiEvent> events;
		_hardware.readMidiOut(events);
		synthLib::SMidiEvent ev(synthLib::MidiEventSource::Host);
		ev.sysex.assign(_request.begin(), _request.end());
		_hardware.sendMidi(ev);
		for(int i = 0; i < 40; ++i)
		{
			advance(_hardware, md::g_samplerate / 20);
			events.clear();
			_hardware.readMidiOut(events);
			for(const auto& e : events)
				if(e.sysex.size() > 7 && e.sysex[6] == _replyCommand)
					return {e.sysex.begin(), e.sysex.end()};
		}
		return {};
	}

	void send(md::Hardware& _hardware, const std::vector<uint8_t>& _sysex)
	{
		synthLib::SMidiEvent ev(synthLib::MidiEventSource::Host);
		ev.sysex.assign(_sysex.begin(), _sysex.end());
		_hardware.sendMidi(ev);
		advance(_hardware, md::g_samplerate / 2);
	}
}

int main(const int argc, char* argv[])
{
	if(argc < 4)
		return 2;
	const auto model = std::string(argv[1]) == "mm" ? md::MachineModel::Monomachine : md::MachineModel::Machinedrum;
	const uint8_t product = model == md::MachineModel::Monomachine ? 3 : 2;
	std::vector<uint8_t> rom, cache, flash;
	if(!baseLib::filesystem::readFile(rom, argv[2]) || !md::RomLoader::isRomForModel(rom, model))
		return 1;
	if(std::string(argv[3]) != "-" && baseLib::filesystem::readFile(cache, argv[3]) && !md::decodeFactoryFlashCache(flash, cache, rom))
		cache.clear(), flash.clear();
	auto publisher = std::make_shared<md::FrontPanelPublisher>();
	auto hardware = std::make_unique<md::Hardware>(rom, argv[2], model, std::vector<uint8_t>{}, publisher, flash, cache);
	const auto lcd = [&]
	{
		const auto panel = hardware->getFrontPanelSnapshot();
		std::vector<std::string> rows;
		for(uint32_t y = 0; y < 64; y += 2)
		{
			std::string row;
			for(uint32_t x = 0; x < 128; ++x)
				row.push_back(panel.getLcdPixel(x, y) || panel.getLcdPixel(x, y + 1) ? '#' : ' ');
			rows.push_back(row);
		}
		return rows;
	};
	const auto printLcd = [&](const char* _label, const std::vector<std::string>& _rows)
	{
		std::printf("LCD %s\n", _label);
		for(const auto& r : _rows)
			std::printf("  |%s|\n", r.c_str());
	};
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(240);
	while(!hardware->isFirmwareMidiReady() && std::chrono::steady_clock::now() < deadline)
		advance(*hardware, 64);
	advance(*hardware, md::g_samplerate * 20);
	std::printf("ready %d\n", hardware->isFirmwareMidiReady());

	const auto trackReply = [&]
	{
		const auto r = query(*hardware, md::machines::currentTrackRequest(model), 0x72);
		return r.size() >= 10 && r[7] == 0x22 ? static_cast<int>(r[8]) : -1;
	};
	std::printf("current track: %d\n", trackReply());

	// select track 3 (SET STATUS 0x71 0x22)
	send(*hardware, {0xf0, 0x00, 0x20, 0x3c, product, 0x00, 0x71, 0x22, 0x03, 0xf7});
	const auto track = trackReply();
	std::printf("after selecting track 4: current track %d\n", track);

	const auto lcdBefore = lcd();
	const std::vector<uint8_t> kitRequest{0xf0, 0x00, 0x20, 0x3c, product, 0x00, 0x53, 0x00, 0xf7};
	const auto before = query(*hardware, kitRequest, 0x52);
	const uint16_t machine = model == md::MachineModel::Monomachine ? 8 : 33;	// FM+-STAT / EFM-SD
	send(*hardware, md::machines::assignMachine(model, static_cast<uint8_t>(track < 0 ? 3 : track), machine));
	const auto after = query(*hardware, kitRequest, 0x52);
	std::printf("kit dump sizes %zu / %zu\n", before.size(), after.size());
	const auto lcdAfter = lcd();
	size_t changedRows = 0;
	for(size_t i = 0; i < lcdBefore.size(); ++i)
		if(lcdBefore[i] != lcdAfter[i]) ++changedRows;
	std::printf("LCD rows changed by assign: %zu\n", changedRows);
	printLcd("before", lcdBefore);
	printLcd("after", lcdAfter);
	size_t diffs = 0;
	for(size_t i = 0; i < std::min(before.size(), after.size()); ++i)
	{
		if(before[i] == after[i])
			continue;
		if(diffs++ < 24)
			std::printf("  kit byte 0x%03zx: %02x -> %02x\n", i, before[i], after[i]);
	}
	std::printf("differing kit bytes: %zu\n", diffs);

	// a UW machine on the Machinedrum: ROM-06
	if(model == md::MachineModel::Machinedrum)
	{
		send(*hardware, md::machines::assignMachine(model, static_cast<uint8_t>(track < 0 ? 3 : track), 133));
		const auto rom6 = query(*hardware, kitRequest, 0x52);
		size_t d = 0;
		for(size_t i = 0; i < std::min(after.size(), rom6.size()); ++i)
			if(after[i] != rom6[i] && d++ < 12)
				std::printf("  ROM-06 kit byte 0x%03zx: %02x -> %02x\n", i, after[i], rom6[i]);
		std::printf("differing kit bytes after ROM-06: %zu\n", d);
	}
	return 0;
}
