#include "mdSettingsPanelFeel.h"

#include "mdEditor.h"
#include "mdLcdInteractionModel.h"
#include "mdPixelPerfectPanel.h"
#include "mdPluginProcessor.h"

#include "jucePluginEditorLib/pluginProcessor.h"
#include "jucePluginEditorLib/settingsPlugin.h"

#include "juceRmlUi/rmlElemButton.h"
#include "juceRmlUi/rmlEventListener.h"
#include "juceRmlUi/rmlHelper.h"

#include "RmlUi/Core/Element.h"

namespace mdJucePlugin
{
	SettingsPanelFeel::SettingsPanelFeel(Editor& _editor, Rml::Element* _root) : m_editor(_editor)
	{
		jucePluginEditorLib::SettingsPlugin::createToggleButton(_root, "btPixelPerfectPanel",
			m_editor.getProcessor().getConfig(), PixelPerfectPanel::configKey, [this](bool)
			{
				m_editor.applyPixelPerfectPanel();
			}, PixelPerfectPanel::defaultEnabled);
		jucePluginEditorLib::SettingsPlugin::createToggleButton(_root, "btLcdRotaryInteraction",
			m_editor.getProcessor().getConfig(), lcdInteraction::configKey, [this](bool)
			{
				m_editor.applyLcdInteraction();
			}, lcdInteraction::defaultEnabled);
		bindGroup(_root, "btWheelSpeed", "panelWheelSpeedPercent");
		bindGroup(_root, "btEncoderSpeed", "panelEncoderSpeedPercent");

		m_ramRecordingComplete = juceRmlUi::helper::findChild(
			_root, "btRamRecordingComplete", false);
		m_ramRecordingOriginal = juceRmlUi::helper::findChild(
			_root, "btRamRecordingOriginal", false);
		if(m_ramRecordingComplete)
			juceRmlUi::EventListener::AddClick(m_ramRecordingComplete, [this]
			{
				static_cast<AudioPluginAudioProcessor&>(m_editor.getProcessor())
					.setRamRecordingMode(md::RamRecordingMode::CompleteTail);
				updateRamRecordingMode();
			});
		if(m_ramRecordingOriginal)
			juceRmlUi::EventListener::AddClick(m_ramRecordingOriginal, [this]
			{
				static_cast<AudioPluginAudioProcessor&>(m_editor.getProcessor())
					.setRamRecordingMode(md::RamRecordingMode::Original);
				updateRamRecordingMode();
			});
		updateRamRecordingMode();

		if(auto* const loadFactory = juceRmlUi::helper::findChild(
			_root, "btLoadInstalledFactoryStorage", false))
		{
			juceRmlUi::EventListener::AddClick(loadFactory, [this]
			{
				m_editor.loadInstalledFactoryStorage();
			});
		}
		if(auto* const chooseStorage = juceRmlUi::helper::findChild(
			_root, "btChooseStorageImage", false))
		{
			juceRmlUi::EventListener::AddClick(chooseStorage, [this]
			{
				m_editor.chooseStorageImage();
			});
		}
		m_firmwareLabel = juceRmlUi::helper::findChild(_root, "lblFirmwareImage", false);
		if(auto* const chooseFirmware = juceRmlUi::helper::findChild(_root, "btChooseFirmware", false))
		{
			juceRmlUi::EventListener::AddClick(chooseFirmware, [this]
			{
				m_editor.chooseFirmwareImage();
			});
		}
		if(auto* const stockFirmware = juceRmlUi::helper::findChild(_root, "btStockFirmware", false))
		{
			juceRmlUi::EventListener::AddClick(stockFirmware, [this]
			{
				m_editor.useStockFirmware();
			});
		}
		updateFirmwareLabel();
		if(m_firmwareLabel && !isTimerRunning())
			startTimerHz(2);

		m_restoreStorage = juceRmlUi::helper::findChild(
			_root, "btRestorePreviousStorage", false);
		if(m_restoreStorage)
		{
			juceRmlUi::EventListener::AddClick(m_restoreStorage, [this]
			{
				m_editor.restorePreviousStorage();
			});
			updateRestoreAvailability();
		}
		if(m_restoreStorage || m_ramRecordingComplete || m_ramRecordingOriginal)
			startTimerHz(2);
	}

	void SettingsPanelFeel::timerCallback()
	{
		updateRestoreAvailability();
		updateFirmwareLabel();
		updateRamRecordingMode();
	}

	void SettingsPanelFeel::updateFirmwareLabel()
	{
		if(!m_firmwareLabel)
			return;
		const auto text = "Running: " + m_editor.getFirmwareDescription();
		if(m_firmwareLabel->GetInnerRML() != text)
			m_firmwareLabel->SetInnerRML(text);
	}

	void SettingsPanelFeel::updateRamRecordingMode()
	{
		if(!m_ramRecordingComplete && !m_ramRecordingOriginal)
			return;
		auto& processor = static_cast<AudioPluginAudioProcessor&>(m_editor.getProcessor());
		const auto mode = processor.getRamRecordingMode();
		const bool available = processor.isRamRecordingModeAvailable();
		if(m_ramRecordingComplete)
		{
			if(auto* const button = juceRmlUi::helper::findChild(
				m_ramRecordingComplete, "button", false))
				juceRmlUi::ElemButton::setChecked(button,
					mode == md::RamRecordingMode::CompleteTail);
			juceRmlUi::helper::setEnabled(m_ramRecordingComplete, available);
		}
		if(m_ramRecordingOriginal)
		{
			if(auto* const button = juceRmlUi::helper::findChild(
				m_ramRecordingOriginal, "button", false))
				juceRmlUi::ElemButton::setChecked(button,
					mode == md::RamRecordingMode::Original);
			juceRmlUi::helper::setEnabled(m_ramRecordingOriginal, available);
		}
	}

	void SettingsPanelFeel::updateRestoreAvailability()
	{
		if(m_restoreStorage)
			juceRmlUi::helper::setEnabled(m_restoreStorage,
				m_editor.hasStorageRecoveryImage());
	}

	void SettingsPanelFeel::bindGroup(Rml::Element* _root, const char* _idPrefix, const char* _configKey)
	{
		auto& config = m_editor.getProcessor().getConfig();

		std::vector<Rml::Element*> checkboxes(std::size(Editor::g_panelSpeedPercents), nullptr);

		for (size_t i = 0; i < std::size(Editor::g_panelSpeedPercents); ++i)
		{
			auto* row = juceRmlUi::helper::findChild(_root,
				_idPrefix + std::to_string(Editor::g_panelSpeedPercents[i]), false);
			if (row)
				checkboxes[i] = juceRmlUi::helper::findChild(row, "button");
		}

		const auto updateChecked = [checkboxes, &config, _configKey]
		{
			const auto current = config.getIntValue(_configKey, 100);
			for (size_t i = 0; i < checkboxes.size(); ++i)
			{
				if (checkboxes[i])
					juceRmlUi::ElemButton::setChecked(checkboxes[i], Editor::g_panelSpeedPercents[i] == current);
			}
		};

		updateChecked();

		for (const auto percent : Editor::g_panelSpeedPercents)
		{
			auto* row = juceRmlUi::helper::findChild(_root, _idPrefix + std::to_string(percent), false);
			if (!row)
				continue;

			juceRmlUi::EventListener::AddClick(row, [this, updateChecked, &config, _configKey, percent]
			{
				config.setValue(_configKey, percent);
				config.saveIfNeeded();
				updateChecked();
				m_editor.applyPanelSpeeds();
			});
		}
	}
}
