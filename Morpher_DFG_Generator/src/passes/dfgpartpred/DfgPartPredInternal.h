#pragma once

#include <morpherdfggen/dfg/DFGPartPred.h>
#include <morpherdfggen/common/DFGError.h>

#include "llvm/Analysis/CFG.h"

#include <algorithm>
#include <map>
#include <queue>
#include <set>
#include <vector>

using namespace std;

#define LV_NAME "dfg_gen"
#define DEBUG_TYPE LV_NAME
