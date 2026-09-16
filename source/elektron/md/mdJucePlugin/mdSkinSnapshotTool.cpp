// Diagnostic tool: boots a Machinedrum or Monomachine processor headless, opens its real editor with
// the software renderer and writes PNG snapshots, optionally after clicking elements by id or class.
//
//   mdSkinSnapshotTool <md|mm> <rom folder> <out prefix> [seconds of emulation before capture]

#include "mdPluginProcessor.h"
#include "mdEditor.h"

#include "jucePluginEditorLib/pluginEditorState.h"
#include "jucePluginEditorLib/rendererPreferenceKeys.h"
#include "juceRmlUi/juceRmlComponent.h"
#include "juceRmlUi/juceRmlLookAndFeel.h"
#include "synthLib/romLoader.h"

#include "RmlUi/Core/Context.h"
#include "RmlUi/Core/Element.h"
#include "RmlUi/Core/ElementDocument.h"

#include "juce_audio_processors/juce_audio_processors.h"
#include "juce_gui_basics/juce_gui_basics.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <cstdlib>
#include <thread>

void mdSkinSnapshotPump(double _seconds);

namespace juceRmlUi
{
	struct RenderingTestAccess
	{
		static void update(RmlComponent& _component) { _component.update(); }
	};
}

namespace
{
	class Driver final : public juce::Timer
	{
	public:
		Driver(mdJucePlugin::AudioPluginAudioProcessor& _processor, std::string _prefix, int _seconds)
			: m_processor(_processor), m_audioProcessor(_processor), m_prefix(std::move(_prefix)), m_seconds(_seconds), m_audio(2, 256)
		{
			m_audioProcessor.prepareToPlay(44100.0, 256);
			startTimer(20);
		}

		void timerCallback() override
		{
			// emulate faster than real time between UI ticks
			for(int i = 0; i < 40 && m_blocks * 256 < m_seconds * 44100; ++i)
			{
				m_audio.clear();
				m_midi.clear();
				m_audioProcessor.processBlock(m_audio, m_midi);
				++m_blocks;
			}
			if(m_blocks * 256 < m_seconds * 44100)
				return;

			if(!m_editor)
			{
				if(const auto* const family = std::getenv("SNAPSHOT_FAMILY"))
					m_processor.getConfig().setValue("machineRackFamily", juce::String(family));
				std::fprintf(stderr, "emulated %d blocks, creating editor\n", m_blocks);
				m_editor.reset(m_processor.createEditorIfNeeded());
				std::fprintf(stderr, "editor %p, component %p, state editor %p\n", static_cast<void*>(m_editor.get()), static_cast<void*>(component()),
					static_cast<void*>(m_processor.getEditorState() ? m_processor.getEditorState()->getEditor() : nullptr));
				m_editorCreatedAt = m_ticks;
			}
			++m_ticks;
			const auto* const family = std::getenv("SNAPSHOT_FAMILY");
			if(m_ticks - m_editorCreatedAt == 60)
				capture(family ? family : "rack");
			if(m_ticks - m_editorCreatedAt == 61)
			{
				std::fflush(stderr);
				std::_Exit(0);
			}
		}

		bool done() const { return m_done; }

	private:
		juceRmlUi::RmlComponent* component() const
		{
			auto* const state = m_processor.getEditorState();
			auto* const editor = state ? state->getEditor() : nullptr;
			return editor ? editor->getRmlComponent() : nullptr;
		}

		void click(const char* _class, const int _index)
		{
			auto* const c = component();
			if(!c)
				return;
			Rml::ElementList list;
			c->getDocument()->GetElementsByClassName(list, _class + 1);
			if(list.empty())
				return;
			auto* const element = list[static_cast<size_t>(std::min<int>(_index, static_cast<int>(list.size()) - 1))];
			element->Click();
			Rml::ElementList tiles;
			c->getDocument()->GetElementsByClassName(tiles, "rackTile");
			std::fprintf(stderr, "clicked %s #%d: %s, active=%d, tiles now %zu\n", _class, _index, element->GetInnerRML().c_str(),
				element->IsClassSet("active") ? 1 : 0, tiles.size());
		}

		void capture(const char* _name)
		{
			auto* const c = component();
			if(!c)
			{
				std::fprintf(stderr, "no editor component\n");
				return;
			}
			juceRmlUi::RenderingTestAccess::update(*c);
			juce::Image image(juce::Image::ARGB, c->getWidth(), c->getHeight(), true, juce::SoftwareImageType());
			if(auto* const lnf = dynamic_cast<juceRmlUi::LookAndFeel*>(&c->getLookAndFeel()))
				lnf->getCurrentImage() = image;
			{
				juce::Graphics g(image);
				c->paint(g);
			}
			const auto file = juce::File(m_prefix + "_" + _name + ".png");
			file.deleteFile();
			juce::PNGImageFormat png;
			if(auto stream = file.createOutputStream())
				png.writeImageToStream(image, *stream);
			std::fprintf(stderr, "wrote %s (%dx%d), current track %d\n", file.getFullPathName().toRawUTF8(), image.getWidth(), image.getHeight(), m_processor.getCurrentTrack());
		}

		mdJucePlugin::AudioPluginAudioProcessor& m_processor;
		juce::AudioProcessor& m_audioProcessor;
		std::string m_prefix;
		int m_seconds;
		juce::AudioBuffer<float> m_audio;
		juce::MidiBuffer m_midi;
		int m_blocks = 0;
		int m_ticks = 0;
		int m_editorCreatedAt = 0;
		bool m_done = false;
		std::unique_ptr<juce::AudioProcessorEditor> m_editor;
	};
}

int main(const int argc, char* argv[])
{
	if(argc < 4)
	{
		std::fprintf(stderr, "usage: mdSkinSnapshotTool <md|mm> <rom folder> <out prefix> [seconds]\n");
		return 2;
	}
	juce::ScopedJuceInitialiser_GUI juce;
	const auto model = std::string(argv[1]) == "mm" ? md::MachineModel::Monomachine : md::MachineModel::Machinedrum;
	synthLib::RomLoader::addSearchPath(argv[2]);
	const int seconds = argc > 4 ? std::atoi(argv[4]) : 30;

	mdJucePlugin::AudioPluginAudioProcessor::EphemeralConfig config;
	auto processor = std::make_unique<mdJucePlugin::AudioPluginAudioProcessor>(model, config, false);
	processor->getConfig().setValue(jucePluginEditorLib::forceSoftwareRendererKey, 1);
	{
		Driver driver(*processor, argv[3], seconds);
		std::fprintf(stderr, "pumping run loop\n");
		while(!driver.done())
			mdSkinSnapshotPump(0.05);
		std::fprintf(stderr, "done\n");
	}
	static_cast<juce::AudioProcessor&>(*processor).releaseResources();
	return 0;
}
