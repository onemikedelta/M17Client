/*
 *   Copyright (C) 2015-2021 by Jonathan Naylor G4KLX
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "M17TS.h"
#include "GitVersion.h"
#include "Version.h"
#include "Thread.h"
#include "Timer.h"
#include "Utils.h"
#include "Log.h"

#include <cassert>
#include <cstring>
#include <cstdio>

#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <pwd.h>

const char* DELIMITER = ":";

static bool m_killed = false;
static int  m_signal = 0;

static void sigHandler(int signum)
{
	m_killed = true;
	m_signal = signum;
}

const char* HEADER1 = "This software is for use on amateur radio networks only,";
const char* HEADER2 = "it is to be used for educational purposes only. Its use on";
const char* HEADER3 = "commercial networks is strictly prohibited.";
const char* HEADER4 = "Copyright(C) 2021 by Jonathan Naylor, G4KLX and others";

int main(int argc, char** argv)
{
	if (argc > 1) {
		for (int currentArg = 1; currentArg < argc; ++currentArg) {
			std::string arg = argv[currentArg];
			if ((arg == "-v") || (arg == "--version")) {
				::fprintf(stdout, "M17TS version %s git #%.7s\n", VERSION, gitversion);
				return 0;
			} else if (arg.substr(0,1) == "-") {
				::fprintf(stderr, "Usage: M17TS [-v|--version]\n");
				return 1;
			}
		}
	}

	::signal(SIGINT,  sigHandler);
	::signal(SIGTERM, sigHandler);
	::signal(SIGHUP,  sigHandler);

	int ret = 0;

	do {
		m_signal = 0;

		CM17TS* host = new CM17TS;
		ret = host->run();

		delete host;

		if (m_signal == 2)
			::LogInfo("M17TS-%s exited on receipt of SIGINT", VERSION);

		if (m_signal == 15)
			::LogInfo("M17TS-%s exited on receipt of SIGTERM", VERSION);

		if (m_signal == 1)
			::LogInfo("M17TS-%s restarted on receipt of SIGHUP", VERSION);
	} while (m_signal == 1);

	::LogFinalise();

	return ret;
}

CM17TS::CM17TS() :
m_conf(),
m_socket(NULL),
m_uart(NULL),
m_sockaddr(),
m_sockaddrLen(0U),
m_channels(),
m_destinations(),
m_modules(),
m_channelIdx(0U),
m_destinationIdx(0U),
m_moduleIdx(0U),
m_transmit(false),
m_slider(SI_NONE),
m_volume(50U),
m_micGain(50U),
m_source(),
m_text()
{
}

CM17TS::~CM17TS()
{
}

int CM17TS::run()
{
	bool ret = m_conf.read();
	if (!ret) {
		::fprintf(stderr, "M17TS: cannot read the configuration file\n");
		return 1;
	}

	bool m_daemon = m_conf.getDaemon();
	if (m_daemon) {
		// Create new process
		pid_t pid = ::fork();
		if (pid == -1) {
			::fprintf(stderr, "Couldn't fork() , exiting\n");
			return -1;
		} else if (pid != 0) {
			exit(EXIT_SUCCESS);
		}

		// Create new session and process group
		if (::setsid() == -1){
			::fprintf(stderr, "Couldn't setsid(), exiting\n");
			return -1;
		}

		// Set the working directory to the root directory
		if (::chdir("/") == -1){
			::fprintf(stderr, "Couldn't cd /, exiting\n");
			return -1;
		}

		// If we are currently root...
		if (getuid() == 0) {
			struct passwd* user = ::getpwnam("m17");
			if (user == NULL) {
				::fprintf(stderr, "Could not get the m17 user, exiting\n");
				return -1;
			}
			
			uid_t mmdvm_uid = user->pw_uid;
			gid_t mmdvm_gid = user->pw_gid;

			// Set user and group ID's to m17:m17
			if (setgid(mmdvm_gid) != 0) {
				::fprintf(stderr, "Could not set m17 GID, exiting\n");
				return -1;
			}

			if (setuid(mmdvm_uid) != 0) {
				::fprintf(stderr, "Could not set m17 UID, exiting\n");
				return -1;
			}
		    
			// Double check it worked (AKA Paranoia) 
			if (setuid(0) != -1) {
				::fprintf(stderr, "It's possible to regain root - something is wrong!, exiting\n");
				return -1;
			}
		}
	}

	ret = ::LogInitialise(m_daemon, ".", "M17TS", 1U, 1U, true);
	if (!ret) {
		::fprintf(stderr, "M17TS: unable to open the log file\n");
		return 1;
	}

	if (m_daemon) {
		::close(STDIN_FILENO);
		::close(STDOUT_FILENO);
		::close(STDERR_FILENO);
	}

	LogInfo(HEADER1);
	LogInfo(HEADER2);
	LogInfo(HEADER3);
	LogInfo(HEADER4);

	LogMessage("M17TS-%s is starting", VERSION);
	LogMessage("Built %s %s (GitID #%.7s)", __TIME__, __DATE__, gitversion);

	m_uart = new CUARTController(m_conf.getScreenPort(), m_conf.getScreenSpeed());
	ret = m_uart->open();
	if (!ret) {
		delete m_uart;
		LogError("Unable to open the screen");
		::LogFinalise();
		return 1;
	}

	if (CUDPSocket::lookup(m_conf.getDaemonAddress(), m_conf.getDaemonPort(), m_sockaddr, m_sockaddrLen) != 0) {
		LogError("Could not lookup the daemon address");
		::LogFinalise();
		return 1;
	}

	m_socket = new CUDPSocket(m_conf.getSelfAddress(), m_conf.getSelfPort());
	ret = m_socket->open();
	if (!ret) {
		delete m_socket;
		LogError("Unable to open the control socket");
		::LogFinalise();
		return 1;
	}

	m_volume  = m_conf.getVolume();
	setVolume(m_volume);

	m_micGain = m_conf.getMicGain();
	setMicGain(m_micGain);

	LogMessage("M17TS-%s is running", VERSION);

	CTimer timer(1000U, 0U, 100U);
	timer.start();

	sendCommand("bkcmd=2");

	gotoPage2();

	uint8_t screenBuffer[50U];
	uint8_t endBuffer[3U] = {0x00U, 0x00U, 0x00U};
	unsigned int screenIdx = 0U;

	while (!m_killed) {
		char command[100U];
		sockaddr_storage sockaddr;
		unsigned int sockaddrLen = 0U;
		int ret = m_socket->read(command, 100U, sockaddr, sockaddrLen);
		if (ret > 0) {
			command[ret] = '\0';
			parseCommand(command);
		}

		uint8_t c;
		ret = m_uart->read(&c, 1U);
		if (ret > 0) {
			screenBuffer[screenIdx++] = c;

			endBuffer[2U] = endBuffer[1U];
			endBuffer[1U] = endBuffer[0U];
			endBuffer[0U] = c;

			if (::memcmp(endBuffer, "\xFF\xFF\xFF", 3U) == 0) {
				parseScreen(screenBuffer, screenIdx);
				::memset(endBuffer, 0x00U, 3U);
				screenIdx = 0U;
			}
		}

		timer.clock(20U);
		if (timer.isRunning() && timer.hasExpired()) {
			if (m_channels.empty()) {
				getChannels();
				timer.start();
			} else if (m_destinations.empty()) {
				getDestinations();
				timer.start();
			} else {
				timer.stop();
			}
		}

		CThread::sleep(20U);
	}

	m_conf.write();

	m_socket->close();
	m_uart->close();

	delete m_uart;
	delete m_socket;

	::LogFinalise();

	return 0;
}

void CM17TS::parseCommand(char* command)
{
	assert(command != NULL);

	std::vector<char *> ptrs;

	char* s = command;
	char* p;
	while ((p = ::strtok(s, DELIMITER)) != NULL) {
		s = NULL;
		ptrs.push_back(p);
	}

	if (::strcmp(ptrs.at(0U), "CHAN") == 0) {
		m_channels.clear();
		for (unsigned int i = 1U; i < ptrs.size(); i++) {
			std::string channel = std::string(ptrs.at(i));
			m_channels.push_back(channel);
		}
		selectChannel();
	} else if (::strcmp(ptrs.at(0U), "DEST") == 0) {
		m_destinations.clear();
		for (unsigned int i = 1U; i < ptrs.size(); i++) {
			std::string destination = std::string(ptrs.at(i));
			m_destinations.push_back(destination);
		}
		selectDestination();
		selectModule();
	} else if (::strcmp(ptrs.at(0U), "RX") == 0) {
		bool end                = ::atoi(ptrs.at(1U)) == 1;
		std::string source      = std::string(ptrs.at(2U));
		std::string destination = std::string(ptrs.at(3U));
		showRX(end, source, destination);
	} else if (::strcmp(ptrs.at(0U), "TEXT") == 0) {
		std::string text = std::string(ptrs.at(1U));
		showText(text);
	} else if (::strcmp(ptrs.at(0U), "RSSI") == 0) {
		int rssi = ::atoi(ptrs.at(1U));
		showRSSI(rssi);
	}
}

void CM17TS::parseScreen(const uint8_t* command, unsigned int length)
{
	assert(command != NULL);

	if (command[0U] == 0x65U) {
		if (command[1U] == 0U) {
			if (command[2U] == 5U) {
				LogMessage("Page 0 CHAN_UP pressed");
				channelChanged(+1);
			} else if (command[2U] == 6U) {
				LogMessage("Page 0 CHAN_DOWN pressed");
				channelChanged(-1);
			} else if (command[2U] == 7U) {
				LogMessage("Page 0 DEST_UP pressed");
				destinationChanged(+1);
			} else if (command[2U] == 8U) {
				LogMessage("Page 0 DEST_DOWN pressed");
				destinationChanged(-1);
			} else if (command[2U] == 9U) {
				LogMessage("Page 0 MOD_UP pressed");
				moduleChanged(+1);
			} else if (command[2U] == 10U) {
				LogMessage("Page 0 MOD_DOWN pressed");
				moduleChanged(-1);
			} else if (command[2U] == 11U) {
				LogMessage("Page 0 RIGHT pressed");
				gotoPage1();
			} else if (command[2U] == 12U) {
				LogMessage("Page 0 LEFT pressed");
				gotoPage2();
			} else {
				CUtils::dump(2U, "Button press on page 0 from an unknown button", command, length);
			}
		} else if (command[1U] == 1U) {
			if (command[2U] == 2U) {
				LogMessage("Page 1 VOLUME adjusted");
				volumeChanged();
			} else if (command[2U] == 3U) {
				LogMessage("Page 1 MIC_GAIN adjusted");
				micGainChanged();
			} else if (command[2U] == 6U) {
				LogMessage("Page 1 RIGHT pressed");
				gotoPage2();
			} else if (command[2U] == 7U) {
				LogMessage("Page 1 LEFT pressed");
				gotoPage0();
			} else {
				CUtils::dump(2U, "Button press on page 1 from an unknown button", command, length);
			}
		} else if (command[1U] == 2U) {
			if (command[2U] == 2U) {
				LogMessage("Page 2 RIGHT pressed");
				gotoPage0();
			} else if (command[2U] == 3U) {
				LogMessage("Page 2 LEFT pressed");
				gotoPage1();
			} else if (command[2U] == 4U) {
				LogMessage("Page 2 TRANSMIT pressed");
				transmit();
			} else {
				CUtils::dump(2U, "Button press on page 2 from an unknown button", command, length);
			}
		} else {
			CUtils::dump(2U, "Button press from an unknown page", command, length);
		}
	} else if (command[0U] == 0x71U) {
		switch (m_slider) {
			case SI_VOLUME:
				m_volume = (uint32_t(command[4U]) << 24) | (uint32_t(command[3U]) << 16) | (uint32_t(command[2U]) << 8) | (uint32_t(command[1U]) << 0);
				setVolume(m_volume);
				m_slider = SI_NONE;
				break;
			case SI_MIC_GAIN:
				m_micGain = (uint32_t(command[4U]) << 24) | (uint32_t(command[3U]) << 16) | (uint32_t(command[2U]) << 8) | (uint32_t(command[1U]) << 0);
				setMicGain(m_volume);
				m_slider = SI_NONE;
				break;
			default:
				// CUtils::dump(2U, "Unknown slider data from the screen", command, length);
				break;
		}
	} else {
		CUtils::dump(2U, "Unknown data from the screen", command, length);
	}
}

void CM17TS::channelChanged(int val)
{
	assert(!m_channels.empty());

	if (val == -1) {
		if (m_channelIdx == 0U)
			m_channelIdx = m_channels.size() - 1U;
		else
			m_channelIdx--;
	} else {
		m_channelIdx++;
		if (m_channelIdx == m_channels.size())
			m_channelIdx = 0U;
	}

	std::string channel = m_channels.at(m_channelIdx);

	char text[100U];
	::sprintf(text, "CHANNEL.txt=\"%s\"", channel.c_str());
	sendCommand(text);

	setChannel(channel);
}

void CM17TS::destinationChanged(int val)
{
	assert(!m_destinations.empty());

	if (val == -1) {
		if (m_destinationIdx == 0U)
			m_destinationIdx = m_destinations.size() - 1U;
		else
			m_destinationIdx--;
	} else {
		m_destinationIdx++;
		if (m_destinationIdx == m_destinations.size())
			m_destinationIdx = 0U;
	}

	std::string destination = m_destinations.at(m_destinationIdx);

	char text[100U];
	::sprintf(text, "DESTINATION.txt=\"%s\"", destination.c_str());
	sendCommand(text);
}

void CM17TS::moduleChanged(int val)
{
	assert(!m_modules.empty());

	if (val == -1) {
		if (m_moduleIdx == 0U)
			m_moduleIdx = m_modules.size() - 1U;
		else
			m_moduleIdx--;
	} else {
		m_moduleIdx++;
		if (m_moduleIdx == m_modules.size())
			m_moduleIdx = 0U;
	}

	char module = m_modules.at(m_moduleIdx);

	char text[100U];
	::sprintf(text, "MODULE.txt=\"%c\"", module);
	sendCommand(text);
}

void CM17TS::volumeChanged()
{
	m_slider = SI_VOLUME;
	sendCommand("get VOLUME.val");
}

void CM17TS::micGainChanged()
{
	m_slider = SI_MIC_GAIN;
	sendCommand("get MIC_GAIN.val");
}

void CM17TS::transmit()
{
	m_transmit = !m_transmit;

	setTransmit(m_transmit);
}

void CM17TS::showRX(bool end, const std::string& source, const std::string& destination)
{
	if (end) {
		m_source.clear();
		m_text.clear();

		sendCommand("SOURCE.txt=\"\"");
		sendCommand("TEXT.txt=\"\"");
	} else {
		m_source = source;

		char text[100U];
		::sprintf(text, "SOURCE.txt=\"%s\"", source.c_str());
		sendCommand(text);
	}
}

void CM17TS::showText(const std::string& value)
{
	m_text = value;

	char text[100U];
	::sprintf(text, "TEXT.txt=\"%s\"", value.c_str());
	sendCommand(text);
}

void CM17TS::showRSSI(int rssi)
{
}

void CM17TS::gotoPage0()
{
	sendCommand("page page0");

	if (!m_modules.empty()) {
		char text[100U];

		::sprintf(text, "CHANNEL.txt=\"%s\"", m_channels.at(m_channelIdx).c_str());
		sendCommand(text);

		::sprintf(text, "DESTINATION.txt=\"%s\"", m_destinations.at(m_destinationIdx).c_str());
		sendCommand(text);

		::sprintf(text, "MODULE.txt=\"%c\"", m_modules.at(m_moduleIdx));
		sendCommand(text);
	}
}

void CM17TS::gotoPage1()
{
	sendCommand("page page1");

	char text[100U];

	::sprintf(text, "VOLUME.val=%u", m_volume);
	sendCommand(text);

	::sprintf(text, "MIC_GAIN.val=%u", m_micGain);
	sendCommand(text);
}

void CM17TS::gotoPage2()
{
	sendCommand("page page2");

	char text[100U];

	if (!m_modules.empty()) {
		::sprintf(text, "CHANNEL.txt=\"%s\"", m_channels.at(m_channelIdx).c_str());
		sendCommand(text);

		::sprintf(text, "DESTINATION.txt=\"%s\"", m_destinations.at(m_destinationIdx).c_str());
		sendCommand(text);

		::sprintf(text, "MODULE.txt=\"%c\"", m_modules.at(m_moduleIdx));
		sendCommand(text);
	}

	::sprintf(text, "SOURCE.txt=\"%s\"", m_source.c_str());
	sendCommand(text);

	::sprintf(text, "TEXT.txt=\"%s\"", m_text.c_str());
	sendCommand(text);
}

bool CM17TS::getChannels()
{
	assert(m_socket != NULL);

	char buffer[20U];
	::strcpy(buffer, "CHAN");
	::strcat(buffer, DELIMITER);
	::strcat(buffer, "?");

	return m_socket->write(buffer, ::strlen(buffer), m_sockaddr, m_sockaddrLen);
}

bool CM17TS::setChannel(const std::string& channel)
{
	assert(m_socket != NULL);

	char buffer[20U];
	::strcpy(buffer, "CHAN");
	::strcat(buffer, DELIMITER);
	::strcat(buffer, channel.c_str());

	return m_socket->write(buffer, ::strlen(buffer), m_sockaddr, m_sockaddrLen);
}

bool CM17TS::getDestinations()
{
	assert(m_socket != NULL);

	char buffer[20U];
	::strcpy(buffer, "DEST");
	::strcat(buffer, DELIMITER);
	::strcat(buffer, "?");

	return m_socket->write(buffer, ::strlen(buffer), m_sockaddr, m_sockaddrLen);
}

bool CM17TS::setDestination(const std::string& destination)
{
	assert(m_socket != NULL);

	char buffer[20U];
	::strcpy(buffer, "DEST");
	::strcat(buffer, DELIMITER);
	::strcat(buffer, destination.c_str());

	return m_socket->write(buffer, ::strlen(buffer), m_sockaddr, m_sockaddrLen);
}

bool CM17TS::setVolume(unsigned int volume)
{
	assert(m_socket != NULL);

	char buffer[20U];
	::strcpy(buffer, "VOL");
	::strcat(buffer, DELIMITER);
	::sprintf(buffer + ::strlen(buffer), "%u", volume);

	return m_socket->write(buffer, ::strlen(buffer), m_sockaddr, m_sockaddrLen);
}

bool CM17TS::setMicGain(unsigned int micGain)
{
	assert(m_socket != NULL);

	char buffer[20U];
	::strcpy(buffer, "MIC");
	::strcat(buffer, DELIMITER);
	::sprintf(buffer + ::strlen(buffer), "%u", micGain);

	return m_socket->write(buffer, ::strlen(buffer), m_sockaddr, m_sockaddrLen);
}

bool CM17TS::setTransmit(bool transmit)
{
	assert(m_socket != NULL);

	char buffer[20U];
	::strcpy(buffer, "TX");
	::strcat(buffer, DELIMITER);
	::strcat(buffer, transmit ? "1" : "0");

	return m_socket->write(buffer, ::strlen(buffer), m_sockaddr, m_sockaddrLen);
}

void CM17TS::sendCommand(const char* command)
{
	assert(command != NULL);
	assert(m_uart != NULL);

	m_uart->write(command, ::strlen(command));
	m_uart->write("\xFF\xFF\xFF", 3U);
}

void CM17TS::selectChannel()
{
	m_channelIdx = 0xFFFFU;

	unsigned int n = 0U;
	for (const auto& it : m_channels) {
		if (it == m_conf.getChannel()) {
			m_channelIdx = n;
			break;
		}

		n++;
	}

	if (m_channelIdx == 0xFFFFU)
		m_channelIdx = 0U;

	std::string channel = m_channels.at(m_channelIdx);

	char text[100U];
	::sprintf(text, "CHANNEL.txt=\"%s\"", channel.c_str());
	sendCommand(text);

	m_conf.setChannel(channel);

	setChannel(channel);
}

void CM17TS::selectDestination()
{
	m_destinations.push_back("        ");

	m_destinationIdx = 0xFFFFU;

	unsigned int n = 0U;
	for (const auto& it : m_destinations) {
		if (it == m_conf.getDestination()) {
			m_destinationIdx = n;
			break;
		}

		n++;
	}

	if (m_destinationIdx == 0xFFFFU)
		m_destinationIdx = 0U;

	std::string destination = m_destinations.at(m_destinationIdx);

	char text[100U];
	::sprintf(text, "DESTINATION.txt=\"%s\"", destination.c_str());
	sendCommand(text);

	m_conf.setDestination(destination);
}

void CM17TS::selectModule()
{
	m_modules.push_back(' ');
	m_modules.push_back('A');
	m_modules.push_back('B');
	m_modules.push_back('C');
	m_modules.push_back('D');
	m_modules.push_back('E');
	m_modules.push_back('F');
	m_modules.push_back('G');
	m_modules.push_back('H');
	m_modules.push_back('I');
	m_modules.push_back('J');
	m_modules.push_back('K');
	m_modules.push_back('L');
	m_modules.push_back('M');
	m_modules.push_back('N');
	m_modules.push_back('O');
	m_modules.push_back('P');
	m_modules.push_back('Q');
	m_modules.push_back('R');
	m_modules.push_back('S');
	m_modules.push_back('T');
	m_modules.push_back('U');
	m_modules.push_back('V');
	m_modules.push_back('W');
	m_modules.push_back('X');
	m_modules.push_back('Y');
	m_modules.push_back('Z');

	m_moduleIdx = 0xFFFFU;

	unsigned int n = 0U;
	for (const auto& it : m_modules) {
		if (it == m_conf.getModule()) {
			m_moduleIdx = n;
			break;
		}

		n++;
	}

	if (m_moduleIdx == 0xFFFFU)
		m_moduleIdx = 0U;

	char module = m_modules.at(m_moduleIdx);

	char text[100U];
	::sprintf(text, "MODULE.txt=\"%c\"", module);
	sendCommand(text);

	m_conf.setModule(module);
}
