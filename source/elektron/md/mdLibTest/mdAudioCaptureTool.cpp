// Diagnostic: boots a Machinedrum or Monomachine on a given firmware image, starts the
// sequencer and records the six codec output channels, then prints the emulator's transport
// scorecard. Used to compare firmware images (stock vs community OS) for audio defects.
//
//   mdAudioCaptureTool <md|mm> <firmware.bin> <factory cache or MM patch RAM, or -> <out prefix> [seconds] [pattern 0..127]
//
// Writes <prefix>_out.bin (float32, 6 channels interleaved, 44100 Hz).

#include "mdLib/mdfrontpanel.h"
#include "mdLib/mdhardware.h"
#include "mdLib/mdmidiprotocol.h"
#include "mdLib/mdpanel.h"
#include "mdLib/mdrom.h"
#include "mdLib/mdromloader.h"
#include "mdLib/mdstate.h"
#include "mdLib/mdtypes.h"

#include "baseLib/filesystem.h"

#include <chrono>
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

	bool tap(md::Hardware& _hardware, const md::MachineModel _model, const md::PanelControl _control)
	{
		const auto packet = md::panelPacket(_model, _control);
		if(!packet)
			return false;
		_hardware.sendPanelEvent(packet->row, packet->mask);
		advance(_hardware, 2048);
		_hardware.sendPanelEvent(packet->row, 0);
		advance(_hardware, 4096);
		return true;
	}
}

int main(const int argc, char* argv[])
{
	if(argc < 5)
	{
		std::cerr << "usage: mdAudioCaptureTool <md|mm> <firmware.bin> <factory cache|patch ram|-> <out prefix> [seconds]\n";
		return 2;
	}
	const auto model = std::string(argv[1]) == "mm" ? md::MachineModel::Monomachine : md::MachineModel::Machinedrum;
	const std::string firmwarePath = argv[2];
	const std::string extraPath = argv[3];
	const std::string prefix = argv[4];
	const uint32_t seconds = argc > 5 ? static_cast<uint32_t>(std::atoi(argv[5])) : 8;

	std::vector<uint8_t> rom;
	if(!baseLib::filesystem::readFile(rom, firmwarePath) || !md::RomLoader::isRomForModel(rom, model))
	{
		std::cerr << "firmware image not accepted\n";
		return 1;
	}
	std::vector<uint8_t> extra, cache, flash, patchRam;
	if(extraPath != "-" && baseLib::filesystem::readFile(extra, extraPath))
	{
		if(model == md::MachineModel::Monomachine)
		{
			if(extra.size() == md::g_patchRamStateSize)
				patchRam = extra;
			else
				std::cerr << "MM patch RAM has unexpected size, ignored\n";
		}
		else if(md::decodeFactoryFlashCache(flash, extra, rom))
			cache = extra;
		else
			std::cerr << "factory cache does not match this firmware, booting without it\n";
	}

	const int pattern = argc > 6 ? std::atoi(argv[6]) : -1;
	auto publisher = std::make_shared<md::FrontPanelPublisher>();
	auto hardwareStorage = std::make_unique<md::Hardware>(rom, firmwarePath, model, patchRam,
		publisher, flash, cache);
	auto& hardware = *hardwareStorage;

	const auto dumpLcd = [&](const std::string& _suffix)
	{
		const auto panel = hardware.getFrontPanelSnapshot();
		std::vector<uint8_t> bits(128 * 64);
		for(uint32_t y = 0; y < 64; ++y)
			for(uint32_t x = 0; x < 128; ++x)
				bits[y * 128 + x] = panel.getLcdPixel(x, y) ? 1 : 0;
		std::ofstream(prefix + _suffix, std::ios::binary).write(reinterpret_cast<const char*>(bits.data()), static_cast<std::streamsize>(bits.size()));
	};

	std::cerr << "booting...\n";
	advance(hardware, md::g_samplerate * 20);
	std::cerr << "audio ready " << hardware.isAudioReady() << " midi ready " << hardware.isFirmwareMidiReady() << "\n";
	advance(hardware, md::g_samplerate * 5);

	if(pattern >= 0)
	{
		const auto body = md::midiProtocol::selectPattern(model, pattern);
		synthLib::SMidiEvent event(synthLib::MidiEventSource::Host);
		event.sysex.push_back(0xf0);
		event.sysex.insert(event.sysex.end(), body.begin(), body.end());
		event.sysex.push_back(0xf7);
		hardware.sendMidi(event);
		advance(hardware, md::g_samplerate);
	}
	dumpLcd("_lcd_before.bin");
	{
		// request the current kit (0x53, slot 0) and report what comes back
		std::vector<synthLib::SMidiEvent> events;
		hardware.readMidiOut(events);
		synthLib::SMidiEvent request(synthLib::MidiEventSource::Host);
		const uint8_t product = model == md::MachineModel::Monomachine ? 0x03 : 0x02;
		request.sysex = {0xf0, 0x00, 0x20, 0x3c, product, 0x00, 0x53, 0x00, 0xf7};
		hardware.sendMidi(request);
		for(int attempt = 0; attempt < 40; ++attempt)
		{
			advance(hardware, md::g_samplerate / 10);
			events.clear();
			hardware.readMidiOut(events);
			bool got = false;
			for(const auto& e : events)
			{
				if(e.sysex.size() < 10 || e.sysex[6] != 0x52)
					continue;
				std::cerr << "kit dump: " << e.sysex.size() << " bytes, header";
				for(size_t i = 0; i < 16 && i < e.sysex.size(); ++i)
					std::cerr << ' ' << std::hex << static_cast<int>(e.sysex[i]) << std::dec;
				std::cerr << "\n";
				std::ofstream(prefix + "_kit.syx", std::ios::binary).write(reinterpret_cast<const char*>(e.sysex.data()), static_cast<std::streamsize>(e.sysex.size()));
				got = true;
			}
			if(got)
				break;
		}
	}
	std::cerr << "PLAY\n";
	tap(hardware, model, md::PanelControl::Play);
	advance(hardware, md::g_samplerate / 2);
	dumpLcd("_lcd_playing.bin");

	std::vector<float> outAll;
	synthLib::TAudioOutputs outputs{};
	std::vector<std::vector<float>> chan(6, std::vector<float>(256, 0.0f));
	for(size_t c = 0; c < 6; ++c)
		outputs[c] = chan[c].data();

	const uint32_t total = md::g_samplerate * seconds;
	const auto scoreBefore = hardware.getTransportScorecard();
	const auto dsp1Before = hardware.getDspMixer().dsp().getInstructionCounter();
	const auto dsp2Before = hardware.getDspProducer().dsp().getInstructionCounter();
	const auto ucBefore = hardware.getUC().getCycles();
	using Clock = std::chrono::steady_clock;
	const double budgetMs = 256.0 * 1000.0 / md::g_samplerate;
	double maxMs = 0, sumMs = 0; uint32_t over = 0, over2 = 0, blocks = 0;
	std::vector<double> perSecond;
	double secondAccum = 0;
	for(uint32_t done = 0; done < total; done += 256)
	{
		const auto t0 = Clock::now();
		hardware.processAudio(outputs, 256, 0);
		const auto ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
		maxMs = std::max(maxMs, ms); sumMs += ms; ++blocks;
		if(ms > budgetMs) ++over;
		if(ms > 2 * budgetMs) ++over2;
		secondAccum += ms;
		if(((done / 256) + 1) % (md::g_samplerate / 256) == 0) { perSecond.push_back(secondAccum); secondAccum = 0; }
		for(uint32_t i = 0; i < 256; ++i)
			for(size_t c = 0; c < 6; ++c)
				outAll.push_back(chan[c][i]);
	}
	tap(hardware, model, md::PanelControl::Stop);
	const auto dsp1 = hardware.getDspMixer().dsp().getInstructionCounter() - dsp1Before;
	const auto dsp2 = hardware.getDspProducer().dsp().getInstructionCounter() - dsp2Before;
	const auto uc = hardware.getUC().getCycles() - ucBefore;
	std::cerr << "work per emulated second: DSP1 " << dsp1 / seconds << " instr, DSP2 " << dsp2 / seconds
		<< " instr, UC " << uc / seconds << " cycles\n";
	std::cerr << "block timing: blocks=" << blocks << " mean=" << sumMs / blocks << "ms max=" << maxMs
		<< "ms budget=" << budgetMs << "ms over=" << over << " over2x=" << over2 << "\n  ms per emulated second:";
	for(const auto v : perSecond) std::cerr << ' ' << static_cast<int>(v);
	std::cerr << "\n";

	std::ofstream(prefix + "_out.bin", std::ios::binary).write(reinterpret_cast<const char*>(outAll.data()), static_cast<std::streamsize>(outAll.size() * 4));

	auto score = hardware.getTransportScorecard();
	for(size_t i = 0; i < 2; ++i)
	{
		auto& l = score.link[i]; const auto& b = scoreBefore.link[i];
		l.transmitFrames -= b.transmitFrames; l.acceptedFrames -= b.acceptedFrames; l.ringFullDrops -= b.ringFullDrops;
		l.receiverDisabledDrops -= b.receiverDisabledDrops; l.mdRendezvousRetainedDrops -= b.mdRendezvousRetainedDrops;
		l.mdRendezvousUnreleasedDrops -= b.mdRendezvousUnreleasedDrops; l.mdRendezvousDmaInactiveDrops -= b.mdRendezvousDmaInactiveDrops;
		l.mdRendezvousRingFullDrops -= b.mdRendezvousRingFullDrops; l.mdWindowOpenedDuringCatchUpDrops -= b.mdWindowOpenedDuringCatchUpDrops;
		l.mdReceiverOverrunDrops -= b.mdReceiverOverrunDrops; l.mdPostFlushRetainedDrops -= b.mdPostFlushRetainedDrops;
		l.poppedFrames -= b.poppedFrames; l.emptyReads -= b.emptyReads; l.stallPurgedFrames -= b.stallPurgedFrames;
		l.mdWindowPurgedFrames -= b.mdWindowPurgedFrames; l.mmStrobePurgedFrames -= b.mmStrobePurgedFrames;
	}
	std::cerr << "scorecard (transport diagnostics compiled " << (score.enabled ? "in" : "out") << "):\n";
	for(size_t i = 0; i < 2; ++i)
	{
		const auto& l = score.link[i];
		std::cerr << "  link[" << i << "] transmit=" << l.transmitFrames << " accepted=" << l.acceptedFrames
			<< " ringFullDrops=" << l.ringFullDrops << " receiverDisabledDrops=" << l.receiverDisabledDrops
			<< " rendezvousRetained=" << l.mdRendezvousRetainedDrops << " rendezvousUnreleased=" << l.mdRendezvousUnreleasedDrops
			<< " rendezvousDmaInactive=" << l.mdRendezvousDmaInactiveDrops << " rendezvousRingFull=" << l.mdRendezvousRingFullDrops
			<< " windowOpenedDuringCatchUp=" << l.mdWindowOpenedDuringCatchUpDrops << " receiverOverrun=" << l.mdReceiverOverrunDrops
			<< " postFlushRetained=" << l.mdPostFlushRetainedDrops
			<< " popped=" << l.poppedFrames << " emptyReads=" << l.emptyReads
			<< " stallPurged=" << l.stallPurgedFrames << " windowPurged=" << l.mdWindowPurgedFrames << "\n";
	}
	{
		auto& pe = hardware.getDspProducer().getPeriph().getEssi0();
		auto& me = hardware.getDspMixer().getPeriph().getEssi0();
		std::cerr << "  md rendezvous active=" << score.mdRendezvousActive
			<< " producer ESSI0 networkMode=" << pe.getCRB().test(dsp56k::Essi::RegCRBbits::CRB_MOD)
			<< " txWordCount=" << pe.getTxWordCount()
			<< " | mixer fastLinkRx=" << me.isFastLinkRx() << "\n";
	}
	std::cerr << "  hostAudioOverflow=" << hardware.hostAudioOverflowCount()
		<< " scheduledMidiOverflow=" << hardware.scheduledMidiOverflowCount()
		<< " midiRxOverflow=" << hardware.midiRxOverflowCount() << "\n";
	return 0;
}
