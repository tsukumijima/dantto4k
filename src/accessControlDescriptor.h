#pragma once
#include "mmtDescriptorBase.h"
#include "mmtGeneralLocationInfo.h"
#include <list>

namespace MmtTlv {

class AccessControlDescriptor
    : public MmtDescriptorTemplate<0x8004> {
public:
    bool unpack(Common::Stream& stream) override;
    
    uint8_t caSystemId;
    MmtGeneralLocationInfo locationInfo;
    std::vector<uint8_t> privateData;
};

}