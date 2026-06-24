#pragma once
/**
 * @defgroup main Audio Over OSC
 * @brief Audio over OSC (AOO) is a protocol to send audio data over OSC
 * @file AOO.h
 * @author Phil Schatzmann
 * @copyright GPLv3
 *
 * @defgroup aoo
 * @ingroup main
 * @brief Public API for Audio Over OSC
 *
 * @defgroup aoo-utils
 * @ingroup main
 * @brief AOO Utils are helper classes to support the AOO protocol

 * @defgroup aoo-protocol
 * @ingroup main
 * @brief AOO Message parsing and sending
 *
 */

#include "AOOSink.h"
#include "AOOSource.h"
#include "AudioTools.h"
#include "AudioTools/Communication/OSCData.h"
#include "aoo/AOOProtocol.h"

#if defined(ARDUINO) && !defined(NO_AOO_NAMESPACE)
using namespace arduino_aoo;
#endif
