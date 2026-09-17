#pragma once

#include "jucePluginEditorLib/pluginProcessor.h"
#include "mdLib/mdremotepanel.h"
#include "mdLib/mdsampledirectory.h"
#include "mdLib/mdtypes.h"
#include "synthLib/performanceReport.h"

#include <atomic>
#include <optional>
#include <string>
#include <string_view>
#include <mutex>
#include <vector>

namespace mdJucePlugin
{
	class AudioPluginAudioProcessor : public jucePluginEditorLib::Processor,
		private juce::Timer
	{
	public:
		struct EphemeralConfig final
		{
			// Tests may explicitly isolate the emulated machine from persistent
			// factory/storage caches. A disengaged value preserves normal discovery;
			// an engaged empty value disables the device home path entirely.
			std::optional<std::string> deviceHomePath;
		};

	    AudioPluginAudioProcessor();
		explicit AudioPluginAudioProcessor(md::MachineModel _model);
		AudioPluginAudioProcessor(md::MachineModel _model, bool _allowMcpServer);
		AudioPluginAudioProcessor(md::MachineModel _model, EphemeralConfig,
			bool _allowMcpServer = false);
		AudioPluginAudioProcessor(md::MachineModel _model,
			std::vector<uint8_t> _initialPatchRam, bool _allowMcpServer = true);
	    ~AudioPluginAudioProcessor() override;

		md::MachineModel getModel() const { return m_model; }
		static md::MachineModel getCompiledProductModel();
		static bool hasEmbeddedProductResource(std::string_view _filename);
		juce::File getInstalledFactoryStorageImage() const;
		juce::File getStorageRecoveryImage() const;
		bool loadStorageImage(const juce::File& _source, juce::String& _result);
		bool serviceFactoryInitialization();
		bool serviceProjectStateRestore();
		std::string getProjectStateRestoreError();
		void setPerformanceDiagnosticsEnabled(bool _enabled);
		bool performanceDiagnosticsActive() const;
		std::string performanceDiagnosticsStatus() const;
		juce::File performanceDiagnosticsFolder() const;
		juce::File performanceDiagnosticsFile() const { return m_performanceReportFile; }
		void setRamRecordingMode(md::RamRecordingMode _mode);
		md::RamRecordingMode getRamRecordingMode() const
		{
			return static_cast<md::RamRecordingMode>(
				m_ramRecordingMode.load(std::memory_order_relaxed));
		}
		bool isRamRecordingModeAvailable();

		// Firmware image for this instance. Empty = the stock OS discovered next to the
		// plugin or in the roms folder. A chosen image is remembered as the default for
		// new instances (config) and travels with the project (state chunk "FWIM").
		static constexpr const char* g_firmwareImageConfigKey = "firmwareImagePath";
		const std::string& getFirmwareImagePath() const { return m_firmwareImagePath; }
		std::string getFirmwareDescription() const;
		// Restarts the machine on another OS image, from a fresh factory state.
		bool setFirmwareImage(const std::string& _path, std::string& _error);

		// Machine selector: assigns a machine (firmware model id, see mdLib/mdmachines.h)
		// to the machine's current track. Returns false while the current track is unknown.
		bool assignMachineToCurrentTrack(uint16_t _machineId);
		int getCurrentTrack();
		// true when the running OS is not the stock image (community X.xx builds add machines)
		bool isExtendedOs() const;
		// Machinedrum UW sample slots as stored in the emulated machine
		std::optional<md::sampleDirectory::Directory> readSampleDirectory();

		// Browser front panel for tablets on the local network, see mdLib/mdremotepanel.h
		void startRemotePanel();
		std::string getRemotePanelUrl() const;
		bool isRemotePanelRunning() const { return m_remotePanel && m_remotePanel->isRunning(); }

	    jucePluginEditorLib::PluginEditorState* createEditorState() override;
	    synthLib::Device* createDevice() override;
		void getRemoteDeviceParams(synthLib::DeviceCreateParams& _params) const override;

	    pluginLib::Controller* createController() override;
		void saveChunkData(baseLib::BinaryStream& _stream) override;
		void loadChunkData(baseLib::ChunkReader& _reader) override;
		bool loadCustomData(const std::vector<uint8_t>& _sourceBuffer) override;

	private:
		static BusesProperties createBusesProperties();
		bool isBusesLayoutSupported(const BusesLayout& _layout) const override;
		AudioPluginAudioProcessor(md::MachineModel _model,
			std::vector<uint8_t> _initialPatchRam, bool _allowMcpServer,
			bool _ephemeralConfig,
			std::optional<std::string> _deviceHomePath = std::nullopt);
		static std::string initialFirmwareImagePath(juce::PropertiesFile& _config, md::MachineModel _model);
		bool readFirmwareImage(const std::string& _path, std::vector<uint8_t>& _data, std::string& _error) const;
		std::string findFirmwareByFingerprint(uint64_t _fingerprint, const std::string& _hint) const;
		void applyProjectFirmware(const std::string& _path, uint64_t _fingerprint);
		bool serviceDeferredStateRestore();
		bool serviceStateRestoreFailure();
		void recordStandaloneStartupDiagnostics();
		void reportProjectStateRestoreFailure(const std::string& _error);
		void timerCallback() override;

		std::unique_ptr<synthLib::PerformanceReport> m_performanceReport;
		std::unique_ptr<md::RemotePanelServer> m_remotePanel;
		std::mutex m_remoteSlotNamesMutex;
		std::vector<std::string> m_remoteSlotNames;
		uint32_t m_remoteSlotNamesTime = 0;
		juce::File m_performanceReportFile;
		bool m_performanceFolderError = false;
		const md::MachineModel m_model;
		const std::vector<uint8_t> m_initialPatchRam;
		const std::optional<std::string> m_deviceHomePath;
		const bool m_ephemeralConfig;
		std::string m_firmwareImagePath;
		uint64_t m_firmwareFingerprint = 0;
		std::mutex m_storageLoadMutex;
		uint64_t m_reportedRestoreFailureGeneration = 0;
		juce::File m_startupDiagnosticsFile;
		double m_startupDiagnosticsStartMilliseconds = 0.0;
		bool m_startupDiagnosticsEnabled = false;
		std::atomic<uint8_t> m_ramRecordingMode{
			static_cast<uint8_t>(md::RamRecordingMode::Original)};
		bool m_ramRecordingModeChunkSeen = false;
		JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioPluginAudioProcessor)
	};
}
