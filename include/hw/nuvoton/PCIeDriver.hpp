#pragma once

#include "hw/PCIeDriver.hpp"

#include <libmctp-nupcie.h>

#include <boost/asio/posix/stream_descriptor.hpp>
#include <phosphor-logging/log.hpp>

namespace hw
{

namespace nuvoton
{

class PCIeDriver : public hw::PCIeDriver
{
  public:
    PCIeDriver(boost::asio::io_context& ioc);
    ~PCIeDriver() override;

    void init() override;
    mctp_binding* binding() override;
    void pollRx() override;

    bool registerAsDefault() override;
    bool getBdf(uint16_t& bdf) override;
    uint8_t getMediumId() override;
    bool setEndpointMap(std::vector<EidInfo>& endpoints) override;

  private:
    boost::asio::posix::stream_descriptor streamMonitor;
    mctp_binding_nupcie* pcie{};
};

} // namespace nuvoton
} // namespace hw
