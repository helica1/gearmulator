#pragma once

#include "jucePluginEditorLib/settingsDeviceSpecific.h"
#include "juce_events/juce_events.h"

#include <string>
#include <utility>
#include <vector>

namespace Rml { class Element; }
namespace jucePluginEditorLib { class Processor; }

namespace mdJucePlugin
{
	class SettingsAudioInput final : public jucePluginEditorLib::SettingsDeviceSpecific,
		private juce::Timer
	{
	public:
		SettingsAudioInput(jucePluginEditorLib::Processor& _processor, Rml::Element* _root);
		~SettingsAudioInput() override;

	private:
		void timerCallback() override;
		void updateRenderAhead();

		jucePluginEditorLib::Processor& m_processor;
		std::vector<std::pair<Rml::Element*, uint32_t>> m_renderAhead;
		Rml::Element* m_status;
		Rml::Element* m_settings;
		std::string m_lastStatus;
	};
}
