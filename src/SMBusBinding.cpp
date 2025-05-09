#include "SMBusBinding.hpp"

#include "MCTPBinding.hpp"
#include "utils/utils.hpp"

extern "C" {
#include <errno.h>
#include <i2c/smbus.h>
#include <linux/i2c-dev.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
}

#include <boost/algorithm/string.hpp>
#include <filesystem>
#include <fstream>
#include <optional>
#include <phosphor-logging/log.hpp>
#include <regex>
#include <string>
#include <xyz/openbmc_project/Inventory/Decorator/I2CDevice/server.hpp>
#include <xyz/openbmc_project/MCTP/Binding/SMBus/server.hpp>

#include "libmctp-msgtypes.h"
#include "libmctp-smbus.h"

using smbus_server =
    sdbusplus::xyz::openbmc_project::MCTP::Binding::server::SMBus;
using I2CDeviceDecorator =
    sdbusplus::xyz::openbmc_project::Inventory::Decorator::server::I2CDevice;

namespace fs = std::filesystem;
std::map<MuxIdleModes, std::string> muxIdleModesMap{
    {MuxIdleModes::muxIdleModeConnect, "-1"},
    {MuxIdleModes::muxIdleModeDisconnect, "-2"},
};
static void throwRunTimeError(const std::string& err)
{
    phosphor::logging::log<phosphor::logging::level::ERR>(err.c_str());
    throw std::runtime_error(err);
}

void SMBusBinding::scanPort(const int scanFd,
                            std::set<std::pair<int, uint8_t>>& deviceMap)
{
    if (scanFd < 0)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Invalid I2C port fd");
        return;
    }

    for (uint8_t it : supportedEndpointSlaveAddress)
    {
        if (ioctl(scanFd, I2C_SLAVE, it) < 0)
        {
            // busy slave
            continue;
        }

        else
        {
            if ((it >= 0x30 && it <= 0x37) || (it >= 0x50 && it <= 0x5F))
            {
                // EEPROM address range. Use read to detect
                if (i2c_smbus_read_byte(scanFd) < 0)
                {
                    continue;
                }
            }
            else
            {
                if (i2c_smbus_write_quick(scanFd, I2C_SMBUS_WRITE) < 0)
                {
                    continue;
                }
            }
        }

        /* If we are scanning a mux fd, we will encounter root bus
         * i2c devices, which needs to be part of root bus's devicemap.
         * Skip adding them to the muxfd related devicemap */

        if (scanFd != outFd &&
            rootDeviceMap.count(std::make_pair(outFd, it)) != 0)
        {
            phosphor::logging::log<phosphor::logging::level::DEBUG>(
                ("Skipping device " + std::to_string(it)).c_str());
            continue;
        }

        phosphor::logging::log<phosphor::logging::level::DEBUG>(
            ("Adding device " + std::to_string(it)).c_str());

        deviceMap.insert(std::make_pair(scanFd, it));
    }
}

static bool isNum(const std::string& s)
{
    if (s.empty())
        return false;

    for (size_t i = 0; i < s.length(); i++)
        if (isdigit(s[i]) == false)
            return false;

    return true;
}

static bool findFiles(const fs::path& dirPath, const std::string& matchString,
                      std::vector<std::string>& foundPaths)
{
    if (!fs::exists(dirPath))
        return false;

    std::regex search(matchString);
    for (const auto& p : fs::directory_iterator(dirPath))
    {
        const std::string path = p.path().string();
        if (std::regex_search(path, search))
        {
            foundPaths.emplace_back(path);
        }
    }
    return true;
}

static bool getBusNumFromPath(const std::string& path, std::string& busStr)
{
    std::vector<std::string> parts;
    boost::split(parts, path, boost::is_any_of("-"));
    if (parts.size() == 2)
    {
        busStr = parts[1];
        if (isNum(busStr))
        {
            return true;
        }
    }
    return false;
}

static bool getRootBus(const std::string& muxBus, std::string& rootBus)
{
    auto ec = std::error_code();
    auto path = fs::read_symlink(
        fs::path("/sys/bus/i2c/devices/i2c-" + muxBus + "/mux_device"), ec);
    if (ec)
    {
        return false;
    }

    std::string filename = path.filename();
    std::vector<std::string> parts;
    boost::split(parts, filename, boost::is_any_of("-"));
    if (parts.size() == 2)
    {
        rootBus = parts[0];
        if (isNum(rootBus))
        {
            return true;
        }
    }
    return false;
}

static bool isMuxBus(const std::string& bus)
{
    return is_symlink(
        fs::path("/sys/bus/i2c/devices/i2c-" + bus + "/mux_device"));
}

std::map<int, int> SMBusBinding::getMuxFds(const std::string& rootPort)
{
    auto devDir = fs::path("/dev/");
    auto matchString = std::string(R"(i2c-\d+$)");
    std::vector<std::string> i2cBuses{};

    // Search for mux ports
    if (!findFiles(devDir, matchString, i2cBuses))
    {
        throwRunTimeError("unable to find i2c devices");
    }

    std::map<int, int> muxes;
    for (const auto& i2cPath : i2cBuses)
    {
        std::string i2cPort;
        std::string rootBus;
        if (!getBusNumFromPath(i2cPath, i2cPort))
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "i2c bus path is malformed",
                phosphor::logging::entry("PATH=%s", i2cPath.c_str()));
            continue;
        }

        if (!isMuxBus(i2cPort))
        {
            continue; // we found regular i2c port
        }

        if (!getRootBus(i2cPort, rootBus))
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "Error getting root port for the bus",
                phosphor::logging::entry("BUS:", i2cPort.c_str()));
            continue;
        }

        // Add to list of muxes if rootport matches to the one defined in mctp
        // configuration
        if (rootPort == rootBus)
        {
            int muxfd = open(i2cPath.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
            if (muxfd < 0)
            {
                continue;
            }
            muxes.emplace(muxfd, std::stoi(i2cPort));
        }
    }
    return muxes;
}

int SMBusBinding::getBusNumByFd(const int fd)
{
    if (muxPortMap.count(fd))
    {
        return muxPortMap.at(fd);
    }

    std::string busNum;
    if (getBusNumFromPath(bus, busNum))
    {
        return std::stoi(busNum);
    }

    // bus cannot be negative, return -1 on error
    return -1;
}

std::optional<std::vector<uint8_t>>
    SMBusBinding::getBindingPrivateData(uint8_t dstEid)
{
    mctp_smbus_pkt_private prvt = {};

    for (auto& device : smbusDeviceTable)
    {
        if (std::get<0>(device) == dstEid)
        {
            mctp_smbus_pkt_private temp = std::get<1>(device);
            prvt.fd = temp.fd;
            if (muxPortMap.count(prvt.fd) != 0)
            {
                prvt.mux_hold_timeout = 1000;
                prvt.mux_flags = IS_MUX_PORT;
            }
            else
            {
                prvt.mux_hold_timeout = 0;
                prvt.mux_flags = 0;
            }
            prvt.slave_addr = temp.slave_addr;
            uint8_t* prvtPtr = reinterpret_cast<uint8_t*>(&prvt);
            return std::vector<uint8_t>(prvtPtr, prvtPtr + sizeof(prvt));
        }
    }
    return std::nullopt;
}

bool SMBusBinding::reserveBandwidth(const mctp_eid_t eid,
                                    const uint16_t timeout)
{
    if (rsvBWActive && eid != reservedEID)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            (("reserveBandwidth is not allowed for EID: " +
              std::to_string(eid) + ". It is active for EID: ") +
             std::to_string(reservedEID))
                .c_str());
        return false;
    }
    std::optional<std::vector<uint8_t>> pvtData = getBindingPrivateData(eid);
    if (!pvtData)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "reserveBandwidth failed. Invalid destination EID");
        return false;
    }
    const mctp_smbus_pkt_private* prvt =
        reinterpret_cast<const mctp_smbus_pkt_private*>(pvtData->data());
    if (prvt->mux_flags != IS_MUX_PORT)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "reserveBandwidth not required, fd is not a mux port");
        return false;
    }

    if (!rsvBWActive)
    {
        if (mctp_smbus_init_pull_model(prvt) < 0)
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "reserveBandwidth: init pull model failed");
            return false;
        }
        // TODO: Set only the required MUX.
        setMuxIdleMode(MuxIdleModes::muxIdleModeConnect);
        rsvBWActive = true;
        reservedEID = eid;
    }

    startTimerAndReleaseBW(timeout, *prvt);
    return true;
}

bool SMBusBinding::releaseBandwidth(const mctp_eid_t eid)
{
    if (!rsvBWActive || eid != reservedEID)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            (("reserveBandwidth is not active for EID: ") + std::to_string(eid))
                .c_str());
        return false;
    }
    reserveBWTimer.cancel();
    return true;
}

void SMBusBinding::startTimerAndReleaseBW(const uint16_t interval,
                                          const mctp_smbus_pkt_private prvt)
{
    // expires_after() return the number of asynchronous operations that were
    // cancelled.
    ret = reserveBWTimer.expires_after(
        std::chrono::milliseconds(interval * 1000));
    reserveBWTimer.async_wait([this,
                               prvt](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted)
        {
            // timer aborted do nothing
            phosphor::logging::log<phosphor::logging::level::DEBUG>(
                "startTimerAndReleaseBW: timer operation_aborted");
        }
        else if (ec)
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "startTimerAndReleaseBW: reserveBWTimer failed");
        }
        if (ret)
        {
            phosphor::logging::log<phosphor::logging::level::DEBUG>(
                "startTimerAndReleaseBW: timer restarted");
            ret = 0;
            return;
        }
        setMuxIdleMode(MuxIdleModes::muxIdleModeDisconnect);
        if (mctp_smbus_exit_pull_model(&prvt) < 0)
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "startTimerAndReleaseBW: mctp_smbus_exit_pull_model failed");
            return;
        }
        rsvBWActive = false;
        reservedEID = 0;
    });
}

SMBusBinding::SMBusBinding(
    std::shared_ptr<sdbusplus::asio::connection> conn,
    std::shared_ptr<object_server>& objServer, const std::string& objPath,
    const SMBusConfiguration& conf, boost::asio::io_context& ioc,
    std::shared_ptr<boost::asio::posix::stream_descriptor>&& i2cMuxMonitor) :
    MctpBinding(conn, objServer, objPath, conf, ioc,
                mctp_server::BindingTypes::MctpOverSmbus),
    smbusReceiverFd(ioc), reserveBWTimer(ioc), scanTimer(ioc),
    addRootDevices(true), muxMonitor{std::move(i2cMuxMonitor)},
    refreshMuxTimer(ioc)
{
    smbusInterface = objServer->add_interface(objPath, smbus_server::interface);

    try
    {
        arpMasterSupport = conf.arpMasterSupport;
        bus = conf.bus;
        bmcSlaveAddr = conf.bmcSlaveAddr;
        supportedEndpointSlaveAddress = conf.supportedEndpointSlaveAddress;
        scanInterval = conf.scanInterval;

        // TODO: If we are not top most busowner, wait for top mostbus owner
        // to issue EID Pool
        if (conf.mode == mctp_server::BindingModeTypes::BusOwner)
        {
            eidPool.initializeEidPool(conf.eidPool);
        }

        if (bindingModeType == mctp_server::BindingModeTypes::BusOwner)
        {
            discoveredFlag = DiscoveryFlags::kNotApplicable;
        }
        else
        {
            discoveredFlag = DiscoveryFlags::kUnDiscovered;
            smbusRoutingInterval = conf.routingIntervalSec;
            smbusRoutingTableTimer =
                std::make_unique<boost::asio::steady_timer>(ioc);
        }

        registerProperty(smbusInterface, "DiscoveredFlag",
                         convertToString(discoveredFlag));
        registerProperty(smbusInterface, "ArpMasterSupport", arpMasterSupport);
        registerProperty(smbusInterface, "BusPath", bus);
        registerProperty(smbusInterface, "BmcSlaveAddress", bmcSlaveAddr);

        if (smbusInterface->initialize() == false)
        {
            throw std::system_error(
                std::make_error_code(std::errc::function_not_supported));
        }
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "SMBus Interface init failed",
            phosphor::logging::entry("Exception:", e.what()));
        throw;
    }
}

void SMBusBinding::triggerDeviceDiscovery()
{
    scanTimer.cancel();
}

void SMBusBinding::scanDevices()
{
    phosphor::logging::log<phosphor::logging::level::DEBUG>("Scanning devices");

    boost::asio::spawn(io, [this](boost::asio::yield_context yield) {
        if (!rsvBWActive)
        {
            deviceWatcher.deviceDiscoveryInit();
            initEndpointDiscovery(yield);
        }
        else
        {
            phosphor::logging::log<phosphor::logging::level::DEBUG>(
                "Reserve bandwidth active. Unable to scan devices");
        }

        scanTimer.expires_after(std::chrono::seconds(scanInterval));
        scanTimer.async_wait([this](const boost::system::error_code& ec) {
            if (ec && ec != boost::asio::error::operation_aborted)
            {
                phosphor::logging::log<phosphor::logging::level::ERR>(
                    "Device scanning timer failed");
                return;
            }
            if (ec == boost::asio::error::operation_aborted)
            {
                phosphor::logging::log<phosphor::logging::level::WARNING>(
                    "Device scan wait timer aborted. Re-triggering device "
                    "discovery");
            }
            scanDevices();
        });
    }, boost::asio::detached);
}

void SMBusBinding::restoreMuxIdleMode()
{
    auto logMuxErr = [](const std::string& path) {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "Unable to restore mux idle mode",
            phosphor::logging::entry("MUX_PATH=%s", path.c_str()));
    };

    for (const auto& [path, idleMode] : muxIdleModeMap)
    {
        fs::path idlePath = fs::path(path);
        if (!fs::exists(idlePath))
        {
            logMuxErr(path);
            continue;
        }

        std::fstream idleFile(idlePath);
        if (idleFile.good())
        {
            idleFile << idleMode;
            if (idleFile.bad())
            {
                logMuxErr(path);
            }
        }
        else
        {
            logMuxErr(path);
        }
    }
}

void SMBusBinding::setMuxIdleMode(const MuxIdleModes mode)
{
    auto itr = muxIdleModesMap.find(mode);
    if (itr == muxIdleModesMap.end())
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Inavlid mux idle mode");
        return;
    }
    std::string rootPort;
    if (!getBusNumFromPath(bus, rootPort))
    {
        throwRunTimeError("Error in finding root port");
    }

    fs::path rootPath = fs::path("/sys/bus/i2c/devices/i2c-" + rootPort + "/");
    std::string matchString = rootPort + std::string(R"(-\d+$)");
    std::vector<std::string> i2cMuxes{};
    static bool muxIdleModeFlag = false;

    // Search for mux ports
    if (!findFiles(rootPath, matchString, i2cMuxes))
    {
        phosphor::logging::log<phosphor::logging::level::DEBUG>(
            "No mux interfaces found");
        return;
    }

    for (const auto& muxPath : i2cMuxes)
    {
        std::string path = muxPath + "/idle_state";
        fs::path idlePath = fs::path(path);
        if (!fs::exists(idlePath))
        {
            continue;
        }

        std::fstream idleFile(idlePath);
        if (idleFile.good())
        {
            if (!muxIdleModeFlag)
            {
                std::string currentMuxIdleMode;
                idleFile >> currentMuxIdleMode;
                muxIdleModeMap.insert_or_assign(path, currentMuxIdleMode);

                phosphor::logging::log<phosphor::logging::level::DEBUG>(
                    (path + " " + currentMuxIdleMode).c_str());
            }

            idleFile << itr->second;
        }
        else
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "Unable to set idle mode for mux",
                phosphor::logging::entry("MUX_PATH=%s", idlePath.c_str()));
        }
    }
    muxIdleModeFlag = true;
}

inline void SMBusBinding::handleMuxInotifyEvent(const std::string& name)
{
    if (boost::starts_with(name, "i2c-"))
    {
        phosphor::logging::log<phosphor::logging::level::DEBUG>(
            ("Detected change on bus " + name).c_str());

        // Delay 1s to refresh only once as multiple i2c
        // buses will change when handling mux
        refreshMuxTimer.expires_after(std::chrono::seconds(1));
        refreshMuxTimer.async_wait(
            [this](const boost::system::error_code& ec2) {
                // Calling expires_after will invoke this handler with
                // operation_aborted, just ignore it as we only need to
                // rescan mux on last inotify event
                if (ec2 == boost::asio::error::operation_aborted)
                {
                    return;
                }

                std::string rootPort;
                if (!getBusNumFromPath(bus, rootPort))
                {
                    throwRunTimeError("Error in finding root port");
                }

                phosphor::logging::log<phosphor::logging::level::INFO>(
                    "i2c bus change detected, refreshing "
                    "muxPortMap");
                muxPortMap = getMuxFds(rootPort);
                scanTimer.cancel();
            });
    }
}

void SMBusBinding::monitorMuxChange()
{
    static std::array<char, 4096> readBuffer;

    muxMonitor->async_read_some(
        boost::asio::buffer(readBuffer),
        [&](const boost::system::error_code& ec, std::size_t bytesTransferred) {
            if (ec)
            {
                phosphor::logging::log<phosphor::logging::level::ERR>(
                    ("monitorMuxChange: Callback Error " + ec.message())
                        .c_str());
                return;
            }
            size_t index = 0;
            while ((index + sizeof(inotify_event)) <= bytesTransferred)
            {
                // Using reinterpret_cast gives a cast-align error here
                inotify_event event;
                const char* eventPtr = &readBuffer[index];
                memcpy(&event, eventPtr, sizeof(inotify_event));
                switch (event.mask)
                {
                    case IN_CREATE:
                    case IN_MOVED_TO:
                    case IN_DELETE:
                        std::string name(eventPtr + sizeof(inotify_event));
                        handleMuxInotifyEvent(name);
                }
                index += sizeof(inotify_event) + event.len;
            }
            monitorMuxChange();
        });
}

void SMBusBinding::setupMuxMonitor()
{
    int fd = inotify_init1(IN_NONBLOCK);
    if (fd < 0)
    {
        throwRunTimeError("inotify_init failed");
    }
    int watch =
        inotify_add_watch(fd, "/dev", IN_CREATE | IN_MOVED_TO | IN_DELETE);
    if (watch < 0)
    {
        throwRunTimeError("inotify_add_watch failed");
    }
    muxMonitor->assign(fd);
    monitorMuxChange();
}

void SMBusBinding::initializeBinding()
{
    try
    {
        initializeMctp();
        auto rootPort = SMBusInit();
        phosphor::logging::log<phosphor::logging::level::INFO>(
            "Scanning root port");
        setMuxIdleMode(MuxIdleModes::muxIdleModeDisconnect);
        // Scan root port
        scanPort(outFd, rootDeviceMap);
        muxPortMap = getMuxFds(rootPort);
    }

    catch (const std::exception& e)
    {
        auto error =
            "Failed to initialise SMBus binding: " + std::string(e.what());
        phosphor::logging::log<phosphor::logging::level::ERR>(error.c_str());
        return;
    }

    setupPowerMatch(connection, this);
    setupMuxMonitor();

    scanDevices();
}

SMBusBinding::~SMBusBinding()
{
    restoreMuxIdleMode();

    if (smbusReceiverFd.native_handle() >= 0)
    {
        smbusReceiverFd.release();
    }
    if (inFd >= 0)
    {
        close(inFd);
    }
    if (outFd >= 0)
    {
        close(outFd);
    }
    mctp_smbus_free(smbus);
    objectServer->remove_interface(smbusInterface);
}

std::string SMBusBinding::SMBusInit()
{
    smbus = mctp_smbus_init();
    if (smbus == nullptr)
    {
        throwRunTimeError("Error in mctp smbus init");
    }

    if (mctp_smbus_register_bus(smbus, mctp, ownEid) != 0)
    {
        throwRunTimeError("Error in SMBus binding registration");
    }

    mctp_set_rx_all(mctp, &MctpBinding::rxMessage,
                    static_cast<MctpBinding*>(this));
    mctp_set_rx_raw(mctp, &MctpBinding::onRawMessage);
    mctp_set_rx_ctrl(mctp, &MctpBinding::handleMCTPControlRequests,
                     static_cast<MctpBinding*>(this));
    std::string rootPort;

    if (!getBusNumFromPath(bus, rootPort))
    {
        throwRunTimeError("Error in opening smbus rootport");
    }

    std::stringstream addrStream;
    addrStream.str("");

    int addr7bit = (bmcSlaveAddr >> 1);

    // want the format as 0x0Y
    addrStream << std::setfill('0') << std::setw(2) << std::hex << addr7bit;

    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        ("Slave Address " + addrStream.str()).c_str());

    // MSB fixed to 10 so hex is 0x10XX ~ 0x1005
    std::string hexSlaveAddr("10");
    hexSlaveAddr.append(addrStream.str());

    std::string inputDevice = "/sys/bus/i2c/devices/" + rootPort + "-" +
                              hexSlaveAddr + "/slave-mqueue";

    // Source slave address is in 8 bit format and should always be an odd
    // number
    mctp_smbus_set_src_slave_addr(smbus, bmcSlaveAddr | 0x01);

    inFd = open(inputDevice.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);

    // Doesn't exist, try to create one
    if (inFd < 0)
    {
        std::string newInputDevice =
            "/sys/bus/i2c/devices/i2c-" + rootPort + "/new_device";
        std::string para("slave-mqueue 0x");
        para.append(hexSlaveAddr);

        std::fstream deviceFile;
        deviceFile.open(newInputDevice, std::ios::out);
        deviceFile << para;
        deviceFile.close();
        inFd = open(inputDevice.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);

        if (inFd < 0)
        {
            throwRunTimeError("Error in opening smbus binding in_bus");
        }
    }

    // Open root bus
    outFd = open(bus.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (outFd < 0)
    {
        throwRunTimeError("Error in opening smbus binding out bus");
    }
    mctp_smbus_set_in_fd(smbus, inFd);
    mctp_smbus_set_out_fd(smbus, outFd);

    smbusReceiverFd.assign(inFd);
    readResponse();
    return rootPort;
}

void SMBusBinding::readResponse()
{
    smbusReceiverFd.async_wait(
        boost::asio::posix::descriptor_base::wait_error, [this](const boost::system::error_code& ec) {
            if (ec)
            {
                phosphor::logging::log<phosphor::logging::level::ERR>(
                    "Error: mctp_smbus_read()");
                readResponse();
            }
            // through libmctp this will invoke rxMessage and message assembly
            mctp_smbus_read(smbus);
            readResponse();
        });
}

void SMBusBinding::scanMuxBus(std::set<std::pair<int, uint8_t>>& deviceMap)
{
    for (const auto& [muxFd, muxPort] : muxPortMap)
    {
        // Scan each port only once
        phosphor::logging::log<phosphor::logging::level::DEBUG>(
            ("Scanning Mux " + std::to_string(muxPort)).c_str());
        scanPort(muxFd, deviceMap);
    }
}

std::optional<std::string>
    SMBusBinding::getLocationCode(const std::vector<uint8_t>& bindingPrivate)
{
    const std::filesystem::path muxSymlinkDirPath("/dev/i2c-mux");
    auto smbusBindingPvt =
        reinterpret_cast<const mctp_smbus_pkt_private*>(bindingPrivate.data());
    const size_t busNum = getBusNumByFd(smbusBindingPvt->fd);

    if (!std::filesystem::exists(muxSymlinkDirPath) ||
        !std::filesystem::is_directory(muxSymlinkDirPath))
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "/dev/i2c-mux does not exist");
        return std::nullopt;
    }
    for (auto file :
         std::filesystem::recursive_directory_iterator(muxSymlinkDirPath))
    {
        if (!file.is_symlink())
        {
            continue;
        }
        std::string linkPath =
            std::filesystem::read_symlink(file.path()).string();
        if (boost::algorithm::ends_with(linkPath,
                                        "i2c-" + std::to_string(busNum)))
        {
            std::string slotName = file.path().filename().string();
            // Only take the part before "_Mux" in mux name
            std::string muxFullname =
                file.path().parent_path().filename().string();
            std::string muxName =
                muxFullname.substr(0, muxFullname.find("_Mux"));
            std::string location = muxName + ' ' + slotName;
            std::replace(location.begin(), location.end(), '_', ' ');
            return location;
        }
    }
    return std::nullopt;
}

void SMBusBinding::populateDeviceProperties(
    const mctp_eid_t eid, const std::vector<uint8_t>& bindingPrivate)
{
    auto smbusBindingPvt =
        reinterpret_cast<const mctp_smbus_pkt_private*>(bindingPrivate.data());

    std::string mctpEpObj =
        "/xyz/openbmc_project/mctp/device/" + std::to_string(eid);

    std::shared_ptr<dbus_interface> smbusIntf;
    smbusIntf =
        objectServer->add_interface(mctpEpObj, I2CDeviceDecorator::interface);
    smbusIntf->register_property<size_t>("Bus",
                                         getBusNumByFd(smbusBindingPvt->fd));
    smbusIntf->register_property<size_t>("Address",
                                         smbusBindingPvt->slave_addr);
    smbusIntf->initialize();
    deviceInterface.emplace(eid, std::move(smbusIntf));
}

void SMBusBinding::initEndpointDiscovery(boost::asio::yield_context& yield)
{
    std::set<std::pair<int, uint8_t>> registerDeviceMap;

    if (addRootDevices)
    {
        addRootDevices = false;
        for (const auto& device : rootDeviceMap)
        {
            registerDeviceMap.insert(device);
        }
    }

    // Scan mux bus to get the list of fd and the corresponding slave address of
    // all the mux ports
    scanMuxBus(registerDeviceMap);

    /* Since i2c muxes restrict that only one command needs to be
     * in flight, we cannot register multiple endpoints in parallel.
     * Thus, in a single yield_context, all the discovered devices
     * are attempted with registration sequentially */
    for (const auto& device : registerDeviceMap)
    {
        phosphor::logging::log<phosphor::logging::level::DEBUG>(
            ("Device discovery: Checking device " +
             std::to_string(std::get<1>(device)))
                .c_str());

        struct mctp_smbus_pkt_private smbusBindingPvt;
        smbusBindingPvt.fd = std::get<0>(device);

        if (muxPortMap.count(smbusBindingPvt.fd) != 0)
        {
            smbusBindingPvt.mux_hold_timeout = ctrlTxRetryDelay;
            smbusBindingPvt.mux_flags = 0x80;
        }
        else
        {
            smbusBindingPvt.mux_hold_timeout = 0;
            smbusBindingPvt.mux_flags = 0;
        }
        /* Set 8 bit i2c slave address */
        smbusBindingPvt.slave_addr =
            static_cast<uint8_t>((std::get<1>(device) << 1));

        auto const ptr = reinterpret_cast<uint8_t*>(&smbusBindingPvt);
        std::vector<uint8_t> bindingPvtVect(ptr, ptr + sizeof(smbusBindingPvt));
        if (!deviceWatcher.isDeviceGoodForInit(bindingPvtVect))
        {
            phosphor::logging::log<phosphor::logging::level::DEBUG>(
                "Device found in ignore list. Skipping discovery");
            continue;
        }

        mctp_eid_t registeredEid = getEIDFromDeviceTable(bindingPvtVect);
        std::optional<mctp_eid_t> eid =
            registerEndpoint(yield, bindingPvtVect, registeredEid);

        if (eid.has_value() && eid.value() != MCTP_EID_NULL)
        {
            DeviceTableEntry_t entry =
                std::make_pair(eid.value(), smbusBindingPvt);
            bool newEntry = !isDeviceEntryPresent(entry, smbusDeviceTable);
            bool noDeviceUpdate = !newEntry && eid.value() == registeredEid;
            bool deviceUpdated = !newEntry && eid.value() != registeredEid;

            auto logDeviceDetails = [&]() {
                phosphor::logging::log<phosphor::logging::level::INFO>(
                    ("SMBus device at bus:" +
                     std::to_string(getBusNumByFd(smbusBindingPvt.fd)) +
                     ", 8 bit address: " +
                     std::to_string(smbusBindingPvt.slave_addr) +
                     " registered at EID " + std::to_string(eid.value()))
                        .c_str());
            };

            if (noDeviceUpdate)
            {
                continue;
            }
            else if (newEntry)
            {
                smbusDeviceTable.push_back(entry);
                logDeviceDetails();
            }
            else if (deviceUpdated)
            {
                unregisterEndpoint(registeredEid);
                removeDeviceTableEntry(registeredEid);
                smbusDeviceTable.push_back(entry);
                logDeviceDetails();
            }
        }
    }

    // Add to check root device
    if (registerDeviceMap.empty() && rootDeviceMap.empty())
    {
        phosphor::logging::log<phosphor::logging::level::DEBUG>(
            "No device found");
        for (auto& deviceTableEntry : smbusDeviceTable)
        {
            unregisterEndpoint(std::get<0>(deviceTableEntry));
        }
        smbusDeviceTable.clear();
    }
}

// TODO: This method is a placeholder and has not been tested
bool SMBusBinding::handleGetEndpointId(mctp_eid_t destEid, void* bindingPrivate,
                                       std::vector<uint8_t>& request,
                                       std::vector<uint8_t>& response)
{
    if (!MctpBinding::handleGetEndpointId(destEid, bindingPrivate, request,
                                          response))
    {
        return false;
    }

    auto const ptr = reinterpret_cast<uint8_t*>(bindingPrivate);

    if (auto bindingPvtVect = getBindingPrivateData(destEid))
    {
        std::copy(bindingPvtVect->begin(), bindingPvtVect->end(), ptr);
        return true;
    }
    return false;
}

bool SMBusBinding::handleSetEndpointId(mctp_eid_t destEid, void* bindingPrivate,
                                       std::vector<uint8_t>& request,
                                       std::vector<uint8_t>& response)
{
    if (!MctpBinding::handleSetEndpointId(destEid, bindingPrivate, request,
                                          response))
    {
        return false;
    }

    response.resize(sizeof(mctp_ctrl_resp_set_eid));
    auto resp = reinterpret_cast<mctp_ctrl_resp_set_eid*>(response.data());

    if (resp->completion_code == MCTP_CTRL_CC_SUCCESS)
    {
        updateDiscoveredFlag(DiscoveryFlags::kDiscovered);
        mctpInterface->set_property("Eid", ownEid);

        mctp_smbus_pkt_private* smbusPrivate =
            reinterpret_cast<mctp_smbus_pkt_private*>(bindingPrivate);
        busOwnerSlaveAddr = smbusPrivate->slave_addr;
        busOwnerFd = smbusPrivate->fd;

    }

    return true;
}

bool SMBusBinding::handleGetVersionSupport(mctp_eid_t destEid,
                                           void* bindingPrivate,
                                           std::vector<uint8_t>& request,
                                           std::vector<uint8_t>& response)
{
    if (!MctpBinding::handleGetVersionSupport(destEid, bindingPrivate, request,
                                              response))
    {
        return false;
    }

    return true;
}

bool SMBusBinding::handleGetMsgTypeSupport(mctp_eid_t destEid,
                                           void* bindingPrivate,
                                           std::vector<uint8_t>& request,
                                           std::vector<uint8_t>& response)
{
    if (!MctpBinding::handleGetMsgTypeSupport(destEid, bindingPrivate, request,
                                              response))
    {
        return false;
    }

    return true;
}

bool SMBusBinding::handleGetVdmSupport(mctp_eid_t destEid,
                                       [[maybe_unused]] void* bindingPrivate,
                                       std::vector<uint8_t>& request,
                                       std::vector<uint8_t>& response)
{
    response.resize(sizeof(mctp_pci_ctrl_resp_get_vdm_support));

    if (request.size() < sizeof(struct mctp_ctrl_cmd_get_vdm_support))
    {
        return false;
    }

    struct mctp_ctrl_cmd_get_vdm_support* req =
        reinterpret_cast<struct mctp_ctrl_cmd_get_vdm_support*>(request.data());

    /* Generic library API. Specialized later on. */
    struct mctp_ctrl_resp_get_vdm_support* libResp =
        reinterpret_cast<struct mctp_ctrl_resp_get_vdm_support*>(
            response.data());

    if (mctp_ctrl_cmd_get_vdm_support(mctp, destEid, libResp) < 0)
    {
        return false;
    }

    /* Cast to full binding specific response. */
    mctp_pci_ctrl_resp_get_vdm_support* resp =
        reinterpret_cast<mctp_pci_ctrl_resp_get_vdm_support*>(response.data());
    uint8_t setIndex = req->vendor_id_set_selector;

    if (setIndex + 1U > vdmSetDatabase.size())
    {
        resp->completion_code = MCTP_CTRL_CC_ERROR_INVALID_DATA;
        response.resize(sizeof(mctp_ctrl_msg_hdr) +
                        sizeof(resp->completion_code));
        return true;
    }

    if (setIndex + 1U == vdmSetDatabase.size())
    {
        resp->vendor_id_set_selector = vendorIdNoMoreSets;
    }
    else
    {
        resp->vendor_id_set_selector = static_cast<uint8_t>(setIndex + 1U);
    }
    resp->vendor_id_format = vdmSetDatabase[setIndex].vendorIdFormat;
    resp->vendor_id_data = vdmSetDatabase[setIndex].vendorId;
    resp->command_set_type = vdmSetDatabase[setIndex].commandSetType;

    return true;
}

void SMBusBinding::removeDeviceTableEntry(const mctp_eid_t eid)
{
    smbusDeviceTable.erase(std::remove_if(smbusDeviceTable.begin(),
                                          smbusDeviceTable.end(),
                                          [eid](auto const& tableEntry) {
                                              return (tableEntry.first == eid);
                                          }),
                           smbusDeviceTable.end());
}

mctp_eid_t SMBusBinding::getEIDFromDeviceTable(
    const std::vector<uint8_t>& bindingPrivate)
{
    mctp_eid_t eid = MCTP_EID_NULL;
    for (auto& deviceEntry : smbusDeviceTable)
    {
        const mctp_smbus_pkt_private* ptr =
            reinterpret_cast<const mctp_smbus_pkt_private*>(
                bindingPrivate.data());
        mctp_smbus_pkt_private bindingDataEntry = std::get<1>(deviceEntry);
        if (bindingDataEntry.slave_addr == ptr->slave_addr &&
            bindingDataEntry.fd == ptr->fd)
        {
            eid = std::get<0>(deviceEntry);
            break;
        }
    }
    return eid;
}

std::string SMBusBinding::convertToString(DiscoveryFlags flag)
{
    std::string discoveredStr;
    switch (flag)
    {
        case DiscoveryFlags::kUnDiscovered: {
            discoveredStr = "Undiscovered";
            break;
        }
        case DiscoveryFlags::kDiscovered: {
            discoveredStr = "Discovered";
            break;
        }
        case DiscoveryFlags::kNotApplicable:
        default: {
            discoveredStr = "NotApplicable";
            break;
        }
    }

    return discoveredStr;
}

void SMBusBinding::updateDiscoveredFlag(DiscoveryFlags flag)
{
    discoveredFlag = flag;
    smbusInterface->set_property("DiscoveredFlag", convertToString(flag));

    if (DiscoveryFlags::kDiscovered == flag) {
        updateRoutingTable();
    }
}

void SMBusBinding::addUnknownEIDToDeviceTable(const mctp_eid_t eid,
                                              void* bindingPrivate)
{
    if (bindingPrivate == nullptr)
    {
        return;
    }

    auto deviceIter = std::find_if(
        smbusDeviceTable.begin(), smbusDeviceTable.end(),
        [eid](auto const eidEntry) { return std::get<0>(eidEntry) == eid; });

    if (deviceIter != smbusDeviceTable.end())
    {
        return;
    }

    auto bindingPtr = reinterpret_cast<mctp_smbus_pkt_private*>(bindingPrivate);

    struct mctp_smbus_pkt_private smbusBindingPvt = {};
    smbusBindingPvt.fd = bindingPtr->fd;
    smbusBindingPvt.mux_hold_timeout = bindingPtr->mux_hold_timeout;
    smbusBindingPvt.mux_flags = bindingPtr->mux_flags;
    smbusBindingPvt.slave_addr =
        static_cast<uint8_t>((bindingPtr->slave_addr) & (~1));

    smbusDeviceTable.emplace_back(std::make_pair(eid, smbusBindingPvt));

    phosphor::logging::log<phosphor::logging::level::INFO>(
        ("New EID added to device table. EID = " + std::to_string(eid))
            .c_str());
}

bool SMBusBinding::isBindingDataSame(const mctp_smbus_pkt_private& dataMain,
                                     const mctp_smbus_pkt_private& dataTmp)
{
    if (std::tie(dataMain.fd, dataMain.slave_addr) ==
        std::tie(dataTmp.fd, dataTmp.slave_addr))
    {
        return true;
    }
    return false;
}

bool SMBusBinding::isDeviceTableChanged(
    const std::vector<DeviceTableEntry_t>& tableMain,
    const std::vector<DeviceTableEntry_t>& tableTmp)
{
    if (tableMain.size() != tableTmp.size())
    {
        return true;
    }
    for (size_t i = 0; i < tableMain.size(); i++)
    {
        if ((std::get<0>(tableMain[i]) != std::get<0>(tableTmp[i])) ||
            (!isBindingDataSame(std::get<1>(tableMain[i]),
                                std::get<1>(tableTmp[i]))))
        {
            return true;
        }
    }
    return false;
}

bool SMBusBinding::isDeviceEntryPresent(
    const DeviceTableEntry_t& deviceEntry,
    const std::vector<DeviceTableEntry_t>& deviceTable)
{
    for (size_t i = 0; i < deviceTable.size(); i++)
    {
        if (std::get<0>(deviceTable[i]) == std::get<0>(deviceEntry))
        {
            return true;
        }
    }
    return false;
}

void SMBusBinding::updateRoutingTable()
{
    if (discoveredFlag != DiscoveryFlags::kDiscovered)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "SMBus Get Routing Table failed, undiscovered");
        return;
    }

    struct mctp_smbus_pkt_private pktPrv = {};
    pktPrv.fd = busOwnerFd;
    pktPrv.mux_hold_timeout = 0;
    pktPrv.mux_flags = 0;
    pktPrv.slave_addr = busOwnerSlaveAddr;
    uint8_t* pktPrvPtr = reinterpret_cast<uint8_t*>(&pktPrv);
    std::vector<uint8_t> prvData = std::vector<uint8_t>(
        pktPrvPtr, pktPrvPtr + sizeof(mctp_smbus_pkt_private));

    boost::asio::spawn(io, [prvData, this](boost::asio::yield_context yield) {
        std::vector<uint8_t> getRoutingTableEntryResp = {};
        std::vector<DeviceTableEntry_t> smbusDeviceTableTmp;
        uint8_t entryHandle = 0x00;
        uint8_t entryHdlCounter = 0x00;
        while ((entryHandle != 0xff) && (entryHdlCounter < 0xff))
        {
            if (!getRoutingTableCtrlCmd(yield, prvData, busOwnerEid,
                                        entryHandle, getRoutingTableEntryResp))
            {
                phosphor::logging::log<phosphor::logging::level::ERR>(
                    "Get Routing Table failed");
                return;
            }

            auto routingTableHdr =
                reinterpret_cast<mctp_ctrl_resp_get_routing_table*>(
                    getRoutingTableEntryResp.data());
            size_t phyAddrOffset = sizeof(mctp_ctrl_resp_get_routing_table);

            for (uint8_t entryIndex = 0;
                 entryIndex < routingTableHdr->number_of_entries; entryIndex++)
            {
                auto routingTableEntry =
                    reinterpret_cast<get_routing_table_entry*>(
                        getRoutingTableEntryResp.data() + phyAddrOffset);

                phyAddrOffset += sizeof(get_routing_table_entry);

                if ((routingTableEntry->phys_transport_binding_id ==
                     MCTP_BINDING_SMBUS) &&
                    (routingTableEntry->phys_address_size == 1))
                {
                    struct mctp_smbus_pkt_private smbusBindingPvt = {};
                    smbusBindingPvt.fd = busOwnerFd;
                    smbusBindingPvt.mux_hold_timeout = 0;
                    smbusBindingPvt.mux_flags = 0;
                    smbusBindingPvt.slave_addr = static_cast<uint8_t>(
                        (getRoutingTableEntryResp[phyAddrOffset] << 1));

                    for (uint8_t eidRange = 0;
                         eidRange < routingTableEntry->eid_range_size;
                         eidRange++)
                    {
                        smbusDeviceTableTmp.push_back(std::make_pair(
                            routingTableEntry->starting_eid + eidRange,
                            smbusBindingPvt));
                    }
                }
                phyAddrOffset += routingTableEntry->phys_address_size;
            }
            entryHandle = routingTableHdr->next_entry_handle;
        }

        if (isDeviceTableChanged(smbusDeviceTable, smbusDeviceTableTmp))
        {
            processRoutingTableChanges(smbusDeviceTableTmp, yield, prvData);
            smbusDeviceTable = smbusDeviceTableTmp;
        }
        entryHdlCounter++;
    }, boost::asio::detached);

    smbusRoutingTableTimer->expires_after(
        std::chrono::seconds(smbusRoutingInterval));
    smbusRoutingTableTimer->async_wait(
        std::bind(&SMBusBinding::updateRoutingTable, this));
}

/* Function takes new routing table, detect changes and creates or removes
 * device interfaces on dbus.
 */
void SMBusBinding::processRoutingTableChanges(
    const std::vector<DeviceTableEntry_t>& newTable,
    boost::asio::yield_context& yield, const std::vector<uint8_t>& prvData)
{
    /* find removed endpoints, in case entry is not present
     * in the newly read routing table remove dbus interface
     * for this device
     */
    for (auto& deviceTableEntry : smbusDeviceTable)
    {
        if (!isDeviceEntryPresent(deviceTableEntry, newTable))
        {
            unregisterEndpoint(std::get<0>(deviceTableEntry));
        }
    }

    /* find new endpoints, in case entry is in the newly read
     * routing table but not present in the routing table stored as
     * the class member, register new dbus device interface
     */
    for (auto& deviceTableEntry : newTable)
    {
        if (!isDeviceEntryPresent(deviceTableEntry, smbusDeviceTable))
        {
            registerEndpoint(yield, prvData, std::get<0>(deviceTableEntry),
                             mctp_server::BindingModeTypes::Endpoint);
        }
    }
}

void SMBusBinding::updateRoutingTableEntry(
    mctpd::RoutingTable::Entry entry, const std::vector<uint8_t>& privateData)
{
    constexpr uint8_t transportIdSmbus = 0x01;
    entry.routeEntry.routing_info.phys_transport_binding_id = transportIdSmbus;

    auto smbusData =
        reinterpret_cast<const mctp_smbus_pkt_private*>(privateData.data());
    entry.routeEntry.phys_address[0] = smbusData->slave_addr; // 8bit address
    entry.routeEntry.routing_info.phys_address_size =
        sizeof(smbusData->slave_addr);

    routingTable.updateEntry(entry.routeEntry.routing_info.starting_eid, entry);
}
