#include "mddevice.h"

#include "mdstate.h"
#include "mdromloader.h"
#include "mdtypes.h"

#include "baseLib/filesystem.h"
#include "synthLib/realtimeInstrumentation.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace
{
	std::atomic<uint64_t> g_sysexDeviceIds{0};
	std::vector<uint8_t> loadInitialPatchRam(const synthLib::DeviceCreateParams& _params,
		const md::MachineModel _model, const std::vector<uint8_t>& _initialPatchRam)
	{
		if(!_initialPatchRam.empty())
			return _initialPatchRam;
		if(_model != md::MachineModel::Monomachine || _params.homePath.empty())
			return {};

		const auto filename = baseLib::filesystem::validatePath(_params.homePath)
			+ "nvram/mm-factory-live3-be.bin";
		std::vector<uint8_t> data;
		if(!baseLib::filesystem::readFile(data, filename))
			return {};

		if(data.size() != md::g_patchRamStateSize)
		{
			std::fprintf(stderr,
				"[MM] ignoring factory patch RAM with unexpected size: %s (%zu bytes, expected %u)\n",
				filename.c_str(), data.size(), md::g_patchRamStateSize);
			return {};
		}

		std::fprintf(stderr, "[MM] factory patch RAM discovered at %s (%zu bytes)\n",
			filename.c_str(), data.size());
		return data;
	}

	std::string mdFlashCacheFilename(const std::string& _homePath,
		const md::MachineModel _model)
	{
		if(_model != md::MachineModel::Machinedrum || _homePath.empty())
			return {};
		return baseLib::filesystem::validatePath(_homePath)
			+ "nvram/md-uw-1.63-factory-v2.cache";
	}

	std::string mdFlashCacheFilename(const synthLib::DeviceCreateParams& _params,
		const md::MachineModel _model)
	{
		// An alternative OS initialises its own factory flash; keep its cache apart
		// from the stock one so the two do not keep invalidating each other.
		if(_model == md::MachineModel::Machinedrum && !_params.homePath.empty()
			&& !_params.romData.empty()
			&& !md::RomLoader::isStockRom(_params.romData, _model))
		{
			char name[64];
			std::snprintf(name, sizeof(name), "nvram/md-uw-%016llx-factory-v2.cache",
				static_cast<unsigned long long>(md::RomLoader::fingerprint(_params.romData)));
			return baseLib::filesystem::validatePath(_params.homePath) + name;
		}
		return mdFlashCacheFilename(_params.homePath, _model);
	}

	struct InitialMdFlash
	{
		std::vector<uint8_t> flash;
		std::vector<uint8_t> cache;
	};

	InitialMdFlash loadInitialMdFlash(
		const synthLib::DeviceCreateParams& _params, const md::MachineModel _model)
	{
		const auto filename = mdFlashCacheFilename(_params, _model);
		std::vector<uint8_t> cache;
		if(filename.empty() || !baseLib::filesystem::readFile(cache, filename))
			return {};

		md::Rom rom;
		if(!_params.romData.empty())
		{
			md::Rom supplied(_params.romData, _params.romName);
			if(supplied.isValid() && md::RomLoader::isRomForModel(
				supplied.data(), _model))
				rom = std::move(supplied);
		}
		if(!rom.isValid())
			rom = md::RomLoader::findROM(_model);

		InitialMdFlash result;
		if(!rom.isValid()
			|| !md::decodeFactoryFlashCache(result.flash, cache, rom.data()))
		{
			std::fprintf(stderr,
				"[MD] ignoring invalid or ROM-mismatched UW factory cache: %s\n",
				filename.c_str());
			return {};
		}
		result.cache = std::move(cache);
		return result;
	}

	InitialMdFlash loadInitialMdFlash(const md::Rom& _rom,
		const std::string& _filename)
	{
		InitialMdFlash result;
		if(!_rom.isValid() || _filename.empty()
			|| !baseLib::filesystem::readFile(result.cache, _filename)
			|| !md::decodeFactoryFlashCache(result.flash, result.cache, _rom.data()))
			return {};
		return result;
	}

	md::Rom loadStateRom(const std::vector<uint8_t>& _romData,
		const std::string& _romName,
		const md::MachineModel _model)
	{
		if(!_romData.empty())
		{
			md::Rom rom(_romData, _romName);
			if(rom.isValid() && md::RomLoader::isRomForModel(rom.data(), _model))
				return rom;
		}
		return md::RomLoader::findROM(_model);
	}
}

namespace md
{
	Device::Device(const synthLib::DeviceCreateParams& _params,
		const std::vector<uint8_t>& _initialPatchRam)
		: synthLib::Device(_params)
		, m_model(machineModelFromDeviceCustomData(_params.customData))
		, m_frontPanelPublisher(std::make_shared<FrontPanelPublisher>())
		, m_preparationContext(new PreparationContext(_params, m_model))
		, m_mdFlashCacheFilename(mdFlashCacheFilename(_params, m_model))
		, m_sysexDeviceId(g_sysexDeviceIds.fetch_add(1, std::memory_order_relaxed) + 1)
	{
		auto initialFlash = loadInitialMdFlash(_params, m_model);
		m_hardware = std::make_unique<Hardware>(_params.romData, _params.romName, m_model,
			loadInitialPatchRam(_params, m_model, _initialPatchRam), m_frontPanelPublisher,
			initialFlash.flash, initialFlash.cache);
	}

	bool Device::captureFactoryFlashCachePersistence(std::string& _filename,
		FactoryFlashSnapshot& _snapshot, std::string& _error)
	{
		_error.clear();
		_filename.clear();
		_snapshot = {};
		if(m_mdFlashCacheFilename.empty() || !m_hardware)
		{
			_error = "factory cache has no writable destination";
			return false;
		}
		if(!m_hardware->factoryFlashCacheReady())
		{
			_error = "factory cache is not ready";
			return false;
		}
		if(!m_hardware->copyFactoryFlashSnapshot(_snapshot))
		{
			_error = "validated factory cache could not be captured";
			return false;
		}
		_filename = m_mdFlashCacheFilename;
		return true;
	}

	bool Device::materializeFactoryFlashCache(FactoryFlashSnapshot& _snapshot,
		const std::shared_ptr<const PreparationContext>& _context,
		std::string& _error)
	{
		_error.clear();
		if(!_snapshot.cache.empty() || _snapshot.baseline.empty())
			return true;
		if(!_context)
		{
			_error = "factory cache has no preparation context";
			return false;
		}
		auto rom = loadStateRom(_context->m_romData, _context->m_romName,
			_context->m_model);
		if(!rom.isValid() || !encodeFactoryFlashCache(_snapshot.cache,
			_snapshot.baseline, rom.data()))
		{
			_error = "validated factory cache could not be encoded";
			return false;
		}
		_snapshot.baseline.clear();
		return true;
	}

	bool Device::writeFactoryFlashCachePersistence(const std::string& _filename,
		const std::vector<uint8_t>& _cache, std::string& _error)
	{
		_error.clear();
		if(_filename.empty() || _cache.empty())
		{
			_error = "factory cache persistence data is incomplete";
			return false;
		}

		// Project activity can never update a valid cache. Invalid or ROM-mismatched
		// data is replaced atomically so one damaged cache cannot poison every boot.
		// This method performs only filesystem work and is called outside the device
		// lock by the processor's first-run service.
		std::vector<uint8_t> existing;
		const bool exists = baseLib::filesystem::readFile(existing, _filename);
		if(exists && existing == _cache)
			return true;
		baseLib::filesystem::createDirectory(
			baseLib::filesystem::getPath(_filename));
		const bool written = exists
			? baseLib::filesystem::writeFileAtomic(_filename, _cache)
			: baseLib::filesystem::writeFileExclusive(_filename, _cache);
		if(written)
		{
			std::fprintf(stderr, "[MD] stored UW factory cache: %s\n",
				_filename.c_str());
			return true;
		}
		// A concurrent first-run instance may have won the exclusive create.
		existing.clear();
		if(baseLib::filesystem::readFile(existing, _filename) && existing == _cache)
			return true;
		_error = "could not store UW factory cache at " + _filename;
		return false;
	}

	Device::~Device()
	{
		if(m_renderAhead)
		{
			std::vector<synthLib::SMidiEvent> pending;
			m_renderAhead->stop(pending);
			m_renderAhead.reset();
		}
	}

	float Device::getSamplerate() const
	{
		return g_samplerate;
	}

	bool Device::isValid() const
	{
		return m_hardware->isValid();
	}

	bool Device::getState(std::vector<uint8_t>& _state, synthLib::StateType _type)
	{
		const std::lock_guard machineLock(m_machineMutex);
		if(isProjectStateRestorePending() && _type == m_requestedStateType
			&& m_requestedState)
		{
			_state.insert(_state.end(), m_requestedState->begin(), m_requestedState->end());
			return true;
		}

		auto* stateHardware = m_hardware.get();
		if(m_deferredPreparedState && m_deferredPreparedState->m_hardware)
			stateHardware = m_deferredPreparedState->m_hardware.get();
		const auto patchRam = stateHardware->copyPatchRam();
		if(m_model == MachineModel::Monomachine)
			return encodeState(_state, patchRam, m_model, _type,
				stateHardware->copyUserFlash());
		std::vector<uint8_t> factoryBaseline;
		if(stateHardware->copyFactoryFlashBaseline(factoryBaseline))
			return encodeStateWithFactoryBaseline(_state, patchRam,
				stateHardware->copyFlashData(),
				factoryBaseline, stateHardware->flashBaseline(), m_model, _type);
		FlashSectorOverlay pending;
		if(stateHardware->copyPendingFlashOverlay(pending))
			return encodeState(_state, patchRam, pending,
				stateHardware->flashBaseline(), m_model, _type);
		// If interaction happened before the first machine-local baseline was
		// captured, preserve a complete flash image. An absolute sector set records
		// ROM-equal deletions and lets the replacement boot coherently without waiting
		// for another factory-initialization pass.
		return encodeState(_state, patchRam, stateHardware->copyFlashData(),
			stateHardware->flashBaseline(), stateHardware->flashBaseline(), m_model, _type);
	}

	bool Device::setState(const std::vector<uint8_t>& _state, synthLib::StateType _type)
	{
		auto transaction = beginStateTransaction(
			std::make_shared<const std::vector<uint8_t>>(_state), _type);
		if(!transaction)
			return false;
		const auto preparationSucceeded = transaction->prepare();
		return finishStateTransaction(*transaction) && preparationSucceeded;
	}

	bool Device::StateTransactionImpl::prepare()
	{
		// Release an interrupted candidate before constructing the replacement. Both
		// operations can tear down or create a complete emulated machine.
		m_displaced.reset();
		m_prepared = m_state
			? Device::prepareState(m_context, *m_state, m_type,
				m_factoryFlash, &m_error) : nullptr;
		if(!m_state)
			m_error = "The project state payload is missing.";
		return m_prepared != nullptr;
	}

	std::unique_ptr<synthLib::Device::StateTransaction> Device::beginStateTransaction(
		std::shared_ptr<const std::vector<uint8_t>> _state,
		const synthLib::StateType _type)
	{
		if(!_state)
			return {};
		const std::lock_guard machineLock(m_machineMutex);
		FactoryFlashSnapshot factoryFlash;
		if(m_model == MachineModel::Machinedrum)
			(void)m_hardware->copyFactoryFlashSnapshot(factoryFlash);
		auto displaced = std::move(m_deferredPreparedState);
		++m_deferredStateGeneration;
		m_requestedState = _state;
		m_requestedStateType = _type;
		m_restoreStatus = ProjectStateRestoreStatus::Preparing;
		m_restoreError.clear();
		auto transaction = std::unique_ptr<StateTransactionImpl>(
			new StateTransactionImpl(m_preparationContext, std::move(_state), _type,
				std::move(factoryFlash), m_deferredStateGeneration,
				std::move(displaced)));
		// A state-load attempt invalidates confirmations and interrupts an import,
		// even if preparing that state later fails. Never continue writing an old
		// file into a newly selected project. The UART cancellation drains normally.
		(void)m_hardware->cancelMidiSysexTransfer(transaction->m_retiredSysex);
		return transaction;
	}

	bool Device::finishStateTransaction(synthLib::Device::StateTransaction& _transaction)
	{
		const std::lock_guard machineLock(m_machineMutex);
		auto* const transaction = dynamic_cast<StateTransactionImpl*>(&_transaction);
		if(!transaction || transaction->m_context != m_preparationContext
			|| transaction->m_generation != m_deferredStateGeneration)
			return false;
		if(!transaction->m_prepared)
		{
			failProjectStateRestore(transaction->m_error.empty()
				? "The project state could not be prepared."
				: transaction->m_error);
			return false;
		}
		transaction->m_prepared->m_hardware->requestRamRecordingMode(m_ramRecordingMode);
		if(transaction->m_prepared->m_hardware->isProjectStateRestorePending())
		{
			m_deferredPreparedState = std::move(transaction->m_prepared);
			m_restoreStatus = ProjectStateRestoreStatus::Initializing;
			return true;
		}
		if(!commitPreparedState(*transaction->m_prepared))
		{
			failProjectStateRestore("The prepared project state could not be committed.");
			return false;
		}
		clearProjectStateRestore();
		return true;
	}

	std::unique_ptr<Device::PreparedState> Device::takeFinishedDeferredState(
		uint64_t& _generation)
	{
		if(!m_deferredPreparedState || !m_deferredPreparedState->m_hardware
			|| m_restoreStatus != ProjectStateRestoreStatus::Initializing
			|| m_deferredPreparedState->m_hardware->isProjectStateRestorePending())
			return {};
		_generation = m_deferredStateGeneration;
		m_restoreStatus = ProjectStateRestoreStatus::Finalizing;
		return std::move(m_deferredPreparedState);
	}

	std::unique_ptr<Device::PreparedState> Device::makeDeferredStateReboot(
		const PreparedState& _validated)
	{
		if(!_validated.m_context || !_validated.m_hardware
			|| !_validated.m_hardware->isValid()
			|| _validated.m_hardware->isProjectStateRestorePending())
			return {};
		const auto cache = _validated.m_hardware->copyFactoryFlashCache();
		if(cache.empty())
			return {};
		auto replacement = std::make_unique<Hardware>(
			_validated.m_context->m_romData, _validated.m_context->m_romName,
			_validated.m_context->m_model, _validated.m_hardware->copyPatchRam(),
			std::shared_ptr<FrontPanelPublisher>{},
			_validated.m_hardware->copyFlashData(), cache);
		if(!replacement->isValid())
			return {};
		return std::unique_ptr<PreparedState>(new PreparedState(
			_validated.m_context, std::move(replacement), true));
	}

	std::unique_ptr<Device::PreparedState> Device::prepareState(
		std::shared_ptr<const PreparationContext> _context,
		const std::vector<uint8_t>& _state, const synthLib::StateType _type,
		const FactoryFlashSnapshot& _factoryFlash, std::string* const _error)
	{
		const auto fail = [_error](const char* const _message)
		{
			if(_error)
				*_error = _message;
			return std::unique_ptr<PreparedState>{};
		};
		if(_error)
			_error->clear();
		if(!_context)
			return fail("The project state has no preparation context.");

		std::vector<uint8_t> patchRam;
		std::vector<uint8_t> initialFlash;
		bool containsFlash = false;
		if(_context->m_model == MachineModel::Monomachine)
		{
			DecodedState decoded;
			if(!decodeState(decoded, _state, {}, _context->m_model, _type))
				return fail("The Monomachine project payload is invalid or incompatible.");
			patchRam = std::move(decoded.patchRam);
			initialFlash = std::move(decoded.userFlash);
			containsFlash = decoded.containsFlash;
		}
		else
		{
			auto stateRom = loadStateRom(_context->m_romData, _context->m_romName,
				_context->m_model);
			if(!stateRom.isValid())
				return fail("The Machinedrum firmware needed to restore this project is unavailable or invalid.");
			DecodedState decoded;
			if(!decodeState(decoded, _state, stateRom.data(), _context->m_model, _type))
				return fail("The Machinedrum project payload is corrupt, incompatible, or belongs to different firmware.");
			patchRam = std::move(decoded.patchRam);
			containsFlash = decoded.containsFlash;
			auto factory = loadInitialMdFlash(stateRom,
				mdFlashCacheFilename(_context->m_homePath, _context->m_model));
			if(factory.cache.empty() && !_factoryFlash.cache.empty())
			{
				if(!decodeFactoryFlashCache(factory.flash, _factoryFlash.cache,
					stateRom.data()))
					return fail("The captured Machinedrum factory-flash cache is invalid for this firmware.");
				factory.cache = _factoryFlash.cache;
			}
			else if(factory.cache.empty() && !_factoryFlash.baseline.empty())
			{
				factory.flash = _factoryFlash.baseline;
				if(!encodeFactoryFlashCache(factory.cache, factory.flash,
					stateRom.data()))
					return fail("The Machinedrum factory-flash baseline could not be prepared.");
			}
			FlashSectorOverlay pending;
			if(containsFlash)
			{
				if(!factory.flash.empty())
				{
					if(!applyFlashOverlay(initialFlash, decoded.flashOverlay,
						factory.flash, stateRom.data()))
						return fail("The project sample-flash overlay does not match the Machinedrum factory baseline.");
				}
				else if(decoded.flashOverlay.sectors.size()
					== g_romSize / g_uwFlashSectorSize)
				{
					// A complete overlay is baseline-independent. Materialize it before
					// the replacement starts so firmware boots from one coherent project.
					if(!applyFlashOverlay(initialFlash, decoded.flashOverlay,
						stateRom.data(), stateRom.data()))
						return fail("The complete project sample-flash image could not be materialized.");
				}
				else
					pending = std::move(decoded.flashOverlay);
			}
			else if(!factory.flash.empty())
				initialFlash = factory.flash;

			auto replacement = std::make_unique<Hardware>(
				_context->m_romData, _context->m_romName, _context->m_model, patchRam,
				std::shared_ptr<FrontPanelPublisher>{},
				initialFlash, factory.cache, pending);
			if(!replacement->isValid())
				return fail("The replacement Machinedrum machine rejected the restored firmware or memory image.");
			return std::unique_ptr<PreparedState>(
				new PreparedState(std::move(_context), std::move(replacement),
					containsFlash));
		}

		auto replacement = std::make_unique<Hardware>(
			_context->m_romData, _context->m_romName, _context->m_model, patchRam,
			std::shared_ptr<FrontPanelPublisher>{}, std::vector<uint8_t>{},
			std::vector<uint8_t>{}, FlashSectorOverlay{}, initialFlash);
		if(!replacement->isValid())
			return fail("The replacement Monomachine rejected the restored firmware or memory image.");
		return std::unique_ptr<PreparedState>(
			new PreparedState(std::move(_context), std::move(replacement),
				containsFlash));
	}

	bool Device::commitPreparedState(PreparedState& _prepared)
	{
		if(_prepared.m_committed || _prepared.m_context != m_preparationContext
			|| !_prepared.m_hardware
			|| !_prepared.m_hardware->isValid())
			return false;

		const auto clockPercent = getDspClockPercent();
		_prepared.m_hardware->requestRamRecordingMode(m_ramRecordingMode);
		_prepared.m_hardware->getDspMixer().getPeriph().getEssiClock()
			.setSpeedPercent(clockPercent);
		if(m_model == MachineModel::Machinedrum && !_prepared.m_containsFlash)
		{
			// Patch-only and legacy states preserve the current sample flash. Both
			// machines are stopped under the outer Device lock, so exchange ownership
			// of the multi-megabyte backing stores and factory-capture progress in O(1).
			if(!_prepared.m_hardware->exchangePersistentFlashState(*m_hardware))
				return false;
		}

		m_frontPanelPublisher->reset();
		_prepared.m_hardware->setFrontPanelPublisher(m_frontPanelPublisher);
		m_hardware.swap(_prepared.m_hardware);
		++m_hardwareEpoch;
		_prepared.m_committed = true;
		return true;
	}

	void Device::setRamRecordingMode(const RamRecordingMode _mode)
	{
		const std::lock_guard machineLock(m_machineMutex);
		m_ramRecordingMode = m_model == MachineModel::Machinedrum
			? _mode : RamRecordingMode::Original;
		if(m_hardware)
			m_hardware->requestRamRecordingMode(m_ramRecordingMode);
		if(m_deferredPreparedState && m_deferredPreparedState->m_hardware)
			m_deferredPreparedState->m_hardware->requestRamRecordingMode(m_ramRecordingMode);
	}

	bool Device::commitDeferredStateRestore(PreparedState& _prepared,
		const uint64_t _generation)
	{
		if(_generation != m_deferredStateGeneration
			|| m_restoreStatus != ProjectStateRestoreStatus::Finalizing)
			return false;
		if(!commitPreparedState(_prepared))
		{
			failProjectStateRestore("The validated project state could not be committed.");
			return false;
		}
		clearProjectStateRestore();
		return true;
	}

	bool Device::rejectDeferredStateRestore(const uint64_t _generation,
		std::string _error)
	{
		if(_generation != m_deferredStateGeneration
			|| m_restoreStatus != ProjectStateRestoreStatus::Finalizing)
			return false;
		failProjectStateRestore(std::move(_error));
		return true;
	}

	void Device::clearProjectStateRestore()
	{
		m_deferredPreparedState.reset();
		m_requestedState.reset();
		m_restoreStatus = ProjectStateRestoreStatus::Idle;
		m_restoreError.clear();
	}

	void Device::failProjectStateRestore(std::string _error)
	{
		m_deferredPreparedState.reset();
		m_requestedState.reset();
		m_restoreStatus = ProjectStateRestoreStatus::Failed;
		m_restoreError = std::move(_error);
	}

	bool Device::matchesUserSysexImport(const SysexImportTicket& ticket) const
	{
		return ticket.request && ticket == m_sysexTicket && ticket.device == m_sysexDeviceId
			&& ticket.hardware == m_hardwareEpoch && ticket.restore == m_deferredStateGeneration;
	}

	std::optional<SysexImportTicket> Device::beginUserSysexImport()
	{
		if(isProjectStateRestorePending() || m_hardware->isMidiSysexTransferActive()) return {};
		m_sysexTicket = {m_sysexDeviceId, m_hardwareEpoch, m_deferredStateGeneration, m_sysexTicket.request + 1};
		m_sysexStarted = m_sysexPendingCancelled = false;
		return m_sysexTicket;
	}

	SysexImportStartResult Device::startUserSysexImport(const SysexImportTicket& ticket,
		PreparedMidiSysexTransfer& transfer, bool receiveModeConfirmed)
	{
		using Result = SysexImportStartResult;
		if(!matchesUserSysexImport(ticket) || m_sysexStarted || m_sysexPendingCancelled) return Result::StaleRequest;
		if(transfer.model() != m_model) return Result::WrongModel;
		if(isProjectStateRestorePending()) return Result::Restoring;
		if(!isValid() || !m_hardware->isFirmwareMidiReady()) return Result::NotReady;
		if(m_hardware->isFactoryFlashInitializationExpected()) return Result::Initializing;
		if(!receiveModeConfirmed && (m_model == MachineModel::Monomachine
			|| transfer.contains(MidiSysexMessageKind::SdsHeader))) return Result::ConfirmationRequired;
		if(!m_hardware->startMidiSysexTransfer(transfer)) return Result::Busy;
		m_sysexStarted = true;
		return Result::Started;
	}

	bool Device::cancelUserSysexImport(const SysexImportTicket& ticket, std::vector<uint8_t>& retired)
	{
		if(!matchesUserSysexImport(ticket) || m_sysexPendingCancelled) return false;
		if(!m_sysexStarted) { m_sysexPendingCancelled = true; return true; }
		return m_hardware->cancelMidiSysexTransfer(retired);
	}

	bool Device::resumeUserSysexImport(const SysexImportTicket& ticket, uint32_t transferId,
		size_t receiveStep, bool receiveModeConfirmed)
	{
		return matchesUserSysexImport(ticket) && m_sysexStarted && receiveModeConfirmed
			&& !isProjectStateRestorePending() && m_hardware->isFirmwareMidiReady()
			&& m_hardware->resumeMidiSysexReceiveMode(transferId, receiveStep);
	}

	bool Device::retireUserSysexImport(const SysexImportTicket& ticket, std::vector<uint8_t>& retired)
	{
		return matchesUserSysexImport(ticket) && m_sysexStarted
			&& m_hardware->retireMidiSysexTransferPayload(retired);
	}

	SysexImportProgress Device::userSysexImportProgress() const
	{
		SysexImportProgress result;
		result.ticket = m_sysexTicket;
		if(!m_sysexTicket.request) return result;
		if(!matchesUserSysexImport(m_sysexTicket))
		{
			result.stage = SysexImportStage::Invalidated;
			return result;
		}
		if(!m_sysexStarted)
		{
			result.stage = m_sysexPendingCancelled ? SysexImportStage::Cancelled : SysexImportStage::Preparing;
			return result;
		}
		static_cast<MidiSysexTransferProgress&>(result) = m_hardware->getMidiSysexTransferProgress();
		switch(result.state)
		{
		case MidiSysexTransferState::Complete: result.stage = SysexImportStage::DeliveredUnverified; break;
		case MidiSysexTransferState::Cancelled: result.stage = SysexImportStage::Cancelled; break;
		case MidiSysexTransferState::Failed: result.stage = SysexImportStage::Failed; break;
		case MidiSysexTransferState::WaitingForReceiveMode: result.stage = SysexImportStage::AwaitingReceiveMode; break;
		default: result.stage = SysexImportStage::Transferring; break;
		}
		return result;
	}

	uint32_t Device::getChannelCountIn()
	{
		return 2;
	}

	uint32_t Device::getChannelCountOut()
	{
		return 6;
	}

	bool Device::setDspClockPercent(const uint32_t _percent)
	{
		const std::lock_guard machineLock(m_machineMutex);
		return m_hardware->getDspMixer().getPeriph().getEssiClock().setSpeedPercent(_percent);
	}

	uint32_t Device::getDspClockPercent() const
	{
		return m_hardware->getDspMixer().getPeriph().getEssiClock().getSpeedPercent();
	}

	uint64_t Device::getDspClockHz() const
	{
		return m_hardware->getDspMixer().getPeriph().getEssiClock().getSpeedInHz();
	}

	namespace
	{
		// GEARMULATOR_MD_MIDI_TRACE=1 logs every MIDI message between host and machine
		bool midiTraceEnabled()
		{
			static const bool enabled = []
			{
				const auto* const v = std::getenv("GEARMULATOR_MD_MIDI_TRACE");
				return v && *v && *v != '0';
			}();
			return enabled;
		}

		void traceMidi(const char* _direction, const synthLib::SMidiEvent& _ev)
		{
			if(!midiTraceEnabled())
				return;
			if(_ev.sysex.empty())
				std::fprintf(stderr, "[MIDI %s] %02x %02x %02x\n", _direction, _ev.a, _ev.b, _ev.c);
			else
			{
				std::fprintf(stderr, "[MIDI %s] sysex %zu bytes:", _direction, _ev.sysex.size());
				for(size_t i = 0; i < _ev.sysex.size() && i < 12; ++i)
					std::fprintf(stderr, " %02x", _ev.sysex[i]);
				std::fprintf(stderr, "\n");
			}
		}
	}

	void Device::readMidiOut(std::vector<synthLib::SMidiEvent>& _midiOut)
	{
		const auto before = _midiOut.size();
		m_hardware->readMidiOut(_midiOut);
		if(midiTraceEnabled())
			for(size_t i = before; i < _midiOut.size(); ++i)
				traceMidi("out", _midiOut[i]);
	}

	void Device::process(const synthLib::TAudioInputs& _inputs, const synthLib::TAudioOutputs& _outputs,
		const size_t _size, const std::vector<synthLib::SMidiEvent>& _midiIn, std::vector<synthLib::SMidiEvent>& _midiOut)
	{
		if(!m_renderAhead)
		{
			synthLib::Device::process(_inputs, _outputs, _size, _midiIn, _midiOut);
			return;
		}

		// Same translation as the synchronous path; the events travel with this callback's chunk and are
		// scheduled by the worker at the same position within the block.
		_midiOut.clear();
		m_asyncMidi.clear();
		for(const auto& ev : _midiIn)
		{
			m_asyncTranslated.clear();
			getMidiTranslator().process(m_asyncTranslated, ev);
			for(auto& e : m_asyncTranslated)
				m_asyncMidi.push_back(e);
		}
		m_renderAhead->process(_inputs, _outputs, static_cast<uint32_t>(_size), m_asyncMidi, _midiOut, m_hostNonRealtime);
	}

	void Device::processAudio(const synthLib::TAudioInputs& _inputs, const synthLib::TAudioOutputs& _outputs, const size_t _samples)
	{
		renderMachine(_inputs, _outputs, static_cast<uint32_t>(_samples), getExtraLatencySamples());
	}

	void Device::renderMachine(const synthLib::TAudioInputs& _inputs, const synthLib::TAudioOutputs& _outputs,
		const uint32_t _frames, const uint32_t _latency)
	{
		m_hardware->processAudio(_inputs, _outputs, _frames, _latency);
		if(m_deferredPreparedState && m_deferredPreparedState->m_hardware
			&& m_deferredPreparedState->m_hardware->isProjectStateRestorePending())
		{
			synthLib::RealtimeInstrumentation::DeferredCandidateScope instrumentation(_frames);
			m_deferredPreparedState->m_hardware->advance(_frames);
		}
	}

	void Device::renderAheadChunk(std::vector<synthLib::SMidiEvent>& _midiIn, const synthLib::TAudioInputs& _inputs,
		const synthLib::TAudioOutputs& _outputs, const uint32_t _frames, std::vector<synthLib::SMidiEvent>& _midiOut)
	{
		// Worker thread, machine mutex held. Exactly what the synchronous path does for one host block; the
		// prefill in RenderAhead delays the result.
		for(const auto& ev : _midiIn)
		{
			traceMidi("in", ev);
			scheduleMidiEvent(ev, getExtraLatencySamples());
		}
		if(_frames)
			renderMachine(_inputs, _outputs, _frames, getExtraLatencySamples());
		readMidiOut(_midiOut);
	}

	void Device::extraLatencyChanged()
	{
		const std::lock_guard machineLock(m_machineMutex);
		m_hardware->retimeMidi(getExtraLatencySamples());
	}

	void Device::setRenderAheadFrames(const uint32_t _frames)
	{
		// Callers hold the Plugin lock (withDeviceLocked), so the audio callback is not inside process().
		const std::lock_guard machineLock(m_machineMutex);
		if(_frames == getRenderAheadFrames())
			return;

		// Everything handed to a running worker but not rendered yet is scheduled now, so switching never
		// loses a Note Off or a transport message.
		std::vector<synthLib::SMidiEvent> pending;
		if(m_renderAhead)
		{
			m_renderAhead->stop(pending);
			m_renderAhead.reset();
		}
		for(const auto& ev : pending)
			scheduleMidiEvent(ev, getExtraLatencySamples());

		if(_frames == 0)
			return;

		m_asyncMidi.reserve(1024);
		m_asyncTranslated.reserve(64);
		m_renderAhead = std::make_unique<RenderAhead>(m_machineMutex,
			[this](std::vector<synthLib::SMidiEvent>& _midiIn, const synthLib::TAudioInputs& _inputs,
				const synthLib::TAudioOutputs& _outputs, const uint32_t _frames, std::vector<synthLib::SMidiEvent>& _midiOut)
			{
				renderAheadChunk(_midiIn, _inputs, _outputs, _frames, _midiOut);
			},
			getChannelCountIn(), getChannelCountOut(), _frames + g_renderAheadSlackFrames, getSamplerate());
	}

	uint32_t Device::getRenderAheadFrames() const
	{
		return m_renderAhead ? m_renderAhead->getPrefillFrames() - g_renderAheadSlackFrames : 0;
	}

	bool Device::scheduleMidiEvent(const synthLib::SMidiEvent& _ev, const uint32_t _extraLatency)
	{
		if(_ev.sysex.empty())
		{
			const auto status = static_cast<uint8_t>(_ev.a & 0xf0);

			// Native Program Change selects a firmware pattern independently of the
			// host's preset list. Forward it by default so firmware can apply its own
			// receive-enable/channel settings. Embedders may explicitly opt out.
			if(m_model != MachineModel::Monomachine
				&& status == synthLib::M_PROGRAMCHANGE
				&& !m_nativeProgramChangesEnabled)
				return true;
		}

		return m_hardware->scheduleMidi(_ev, _extraLatency);
	}

	bool Device::sendMidi(const synthLib::SMidiEvent& _ev, std::vector<synthLib::SMidiEvent>&)
	{
		traceMidi("in", _ev);
		return scheduleMidiEvent(_ev, getExtraLatencySamples());
	}
}
