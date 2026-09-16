#include "mdSampleImport.h"

#include "mdLib/mdsdsencode.h"

#include "juce_audio_basics/juce_audio_basics.h"
#include "juce_audio_formats/juce_audio_formats.h"
#include "juce_data_structures/juce_data_structures.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace mdJucePlugin::sampleImport
{
	namespace
	{
		constexpr const char* g_ledgerKey = "mdSampleSlotLedger";
		constexpr int64_t g_maxSourceFrames = 48000LL * 60 * 10;	// refuse absurdly long files before decoding

		juce::AudioFormatManager& formatManager()
		{
			static juce::AudioFormatManager manager;
			static const bool registered = []
			{
				manager.registerBasicFormats();
				return true;
			}();
			(void)registered;
			return manager;
		}
	}

	bool isAudioFile(const juce::File& _file)
	{
		return formatManager().findFormatForFileExtension(_file.getFileExtension()) != nullptr;
	}

	bool isSysexFile(const juce::File& _file)
	{
		return _file.hasFileExtension("syx");
	}

	std::optional<DecodedSample> decode(const juce::File& _file, std::string& _error)
	{
		std::unique_ptr<juce::AudioFormatReader> reader(formatManager().createReaderFor(_file));
		if(!reader)
		{
			_error = "The file format is not supported or the file is damaged.";
			return std::nullopt;
		}
		if(reader->lengthInSamples <= 0 || reader->numChannels == 0 || reader->sampleRate <= 0)
		{
			_error = "The file contains no audio.";
			return std::nullopt;
		}
		if(reader->lengthInSamples > g_maxSourceFrames)
		{
			_error = "The file is longer than ten minutes.";
			return std::nullopt;
		}

		const auto frames = static_cast<int>(reader->lengthInSamples);
		const auto channels = static_cast<int>(reader->numChannels);
		juce::AudioBuffer<float> buffer(channels, frames);
		if(!reader->read(&buffer, 0, frames, 0, true, true))
		{
			_error = "The file could not be read completely.";
			return std::nullopt;
		}

		// mix to mono: average so a full-scale stereo file stays full scale
		std::vector<float> mono(static_cast<size_t>(frames), 0.0f);
		for(int c = 0; c < channels; ++c)
		{
			const auto* data = buffer.getReadPointer(c);
			for(int i = 0; i < frames; ++i)
				mono[static_cast<size_t>(i)] += data[i] / static_cast<float>(channels);
		}

		auto sampleRate = static_cast<uint32_t>(std::lround(reader->sampleRate));
		if(sampleRate > g_maxSampleRate)
		{
			const double ratio = reader->sampleRate / g_resampleTarget;
			const auto outFrames = static_cast<int>(std::floor(frames / ratio));
			std::vector<float> resampled(static_cast<size_t>(std::max(outFrames, 1)), 0.0f);
			// the interpolator has no anti-aliasing filter of its own; a short windowed
			// low-pass at the new Nyquist keeps downsampling clean enough for drums
			juce::IIRFilter lowPass;
			lowPass.setCoefficients(juce::IIRCoefficients::makeLowPass(reader->sampleRate, g_resampleTarget * 0.45));
			std::vector<float> filtered(mono);
			lowPass.processSamples(filtered.data(), frames);
			juce::IIRFilter lowPass2;
			lowPass2.setCoefficients(juce::IIRCoefficients::makeLowPass(reader->sampleRate, g_resampleTarget * 0.45));
			lowPass2.processSamples(filtered.data(), frames);
			juce::LagrangeInterpolator interpolator;
			interpolator.process(ratio, filtered.data(), resampled.data(), static_cast<int>(resampled.size()));
			mono.swap(resampled);
			sampleRate = g_resampleTarget;
		}

		DecodedSample result;
		result.sampleRate = sampleRate;
		result.sourcePath = _file.getFullPathName().toStdString();
		result.name = md::sds::makeName(_file.getFileNameWithoutExtension().toStdString());
		result.samples.resize(mono.size());
		for(size_t i = 0; i < mono.size(); ++i)
		{
			const auto scaled = std::lround(mono[i] * 32767.0f);
			if(scaled > 32767 || scaled < -32768)
				result.clipped = true;
			result.samples[i] = static_cast<int16_t>(std::clamp<long>(scaled, -32768, 32767));
		}
		return result;
	}

	std::vector<uint8_t> encode(const std::vector<DecodedSample>& _samples, const uint32_t _firstSlot)
	{
		std::vector<uint8_t> stream;
		uint32_t slot = _firstSlot;
		for(const auto& sample : _samples)
		{
			if(slot >= g_slotCount)
				return {};
			md::sds::Options options;
			options.slot = static_cast<uint8_t>(slot);
			options.name = sample.name;
			options.sampleRateHz = sample.sampleRate;
			const auto encoded = md::sds::encode(sample.samples, options);
			if(encoded.empty())
				return {};
			stream.insert(stream.end(), encoded.begin(), encoded.end());
			++slot;
		}
		return stream;
	}

	SlotLedger loadLedger(juce::PropertiesFile& _config)
	{
		// "slot:words:name;slot:words:name"
		SlotLedger ledger;
		const auto text = _config.getValue(g_ledgerKey);
		for(const auto& entry : juce::StringArray::fromTokens(text, ";", ""))
		{
			const auto parts = juce::StringArray::fromTokens(entry, ":", "");
			if(parts.size() < 3)
				continue;
			const auto slot = parts[0].getIntValue();
			if(slot < 0 || slot >= static_cast<int>(g_slotCount))
				continue;
			ledger[static_cast<uint32_t>(slot)] = SlotInfo{parts[2].toStdString(), static_cast<uint64_t>(parts[1].getLargeIntValue())};
		}
		return ledger;
	}

	void saveLedger(juce::PropertiesFile& _config, const SlotLedger& _ledger)
	{
		juce::StringArray entries;
		for(const auto& [slot, info] : _ledger)
		{
			auto name = juce::String(info.name).removeCharacters(":;");
			entries.add(juce::String(slot) + ":" + juce::String(static_cast<juce::int64>(info.words)) + ":" + name);
		}
		_config.setValue(g_ledgerKey, entries.joinIntoString(";"));
		_config.saveIfNeeded();
	}

	uint64_t usedWords(const SlotLedger& _ledger, const uint32_t _firstSlot, const size_t _count)
	{
		uint64_t used = 0;
		for(const auto& [slot, info] : _ledger)
		{
			if(slot >= _firstSlot && slot < _firstSlot + _count)
				continue;
			used += info.words;
		}
		return used;
	}

	std::string slotLabel(const uint32_t _slot)
	{
		char text[8];
		std::snprintf(text, sizeof(text), "R%02u", _slot + 1);
		return text;
	}
}
