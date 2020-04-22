/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <openr/common/Constants.h>
#include <openr/messaging/ReplicateQueue.h>
#include <openr/spark/Spark.h>

namespace openr {

struct SparkInterfaceEntry {
  std::string ifName;
  int ifIndex;
  folly::CIDRNetwork v4Network;
  folly::CIDRNetwork v6LinkLocalNetwork;
};

struct SparkTimeConfig {
  SparkTimeConfig(
      std::chrono::milliseconds helloTime = std::chrono::milliseconds{0},
      std::chrono::milliseconds helloFastInitTime =
          std::chrono::milliseconds{0},
      std::chrono::milliseconds handshakeTime = std::chrono::milliseconds{0},
      std::chrono::milliseconds heartbeatTime = std::chrono::milliseconds{0},
      std::chrono::milliseconds negotiateHoldTime =
          std::chrono::milliseconds{0},
      std::chrono::milliseconds heartbeatHoldTime =
          std::chrono::milliseconds{0})
      : myHelloTime(helloTime),
        myHelloFastInitTime(helloFastInitTime),
        myHandshakeTime(handshakeTime),
        myHeartbeatTime(heartbeatTime),
        myNegotiateHoldTime(negotiateHoldTime),
        myHeartbeatHoldTime(heartbeatHoldTime) {}

  std::chrono::milliseconds myHelloTime;
  std::chrono::milliseconds myHelloFastInitTime;
  std::chrono::milliseconds myHandshakeTime;
  std::chrono::milliseconds myHeartbeatTime;
  std::chrono::milliseconds myNegotiateHoldTime;
  std::chrono::milliseconds myHeartbeatHoldTime;
};

/**
 * A utility class to wrap and interact with Spark. It exposes the APIs to
 * send commands to and receive publications from Spark.
 * Mainly used for testing.
 *
 * This should be managed from only one thread. Otherwise behaviour will be
 * undesirable.
 */
class SparkWrapper {
 public:
  SparkWrapper(
      std::string const& myDomainName,
      std::string const& myNodeName,
      std::chrono::milliseconds myHoldTime,
      std::chrono::milliseconds myKeepAliveTime,
      std::chrono::milliseconds myFastInitKeepAliveTime,
      bool enableV4,
      std::pair<uint32_t, uint32_t> version,
      fbzmq::Context& zmqContext,
      std::shared_ptr<IoProvider> ioProvider,
      std::shared_ptr<thrift::OpenrConfig> config,
      bool enableSpark2,
      bool increaseHelloInterval,
      SparkTimeConfig timeConfig);

  ~SparkWrapper();

  // start spark
  void run();

  // stop spark
  void stop();

  // add interfaceDb for Spark to tracking
  // return true upon success and false otherwise
  bool updateInterfaceDb(
      const std::vector<SparkInterfaceEntry>& interfaceEntries);

  // receive spark neighbor event
  folly::Expected<thrift::SparkNeighborEvent, fbzmq::Error> recvNeighborEvent(
      std::optional<std::chrono::milliseconds> timeout = std::nullopt);

  std::optional<thrift::SparkNeighborEvent> waitForEvent(
      const thrift::SparkNeighborEventType eventType,
      std::optional<std::chrono::milliseconds> rcvdTimeout = std::nullopt,
      std::optional<std::chrono::milliseconds> procTimeout =
          Constants::kPlatformRoutesProcTimeout) noexcept;

  // utility call to check neighbor state
  std::optional<SparkNeighState> getSparkNeighState(
      std::string const& ifName, std::string const& neighborName);

  static std::pair<folly::IPAddress, folly::IPAddress> getTransportAddrs(
      const thrift::SparkNeighborEvent& event);

  // utility function to construct thrift::AreaConfig
  static thrift::AreaConfig createAreaConfig(
      const std::string& areaId,
      const std::vector<std::string>& nodeRegexes,
      const std::vector<std::string>& interfaceRegexes);

  //
  // Private statex
  //

 private:
  std::string myNodeName_{""};

  messaging::ReplicateQueue<thrift::SparkNeighborEvent> neighborUpdatesQueue_;
  messaging::RQueue<thrift::SparkNeighborEvent> neighborUpdatesReader_{
      neighborUpdatesQueue_.getReader()};

  // Queue to send interface updates to spark
  messaging::ReplicateQueue<thrift::InterfaceDatabase> interfaceUpdatesQueue_;

  // Thrift serializer object for serializing/deserializing of thrift objects
  // to/from bytes
  apache::thrift::CompactSerializer serializer_;

  // Spark owned by this wrapper.
  std::shared_ptr<Spark> spark_{nullptr};

  // Thread in which Spark will be running.
  std::unique_ptr<std::thread> thread_{nullptr};
};

} // namespace openr
