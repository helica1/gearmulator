// Experiment: does the Machinedrum honour the SDS sample period for arbitrary rates?
// Sends a 1 kHz sine encoded with a 44.1 kHz header into slot 0 and the same tone
// with a 32 kHz header into slot 1, assigns ROM machines and measures the played pitch.
//
//   mdSampleRateProbe <firmware.bin> <factory cache|-> [out prefix]

#include "mdLib/mdhardware.h"
#include "mdLib/mdpanel.h"
#include "mdLib/mdromloader.h"
#include "mdLib/mdsdsencode.h"
#include "mdLib/mdstate.h"
#include "mdLib/mdsysexfile.h"
#include "mdLib/mdsysextransfer.h"
#include "mdLib/mdtypes.h"

#include "baseLib/filesystem.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
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

	bool import(md::Hardware& _hardware, const std::vector<uint8_t>& _bytes)
	{
		auto prepared = md::prepareMidiSysexTransfer(_bytes);
		if(!prepared || !_hardware.startMidiSysexTransfer(*prepared))
		{
			std::cerr << "transfer could not start\n";
			return false;
		}
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(300);
		while(std::chrono::steady_clock::now() < deadline)
		{
			advance(_hardware, 64);
			const auto progress = _hardware.getMidiSysexTransferProgress();
			if(progress.state == md::MidiSysexTransferState::Complete)
				return true;
			if(progress.state == md::MidiSysexTransferState::Failed || progress.state == md::MidiSysexTransferState::Cancelled)
			{
				std::cerr << "transfer failed, state " << int(progress.state) << " error " << int(progress.error) << "\n";
				return false;
			}
		}
		const auto progress = _hardware.getMidiSysexTransferProgress();
		std::cerr << "transfer timed out: state " << int(progress.state) << " sent " << progress.sent << "/" << progress.total << "\n";
		return false;
	}

	double measureFrequency(const std::vector<float>& _audio, const size_t _begin, const size_t _end)
	{
		// zero crossings, positive direction
		size_t crossings = 0; size_t first = 0, last = 0;
		for(size_t i = _begin + 1; i < _end; ++i)
		{
			if(_audio[i - 1] <= 0.0f && _audio[i] > 0.0f)
			{
				if(!crossings) first = i;
				last = i;
				++crossings;
			}
		}
		if(crossings < 2)
			return 0.0;
		return static_cast<double>(crossings - 1) * md::g_samplerate / static_cast<double>(last - first);
	}

	bool probe(md::Hardware& _hardware, const uint8_t _slot, const uint8_t _model, const std::string& _label, const std::string& _prefix)
	{
		// assign machine _model (ROM slot) to track 1, UW flag set; parameters: pitch centre, long decay/hold, no BRR, full range
		synthLib::SMidiEvent assign(synthLib::MidiEventSource::Host);
		assign.sysex = {0xf0, 0x00, 0x20, 0x3c, 0x02, 0x00, 0x5b, 0x00, _model, 0x01, 0xf7};
		if(!_hardware.sendMidi(assign)) return false;
		advance(_hardware, 8192);
		const uint8_t values[] = {64, 127, 127, 0, 0, 127};
		for(uint8_t i = 0; i < 6; ++i)
			if(!_hardware.sendMidi({synthLib::MidiEventSource::Host, synthLib::M_CONTROLCHANGE, static_cast<uint8_t>(0x10 + i), values[i]})) return false;
		advance(_hardware, 8192);

		const auto trigger = md::panelPacket(md::MachineModel::Machinedrum, md::PanelControl::Trigger1);
		if(!trigger) return false;
		constexpr size_t frames = 44100;
		std::vector<float> left(frames, 0.0f), right(frames, 0.0f);
		synthLib::TAudioOutputs outputs{};
		outputs[0] = left.data(); outputs[1] = right.data();
		_hardware.sendPanelEvent(trigger->row, trigger->mask);
		_hardware.processAudio(outputs, 2048, 0);
		_hardware.sendPanelEvent(trigger->row, 0);
		outputs[0] += 2048; outputs[1] += 2048;
		_hardware.processAudio(outputs, frames - 2048, 0);

		float peak = 0.0f;
		for(const auto v : left) peak = std::max(peak, std::abs(v));
		const auto f1 = measureFrequency(left, 4410, 17640);
		const auto f2 = measureFrequency(left, 17640, 30870);
		std::printf("%s: slot %u peak %.4f  frequency %.1f Hz (0.1-0.4 s)  %.1f Hz (0.4-0.7 s)\n", _label.c_str(), _slot, peak, f1, f2);
		if(!_prefix.empty())
			std::ofstream(_prefix + "_" + _label + ".f32", std::ios::binary).write(reinterpret_cast<const char*>(left.data()), static_cast<std::streamsize>(left.size() * 4));
		return peak > 0.001f;
	}
}

int main(const int argc, char* argv[])
{
	if(argc < 3)
	{
		std::cerr << "usage: mdSampleRateProbe <firmware.bin> <factory cache|-> [out prefix]\n";
		return 2;
	}
	const std::string prefix = argc > 3 ? argv[3] : "";
	std::vector<uint8_t> rom;
	if(!baseLib::filesystem::readFile(rom, argv[1]) || !md::RomLoader::isRomForModel(rom, md::MachineModel::Machinedrum))
	{
		std::cerr << "firmware image not accepted\n";
		return 1;
	}
	std::vector<uint8_t> cache, flash;
	if(std::string(argv[2]) != "-" && baseLib::filesystem::readFile(cache, argv[2]) && !md::decodeFactoryFlashCache(flash, cache, rom))
	{
		cache.clear(); flash.clear();
	}
	std::printf("factory cache %zu bytes, decoded flash %zu bytes\n", cache.size(), flash.size());
	auto publisher = std::make_shared<md::FrontPanelPublisher>();
	auto hardwareStorage = std::make_unique<md::Hardware>(rom, argv[1], md::MachineModel::Machinedrum, std::vector<uint8_t>{},
		publisher, flash, cache);
	auto& hardware = *hardwareStorage;
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
		uint32_t frames = 0;
		while(!hardware.isFirmwareMidiReady() && std::chrono::steady_clock::now() < deadline)
		{
			advance(hardware, 64);
			frames += 64;
			if(frames % (md::g_samplerate * 5) < 64)
				std::printf("  t=%us audio %d panel %d midiRx %d flashCacheExpected %d\n", frames / md::g_samplerate, hardware.isAudioReady(),
					hardware.getUC().isPanelHandshakeComplete(), hardware.getUC().isMidiReceiveReady(), hardware.isFactoryFlashInitializationExpected());
		}
	}
	std::printf("booted: audio %d midi %d\n", hardware.isAudioReady(), hardware.isFirmwareMidiReady());
	if(!hardware.isFirmwareMidiReady())
	{
		const auto panel = hardware.getFrontPanelSnapshot();
		for(uint32_t y = 0; y < 64; y += 2)
		{
			std::string row;
			for(uint32_t x = 0; x < 128; ++x)
				row.push_back(panel.getLcdPixel(x, y) || panel.getLcdPixel(x, y + 1) ? '#' : ' ');
			std::printf("|%s|\n", row.c_str());
		}
		return 1;
	}
	advance(hardware, md::g_samplerate * 20);

	// a quarter second of 1 kHz sine at each rate
	const auto tone = [](const uint32_t _rate)
	{
		std::vector<int16_t> s(_rate);
		for(uint32_t i = 0; i < _rate; ++i)
			s[i] = static_cast<int16_t>(std::lround(20000.0 * std::sin(2.0 * M_PI * 1000.0 * i / _rate)));
		return s;
	};
	md::sds::Options a; a.slot = 0; a.name = "S441"; a.sampleRateHz = 44100;
	md::sds::Options b; b.slot = 1; b.name = "S320"; b.sampleRateHz = 32000;
	const auto streamA = md::sds::encode(tone(44100), a);
	const auto streamB = md::sds::encode(tone(32000), b);
	if(md::validateMidiSysexStream(streamA, md::MachineModel::Machinedrum) != md::MidiSysexStreamValidation::Valid
		|| md::validateMidiSysexStream(streamB, md::MachineModel::Machinedrum) != md::MidiSysexStreamValidation::Valid)
	{
		std::cerr << "encoder output rejected by validator\n";
		return 1;
	}

	std::printf("importing 44.1 kHz tone (%zu bytes)...\n", streamA.size());
	if(!import(hardware, streamA)) return 1;
	advance(hardware, md::g_samplerate * 15);
	std::printf("importing 32 kHz tone (%zu bytes)...\n", streamB.size());
	if(!import(hardware, streamB)) return 1;
	advance(hardware, md::g_samplerate * 15);

	const bool okA = probe(hardware, 0, 0, "tone44100", prefix);
	advance(hardware, md::g_samplerate * 2);
	const bool okB = probe(hardware, 1, 1, "tone32000", prefix);
	return okA && okB ? 0 : 1;
}
