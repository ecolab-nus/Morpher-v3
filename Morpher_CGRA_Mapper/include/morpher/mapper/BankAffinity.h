#ifndef MORPHER_MAPPER_BANK_AFFINITY_H
#define MORPHER_MAPPER_BANK_AFFINITY_H

#include <map>
#include <string>

namespace CGRAXMLCompile {

// Equal-count ties resolve to the lexicographically smallest key because the
// input is ordered, preserving deterministic mapping.
inline std::string dominantBankKey(const std::map<std::string, int> &bankCounts)
{
	std::string dominant;
	int dominantCount = -1;
	for (const auto &entry : bankCounts)
	{
		if (entry.second > dominantCount)
		{
			dominant = entry.first;
			dominantCount = entry.second;
		}
	}
	return dominant;
}

inline int bankAffinityPenalty(const std::map<std::string, int> &bankCounts,
	const std::string &candidateBank, int configuredCost)
{
	if (configuredCost <= 0 || bankCounts.empty() || candidateBank.empty())
		return 0;
	return candidateBank == dominantBankKey(bankCounts) ? 0 : configuredCost;
}

} // namespace CGRAXMLCompile

#endif
