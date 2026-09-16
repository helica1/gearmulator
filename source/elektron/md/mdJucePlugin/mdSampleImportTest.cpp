// Sample import: WAV decode, mono mix, resampling, SDS encode for several slots, slot ledger.
// With GEARMULATOR_MD_FIRMWARE_BIN (and optionally GEARMULATOR_MD_FACTORY_CACHE) it also sends
// the samples into an emulated Machinedrum UW and checks that each slot plays at the right pitch.

#include "mdSampleImport.h"

#include "mdLib/mdhardware.h"
#include "mdLib/mdpanel.h"
#include "mdLib/mdromloader.h"
#include "mdLib/mdsdsencode.h"
#include "mdLib/mdstate.h"
#include "mdLib/mdsysexfile.h"
#include "mdLib/mdsysextransfer.h"

#include "baseLib/filesystem.h"

#include "juce_audio_formats/juce_audio_formats.h"
#include "juce_data_structures/juce_data_structures.h"
#include "juce_events/juce_events.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	void require(const bool _condition, const std::string& _message)
	{
		if(!_condition)
			throw std::runtime_error(_message);
	}

	juce::File writeTone(const juce::File& _folder, const juce::String& _name, const double _rate, const int _channels,
		const double _frequency, const double _seconds, const int _bits)
	{
		const auto file = _folder.getChildFile(_name);
		file.deleteFile();
		juce::WavAudioFormat wav;
		auto* stream = file.createOutputStream().release();
		require(stream != nullptr, "cannot create " + _name.toStdString());
		std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(stream, _rate, static_cast<unsigned>(_channels), _bits, {}, 0));
		if(!writer)
			delete stream;
		require(writer != nullptr, "no WAV writer");
		const auto frames = static_cast<int>(_rate * _seconds);
		juce::AudioBuffer<float> buffer(_channels, frames);
		for(int i = 0; i < frames; ++i)
		{
			const auto v = static_cast<float>(0.5 * std::sin(2.0 * juce::MathConstants<double>::pi * _frequency * i / _rate));
			for(int c = 0; c < _channels; ++c)
				buffer.setSample(c, i, c == 0 ? v : v * 0.5f);	// channels differ so the mono mix is observable
		}
		require(writer->writeFromAudioSampleBuffer(buffer, 0, frames), "WAV write failed");
		writer.reset();
		return file;
	}

	double zeroCrossingFrequency(const std::vector<float>& _audio, const size_t _begin, const size_t _end, const double _rate)
	{
		size_t crossings = 0, first = 0, last = 0;
		for(size_t i = _begin + 1; i < _end; ++i)
		{
			if(_audio[i - 1] <= 0.0f && _audio[i] > 0.0f)
			{
				if(!crossings) first = i;
				last = i;
				++crossings;
			}
		}
		return crossings < 2 ? 0.0 : static_cast<double>(crossings - 1) * _rate / static_cast<double>(last - first);
	}

	void advance(md::Hardware& _hardware, const uint32_t _frames)
	{
		for(uint32_t done = 0; done < _frames;)
		{
			const auto n = std::min<uint32_t>(64, _frames - done);
			_hardware.advance(n);
			done += n;
		}
	}

	double playSlot(md::Hardware& _hardware, const uint8_t _slot)
	{
		synthLib::SMidiEvent assign(synthLib::MidiEventSource::Host);
		assign.sysex = {0xf0, 0x00, 0x20, 0x3c, 0x02, 0x00, 0x5b, 0x00, _slot, 0x01, 0xf7};
		require(_hardware.sendMidi(assign), "assign rejected");
		advance(_hardware, 8192);
		const uint8_t values[] = {64, 127, 127, 0, 0, 127};
		for(uint8_t i = 0; i < 6; ++i)
			require(_hardware.sendMidi({synthLib::MidiEventSource::Host, synthLib::M_CONTROLCHANGE, static_cast<uint8_t>(0x10 + i), values[i]}), "CC rejected");
		advance(_hardware, 8192);
		const auto trigger = md::panelPacket(md::MachineModel::Machinedrum, md::PanelControl::Trigger1);
		constexpr size_t frames = 22050;
		std::vector<float> left(frames, 0.0f), right(frames, 0.0f);
		synthLib::TAudioOutputs outputs{};
		outputs[0] = left.data(); outputs[1] = right.data();
		_hardware.sendPanelEvent(trigger->row, trigger->mask);
		_hardware.processAudio(outputs, 2048, 0);
		_hardware.sendPanelEvent(trigger->row, 0);
		outputs[0] += 2048; outputs[1] += 2048;
		_hardware.processAudio(outputs, frames - 2048, 0);
		advance(_hardware, md::g_samplerate);
		return zeroCrossingFrequency(left, 4410, 17640, md::g_samplerate);
	}

	int runFirmware(const std::vector<uint8_t>& _stream, const std::vector<double>& _expected)
	{
		const auto* const firmware = std::getenv("GEARMULATOR_MD_FIRMWARE_BIN");
		if(!firmware || !*firmware)
		{
			std::puts("mdSampleImportTest: firmware part skipped (GEARMULATOR_MD_FIRMWARE_BIN not set)");
			return 0;
		}
		std::vector<uint8_t> rom, cache, flash;
		require(baseLib::filesystem::readFile(rom, firmware) && md::RomLoader::isRomForModel(rom, md::MachineModel::Machinedrum), "firmware not accepted");
		if(const auto* const cachePath = std::getenv("GEARMULATOR_MD_FACTORY_CACHE"); cachePath && *cachePath)
			if(!baseLib::filesystem::readFile(cache, cachePath) || !md::decodeFactoryFlashCache(flash, cache, rom))
				cache.clear(), flash.clear();

		auto hardware = std::make_unique<md::Hardware>(rom, firmware, md::MachineModel::Machinedrum, std::vector<uint8_t>{},
			std::shared_ptr<md::FrontPanelPublisher>{}, flash, cache);
		const auto bootDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(240);
		while(!hardware->isFirmwareMidiReady() || hardware->isFactoryFlashInitializationExpected())
		{
			advance(*hardware, 64);
			require(std::chrono::steady_clock::now() < bootDeadline, "machine did not become ready");
		}
		advance(*hardware, md::g_samplerate * 20);

		auto prepared = md::prepareMidiSysexTransfer(_stream, md::MachineModel::Machinedrum);
		require(prepared.has_value() && hardware->startMidiSysexTransfer(*prepared), "transfer did not start");
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(300);
		while(true)
		{
			advance(*hardware, 64);
			const auto progress = hardware->getMidiSysexTransferProgress();
			if(progress.state == md::MidiSysexTransferState::Complete)
			{
				std::printf("transfer complete, %u samples acknowledged\n", progress.acknowledgedSamples);
				break;
			}
			require(progress.state != md::MidiSysexTransferState::Failed && progress.state != md::MidiSysexTransferState::Cancelled, "transfer failed");
			require(std::chrono::steady_clock::now() < deadline, "transfer timed out");
		}
		advance(*hardware, md::g_samplerate * 15);

		for(size_t i = 0; i < _expected.size(); ++i)
		{
			const auto slot = static_cast<uint8_t>(5 + i);
			const auto frequency = playSlot(*hardware, slot);
			std::printf("slot R%02u plays %.1f Hz, expected %.1f Hz\n", slot + 1, frequency, _expected[i]);
			require(std::abs(frequency - _expected[i]) < _expected[i] * 0.01, "wrong playback pitch in slot " + std::to_string(slot + 1));
		}
		return 0;
	}
}

int main()
{
	juce::ScopedJuceInitialiser_GUI juce;
	try
	{
		namespace si = mdJucePlugin::sampleImport;
		const auto folder = juce::File::createTempFile("mdSampleImportTest");
		require(folder.createDirectory().wasOk(), "no temp folder");

		// 48 kHz stereo 24 bit: kept at 48 kHz, channels averaged
		const auto a = writeTone(folder, "kick drum.wav", 48000.0, 2, 440.0, 0.5, 24);
		// 96 kHz mono 16 bit: resampled to 44.1 kHz
		const auto b = writeTone(folder, "hat.wav", 96000.0, 1, 880.0, 0.5, 16);

		require(si::isAudioFile(a) && !si::isSysexFile(a), "wav not recognised");
		require(si::isSysexFile(juce::File("/tmp/x.syx")) && !si::isAudioFile(juce::File("/tmp/x.syx")), "syx not recognised");

		std::string error;
		const auto decodedA = si::decode(a, error);
		require(decodedA.has_value(), "decode A: " + error);
		require(decodedA->sampleRate == 48000 && decodedA->samples.size() == 24000, "A rate/length");
		require(decodedA->name == "KICK", "A name '" + decodedA->name + "'");
		int16_t peakA = 0;
		for(const auto s : decodedA->samples) peakA = std::max<int16_t>(peakA, static_cast<int16_t>(std::abs(s)));
		require(peakA > 12000 && peakA < 12700, "A mono mix peak " + std::to_string(peakA));	// (0.5 + 0.25) / 2 * 32767

		const auto decodedB = si::decode(b, error);
		require(decodedB.has_value(), "decode B: " + error);
		require(decodedB->sampleRate == 44100, "B not resampled");
		require(std::abs(static_cast<long>(decodedB->samples.size()) - 22050) <= 2, "B length " + std::to_string(decodedB->samples.size()));
		{
			std::vector<float> f(decodedB->samples.begin(), decodedB->samples.end());
			const auto freq = zeroCrossingFrequency(f, 1000, f.size() - 1000, 44100.0);
			require(std::abs(freq - 880.0) < 2.0, "B resampled pitch " + std::to_string(freq));
		}

		const auto garbage = folder.getChildFile("garbage.wav");
		garbage.replaceWithText("not audio");
		require(!si::decode(garbage, error).has_value() && !error.empty(), "garbage accepted");

		const std::vector<si::DecodedSample> samples{*decodedA, *decodedB};
		const auto stream = si::encode(samples, 5);
		require(md::validateMidiSysexStream(stream, md::MachineModel::Machinedrum) == md::MidiSysexStreamValidation::Valid, "stream rejected by validator");
		require(stream.size() == md::sds::encodedSize(24000) + md::sds::encodedSize(decodedB->samples.size()), "stream size");
		require(si::encode(samples, 47).empty(), "overflowing slots accepted");

		// ledger round trip
		juce::PropertiesFile::Options options;
		options.applicationName = "mdSampleImportTest";
		options.filenameSuffix = ".settings";
		options.folderName = "mdSampleImportTest";
		juce::PropertiesFile config(folder.getChildFile("test.settings"), options);
		si::SlotLedger ledger;
		ledger[5] = {"KICK", 24000};
		ledger[6] = {"HAT ", 22050};
		ledger[40] = {"A:B;", 100};
		si::saveLedger(config, ledger);
		const auto loaded = si::loadLedger(config);
		require(loaded.size() == 3 && loaded.at(5).name == "KICK" && loaded.at(6).words == 22050 && loaded.at(40).name == "AB", "ledger round trip");
		require(si::usedWords(loaded, 5, 2) == 100 && si::usedWords(loaded, 0, 1) == 46150, "usedWords");
		require(si::slotLabel(0) == "R01" && si::slotLabel(47) == "R48", "slot labels");

		std::puts("mdSampleImportTest: decode/encode/ledger PASS");
		const auto result = runFirmware(stream, {440.0, 880.0});
		folder.deleteRecursively();
		if(result == 0)
			std::puts("mdSampleImportTest: PASS");
		return result;
	}
	catch(const std::exception& e)
	{
		std::fprintf(stderr, "mdSampleImportTest: %s\n", e.what());
		return 1;
	}
}
