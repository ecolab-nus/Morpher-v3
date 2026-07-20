#pragma once

#include <morpherdfggen/common/dfg.h>

#include "llvm/Analysis/CFG.h"

#include <algorithm>
#include <bitset>
#include <cmath>
#include <ctime>
#include <functional>
#include <queue>
#include <set>
#include <sstream>
#include <unordered_set>

#include <morpherdfggen/arch/CGRA.h>
#include <morpherdfggen/common/DFGError.h>
#include <morpherdfggen/common/astar.h>
#include <morpherdfggen/util/tinyxml2.h>

#define LV_NAME "dfg_gen"
#define DEBUG_TYPE LV_NAME

namespace dfg_core_detail {

inline std::string jsonEscape(const std::string &value)
{
	std::ostringstream os;
	for (char ch : value)
	{
		switch (ch)
		{
		case '\\':
			os << "\\\\";
			break;
		case '"':
			os << "\\\"";
			break;
		case '\n':
			os << "\\n";
			break;
		case '\r':
			os << "\\r";
			break;
		case '\t':
			os << "\\t";
			break;
		default:
			os << ch;
			break;
		}
	}
	return os.str();
}

inline std::string jsonString(const std::string &value)
{
	return "\"" + jsonEscape(value) + "\"";
}

inline bool isPositivePowerOfTwo(int value)
{
	return value > 0 && (value & (value - 1)) == 0;
}

inline void configureGEPScaleNode(dfgNode *node, int stride)
{
	assert(node);
	assert(stride > 0);
	if (isPositivePowerOfTwo(stride))
	{
		node->setNameType("GEPLEFTSHIFT");
		node->setConstantVal(Log2_32(stride));
	}
	else
	{
		node->setNameType("GEPSCALE");
		node->setConstantVal(stride);
	}
}

} // namespace dfg_core_detail

using dfg_core_detail::configureGEPScaleNode;
using dfg_core_detail::isPositivePowerOfTwo;
using dfg_core_detail::jsonString;
