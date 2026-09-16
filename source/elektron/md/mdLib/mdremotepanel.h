#pragma once

#include "mdfrontpanel.h"
#include "mdpanel.h"
#include "mdtypes.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace networkLib
{
	class TcpServer;
	class TcpStream;
	class Stream;
}

namespace md
{
	/*	Serves the machine's front panel to a browser on the local network, a tablet typically.

		HTTP GET /            the panel web app (index.html or index-mm.html for the Monomachine, from the resource callback)
		HTTP GET /ws          WebSocket. The server pushes the panel state whenever it changes:
		                      'S', model byte, 1024 bytes LCD VRAM (half, page, column), 14 LED bank bytes.
		                      The client sends text messages:
		                        b <PanelControl name> <1|0>    button press / release
		                        e <PanelEncoder name> <delta>  encoder turned, signed step count
		                        p <PanelEncoder name> <1|0>    encoder pushed / released
		                        t <track 0..15>                select a track (Machinedrum, via sysex)
		                        m <machine id>                 assign a machine to the current track
		                      The server also pushes a text frame "M <json>" with the machine catalogue,
		                      the current track and the UW sample slot names whenever they change.
		                        hello                          ask for the current state right away
	*/
	class RemotePanelServer
	{
	public:
		struct Callbacks
		{
			std::function<bool(uint8_t _command, uint8_t _argument)> sendPanelEvent;
			std::function<FrontPanel()> snapshot;
			// complete sysex message including F0/F7, sent to the machine's MIDI in
			std::function<void(const std::vector<uint8_t>&)> sendSysex;
			// path without leading slash -> content. Returns false when unknown.
			std::function<bool(const std::string& _path, std::string& _data, std::string& _mime)> resource;
			// machine selector: catalogue and state as JSON (see mdmachines.h), and assignment to the current track
			std::function<std::string()> machineInfo;
			std::function<bool(uint16_t _machineId)> assignMachine;
		};

		RemotePanelServer(MachineModel _model, int _port, Callbacks _callbacks);
		~RemotePanelServer();

		// tries _port and the following ones, returns false if none could be bound
		bool start();
		void stop();

		bool isRunning() const { return m_tcpServer != nullptr; }
		int getPort() const { return m_port; }
		size_t getClientCount() const;

		static std::vector<uint8_t> encodeState(MachineModel _model, const FrontPanel& _panel);

	private:
		struct Client
		{
			std::shared_ptr<networkLib::TcpStream> stream;
			std::mutex writeMutex;
			std::vector<uint8_t> lastState;
			std::string lastMachineInfo;
			std::atomic<bool> websocket{false};
			std::atomic<bool> closed{false};
			std::unique_ptr<std::thread> thread;
		};

		void onClientConnected(std::unique_ptr<networkLib::TcpStream> _stream);
		void clientThreadFunc(const std::shared_ptr<Client>& _client);
		void publisherThreadFunc();

		bool serveHttp(Client& _client, const std::string& _method, const std::string& _path, const std::vector<std::pair<std::string, std::string>>& _headers);
		bool websocketLoop(Client& _client);
		bool sendWebSocketFrame(Client& _client, uint8_t _opcode, const uint8_t* _data, size_t _size);
		void handleMessage(const std::string& _message);
		void releaseAllRows();

		static bool readLine(networkLib::Stream& _stream, std::string& _line);
		static std::string mimeForPath(const std::string& _path);

		const MachineModel m_model;
		int m_port;
		Callbacks m_callbacks;

		std::unique_ptr<networkLib::TcpServer> m_tcpServer;
		std::unique_ptr<std::thread> m_publisherThread;
		std::atomic<bool> m_exit{false};

		mutable std::mutex m_clientsMutex;
		std::vector<std::shared_ptr<Client>> m_clients;

		std::mutex m_inputMutex;
		uint32_t m_infoTick = 0;
		std::atomic<bool> m_infoForce{false};
		PanelRowState m_rows;
	};
}
