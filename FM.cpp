/*
 *   Copyright (C) 2020 by Jonathan Naylor G4KLX
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

#include "Config.h"
#include "Globals.h"
#include "FM.h"

CFM::CFM() :
m_callsign(),
m_rfAck(),
m_ctcssRX(),
m_ctcssTX(),
m_timeoutTone(),
m_state(FS_LISTENING),
m_callsignAtStart(false),
m_callsignAtEnd(false),
m_callsignAtLatch(false),
m_callsignTimer(),
m_timeoutTimer(),
m_holdoffTimer(),
m_kerchunkTimer(),
m_ackMinTimer(),
m_ackDelayTimer(),
m_hangTimer(),
m_filterStage1(  724,   1448,   724, 32768, -37895, 21352),//3rd order Cheby Filter 300 to 2700Hz, 0.2dB passband ripple, sampling rate 24kHz
m_filterStage2(32768,      0,-32768, 32768, -50339, 19052),
m_filterStage3(32768, -65536, 32768, 32768, -64075, 31460),
m_blanking(),
m_useCOS(true),
m_cosInvert(false),
m_rfAudioBoost(1U),
m_downsampler(128U),//Size might need adjustement
m_rxLevel(1),
m_inputRB(4800U),   // 200ms of audio
m_outputRB(2400U)   // 100ms of audio
{
  insertDelay(100U);
}

void CFM::samples(bool cos, const q15_t* samples, uint8_t length)
{
  if (m_useCOS) {
    if (m_cosInvert)
      cos = !cos;
  } else {
    cos = true;
  }

  clock(length);

  uint8_t i = 0U;
  for (; i < length; i++) {
    // ARMv7-M has hardware integer division 
    q15_t currentSample = q15_t((q31_t(samples[i]) << 8) / m_rxLevel);

    uint8_t ctcssState = m_ctcssRX.process(currentSample);

    if (!m_useCOS) {
      // Delay the audio by 100ms to better match the CTCSS detector output
      m_inputRB.put(currentSample);
      m_inputRB.get(currentSample);
    }

    if (CTCSS_NOT_READY(ctcssState) && m_modemState != STATE_FM) {
      //Not enough samples to determine if you have CTCSS, just carry on
      continue;
    } else if (CTCSS_READY(ctcssState) && m_modemState != STATE_FM) {
      //we had enough samples for CTCSS and we are in some other mode than FM
      bool validCTCSS = CTCSS_VALID(ctcssState);
      stateMachine(validCTCSS && cos);
      if (m_modemState != STATE_FM)
        continue;
    } else if (CTCSS_READY(ctcssState) && m_modemState == STATE_FM) {
      //We had enough samples for CTCSS and we are in FM mode, trigger the state machine
      bool validCTCSS = CTCSS_VALID(ctcssState);
      stateMachine(validCTCSS && cos);
      if (m_modemState != STATE_FM)
        break;
    } else if (CTCSS_NOT_READY(ctcssState) && m_modemState == STATE_FM && i == length - 1) {
      //Not enough samples for CTCSS but we already are in FM, trigger the state machine
      //but do not trigger the state machine on every single sample, save CPU!
      bool validCTCSS = CTCSS_VALID(ctcssState);
      stateMachine(validCTCSS && cos);
    }

    // Only let audio through when relaying audio
    if (m_state == FS_RELAYING || m_state == FS_KERCHUNK) {  
      // m_downsampler.addSample(currentSample);
      currentSample = m_blanking.process(currentSample);
      currentSample *= m_rfAudioBoost;
    } else {
      currentSample = 0;
    }

    if (!m_callsign.isRunning())
      currentSample += m_rfAck.getHighAudio();
    
    if (!m_rfAck.isRunning()) {
      if (m_state == FS_LISTENING)
        currentSample += m_callsign.getHighAudio();
      else
        currentSample += m_callsign.getLowAudio();
    }

    if (!m_callsign.isRunning() && !m_rfAck.isRunning())
      currentSample += m_timeoutTone.getAudio();

    currentSample = m_filterStage3.filter(m_filterStage2.filter(m_filterStage1.filter(currentSample)));

    currentSample += m_ctcssTX.getAudio();

    if (m_modemState == STATE_FM)
      m_outputRB.put(currentSample);
  }
}

void CFM::process()
{
  q15_t sample;
  while (io.getSpace() >= 3U && m_outputRB.get(sample))
    io.write(STATE_FM, &sample, 1U);
}

void CFM::reset()
{
  m_state = FS_LISTENING;

  m_callsignTimer.stop();
  m_timeoutTimer.stop();
  m_kerchunkTimer.stop();
  m_ackMinTimer.stop();
  m_ackDelayTimer.stop();
  m_hangTimer.stop();

  m_ctcssRX.reset();
  m_rfAck.stop();
  m_callsign.stop();
  m_timeoutTone.stop();

  m_outputRB.reset();
}

uint8_t CFM::setCallsign(const char* callsign, uint8_t speed, uint16_t frequency, uint8_t time, uint8_t holdoff, uint8_t highLevel, uint8_t lowLevel, bool callsignAtStart, bool callsignAtEnd, bool callsignAtLatch)
{
  m_callsignAtStart = callsignAtStart;
  m_callsignAtEnd   = callsignAtEnd;
  m_callsignAtLatch = callsignAtLatch;

  uint16_t holdoffTime  = holdoff * 60U;
  uint16_t callsignTime = time * 60U;

  m_holdoffTimer.setTimeout(holdoffTime, 0U);
  m_callsignTimer.setTimeout(callsignTime, 0U);

  if (holdoffTime > 0U)
    m_holdoffTimer.start();

  return m_callsign.setParams(callsign, speed, frequency, highLevel, lowLevel);
}

uint8_t CFM::setAck(const char* rfAck, uint8_t speed, uint16_t frequency, uint8_t minTime, uint16_t delay, uint8_t level)
{
  m_ackDelayTimer.setTimeout(0U, delay);

  if (minTime > 0U)
    m_ackMinTimer.setTimeout(minTime, delay);

  return m_rfAck.setParams(rfAck, speed, frequency, level, level);
}

uint8_t CFM::setMisc(uint16_t timeout, uint8_t timeoutLevel, uint8_t ctcssFrequency, uint8_t ctcssHighThreshold, uint8_t ctcssLowThreshold, uint8_t ctcssLevel, uint8_t kerchunkTime, uint8_t hangTime, bool useCOS, bool cosInvert, uint8_t rfAudioBoost, uint8_t maxDev, uint8_t rxLevel)
{
  m_useCOS    = useCOS;
  m_cosInvert = cosInvert;

  m_rfAudioBoost = q15_t(rfAudioBoost);

  m_timeoutTimer.setTimeout(timeout, 0U);
  m_kerchunkTimer.setTimeout(kerchunkTime, 0U);
  m_hangTimer.setTimeout(hangTime, 0U);

  m_timeoutTone.setParams(timeoutLevel);
  m_blanking.setParams(maxDev, timeoutLevel);

  m_rxLevel = rxLevel; //q15_t(255)/q15_t(rxLevel >> 1);

  uint8_t ret = m_ctcssRX.setParams(ctcssFrequency, ctcssHighThreshold, ctcssLowThreshold);
  if (ret != 0U)
    return ret;

  return m_ctcssTX.setParams(ctcssFrequency, ctcssLevel);
}

void CFM::stateMachine(bool validSignal)
{
  switch (m_state) {
    case FS_LISTENING:
      listeningState(validSignal);
      break;
    case FS_KERCHUNK:
      kerchunkState(validSignal);
      break;
    case FS_RELAYING:
      relayingState(validSignal);
      break;
    case FS_RELAYING_WAIT:
      relayingWaitState(validSignal);
      break;
    case FS_TIMEOUT:
      timeoutState(validSignal);
      break;
    case FS_TIMEOUT_WAIT:
      timeoutWaitState(validSignal);
      break;
    case FS_HANG:
      hangState(validSignal);
      break;
    default:
      break;
  }

  if (m_state == FS_LISTENING && m_modemState == STATE_FM) {
    if (!m_callsign.isWanted() && !m_rfAck.isWanted()) {
      DEBUG1("Change to STATE_IDLE");
      m_modemState = STATE_IDLE;
      m_callsignTimer.stop();
      m_timeoutTimer.stop();
      m_kerchunkTimer.stop();
      m_ackMinTimer.stop();
      m_ackDelayTimer.stop();
      m_hangTimer.stop();
    }
  }
}

void CFM::clock(uint8_t length)
{
  m_callsignTimer.clock(length);
  m_timeoutTimer.clock(length);
  m_holdoffTimer.clock(length);
  m_kerchunkTimer.clock(length);
  m_ackMinTimer.clock(length);
  m_ackDelayTimer.clock(length);
  m_hangTimer.clock(length);
}

void CFM::listeningState(bool validSignal)
{
  if (validSignal) {
    if (m_kerchunkTimer.getTimeout() > 0U) {
      DEBUG1("State to KERCHUNK");
      m_state = FS_KERCHUNK;
      m_kerchunkTimer.start();
      if (m_callsignAtStart && !m_callsignAtLatch)
        sendCallsign();
    } else {
      DEBUG1("State to RELAYING");
      m_state = FS_RELAYING;
      if (m_callsignAtStart)
        sendCallsign();
    }

    insertSilence(50U);

    beginRelaying();

    m_callsignTimer.start();

    io.setDecode(true);
    io.setADCDetection(true);

    DEBUG1("Change to STATE_FM");
    m_modemState = STATE_FM;
  }
}

void CFM::kerchunkState(bool validSignal)
{
  if (validSignal) {
    if (m_kerchunkTimer.hasExpired()) {
      DEBUG1("State to RELAYING");
      m_state = FS_RELAYING;
      m_kerchunkTimer.stop();
      if (m_callsignAtStart && m_callsignAtLatch) {
        sendCallsign();
        m_callsignTimer.start();
      }
    }
  } else {
    io.setDecode(false);
    io.setADCDetection(false);

    DEBUG1("State to LISTENING");
    m_state = FS_LISTENING;
    m_kerchunkTimer.stop();
    m_timeoutTimer.stop();
    m_ackMinTimer.stop();
    m_callsignTimer.stop();
  }
}

void CFM::relayingState(bool validSignal)
{
  if (validSignal) {
    if (m_timeoutTimer.isRunning() && m_timeoutTimer.hasExpired()) {
      DEBUG1("State to TIMEOUT");
      m_state = FS_TIMEOUT;
      m_ackMinTimer.stop();
      m_timeoutTimer.stop();
      m_timeoutTone.start();
    }
  } else {
    io.setDecode(false);
    io.setADCDetection(false);

    DEBUG1("State to RELAYING_WAIT");
    m_state = FS_RELAYING_WAIT;
    m_ackDelayTimer.start();
  }

  if (m_callsignTimer.isRunning() && m_callsignTimer.hasExpired()) {
    sendCallsign();
    m_callsignTimer.start();
  }
}

void CFM::relayingWaitState(bool validSignal)
{
  if (validSignal) {
    io.setDecode(true);
    io.setADCDetection(true);

    DEBUG1("State to RELAYING");
    m_state = FS_RELAYING;
    m_ackDelayTimer.stop();
  } else {
    if (m_ackDelayTimer.isRunning() && m_ackDelayTimer.hasExpired()) {
      DEBUG1("State to HANG");
      m_state = FS_HANG;

      if (m_ackMinTimer.isRunning()) {
        if (m_ackMinTimer.hasExpired()) {
          DEBUG1("Send ack");
          m_rfAck.start();
          m_ackMinTimer.stop();
        }
      } else {
          DEBUG1("Send ack");
          m_rfAck.start();
          m_ackMinTimer.stop();
      }

      m_ackDelayTimer.stop();
      m_timeoutTimer.stop();
      m_hangTimer.start();
    }
  }

  if (m_callsignTimer.isRunning() && m_callsignTimer.hasExpired()) {
    sendCallsign();
    m_callsignTimer.start();
  }
}

void CFM::hangState(bool validSignal)
{
  if (validSignal) {
    io.setDecode(true);
    io.setADCDetection(true);

    DEBUG1("State to RELAYING");
    m_state = FS_RELAYING;
    DEBUG1("Stop ack");
    m_rfAck.stop();
    beginRelaying();
  } else {
    if (m_hangTimer.isRunning() && m_hangTimer.hasExpired()) {
      DEBUG1("State to LISTENING");
      m_state = FS_LISTENING;
      m_hangTimer.stop();

      if (m_callsignAtEnd)
        sendCallsign();

      m_callsignTimer.stop();
    }
  }

  if (m_callsignTimer.isRunning() && m_callsignTimer.hasExpired()) {
    sendCallsign();
    m_callsignTimer.start();
  }
}

void CFM::timeoutState(bool validSignal)
{
  if (!validSignal) {
    io.setDecode(false);
    io.setADCDetection(false);

    DEBUG1("State to TIMEOUT_WAIT");
    m_state = FS_TIMEOUT_WAIT;
    m_ackDelayTimer.start();
  }

  if (m_callsignTimer.isRunning() && m_callsignTimer.hasExpired()) {
    sendCallsign();
    m_callsignTimer.start();
  }
}

void CFM::timeoutWaitState(bool validSignal)
{
  if (validSignal) {
    io.setDecode(true);
    io.setADCDetection(true);

    DEBUG1("State to TIMEOUT");
    m_state = FS_TIMEOUT;
    m_ackDelayTimer.stop();
  } else {
    if (m_ackDelayTimer.isRunning() && m_ackDelayTimer.hasExpired()) {
      DEBUG1("State to HANG");
      m_state = FS_HANG;
      m_timeoutTone.stop();
      DEBUG1("Send ack");
      m_rfAck.start();
      m_ackDelayTimer.stop();
      m_ackMinTimer.stop();
      m_timeoutTimer.stop();
      m_hangTimer.start();
    }
  }

  if (m_callsignTimer.isRunning() && m_callsignTimer.hasExpired()) {
    sendCallsign();
    m_callsignTimer.start();
  }
}

void CFM::sendCallsign()
{
  if (m_holdoffTimer.isRunning()) {
    if (m_holdoffTimer.hasExpired()) {
      DEBUG1("Send callsign");
      m_callsign.start();
      m_holdoffTimer.start();
    }
  } else {
    DEBUG1("Send callsign");
    m_callsign.start();
  }
}

void CFM::beginRelaying()
{
  m_timeoutTimer.start();
  m_ackMinTimer.start();
}

void CFM::insertDelay(uint16_t ms)
{
  uint32_t nSamples = ms * 24U;

  for (uint32_t i = 0U; i < nSamples; i++)
    m_inputRB.put(0);
}

void CFM::insertSilence(uint16_t ms)
{
  uint32_t nSamples = ms * 24U;

  for (uint32_t i = 0U; i < nSamples; i++)
    m_outputRB.put(0);
}
