#include "utils/Configuration.hpp"

#include "utils/types.hpp"

#include <boost/algorithm/string.hpp>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <phosphor-logging/log.hpp>
#include <regex>
#include <sdbusplus/asio/connection.hpp>
#include <string>
#include <variant>
#include <vector>

using json = nlohmann::json;

using ConfigurationField =
    std::variant<bool, uint64_t, std::string, std::vector<uint64_t>,
                 std::vector<std::string>>;

using ConfigurationMap = std::unordered_map<std::string, ConfigurationField>;

static const std::string mctpTypeName =
    "xyz.openbmc_project.Configuration.MctpConfiguration";

static const std::string boardPathNamespace =
    "/xyz/openbmc_project/inventory/system/board";

static const std::unordered_map<std::string, mctp_server::BindingModeTypes>
    stringToBindingModeMap = {
        {"busowner", mctp_server::BindingModeTypes::BusOwner},
        {"BusOwner", mctp_server::BindingModeTypes::BusOwner},
        {"endpoint", mctp_server::BindingModeTypes::Endpoint},
        {"Endpoint", mctp_server::BindingModeTypes::Endpoint},
        {"bridge", mctp_server::BindingModeTypes::Bridge},
        {"Bridge", mctp_server::BindingModeTypes::Bridge}};

static const std::unordered_map<std::string,
                                mctp_server::MctpPhysicalMediumIdentifiers>
    stringToMediumID = {
        {"Smbus", mctp_server::MctpPhysicalMediumIdentifiers::Smbus},
        {"SmbusI2c", mctp_server::MctpPhysicalMediumIdentifiers::SmbusI2c},
        {"I2cCompatible",
         mctp_server::MctpPhysicalMediumIdentifiers::I2cCompatible},
        {"Smbus3OrI2c400khzCompatible",
         mctp_server::MctpPhysicalMediumIdentifiers::
             Smbus3OrI2c400khzCompatible},
        {"Smbus3OrI2c1MhzCompatible",
         mctp_server::MctpPhysicalMediumIdentifiers::Smbus3OrI2c1MhzCompatible},
        {"I2c3Mhz4Compatible",
         mctp_server::MctpPhysicalMediumIdentifiers::I2c3Mhz4Compatible},
        {"Pcie11", mctp_server::MctpPhysicalMediumIdentifiers::Pcie11},
        {"Pcie2", mctp_server::MctpPhysicalMediumIdentifiers::Pcie2},
        {"Pcie21", mctp_server::MctpPhysicalMediumIdentifiers::Pcie21},
        {"Pcie3", mctp_server::MctpPhysicalMediumIdentifiers::Pcie3},
        {"Pcie4", mctp_server::MctpPhysicalMediumIdentifiers::Pcie4},
        {"Pcie5", mctp_server::MctpPhysicalMediumIdentifiers::Pcie5},
        {"PciCompatible",
         mctp_server::MctpPhysicalMediumIdentifiers::PciCompatible},
        {"Usb11Compatible",
         mctp_server::MctpPhysicalMediumIdentifiers::Usb11Compatible},
        {"Usb20Compatible",
         mctp_server::MctpPhysicalMediumIdentifiers::Usb20Compatible},
        {"Usb30Compatible",
         mctp_server::MctpPhysicalMediumIdentifiers::Usb30Compatible},
        {"NcSiOverRbt",
         mctp_server::MctpPhysicalMediumIdentifiers::NcSiOverRbt},
        {"KcsLegacy", mctp_server::MctpPhysicalMediumIdentifiers::KcsLegacy},
        {"KcsPci", mctp_server::MctpPhysicalMediumIdentifiers::KcsPci},
        {"SerialHostLegacy",
         mctp_server::MctpPhysicalMediumIdentifiers::SerialHostLegacy},
        {"SerialHostPci",
         mctp_server::MctpPhysicalMediumIdentifiers::SerialHostPci},
        {"AsynchronousSerial",
         mctp_server::MctpPhysicalMediumIdentifiers::AsynchronousSerial},
        {"I3cSDR", mctp_server::MctpPhysicalMediumIdentifiers::I3cSDR},
        {"I3cHDRDDR", mctp_server::MctpPhysicalMediumIdentifiers::I3cHDRDDR}};

template <typename T>
static bool getField(const ConfigurationMap& configuration,
                     const std::string& fieldName, T& value)
{
    auto it = configuration.find(fieldName);
    if (it != configuration.end())
    {
        const T* ptrValue = std::get_if<T>(&it->second);
        if (ptrValue != nullptr)
        {
            value = *ptrValue;
            return true;
        }
    }
    phosphor::logging::log<phosphor::logging::level::WARNING>(
        ("Missing configuration field " + fieldName).c_str());
    return false;
}

template <typename T>
static bool getField(const json& configuration, const std::string& fieldName,
                     T& value)
{
    if (!configuration.contains(fieldName))
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            ("Missing configuration field " + fieldName).c_str());
        return false;
    }

    try
    {
        value = configuration.at(fieldName).get<T>();
        return true;
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            ("Error reading configuration field " + fieldName + ": " + e.what())
                .c_str());
        return false;
    }
}

template <typename T>
std::set<std::string> getAllowedBuses(const T& map)
{
    std::vector<std::string> allowedBuses;
    if (!getField(map, "AllowedBuses", allowedBuses))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Allowed buses list not found in MCTPD configuration. Everything "
            "will be white listed");
    }
    phosphor::logging::log<phosphor::logging::level::WARNING>(
        (std::string("Allowed buses in config : ") +
         std::to_string(allowedBuses.size()))
            .c_str());
    return std::set<std::string>(allowedBuses.begin(), allowedBuses.end());
}

template <typename T>
static std::optional<SMBusConfiguration> getSMBusConfiguration(const T& map)
{
    std::string physicalMediumID;
    std::string role;
    uint64_t defaultEID = 0;
    std::vector<uint64_t> eidPool;
    std::string bus;
    bool arpOwnerSupport = false;
    uint64_t bmcReceiverAddress = 0;
    uint64_t reqToRespTimeMs = 0;
    uint64_t reqRetryCount = 0;
    uint64_t scanInterval = 0;
    uint64_t getRoutingInterval = 0;
    std::vector<uint64_t> supportedEndpointSlaveAddress;
    std::vector<uint64_t> ignoredEndpintSlaveAddress;

    if (!getField(map, "PhysicalMediumID", physicalMediumID))
    {
        return std::nullopt;
    }

    if (!getField(map, "Role", role) && !getField(map, "role", role))
    {
        return std::nullopt;
    }

    if (!getField(map, "DefaultEID", defaultEID) &&
        !getField(map, "default-eid", defaultEID))
    {
        return std::nullopt;
    }

    if (!getField(map, "Bus", bus) && !getField(map, "bus", bus))
    {
        return std::nullopt;
    }

    if (!getField(map, "ARPOwnerSupport", arpOwnerSupport) &&
        !getField(map, "ARPMasterSupport", arpOwnerSupport))
    {
        return std::nullopt;
    }

    if (!getField(map, "BMCReceiverAddress", bmcReceiverAddress) &&
        !getField(map, "BMCSlaveAddress", bmcReceiverAddress))
    {
        return std::nullopt;
    }

    if (!getField(map, "ReqToRespTimeMs", reqToRespTimeMs) ||
        !getField(map, "ReqRetryCount", reqRetryCount))
    {
        return std::nullopt;
    }

    if (!getField(map, "ScanInterval", scanInterval) || !scanInterval)
    {
        // Set default 10min interval if not specified or invalid
        scanInterval = 600;
    }

    const auto mode = stringToBindingModeMap.at(role);
    if (mode != mctp_server::BindingModeTypes::BusOwner &&
        !getField(map, "GetRoutingInterval", getRoutingInterval))
    {
        getRoutingInterval = 5;
    }

    if (mode == mctp_server::BindingModeTypes::BusOwner &&
        !getField(map, "EIDPool", eidPool) &&
        !getField(map, "eid-pool", eidPool))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Role is set to BusOwner but EIDPool is missing");
        return std::nullopt;
    }

    if (!getField(map, "SupportedEndpointSlaveAddress",
                  supportedEndpointSlaveAddress))
    {
        constexpr uint8_t startAddr = 0x08;
        constexpr uint8_t endAddr = 0x78;
        supportedEndpointSlaveAddress.reserve(endAddr - startAddr);
        for (uint8_t it = startAddr; it < endAddr; it++)
        {
            supportedEndpointSlaveAddress.push_back(it);
        }
    }

    if (!getField(map, "IgnoredEndpointSlaveAddress",
                  ignoredEndpintSlaveAddress))
    {
        ignoredEndpintSlaveAddress = {};
    }

    auto endpointSlaveAddress =
        std::set<uint8_t>(supportedEndpointSlaveAddress.begin(),
                          supportedEndpointSlaveAddress.end());

    // Remove address in ignored list
    for (uint64_t it : ignoredEndpintSlaveAddress)
    {
        endpointSlaveAddress.erase(static_cast<uint8_t>(it));
    }

    SMBusConfiguration config;
    config.mediumId = stringToMediumID.at(physicalMediumID);
    config.mode = mode;
    config.defaultEid = static_cast<uint8_t>(defaultEID);
    if (mode == mctp_server::BindingModeTypes::BusOwner)
    {
        config.eidPool = std::set<uint8_t>(eidPool.begin(), eidPool.end());
    }
    config.supportedEndpointSlaveAddress = endpointSlaveAddress;
    config.bus = bus;
    config.arpMasterSupport = arpOwnerSupport;
    config.bmcSlaveAddr = static_cast<uint8_t>(bmcReceiverAddress);
    config.reqToRespTime = static_cast<unsigned int>(reqToRespTimeMs);
    config.reqRetryCount = static_cast<uint8_t>(reqRetryCount);
    config.scanInterval = scanInterval;
    config.allowedBuses = getAllowedBuses(map);
    if (mode != mctp_server::BindingModeTypes::BusOwner)
    {
        config.routingIntervalSec = static_cast<uint8_t>(getRoutingInterval);
    }

    return config;
}

template <typename T>
static std::optional<PcieConfiguration> getPcieConfiguration(const T& map)
{
    std::string physicalMediumID;
    std::string role;
    uint64_t defaultEID;
    uint64_t bdf;
    uint64_t reqToRespTimeMs;
    uint64_t reqRetryCount;
    uint64_t getRoutingInterval;

    if (!getField(map, "PhysicalMediumID", physicalMediumID))
    {
        return std::nullopt;
    }

    if (!getField(map, "Role", role) && !getField(map, "role", role))
    {
        return std::nullopt;
    }

    if (!getField(map, "DefaultEID", defaultEID) &&
        !getField(map, "default-eid", defaultEID))
    {
        return std::nullopt;
    }

    if (!getField(map, "BDF", bdf) && !getField(map, "bdf", bdf))
    {
        return std::nullopt;
    }

    if (!getField(map, "ReqToRespTimeMs", reqToRespTimeMs) ||
        !getField(map, "ReqRetryCount", reqRetryCount))
    {
        return std::nullopt;
    }

    const auto mode = stringToBindingModeMap.at(role);
    if (mode != mctp_server::BindingModeTypes::BusOwner &&
        !getField(map, "GetRoutingInterval", getRoutingInterval))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Role is not BusOwner but Get Routing update interval is missing");
        return std::nullopt;
    }

    PcieConfiguration config;
    config.mediumId = stringToMediumID.at(physicalMediumID);
    config.mode = stringToBindingModeMap.at(role);
    config.defaultEid = static_cast<uint8_t>(defaultEID);
    config.bdf = static_cast<uint16_t>(bdf);
    config.reqToRespTime = static_cast<unsigned int>(reqToRespTimeMs);
    config.reqRetryCount = static_cast<uint8_t>(reqRetryCount);
    if (mode != mctp_server::BindingModeTypes::BusOwner)
    {
        config.getRoutingInterval = static_cast<uint8_t>(getRoutingInterval);
    }

    return config;
}

static ConfigurationMap
    getConfigurationMap(std::shared_ptr<sdbusplus::asio::connection> conn,
                        const std::string& configurationPath)
{
    auto method_call = conn->new_method_call(
        "xyz.openbmc_project.EntityManager", configurationPath.c_str(),
        "org.freedesktop.DBus.Properties", "GetAll");
    method_call.append(mctpTypeName);

    // Note: This is a blocking call.
    // However, there is nothing to do until the configuration is retrieved.
    auto reply = conn->call(method_call);
    ConfigurationMap map;
    reply.read(map);
    return map;
}

static std::optional<std::pair<std::string, std::unique_ptr<Configuration>>>
    getConfigurationFromEntityManager(
        std::shared_ptr<sdbusplus::asio::connection> conn,
        const std::string& configurationName)
{
    const std::string relativePath =
        boost::algorithm::replace_all_copy(configurationName, "_2f", "/");
    if (relativePath == configurationName)
    {
        return std::nullopt;
    }

    const std::string objectPath = boardPathNamespace + "/" + relativePath;
    ConfigurationMap map;
    try
    {
        map = getConfigurationMap(conn, objectPath);
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Error retrieving configuration from EntityManager");
        return std::nullopt;
    }

    std::string name;
    if (!getField(map, "Name", name))
    {
        return std::nullopt;
    }

    std::string bindingType;
    if (!getField(map, "TransportBindingType", bindingType) &&
        !getField(map, "BindingType", bindingType))
    {
        return std::nullopt;
    }

    std::unique_ptr<Configuration> configuration;
    if (bindingType == "MctpSMBus")
    {
        if (auto optConfig = getSMBusConfiguration(map))
        {
            configuration =
                std::make_unique<SMBusConfiguration>(std::move(*optConfig));
        }
    }
    else if (bindingType == "MctpPCIe")
    {
        if (auto optConfig = getPcieConfiguration(map))
        {
            configuration =
                std::make_unique<PcieConfiguration>(std::move(*optConfig));
        }
    }
    if (!configuration)
    {
        return std::nullopt;
    }

    const std::regex illegal_name_regex("[^A-Za-z0-9_.]");
    std::regex_replace(name.begin(), name.begin(), name.end(),
                       illegal_name_regex, "_");
    return std::make_pair(name, std::move(configuration));
}

static std::optional<std::pair<std::string, std::unique_ptr<Configuration>>>
    getConfigurationFromFile(const std::filesystem::path& configPath,
                             const std::string& configurationName)
{
    std::ifstream jsonFile(configPath);
    if (!jsonFile.is_open())
    {
        return std::nullopt;
    }

    json jsonConfig = json::parse(jsonFile, nullptr, false);
    if (jsonConfig.size() == 0 || !jsonConfig.contains(configurationName))
    {
        return std::nullopt;
    }

    std::unique_ptr<Configuration> configuration;
    if (configurationName == "smbus")
    {
        if (auto optConfig = getSMBusConfiguration(jsonConfig.at("smbus")))
        {
            configuration =
                std::make_unique<SMBusConfiguration>(std::move(*optConfig));
        }
    }
    else if (configurationName == "pcie")
    {
        if (auto optConfig = getPcieConfiguration(jsonConfig.at("pcie")))
        {
            configuration =
                std::make_unique<PcieConfiguration>(std::move(*optConfig));
        }
    }
    if (!configuration)
    {
        return std::nullopt;
    }
    return std::make_pair("MCTP-" + configurationName,
                          std::move(configuration));
}

std::optional<std::pair<std::string, std::unique_ptr<Configuration>>>
    getConfiguration(std::shared_ptr<sdbusplus::asio::connection> conn,
                     const std::string& configurationName,
                     const std::filesystem::path& configPath)
{
    auto configurationPair =
        getConfigurationFromEntityManager(conn, configurationName);
    if (!configurationPair)
    {
        configurationPair =
            getConfigurationFromFile(configPath, configurationName);
    }
    return configurationPair;
}

Configuration::~Configuration()
{
}

SMBusConfiguration::~SMBusConfiguration()
{
}

PcieConfiguration::~PcieConfiguration()
{
}
