#include "appledeviceinfo.h"

#ifndef Q_OS_DARWIN
QString AppleDeviceInfo::iconSource(const AppleRemoteDeviceInfo&)
{
    return {};
}
#endif
