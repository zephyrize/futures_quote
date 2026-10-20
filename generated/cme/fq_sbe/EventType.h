/* Generated SBE (Simple Binary Encoding) message codec */
#ifndef _FQ_SBE_EVENTTYPE_CXX_H_
#define _FQ_SBE_EVENTTYPE_CXX_H_

#if !defined(__STDC_LIMIT_MACROS)
#  define __STDC_LIMIT_MACROS 1
#endif

#include <cstdint>
#include <iomanip>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <sstream>
#include <string>

#define SBE_NULLVALUE_INT8 (std::numeric_limits<std::int8_t>::min)()
#define SBE_NULLVALUE_INT16 (std::numeric_limits<std::int16_t>::min)()
#define SBE_NULLVALUE_INT32 (std::numeric_limits<std::int32_t>::min)()
#define SBE_NULLVALUE_INT64 (std::numeric_limits<std::int64_t>::min)()
#define SBE_NULLVALUE_UINT8 (std::numeric_limits<std::uint8_t>::max)()
#define SBE_NULLVALUE_UINT16 (std::numeric_limits<std::uint16_t>::max)()
#define SBE_NULLVALUE_UINT32 (std::numeric_limits<std::uint32_t>::max)()
#define SBE_NULLVALUE_UINT64 (std::numeric_limits<std::uint64_t>::max)()

namespace fq_sbe {

class EventType
{
public:
    enum Value
    {
        Activation = static_cast<std::uint8_t>(5),
        LastEligibleTradeDate = static_cast<std::uint8_t>(7),
        SBE_UNKNOWN = static_cast<std::uint8_t>(255),
        NULL_VALUE = static_cast<std::uint8_t>(255)
    };

    static EventType::Value get(const std::uint8_t value)
    {
        switch (value)
        {
            case static_cast<std::uint8_t>(5): return Activation;
            case static_cast<std::uint8_t>(7): return LastEligibleTradeDate;
            case static_cast<std::uint8_t>(255): return NULL_VALUE;
        }

        return SBE_UNKNOWN;
    }

    static const char *c_str(const EventType::Value value)
    {
        switch (value)
        {
            case Activation: return "Activation";
            case LastEligibleTradeDate: return "LastEligibleTradeDate";
            case NULL_VALUE: return "NULL_VALUE";
        }

        return "SBE_UNKNOWN";
    }

    template<typename CharT, typename Traits>
    friend std::basic_ostream<CharT, Traits> & operator << (
        std::basic_ostream<CharT, Traits> &os, EventType::Value m)
    {
        return os << EventType::c_str(m);
    }
};

}

#endif
