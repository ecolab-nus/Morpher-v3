#ifndef MORPHER_MAPPER_SYSTOLIC_MEMORY_AFFINITY_H
#define MORPHER_MAPPER_SYSTOLIC_MEMORY_AFFINITY_H

#include <string>

namespace CGRAXMLCompile {

// The bundled edge-memory architectures use increasing X from left to right
// and increasing Y from top to bottom.  Compute-array bounds make the test
// independent of the exterior memory-ring coordinate chosen by a template.
inline bool matchesSystolicMemorySide(const std::string &streamRole,
	int candidateX, int candidateY, int minComputeX, int maxComputeY)
{
	if (streamRole == "row")
		return candidateX < minComputeX;
	if (streamRole == "column")
		return candidateY > maxComputeY;
	return false;
}

inline bool matchesIndexedSystolicStream(const std::string &streamRole,
	int streamIndex, int candidateX, int candidateY,
	int minComputeX, int minComputeY, int maxComputeY)
{
	if (streamIndex < 0)
		return false;
	if (streamRole == "row")
		return candidateX < minComputeX &&
			candidateY == minComputeY + streamIndex;
	if (streamRole == "column")
		return candidateY > maxComputeY &&
			candidateX == minComputeX + streamIndex;
	return false;
}

inline bool matchesSystolicMacCoordinate(int macRow, int macColumn,
	int candidateX, int candidateY,
	int minComputeX, int minComputeY, int maxComputeX, int maxComputeY)
{
	if (macRow < 0 || macColumn < 0)
		return false;
	const int targetX = minComputeX + macColumn;
	const int targetY = minComputeY + macRow;
	return targetX <= maxComputeX && targetY <= maxComputeY &&
		candidateX == targetX && candidateY == targetY;
}

inline int systolicPathFinderAttemptCount(int configuredAttempts,
	bool hasSystolicStreams)
{
	const int normalized = configuredAttempts > 0 ? configuredAttempts : 1;
	return hasSystolicStreams && normalized < 2 ? 2 : normalized;
}

inline bool enforceSystolicSidesOnAttempt(bool hasSystolicStreams,
	bool useSimulatedAnnealing, int attemptIndex)
{
	return hasSystolicStreams && !useSimulatedAnnealing && attemptIndex == 0;
}

} // namespace CGRAXMLCompile

#endif
