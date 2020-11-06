/*
 * File: alsa_client.cpp
 *
 *
 * Copyright 2020 Harald Postner <Harald at free_creations.de>.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "alsa_client.h"
#include "alsa_receiver_queue.h"

#include "alsa_util.h"
#include "spdlog/spdlog.h"
#include <alsa/asoundlib.h>
#include <regex>
#include <stdexcept>
#include <string>

namespace alsaClient {

/**
 * Implementation specific stuff.
 */
inline namespace impl {

static int g_portId{NULL_ID};                 ///< the ID-number of our ALSA input port
static snd_seq_t *g_sequencerHandle{nullptr}; ///< handle to access the ALSA sequencer
static snd_midi_event_t *g_midiEventParserHandle{
    nullptr};                            ///< handle to access the ALSA MIDI parser
static int g_clientId{NULL_ID};          ///< the client-number of this client
static State g_stateFlag{State::closed}; ///< the current state of the alsaClient
static std::mutex g_stateAccessMutex;    ///< protects g_stateFlag against race conditions.
static std::string g_connectTo;///< the name of a port we shall try to connect to

// this should be large enough to hold the largest MIDI message to be encoded by the
// AlsaMidiEventParser
constexpr int MAX_MIDI_EVENT_SIZE{16};

/**
 * The `g_onMonitorConnectionsHandler` is invoked on regular time intervals.
 */
OnMonitorConnectionsHandler g_onMonitorConnectionsHandler{nullptr};
void defaultConnectionsHandler(const std::string &connectTo);

/**
 * Returns a string representation of the given state.
 * @param state - the state
 * @return a string representation of the given state
 */
std::string stateAsString(State state) {
  switch (state) {
  case State::closed:
    return "closed";
  case State::idle:
    return "idle";
  case State::running:
    return "running";
  }
}

void tryToConnect(const std::string &designation) {
  if (designation.empty()) {
    return;
  }
  auto searchProfile = toProfile(SENDER_PORT, designation);
  PortID target = findPort(searchProfile, match);
  if (target == NULL_PORT_ID) {
    SPDLOG_INFO("no such port named {}", designation);
  }

  int err = snd_seq_connect_from(g_sequencerHandle, g_portId, target.client, target.port);
  if (ALSA_ERROR(err, "snd_seq_connect_from")) {
    throw std::runtime_error("ALSA cannot connect to port [" + designation + "]");
  }
}

static std::atomic<bool> g_monitoringActive{false}; ///< when false, ConnectionMonitoring will end.

void stopConnectionMonitoring() { g_monitoringActive = false; }
void stopInternal() noexcept {
  stopConnectionMonitoring();
  alsaClient::receiverQueue::stop();
}
void monitorLoop() {
  while (g_monitoringActive) {
    if (g_onMonitorConnectionsHandler) {
      g_onMonitorConnectionsHandler(g_connectTo);
    }
    std::this_thread::sleep_for(MONITOR_INTERVAL);
  }
}

void activateConnectionMonitoring() {
  g_monitoringActive = true;
  // start the monitoring thread.
  std::thread monitorThread(monitorLoop);
  monitorThread.detach();
}

void activateInternal() {
  activateConnectionMonitoring();
  alsaClient::receiverQueue::start(g_sequencerHandle);
}
int identifierStrToInt(const std::string &identifier) noexcept {
  try {
    return std::stoi(identifier);
  } catch (...) {
    return NULL_ID;
  }
}

std::string normalizedIdentifier(const std::string &identifier) noexcept {
  try {
    std::string noBlanks = std::regex_replace(identifier, std::regex{"\\s"}, "");
    std::string noSpecial = std::regex_replace(noBlanks, std::regex{"[^a-zA-Z0-9]"}, "_");
    return noSpecial;
  } catch (...) {
    return identifier; // an ugly result is better than no result at all.
  }
}

PortProfile toProfile(PortCaps caps, const std::string &designation) {
  PortProfile result;
  result.caps = caps;

  if (designation.empty()) {
    result.hasError = true;
    result.errorMessage << "Port-Identifier seems to be empty.";
    return result;
  }

  std::smatch matchResults;

  std::regex twoNamesSeparatedByColon{"^([^:]+):([^:]+)$"};
  regex_match(designation, matchResults, twoNamesSeparatedByColon);
  if (!matchResults.empty()) {
    result.hasColon = true;
    result.firstName = normalizedIdentifier(matchResults[1]);
    result.secondName = normalizedIdentifier(matchResults[2]);
    result.firstInt = identifierStrToInt(result.firstName);
    result.secondInt = identifierStrToInt(result.secondName);
    return result;
  }

  std::regex oneName{"^[^:]+$"};
  regex_match(designation, matchResults, oneName);
  if (!matchResults.empty()) {
    result.hasColon = false;
    result.firstName = normalizedIdentifier(matchResults[0]);
    result.secondName.clear();
    result.firstInt = identifierStrToInt(result.firstName);
    result.secondInt = NULL_ID;
    return result;
  }

  result.hasError = true;
  result.errorMessage << "Invalid Port-Identifier: " << designation;
  return result;
}

/**
 * The implementation of the MatchCallback function.
 * @param caps - the capabilities of the actual port.
 * @param port - the formal identity of he actual port.
 * @param clientName - the name of the client to which the actual port belongs.
 * @param portName - the name of the port.
 * @param requested - the profile of the requested port.
 * @return true if the actual port matches the requested profile, false otherwise.
 */
bool match(PortCaps caps, PortID port, const std::string &clientName, const std::string &portName,
           const PortProfile &requested) {
  if (!fulfills(caps, requested.caps)) {
    return false;
  }
  std::string normalClientName{normalizedIdentifier(clientName)};
  std::string normalPortName{normalizedIdentifier(portName)};

  if (requested.hasColon) {
    if (requested.firstInt == port.client) {
      if (requested.secondInt == port.port) {
        return true;
      }
      if (normalizedIdentifier(requested.secondName) == normalPortName) {
        return true;
      }
    }
    if (normalizedIdentifier(requested.firstName) == normalClientName) {
      if (normalizedIdentifier(requested.secondName) == normalPortName) {
        return true;
      }
      if (requested.secondInt == port.port) {
        return true;
      }
    }
  } else {
    if (normalizedIdentifier(requested.firstName) == normalPortName) {
      return true;
    }
  }

  return false;
}

/**
 * Search through all MIDI ports known to the ALSA sequencer.
 * @param requested - the profile describing the kind of searched port.
 * @param match - a function that tests whether the actual port fulfills the requests from the
 * profile.
 * @return the first port that fulfills the requests or `NULL_PORT_ID` when non found.
 */
PortID findPort(const PortProfile &requested, const MatchCallback &match) {
  if (requested.hasError) {
    return NULL_PORT_ID;
  }
  snd_seq_client_info_t *clientInfo;
  snd_seq_port_info_t *portInfo;
  snd_seq_client_info_alloca(&clientInfo);
  snd_seq_port_info_alloca(&portInfo);

  snd_seq_client_info_set_client(clientInfo, NULL_ID);
  while (snd_seq_query_next_client(g_sequencerHandle, clientInfo) >= 0) {
    int clientNr = snd_seq_client_info_get_client(clientInfo);
    std::string clientName{snd_seq_client_info_get_name(clientInfo)};
    snd_seq_port_info_set_client(portInfo, clientNr);
    snd_seq_port_info_set_port(portInfo, NULL_ID);
    while (snd_seq_query_next_port(g_sequencerHandle, portInfo) >= 0) {
      int portNr = snd_seq_port_info_get_port(portInfo);
      std::string portName{snd_seq_port_info_get_name(portInfo)};
      PortCaps caps = snd_seq_port_info_get_capability(portInfo);
      PortID portId{clientNr, portNr};

      if (match(caps, portId, clientName, portName, requested)) {
        return portId;
      }
    }
  }
  return NULL_PORT_ID;
}
/**
 * The not-synchronized version of `receiverPortGetConnections()`.
 * @return a list of the ports to which the ReceiverPort is connected. If no
 * port is currently connected or the ReceiverPort has not been created yet,
 * an empty list is returned.
 */
std::vector<PortID> receiverPortGetConnectionsInternal() {
  std::vector<PortID> result;

  snd_seq_addr_t thisAddr;
  thisAddr.client = g_clientId;
  thisAddr.port = g_portId;

  snd_seq_query_subscribe_t *subscriptionData;
  snd_seq_query_subscribe_alloca(&subscriptionData);
  snd_seq_query_subscribe_set_root(subscriptionData, &thisAddr);
  snd_seq_query_subscribe_set_type(subscriptionData, SND_SEQ_QUERY_SUBS_WRITE);
  snd_seq_query_subscribe_set_index(subscriptionData, 0);

  while (snd_seq_query_port_subscribers(g_sequencerHandle, subscriptionData) >= 0) {
    const auto *subscriberAddr = snd_seq_query_subscribe_get_addr(subscriptionData);
    result.emplace_back(subscriberAddr->client, subscriberAddr->port);
    snd_seq_query_subscribe_set_index(subscriptionData,
                                      snd_seq_query_subscribe_get_index(subscriptionData) + 1);
  }
  return result;
}

midi::Event parseAlsaEvent(const snd_seq_event_t &alsaEvent) {
  static const midi::Event emptyEvent{};
  unsigned char pMidiData[MAX_MIDI_EVENT_SIZE];
  long evLength =
      snd_midi_event_decode(g_midiEventParserHandle, pMidiData, MAX_MIDI_EVENT_SIZE, &alsaEvent);
  if (evLength <= 0) {
    if (evLength == -ENOENT) {
      // The sequencer event does not correspond to one or more MIDI messages.
      return emptyEvent; // that's OK ... just ignore
    }
    ALSA_ERROR(evLength, "snd_midi_event_decode");
    return emptyEvent;
  }

  midi::Event result(pMidiData, pMidiData + evLength);
  return result;
}

/**
 * Register a handler that shall be called at regular time-intervals
 * to control the state of the connections to the port.
 * @param handler - the function to be called
 * @throws BadStateException - if the `alsaClient` is in `running` state.
 */
void onMonitorConnections(const OnMonitorConnectionsHandler &handler) {
  if (g_stateFlag == State::running) {
    throw BadStateException("Cannot register an OnMonitorConnectionsHandler. Wrong state " +
                            stateAsString(g_stateFlag));
  }
  g_onMonitorConnectionsHandler = handler;
}
void defaultConnectionsHandler(const std::string &connectTo){
  if (connectTo.empty()){
    // no connection requested -> nothing to do.
    return;
  }
  if(g_portId==NULL_ID){
    // we have no receiver port!
    return;
  }
  std::vector<PortID> connected = receiverPortGetConnectionsInternal();
  if(!connected.empty()){
    // there is something connected - we assume that this is what we ought to connect to.
    return;
  }

  // let's try to connect to whatever "connectTo" might be.
  tryToConnect(connectTo);

}
} // namespace impl

/**
 * Open the ALSA sequencer in non-blocking mode.
 */
void open(const std::string &clientName) noexcept(false) {
  std::unique_lock<std::mutex> lock{g_stateAccessMutex};
  if (g_stateFlag != State::closed) {
    throw BadStateException("Cannot open ALSA client. Wrong state " + stateAsString(g_stateFlag));
  }
  snd_seq_t *newSequencerHandle;
  snd_midi_event_t *newParserHandle;
  int err;
  // open sequencer (do we need a duplex stream?).
  err = snd_seq_open(&newSequencerHandle, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK);
  if (ALSA_ERROR(err, "open sequencer")) {
    throw std::runtime_error("ALSA cannot open sequencer");
  }

  // set our client's name
  err = snd_seq_set_client_name(newSequencerHandle, clientName.c_str());
  if (ALSA_ERROR(err, "snd_seq_set_client_name")) {
    throw std::runtime_error("ALSA cannot set client name.");
  }

  // create the event parser
  err = snd_midi_event_new(MAX_MIDI_EVENT_SIZE, &newParserHandle);
  if (ALSA_ERROR(err, "snd_midi_event_new")) {
    throw std::runtime_error("ALSA cannot create MIDI parser.");
  }
  snd_midi_event_init(newParserHandle);
  snd_midi_event_no_status(newParserHandle, 1); // no running status byte!!!
  SPDLOG_TRACE("alsaClient::open - MIDI Event parser created.");

  // set common variables.
  g_portId = NULL_ID;
  g_sequencerHandle = newSequencerHandle;
  g_midiEventParserHandle = newParserHandle;
  g_clientId = snd_seq_client_id(g_sequencerHandle);
  if (ALSA_ERROR(g_clientId, "snd_seq_client_id")) {
    throw std::runtime_error("ALSA cannot create client");
  }
  g_stateFlag = State::idle;
  SPDLOG_TRACE("alsaClient::open - client {} created.", g_clientId);
}

/**
 * Create a new ALSA MIDI input port. External applications can write to this port.
 *
 * __Note 1__: in the current implementation, __only one single__ input port can be created.
 *
 * __Note 2__: in the current implementation, this function shall only be called from the
 * `idle` state.
 *
 * @param portName  - a desired name for the new port.
 * The server may modify this name to create a unique variant, if needed.
 * @param connectTo - the designation of a sender-port that this port shall try to connect.
 * If the connection fails, the port is nevertheless created. An empty string denotes
 * that no connection shall be attempted.
 * @return the input port.
 * @throws BadStateException - if port creation is attempted from a state other than `idle`.
 * @throws ServerException - if the ALSA server has encountered a problem.
 */
ReceiverPort newReceiverPort(const std::string &portName,
                             const std::string &connectTo) noexcept(false) {
  std::unique_lock<std::mutex> lock{g_stateAccessMutex};
  if (g_stateFlag != State::idle) {
    throw BadStateException("Cannot create input port. Wrong state " + stateAsString(g_stateFlag));
  }
  if (g_portId != NULL_ID) {
    throw ServerException("Cannot create more that one port.");
  }
  g_portId = snd_seq_create_simple_port(g_sequencerHandle, portName.c_str(),
                                        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                                        SND_SEQ_PORT_TYPE_APPLICATION);
  if (ALSA_ERROR(g_portId, "create port")) {
    g_portId = NULL_ID;
    throw std::runtime_error("ALSA cannot create port");
  }
  SPDLOG_TRACE("alsaClient::newInputAlsaPort - port \"{}\" created.", portName);

  g_connectTo = connectTo;
  onMonitorConnections(defaultConnectionsHandler);
  //tryToConnect(connectTo);
}

/**
 * List all ports that are connected to the ReceiverPort.
 * @return a list of the ports to which the ReceiverPort is connected. If no
 * port is currently connected or the ReceiverPort has not been created yet,
 * an empty list is returned.
 */
std::vector<PortID> receiverPortGetConnections() {
  std::vector<PortID> emptyList{};
  std::unique_lock<std::mutex> lock{g_stateAccessMutex};
  if (g_stateFlag == State::closed) {
    return emptyList;
  }
  if (g_portId == NULL_ID) {
    return emptyList;
  }
  return receiverPortGetConnectionsInternal();
}

void close() noexcept {
  std::unique_lock<std::mutex> lock{g_stateAccessMutex};
  if (g_stateFlag == State::closed) {
    return;
  }
  // make sure that the input queue is stopped.
  stopInternal();

  SPDLOG_TRACE("alsaClient::closeAlsaSequencer - closing client {}.", g_clientId);
  snd_midi_event_free(g_midiEventParserHandle);
  int err = snd_seq_close(g_sequencerHandle);
  ALSA_ERROR(err, "close sequencer");

  // reset common variables to their null values.
  g_portId = NULL_ID;
  g_sequencerHandle = nullptr;
  g_midiEventParserHandle = nullptr;
  g_clientId = NULL_ID;
  g_stateFlag = State::closed;
}

std::string clientName() {
  std::unique_lock<std::mutex> lock{g_stateAccessMutex};
  if (g_stateFlag == State::closed) {
    return "";
  }
  snd_seq_client_info_t *info;
  snd_seq_client_info_alloca(&info);
  int err;

  err = snd_seq_get_client_info(g_sequencerHandle, info);
  if (ALSA_ERROR(err, "snd_seq_get_client_info")) {
    return "";
  }
  return snd_seq_client_info_get_name(info);
}
std::string portName() {
  std::unique_lock<std::mutex> lock{g_stateAccessMutex};
  if (g_stateFlag == State::closed) {
    return "";
  }
  if (g_portId == NULL_ID) {
    return "";
  }
  snd_seq_port_info_t *portInfo;
  snd_seq_port_info_alloca(&portInfo);

  int err = snd_seq_get_port_info(g_sequencerHandle, g_portId, portInfo);
  if (ALSA_ERROR(err, "snd_seq_get_port_info")) {
    return "";
  }
  return snd_seq_port_info_get_name(portInfo);
}
/**
 * Indicates the current state of the `alsaClient`.
 *
 * This function will block while the client is shutting down or starting up.
 * @return the current state of the `alsaClient`.
 */
State state() {
  std::unique_lock<std::mutex> lock{g_stateAccessMutex};
  return g_stateFlag;
}
void activate() noexcept(false) {
  std::unique_lock<std::mutex> lock{g_stateAccessMutex};
  if (g_stateFlag != State::idle) {
    throw BadStateException("Cannot create activate. Wrong state " + stateAsString(g_stateFlag));
  }
  activateInternal();
  g_stateFlag = State::running;
  // make sure that the port monitor runs at least once.
  std::this_thread::sleep_for(MONITOR_INTERVAL);
}

void stop() noexcept {
  std::unique_lock<std::mutex> lock{g_stateAccessMutex};
  if (g_stateFlag != State::running) {
    return;
  }
  stopInternal();
  g_stateFlag = State::idle;
}

int retrieve(sysClock::TimePoint deadline, const RetrieveCallback &forEachClosure) noexcept {
  std::unique_lock<std::mutex> lock{g_stateAccessMutex};
  if (g_stateFlag != State::running) {
    return -1;
  }

  int err = 0;

  // we define the procedure to be executed on each MIDI event in the queue
  auto processClosure = [&forEachClosure, &err](const snd_seq_event_t &event,
                                                sysClock::TimePoint timeStamp) {
    const midi::Event midiEvent = parseAlsaEvent(event);
    if (!midiEvent.empty() && !err) {
      // we delegate to the given forEachClosure
      err = forEachClosure(midiEvent, timeStamp);
    }
  };
  // apply the processClosure on the queue
  alsaClient::receiverQueue::process(deadline, processClosure);
  return err;
}

} // namespace alsaClient
