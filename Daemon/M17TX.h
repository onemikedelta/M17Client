/*
 *   Copyright (C) 2020,2021 by Jonathan Naylor G4KLX
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

#if !defined(M17TX_H)
#define	M17TX_H

#include "codec2/codec2.h"
#include "M17Defines.h"
#include "RingBuffer.h"
#include "Defines.h"
#include "M17LSF.h"
#include "Modem.h"

#include <string>

class CM17TX {
public:
	CM17TX(const std::string& callsign, const std::string& text, CCodec2& codec2);
	~CM17TX();

	void setCAN(unsigned int can);

	void setDestination(const std::string& callsign);

	void setMicGain(unsigned int percentage);

	void write(const short* audio, bool end);

	unsigned int read(unsigned char* data);

private:
	CCodec2&                   m_codec2;
	std::string                m_source;
	std::string                m_destination;
	float                      m_micGain;
	unsigned int               m_can;
	bool                       m_transmit;
	CRingBuffer<unsigned char> m_queue;
	uint16_t                   m_frames;
	CM17LSF*                   m_currLSF;
	CM17LSF*                   m_textLSF;
	CM17LSF*                   m_gpsLSF;
	unsigned int               m_lsfN;

	void writeQueue(const unsigned char* data);

	void interleaver(const unsigned char* in, unsigned char* out) const;
	void decorrelator(const unsigned char* in, unsigned char* out) const;

	void addLinkSetupSync(unsigned char* data);
	void addStreamSync(unsigned char* data);
};

#endif