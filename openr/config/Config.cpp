/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fb303/ServiceData.h>
#include <folly/logging/xlog.h>
#include <glog/logging.h>
#include <thrift/lib/cpp/util/EnumUtils.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>
#include <stdexcept>

#include <openr/common/Constants.h>
#include <openr/config/Config.h>
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/if/gen-cpp2/OpenrConfig_types.h>

using apache::thrift::util::enumName;
using openr::thrift::PrefixAllocationMode;
using openr::thrift::PrefixForwardingAlgorithm;
using openr::thrift::PrefixForwardingType;

namespace openr {

std::shared_ptr<re2::RE2::Set>
AreaConfiguration::compileRegexSet(std::vector<std::string> const& strings) {
  re2::RE2::Options regexOpts;
  std::string regexErr;
  regexOpts.set_case_sensitive(false);

  auto reSet =
      std::make_shared<re2::RE2::Set>(regexOpts, re2::RE2::ANCHOR_BOTH);

  if (strings.empty()) {
    // make this regex set unmatchable
    std::string const unmatchable = "a^";
    CHECK_NE(-1, reSet->Add(unmatchable, &regexErr)) << fmt::format(
        "Failed to add regex: {}. Error: {}", unmatchable, regexErr);
  }
  for (const auto& str : strings) {
    if (reSet->Add(str, &regexErr) == -1) {
      throw std::invalid_argument(
          fmt::format("Failed to add regex: {}. Error: {}", str, regexErr));
    }
  }
  CHECK(reSet->Compile()) << "Regex compilation failed";
  return reSet;
}

Config::Config(const std::string& configFile) {
  std::string contents;
  if (not FileUtil::readFileToString(configFile, contents)) {
    auto errStr = fmt::format("Could not read config file: {}", configFile);
    XLOG(ERR) << errStr;
    throw thrift::ConfigError(errStr);
  }

  auto jsonSerializer = apache::thrift::SimpleJSONSerializer();
  try {
    jsonSerializer.deserialize(contents, config_);
  } catch (const std::exception& ex) {
    auto errStr = fmt::format(
        "Could not parse OpenrConfig struct: {}", folly::exceptionStr(ex));
    XLOG(ERR) << errStr;
    throw thrift::ConfigError(errStr);
  }
  populateInternalDb();
}

std::string
Config::getRunningConfig() const {
  auto jsonSerializer = apache::thrift::SimpleJSONSerializer();
  std::string contents;
  try {
    jsonSerializer.serialize(config_, &contents);
  } catch (const std::exception& ex) {
    XLOG(ERR) << "Could not serialize config: " << folly::exceptionStr(ex);
  }

  return contents;
}

PrefixAllocationParams
Config::createPrefixAllocationParams(
    const std::string& seedPfxStr, uint8_t allocationPfxLen) {
  // check seed_prefix and allocate_prefix_len are set
  if (seedPfxStr.empty() or allocationPfxLen == 0) {
    throw std::invalid_argument(
        "seed_prefix and allocate_prefix_len must be filled.");
  }

  // validate seed prefix
  auto seedPfx = folly::IPAddress::createNetwork(seedPfxStr);

  // validate allocate_prefix_len
  if (seedPfx.first.isV4() and
      (allocationPfxLen <= seedPfx.second or allocationPfxLen > 32)) {
    throw std::out_of_range(fmt::format(
        "invalid allocate_prefix_len ({}), valid range = ({}, 32]",
        allocationPfxLen,
        seedPfx.second));
  }

  if ((seedPfx.first.isV6()) and
      (allocationPfxLen <= seedPfx.second or allocationPfxLen > 128)) {
    throw std::out_of_range(fmt::format(
        "invalid allocate_prefix_len ({}), valid range = ({}, 128]",
        allocationPfxLen,
        seedPfx.second));
  }

  return {seedPfx, allocationPfxLen};
}

void
Config::checkPrependLabelConfig(
    const openr::thrift::AreaConfig& areaConf) const {
  // Check label range values for prepend labels
  if (areaConf.prepend_label_ranges_ref().has_value()) {
    const auto& v4LblRange = *areaConf.prepend_label_ranges_ref()->v4_ref();
    if (not isLabelRangeValid(v4LblRange)) {
      throw std::invalid_argument(fmt::format(
          "v4: prepend label range [{}, {}] is invalid for area id {}",
          *v4LblRange.start_label_ref(),
          *v4LblRange.end_label_ref(),
          *areaConf.area_id_ref()));
    }

    const auto& v6LblRange = *areaConf.prepend_label_ranges_ref()->v6_ref();
    if (not isLabelRangeValid(v6LblRange)) {
      throw std::invalid_argument(fmt::format(
          "v6: prepend label range [{}, {}] is invalid for area id {}",
          *v6LblRange.start_label_ref(),
          *v6LblRange.end_label_ref(),
          *areaConf.area_id_ref()));
    }
  }
}

void
Config::checkAdjacencyLabelConfig(
    const openr::thrift::AreaConfig& areaConf) const {
  if (areaConf.sr_adj_label_ref().has_value()) {
    // Check adj segment labels if configured or if label range is valid
    if (areaConf.sr_adj_label_ref()->sr_adj_label_type_ref() ==
        thrift::SegmentRoutingAdjLabelType::AUTO_IFINDEX) {
      if (not areaConf.sr_adj_label_ref()->adj_label_range_ref().has_value()) {
        throw std::invalid_argument(fmt::format(
            "label range for adjacency labels is not configured for area id {}",
            *areaConf.area_id_ref()));
      } else if (not isLabelRangeValid(
                     *areaConf.sr_adj_label_ref()->adj_label_range_ref())) {
        const auto& label_range =
            *areaConf.sr_adj_label_ref()->adj_label_range_ref();
        throw std::invalid_argument(fmt::format(
            "label range [{}, {}] for adjacency labels is invalid for area id {}",
            *label_range.start_label_ref(),
            *label_range.end_label_ref(),
            *areaConf.area_id_ref()));
      }
    }
  }
}

void
Config::checkNodeSegmentLabelConfig(
    const openr::thrift::AreaConfig& areaConf) const {
  // Check if Node Segment Label is configured or if label range is valid
  if (areaConf.area_sr_node_label_ref().has_value()) {
    const auto& srNodeConfig = *areaConf.area_sr_node_label_ref();
    if (*srNodeConfig.sr_node_label_type_ref() ==
        thrift::SegmentRoutingNodeLabelType::AUTO) {
      // Automatic node segment label allocation
      if (not srNodeConfig.node_segment_label_range_ref().has_value()) {
        throw std::invalid_argument(fmt::format(
            "node segment label range is not configured for area id: {}",
            *areaConf.area_id_ref()));
      } else if (not isLabelRangeValid(
                     *srNodeConfig.node_segment_label_range_ref())) {
        const auto& label_range = *srNodeConfig.node_segment_label_range_ref();
        throw std::invalid_argument(fmt::format(
            "node segment label range [{}, {}] is invalid for area config id: {}",
            *label_range.start_label_ref(),
            *label_range.end_label_ref(),
            *areaConf.area_id_ref()));
      }
    } else if (not srNodeConfig.node_segment_label_ref().has_value()) {
      throw std::invalid_argument(fmt::format(
          "static node segment label is not configured for area config id: {}",
          *areaConf.area_id_ref()));
    } else if (not isMplsLabelValid(*srNodeConfig.node_segment_label_ref())) {
      throw std::invalid_argument(fmt::format(
          "node segment label {} is invalid for area config id: {}",
          *srNodeConfig.node_segment_label_ref(),
          *areaConf.area_id_ref()));
    }
  }
}

void
Config::populateAreaConfig() {
  if (config_.areas_ref()->empty()) {
    // TODO remove once transition to areas is complete
    thrift::AreaConfig defaultArea;
    defaultArea.area_id_ref() = Constants::kDefaultArea.toString();
    config_.areas_ref() = {defaultArea};
  }

  std::optional<neteng::config::routing_policy::Filters> propagationPolicy{
      std::nullopt};
  if (auto areaPolicies = getAreaPolicies()) {
    propagationPolicy =
        areaPolicies->filters_ref()->routePropagationPolicy_ref().to_optional();
  }

  for (auto& areaConf : *config_.areas_ref()) {
    if (areaConf.neighbor_regexes_ref()->empty()) {
      areaConf.neighbor_regexes_ref() = {".*"};
    }

    if (auto importPolicyName = areaConf.import_policy_name_ref()) {
      if (not propagationPolicy or
          propagationPolicy->objects_ref()->count(*importPolicyName) == 0) {
        throw std::invalid_argument(fmt::format(
            "No area policy definition found for {}", *importPolicyName));
      }
    }

    if (!areaConfigs_.emplace(*areaConf.area_id_ref(), areaConf).second) {
      throw std::invalid_argument(
          fmt::format("Duplicate area config id: {}", *areaConf.area_id_ref()));
    }

    checkNodeSegmentLabelConfig(areaConf);
    checkAdjacencyLabelConfig(areaConf);
    checkPrependLabelConfig(areaConf);
  }
}

void
Config::checkKvStoreConfig() const {
  auto& kvStoreConf = *config_.kvstore_config_ref();
  if (const auto& floodRate = kvStoreConf.flood_rate_ref()) {
    if (*floodRate->flood_msg_per_sec_ref() <= 0) {
      throw std::out_of_range("kvstore flood_msg_per_sec should be > 0");
    }
    if (*floodRate->flood_msg_burst_size_ref() <= 0) {
      throw std::out_of_range("kvstore flood_msg_burst_size should be > 0");
    }
  }

  if (kvStoreConf.key_ttl_ms_ref() == Constants::kTtlInfinity) {
    throw std::out_of_range("kvstore key_ttl_ms should be a finite number");
  }
}

void
Config::checkDecisionConfig() const {
  auto& decisionConf = *config_.decision_config_ref();
  if (*decisionConf.debounce_min_ms_ref() >
      *decisionConf.debounce_max_ms_ref()) {
    throw std::invalid_argument(fmt::format(
        "decision_config.debounce_min_ms ({}) should be <= decision_config.debounce_max_ms ({})",
        *decisionConf.debounce_min_ms_ref(),
        *decisionConf.debounce_max_ms_ref()));
  }
}

void
Config::checkSparkConfig() const {
  auto& sparkConfig = *config_.spark_config_ref();
  if (*sparkConfig.neighbor_discovery_port_ref() <= 0 ||
      *sparkConfig.neighbor_discovery_port_ref() > 65535) {
    throw std::out_of_range(fmt::format(
        "neighbor_discovery_port ({}) should be in range [0, 65535]",
        *sparkConfig.neighbor_discovery_port_ref()));
  }

  if (*sparkConfig.hello_time_s_ref() <= 0) {
    throw std::out_of_range(fmt::format(
        "hello_time_s ({}) should be > 0", *sparkConfig.hello_time_s_ref()));
  }

  // When a node starts or a new link comes up we perform fast initial neighbor
  // discovery by sending hello packets with solicitResponse bit set to request
  // an immediate reply. This allows us to discover new neighbors in hundreds
  // of milliseconds (or as configured).
  if (*sparkConfig.fastinit_hello_time_ms_ref() <= 0) {
    throw std::out_of_range(fmt::format(
        "fastinit_hello_time_ms ({}) should be > 0",
        *sparkConfig.fastinit_hello_time_ms_ref()));
  }

  if (*sparkConfig.fastinit_hello_time_ms_ref() >
      1000 * *sparkConfig.hello_time_s_ref()) {
    throw std::invalid_argument(fmt::format(
        "fastinit_hello_time_ms ({}) should be <= hold_time_s ({}) * 1000",
        *sparkConfig.fastinit_hello_time_ms_ref(),
        *sparkConfig.hello_time_s_ref()));
  }

  // The rate of hello packet send is defined by keepAliveTime.
  // This time must be less than the holdTime for each node.
  if (*sparkConfig.keepalive_time_s_ref() <= 0) {
    throw std::out_of_range(fmt::format(
        "keepalive_time_s ({}) should be > 0",
        *sparkConfig.keepalive_time_s_ref()));
  }

  if (*sparkConfig.keepalive_time_s_ref() > *sparkConfig.hold_time_s_ref()) {
    throw std::invalid_argument(fmt::format(
        "keepalive_time_s ({}) should be <= hold_time_s ({})",
        *sparkConfig.keepalive_time_s_ref(),
        *sparkConfig.hold_time_s_ref()));
  }

  // Hold time tells the receiver how long to keep the information valid for.
  if (*sparkConfig.hold_time_s_ref() <= 0) {
    throw std::out_of_range(fmt::format(
        "hold_time_s ({}) should be > 0", *sparkConfig.hold_time_s_ref()));
  }

  if (*sparkConfig.graceful_restart_time_s_ref() <= 0) {
    throw std::out_of_range(fmt::format(
        "graceful_restart_time_s ({}) should be > 0",
        *sparkConfig.graceful_restart_time_s_ref()));
  }

  if (*sparkConfig.graceful_restart_time_s_ref() <
      3 * *sparkConfig.keepalive_time_s_ref()) {
    throw std::invalid_argument(fmt::format(
        "graceful_restart_time_s ({}) should be >= 3 * keepalive_time_s ({})",
        *sparkConfig.graceful_restart_time_s_ref(),
        *sparkConfig.keepalive_time_s_ref()));
  }

  if (*sparkConfig.step_detector_conf_ref()->lower_threshold_ref() < 0 ||
      *sparkConfig.step_detector_conf_ref()->upper_threshold_ref() < 0 ||
      *sparkConfig.step_detector_conf_ref()->lower_threshold_ref() >=
          *sparkConfig.step_detector_conf_ref()->upper_threshold_ref()) {
    throw std::invalid_argument(fmt::format(
        "step_detector_conf.lower_threshold ({}) should be < step_detector_conf.upper_threshold ({}), and they should be >= 0",
        *sparkConfig.step_detector_conf_ref()->lower_threshold_ref(),
        *sparkConfig.step_detector_conf_ref()->upper_threshold_ref()));
  }

  if (*sparkConfig.step_detector_conf_ref()->fast_window_size_ref() < 0 ||
      *sparkConfig.step_detector_conf_ref()->slow_window_size_ref() < 0 ||
      (*sparkConfig.step_detector_conf_ref()->fast_window_size_ref() >
       *sparkConfig.step_detector_conf_ref()->slow_window_size_ref())) {
    throw std::invalid_argument(fmt::format(
        "step_detector_conf.fast_window_size ({}) should be <= step_detector_conf.slow_window_size ({}), and they should be >= 0",
        *sparkConfig.step_detector_conf_ref()->fast_window_size_ref(),
        *sparkConfig.step_detector_conf_ref()->slow_window_size_ref()));
  }

  if (*sparkConfig.step_detector_conf_ref()->lower_threshold_ref() < 0 ||
      *sparkConfig.step_detector_conf_ref()->upper_threshold_ref() < 0 ||
      *sparkConfig.step_detector_conf_ref()->lower_threshold_ref() >=
          *sparkConfig.step_detector_conf_ref()->upper_threshold_ref()) {
    throw std::invalid_argument(fmt::format(
        "step_detector_conf.lower_threshold ({}) should be < step_detector_conf.upper_threshold ({})",
        *sparkConfig.step_detector_conf_ref()->lower_threshold_ref(),
        *sparkConfig.step_detector_conf_ref()->upper_threshold_ref()));
  }
}

void
Config::checkMonitorConfig() const {
  auto& monitorConfig = *config_.monitor_config_ref();
  if (*monitorConfig.max_event_log_ref() < 0) {
    throw std::out_of_range(fmt::format(
        "monitor_max_event_log ({}) should be >= 0",
        *monitorConfig.max_event_log_ref()));
  }
}

void
Config::checkLinkMonitorConfig() const {
  auto& lmConf = *config_.link_monitor_config_ref();
  // backoff validation
  if (*lmConf.linkflap_initial_backoff_ms_ref() < 0) {
    throw std::out_of_range(fmt::format(
        "linkflap_initial_backoff_ms ({}) should be >= 0",
        *lmConf.linkflap_initial_backoff_ms_ref()));
  }

  if (*lmConf.linkflap_max_backoff_ms_ref() < 0) {
    throw std::out_of_range(fmt::format(
        "linkflap_max_backoff_ms ({}) should be >= 0",
        *lmConf.linkflap_max_backoff_ms_ref()));
  }

  if (*lmConf.linkflap_initial_backoff_ms_ref() >
      *lmConf.linkflap_max_backoff_ms_ref()) {
    throw std::out_of_range(fmt::format(
        "linkflap_initial_backoff_ms ({}) should be < linkflap_max_backoff_ms ({})",
        *lmConf.linkflap_initial_backoff_ms_ref(),
        *lmConf.linkflap_max_backoff_ms_ref()));
  }
}

void
Config::checkSegmentRoutingConfig() const {
  if (const auto& srConfig = config_.segment_routing_config_ref()) {
    // Check label range values for prepend labels
    if (srConfig->prepend_label_ranges_ref().has_value()) {
      const auto& v4LblRange = *srConfig->prepend_label_ranges_ref()->v4_ref();
      if (not isLabelRangeValid(v4LblRange)) {
        throw std::invalid_argument(fmt::format(
            "v4: prepend label range [{}, {}] is invalid",
            *v4LblRange.start_label_ref(),
            *v4LblRange.end_label_ref()));
      }

      const auto& v6LblRange = *srConfig->prepend_label_ranges_ref()->v6_ref();
      if (not isLabelRangeValid(v6LblRange)) {
        throw std::invalid_argument(fmt::format(
            "v6: prepend label range [{}, {}] is invalid",
            *v6LblRange.start_label_ref(),
            *v6LblRange.end_label_ref()));
      }
    }

    if (srConfig->sr_adj_label_ref().has_value()) {
      // Check adj segment labels if configured or if label range is valid
      if (srConfig->sr_adj_label_ref()->sr_adj_label_type_ref() ==
          thrift::SegmentRoutingAdjLabelType::AUTO_IFINDEX) {
        if (not srConfig->sr_adj_label_ref()
                    ->adj_label_range_ref()
                    .has_value()) {
          throw std::invalid_argument(
              "label range for adjacency labels is not configured");
        } else if (not isLabelRangeValid(
                       *srConfig->sr_adj_label_ref()->adj_label_range_ref())) {
          const auto& label_range =
              *srConfig->sr_adj_label_ref()->adj_label_range_ref();
          throw std::invalid_argument(fmt::format(
              "label range [{}, {}] for adjacency labels is invalid",
              *label_range.start_label_ref(),
              *label_range.end_label_ref()));
        }
      }
    }
  }
}

void
Config::checkPrefixAllocationConfig() {
  const auto& paConf = config_.prefix_allocation_config_ref();
  // check if config exists
  if (not paConf) {
    throw std::invalid_argument(
        "enable_prefix_allocation = true, but prefix_allocation_config is empty");
  }

  // sanity check enum prefix_allocation_mode
  if (not enumName(*paConf->prefix_allocation_mode_ref())) {
    throw std::invalid_argument("invalid prefix_allocation_mode");
  }

  auto seedPrefix = paConf->seed_prefix_ref().value_or("");
  auto allocatePfxLen = paConf->allocate_prefix_len_ref().value_or(0);

  switch (*paConf->prefix_allocation_mode_ref()) {
  case PrefixAllocationMode::DYNAMIC_ROOT_NODE: {
    // populate prefixAllocationParams_ from seed_prefix and
    // allocate_prefix_len
    prefixAllocationParams_ =
        createPrefixAllocationParams(seedPrefix, allocatePfxLen);

    if (prefixAllocationParams_->first.first.isV4() and not isV4Enabled()) {
      throw std::invalid_argument(
          "v4 seed_prefix detected, but enable_v4 = false");
    }
    break;
  }
  case PrefixAllocationMode::DYNAMIC_LEAF_NODE:
  case PrefixAllocationMode::STATIC: {
    // seed_prefix and allocate_prefix_len have to to empty
    if (not seedPrefix.empty() or allocatePfxLen > 0) {
      throw std::invalid_argument(
          "prefix_allocation_mode != DYNAMIC_ROOT_NODE, seed_prefix and allocate_prefix_len must be empty");
    }
    break;
  }
  }
}

void
Config::checkVipServiceConfig() const {
  if (isVipServiceEnabled()) {
    if (not config_.vip_service_config_ref()) {
      throw std::invalid_argument(
          "enable_vip_service = true, but vip_service_config is empty");
    } else {
      if (config_.vip_service_config_ref()->ingress_policy_ref().has_value()) {
        std::optional<neteng::config::routing_policy::Filters>
            propagationPolicy{std::nullopt};
        if (auto areaPolicies = getAreaPolicies()) {
          propagationPolicy = areaPolicies->filters_ref()
                                  ->routePropagationPolicy_ref()
                                  .to_optional();
        }
        auto ingress_policy =
            *config_.vip_service_config_ref()->ingress_policy_ref();
        if (not propagationPolicy or
            propagationPolicy->objects_ref()->count(ingress_policy) == 0) {
          throw std::invalid_argument(fmt::format(
              "No area policy definition found for {}", ingress_policy));
        }
      }
    }
  }
}

void
Config::checkBgpPeeringConfig() {
  if (isBgpPeeringEnabled() and not config_.bgp_config_ref()) {
    throw std::invalid_argument(
        "enable_bgp_peering = true, but bgp_config is empty");
  }

  if (isBgpPeeringEnabled() and config_.bgp_config_ref()) {
    // Identify if BGP Add Path is enabled for local peering or not
    for (const auto& peer : *config_.bgp_config_ref()->peers_ref()) {
      if (folly::IPAddress(peer.peer_addr_ref().value()).isLoopback() and
          peer.add_path_ref() and
          *peer.add_path_ref() == thrift::AddPath::RECEIVE and
          not isSegmentRoutingEnabled()) {
        // TODO
        // Additionally check later if prepend label range is also convered.
        throw std::invalid_argument(
            "segment routing should be congfigured when BGP add_path is configured");
      }
    }
  }

  // Set BGP Translation Config if unset
  if (isBgpPeeringEnabled() and not config_.bgp_translation_config_ref()) {
    // Hack for transioning phase. TODO: Remove after coop is on-boarded
    config_.bgp_translation_config_ref() = thrift::BgpRouteTranslationConfig();
    // throw std::invalid_argument(
    //     "enable_bgp_peering = true, but bgp_translation_config is empty");
  }

  // Validate BGP Translation config
  if (isBgpPeeringEnabled()) {
    const auto& bgpTranslationConf = config_.bgp_translation_config_ref();
    CHECK(bgpTranslationConf.has_value());
    if (*bgpTranslationConf->disable_legacy_translation_ref() and
        (not *bgpTranslationConf->enable_openr_to_bgp_ref() or
         not *bgpTranslationConf->enable_bgp_to_openr_ref())) {
      throw std::invalid_argument(
          "Legacy translation can be disabled only when new translation is "
          "enabled");
    }
  }
}

void
Config::checkThriftServerConfig() const {
  const auto& thriftServerConfig = getThriftServerConfig();

  // Checking the fields needed when we enable the secure thrift server
  const auto& caPath = thriftServerConfig.x509_ca_path_ref();
  const auto& certPath = thriftServerConfig.x509_cert_path_ref();
  const auto& eccCurve = thriftServerConfig.ecc_curve_name_ref();
  if (not(caPath and certPath and eccCurve)) {
    throw std::invalid_argument(
        "enable_secure_thrift_server = true, but x509_ca_path, x509_cert_path or ecc_curve_name is empty.");
  }
  if ((not fs::exists(caPath.value())) or (not fs::exists(certPath.value()))) {
    throw std::invalid_argument(
        "x509_ca_path or x509_cert_path is specified in the config but not found in the disk.");
  }

  // x509_key_path could be empty. If specified, need to be present in the
  // file system.
  const auto& keyPath = getThriftServerConfig().x509_key_path_ref();
  if (keyPath and (not fs::exists(keyPath.value()))) {
    throw std::invalid_argument(
        "x509_key_path is specified in the config but not found in the disk.");
  }
}

void
Config::populateInternalDb() {
  populateAreaConfig();

  // validate prefix forwarding type and algorithm
  const auto& pfxType = *config_.prefix_forwarding_type_ref();
  const auto& pfxAlgo = *config_.prefix_forwarding_algorithm_ref();

  if (not enumName(pfxType) or not enumName(pfxAlgo)) {
    throw std::invalid_argument(
        "invalid prefix_forwarding_type or prefix_forwarding_algorithm");
  }

  if (pfxAlgo == PrefixForwardingAlgorithm::KSP2_ED_ECMP and
      pfxType != PrefixForwardingType::SR_MPLS) {
    throw std::invalid_argument(
        "prefix_forwarding_type must be set to SR_MPLS for KSP2_ED_ECMP");
  }

  // validate IP-TOS
  if (const auto& ipTos = config_.ip_tos_ref()) {
    if (*ipTos < 0 or *ipTos >= 256) {
      throw std::out_of_range(
          "ip_tos must be greater or equal to 0 and less than 256");
    }
  }

  // To avoid bgp and vip service advertise the same prefixes,
  // bgp speaker and vip service shouldn't co-exist
  if (isBgpPeeringEnabled() && isVipServiceEnabled()) {
    throw std::invalid_argument(
        "Bgp Peering and Vip Service can not be both enabled");
  }

  // check watchdog has config if enabled
  if (isWatchdogEnabled() and not config_.watchdog_config_ref()) {
    throw std::invalid_argument(
        "enable_watchdog = true, but watchdog_config is empty");
  }

  // Check Route Deletion Parameter
  if (*config_.route_delete_delay_ms_ref() < 0) {
    throw std::invalid_argument("Route delete duration must be >= 0ms");
  }

  // validate KvStore config (e.g. ttl/flood-rate/etc.)
  checkKvStoreConfig();

  // validate Decision config (e.g. debounce)
  checkDecisionConfig();

  // validate Spark config
  checkSparkConfig();

  // validate Monitor config (e.g. event log)
  checkMonitorConfig();

  // validate Link Monitor config (e.g. backoff)
  checkLinkMonitorConfig();

  // validate Segment Routing config
  checkSegmentRoutingConfig();

  // validate Prefix Allocation config
  if (isPrefixAllocationEnabled()) {
    // by now areaConfigs_ should be filled.
    if (areaConfigs_.size() > 1) {
      throw std::invalid_argument(
          "prefix_allocation only support single area config");
    }
    checkPrefixAllocationConfig();
  }

  // validate VipServiceConfig config
  checkVipServiceConfig();

  // validate BGP Peering config and BGP Translation config
  checkBgpPeeringConfig();

  // validate thrift server config
  if (isSecureThriftServerEnabled()) {
    checkThriftServerConfig();
  }

  //
  // Set an implicit value for eor_time_s (Decision Hold time) if not specified
  // explicitly.
  // NOTE: `eor_time_s` variable would go away once new initialization process
  // is completely implemented & rolled out.
  //
  if (not config_.eor_time_s_ref()) {
    config_.eor_time_s_ref() = 3 * (*getSparkConfig().keepalive_time_s_ref());
  }
}

/**
 * TODO: This is the util method to do a translation from:
 *
 * thrift::KvstoreConfig => if/OpenrConfig.thrift
 *
 * to:
 *
 * thrift::KvStoreConfig => if/KvStore.thrift
 *
 * to give smooth migration toward KvStore isolation.
 */
thrift::KvStoreConfig
Config::toThriftKvStoreConfig() const {
  // ATTN: oldConfig and config are defined in different thrift files
  thrift::KvStoreConfig config;

  auto oldConfig = getKvStoreConfig();
  config.node_name_ref() = getNodeName();
  config.key_ttl_ms_ref() = *oldConfig.key_ttl_ms_ref();
  config.ttl_decrement_ms_ref() = *oldConfig.ttl_decrement_ms_ref();

  if (auto floodRate = oldConfig.flood_rate_ref()) {
    thrift::KvStoreFloodRate rate;
    rate.flood_msg_per_sec_ref() = *floodRate->flood_msg_per_sec_ref();
    rate.flood_msg_burst_size_ref() = *floodRate->flood_msg_burst_size_ref();

    config.flood_rate_ref() = std::move(rate);
  }
  if (auto setLeafNode = oldConfig.set_leaf_node_ref()) {
    config.set_leaf_node_ref() = *setLeafNode;
  }
  if (auto keyPrefixFilters = oldConfig.key_prefix_filters_ref()) {
    config.key_prefix_filters_ref() = *keyPrefixFilters;
  }
  if (auto keyOriginatorIdFilters = oldConfig.key_originator_id_filters_ref()) {
    config.key_originator_id_filters_ref() = *keyOriginatorIdFilters;
  }
  if (auto enableFloodOptimization =
          oldConfig.enable_flood_optimization_ref()) {
    config.enable_flood_optimization_ref() = *enableFloodOptimization;
  }
  if (auto isFloodRoot = oldConfig.is_flood_root_ref()) {
    config.is_flood_root_ref() = *isFloodRoot;
  }
  if (auto maybeIpTos = getConfig().ip_tos_ref()) {
    config.ip_tos_ref() = *maybeIpTos;
  }
  return config;
}

} // namespace openr
