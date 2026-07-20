#include "DfgCoreInternal.h"

void DFG::packSIMD(int lanes, const std::unordered_set<std::string>& vectorOps,
				   bool scalarBroadcast)
{
	if (lanes <= 1)
		return;

	struct Candidate
	{
		int group = -1;
		std::vector<dfgNode*> laneNodes;
		HyCUBEIns vectorInstruction = NOP;
		std::string vectorOp;
		bool active = true;
		std::string reason;
		bool reductionProducer = false;
		bool vectorAccumulator = false;
	};

	struct RejectedGroup
	{
		int group = -1;
		std::string reason;
	};

	auto vectorInstructionFor = [](HyCUBEIns scalar) -> HyCUBEIns {
		switch (scalar)
		{
		case ADD: return VADD;
		case SUB: return VSUB;
		case MUL: return VMUL;
		case SEXT: return VSEXT;
		case DIV: return VDIV;
		case LS: return VLS;
		case RS: return VRS;
		case ARS: return VARS;
		case AND: return VAND;
		case OR: return VOR;
		case XOR: return VXOR;
		case CMP: return VCMP;
		case CLT: return VCLT;
		case CGT: return VCGT;
		case Hy_LOAD: return VLOAD;
		case Hy_LOADH: return VLOADH;
		case Hy_LOADB: return VLOADB;
		case Hy_STORE: return VSTORE;
		case Hy_STOREH: return VSTOREH;
		case Hy_STOREB: return VSTOREB;
		case MAC: return VMAC;
		default: return NOP;
		}
	};

	auto emittedVectorOp = [this](dfgNode* node, HyCUBEIns instruction) {
		std::string op = HyCUBEInsStrings[instruction];
		if ((node->getNameType() == "OutLoopLOAD" || node->getNameType() == "OutLoopSTORE") &&
			op.size() > 1 && op[0] == 'V')
			return std::string("VO") + op.substr(1);
		return op;
	};

	auto hasUnsupportedOrdinaryEdges = [](dfgNode* node) {
		for (dfgNode* parent : node->getAncestors())
		{
			auto back = node->ancestorBackEdgeMap.find(parent);
			auto cond = node->ancestorConditionaMap.find(parent);
			if ((back != node->ancestorBackEdgeMap.end() && back->second) ||
				(cond != node->ancestorConditionaMap.end() && cond->second != UNCOND))
				return true;
		}
		for (dfgNode* child : node->getChildren())
		{
			auto back = node->childBackEdgeMap.find(child);
			auto cond = node->childConditionalMap.find(child);
			if ((back != node->childBackEdgeMap.end() && back->second) ||
				(cond != node->childConditionalMap.end() && cond->second != UNCOND))
				return true;
		}
		return false;
	};

	std::map<int, std::vector<dfgNode*> > groups;
	for (dfgNode* node : NodeList)
		if (node->getSimdGroup() >= 0)
			groups[node->getSimdGroup()].push_back(node);

	struct ReductionRewrite
	{
		int producerGroup = -1;
		std::vector<dfgNode*> stores;
		std::unordered_set<dfgNode*> scalarTree;
		dfgNode* scalarTreeRoot = nullptr;
		dfgNode* finalAccumulator = nullptr;
		std::string op;
	};

	struct AccumulatorRewrite
	{
		std::unordered_set<dfgNode*> scaffolding;
		std::vector<dfgNode*> exitTransfers;
		std::unordered_map<dfgNode*, std::string> laneTraceVars;
		std::unordered_map<dfgNode*, std::string> seedTraceVars;
		std::string op;
		std::string initTraceVar;
		std::string exitTraceVar;
	};

	std::unordered_map<int, ReductionRewrite> reductionRewrites;
	if (vectorOps.count("VRED") != 0)
	{
		for (std::pair<const int, std::vector<dfgNode*> >& entry : groups)
		{
			std::vector<dfgNode*>& stores = entry.second;
			if ((int)stores.size() != lanes || stores.empty() ||
				stores.front()->getSimdReduceOp().empty())
				continue;

			const std::string reduceOp = stores.front()->getSimdReduceOp();
			const std::string reduceBase = stores.front()->getSimdReduceBase();
			if (reduceBase.empty() ||
				(reduceOp != "ADD" && reduceOp != "AND" &&
				 reduceOp != "OR" && reduceOp != "XOR"))
				continue;

			std::vector<dfgNode*> producers;
			bool valid = true;
			for (dfgNode* store : stores)
			{
				if (store->getSimdReduceOp() != reduceOp ||
					store->getSimdReduceBase() != reduceBase ||
					store->getAncestors().size() != 1 ||
					!store->getChildren().empty())
				{
					valid = false;
					break;
				}
				producers.push_back(store->getAncestors().front());
			}
			if (!valid || producers.empty())
				continue;

			const int producerGroup = producers.front()->getSimdGroup();
			auto producerEntry = groups.find(producerGroup);
			if (producerGroup < 0 || producerEntry == groups.end() ||
				(int)producerEntry->second.size() != lanes)
				continue;
			std::unordered_set<dfgNode*> producerSet(producers.begin(), producers.end());
			if ((int)producerSet.size() != lanes)
				continue;
			for (dfgNode* producer : producerEntry->second)
				if (!producerSet.count(producer))
					valid = false;
			if (!valid || reductionRewrites.count(producerGroup))
				continue;

			dfgNode* finalAccumulator = nullptr;
			for (dfgNode* node : NodeList)
			{
				if (node->getArrBasePtr() == reduceBase &&
					node->getNameType() == "OutLoopSTORE" &&
					node->getAncestors().size() == 1)
				{
					finalAccumulator = node->getAncestors().front();
					break;
				}
				Instruction* instruction = node->getNode();
				if (instruction && instruction->hasName() &&
					instruction->getName().str() == reduceBase)
				{
					finalAccumulator = node;
					break;
				}
			}
			if (!finalAccumulator)
				continue;

			std::function<bool(dfgNode*, std::unordered_set<dfgNode*>&)> reachesProducer =
				[&](dfgNode* node, std::unordered_set<dfgNode*>& visited) {
					if (producerSet.count(node))
						return true;
					if (!visited.insert(node).second)
						return false;
					for (dfgNode* parent : node->getAncestors())
					{
						auto back = node->ancestorBackEdgeMap.find(parent);
						auto cond = node->ancestorConditionaMap.find(parent);
						if ((back != node->ancestorBackEdgeMap.end() && back->second) ||
							(cond != node->ancestorConditionaMap.end() && cond->second != UNCOND))
							continue;
						if (reachesProducer(parent, visited))
							return true;
					}
					return false;
				};

			std::vector<dfgNode*> roots;
			for (dfgNode* parent : finalAccumulator->getAncestors())
			{
				std::unordered_set<dfgNode*> visited;
				if (reachesProducer(parent, visited))
					roots.push_back(parent);
			}
			if (roots.size() != 1)
				continue;

			ReductionRewrite rewrite;
			rewrite.producerGroup = producerGroup;
			rewrite.stores = stores;
			rewrite.scalarTreeRoot = roots.front();
			rewrite.finalAccumulator = finalAccumulator;
			rewrite.op = reduceOp;
			std::function<bool(dfgNode*)> collectTree = [&](dfgNode* node) {
				if (producerSet.count(node))
					return true;
				if (!rewrite.scalarTree.insert(node).second)
					return true;
				if (HyCUBEInsStrings[node->getFinalIns()] != reduceOp ||
					hasUnsupportedOrdinaryEdges(node))
					return false;
				for (dfgNode* parent : node->getAncestors())
					if (!collectTree(parent))
						return false;
				return true;
			};
			if (!collectTree(rewrite.scalarTreeRoot))
				continue;

			for (dfgNode* producer : producerEntry->second)
				for (dfgNode* child : producer->getChildren())
					if (!rewrite.scalarTree.count(child) &&
						std::find(stores.begin(), stores.end(), child) == stores.end())
						valid = false;
			for (dfgNode* node : rewrite.scalarTree)
				for (dfgNode* child : node->getChildren())
					if (child != finalAccumulator && !rewrite.scalarTree.count(child))
						valid = false;
			if (valid)
				reductionRewrites.emplace(producerGroup, std::move(rewrite));
		}
	}

	std::unordered_map<int, AccumulatorRewrite> accumulatorRewrites;
	for (std::pair<const int, std::vector<dfgNode*> >& entry : groups)
	{
		std::vector<dfgNode*>& updates = entry.second;
		if ((int)updates.size() != lanes || updates.empty() ||
			updates.front()->getSimdAccumulatorOp().empty())
			continue;

		AccumulatorRewrite rewrite;
		rewrite.op = updates.front()->getSimdAccumulatorOp();
		rewrite.initTraceVar = updates.front()->getSimdAccumulatorInitVar();
		rewrite.exitTraceVar = updates.front()->getSimdAccumulatorExitVar();
		if (rewrite.op != "ADD" && rewrite.op != "AND" &&
			rewrite.op != "OR" && rewrite.op != "XOR")
			continue;
		if (rewrite.initTraceVar.empty() || rewrite.exitTraceVar.empty())
			continue;

		bool valid = true;
		for (dfgNode* update : updates)
		{
			if (update->getSimdAccumulatorOp() != rewrite.op ||
				update->getSimdAccumulatorInitVar() != rewrite.initTraceVar ||
				update->getSimdAccumulatorExitVar() != rewrite.exitTraceVar ||
				update->getSimdAccumulatorSeedVar().empty() ||
				HyCUBEInsStrings[update->getFinalIns()] != rewrite.op)
			{
				valid = false;
				break;
			}

			dfgNode* select = nullptr;
			for (dfgNode* parent : update->getAncestors())
				if (HyCUBEInsStrings[parent->getFinalIns()] == "SELECT")
				{
					if (select) valid = false;
					select = parent;
				}
			if (!valid || !select || select->getChildren().size() != 1 ||
				select->getChildren().front() != update ||
				select->getAncestors().size() != 2)
			{
				valid = false;
				break;
			}

			dfgNode* backMerge = nullptr;
			dfgNode* initMerge = nullptr;
			for (dfgNode* parent : select->getAncestors())
			{
				if (HyCUBEInsStrings[parent->getFinalIns()] != "CMERGE" ||
					parent->getChildren().size() != 1 ||
					parent->getChildren().front() != select)
				{
					valid = false;
					break;
				}
				auto back = parent->childBackEdgeMap.find(select);
				if (back != parent->childBackEdgeMap.end() && back->second)
					backMerge = parent;
				else
					initMerge = parent;
			}
			const std::vector<dfgNode*> backMergeParents =
				backMerge ? backMerge->getAncestors() : std::vector<dfgNode*>();
			if (!valid || !backMerge || !initMerge ||
				std::find(backMergeParents.begin(), backMergeParents.end(), update) ==
					backMergeParents.end())
			{
				valid = false;
				break;
			}

			std::vector<dfgNode*> exitTransfers;
			for (dfgNode* child : update->getChildren())
			{
				if (child == backMerge)
					continue;
				if (child->getNameType() != "OutLoopSTORE" ||
					child->getAncestors().size() != 1 ||
					child->getAncestors().front() != update ||
					!child->getChildren().empty())
				{
					valid = false;
					break;
				}
				exitTransfers.push_back(child);
			}
			if (!valid || exitTransfers.size() != 1)
			{
				valid = false;
				break;
			}

			rewrite.scaffolding.insert(select);
			rewrite.scaffolding.insert(backMerge);
			rewrite.scaffolding.insert(initMerge);
			rewrite.exitTransfers.push_back(exitTransfers.front());
			rewrite.laneTraceVars[update] =
				exitTransfers.front()->getArrBasePtr();
			rewrite.seedTraceVars[update] =
				update->getSimdAccumulatorSeedVar();
		}
		if (valid && rewrite.scaffolding.size() == (size_t)(3 * lanes) &&
			rewrite.exitTransfers.size() == (size_t)lanes)
			accumulatorRewrites.emplace(entry.first, std::move(rewrite));
	}

	std::vector<Candidate> candidates;
	std::vector<RejectedGroup> rejected;
	for (std::pair<const int, std::vector<dfgNode*> >& entry : groups)
	{
		std::vector<dfgNode*>& groupNodes = entry.second;
		std::sort(groupNodes.begin(), groupNodes.end(),
			[](dfgNode* lhs, dfgNode* rhs) { return lhs->getIdx() < rhs->getIdx(); });

		if ((int)groupNodes.size() != lanes)
		{
			rejected.push_back({entry.first, "partial or multi-vector lane group"});
			continue;
		}

		dfgNode* first = groupNodes.front();
		HyCUBEIns vectorInstruction = vectorInstructionFor(first->getFinalIns());
		std::string vectorOp = emittedVectorOp(first, vectorInstruction);
		std::string reason;
		if (vectorInstruction == NOP)
			reason = "unsupported scalar operation";
		else if (vectorOps.count(vectorOp) == 0)
			reason = "architecture does not support " + vectorOp;

		for (dfgNode* node : groupNodes)
		{
			if (!reason.empty())
				break;
			if (node->getSimdLanes() != lanes ||
				node->getFinalIns() != first->getFinalIns() ||
				node->hasConstantVal() != first->hasConstantVal() ||
				(node->hasConstantVal() && node->getConstantVal() != first->getConstantVal()) ||
				node->getArrBasePtr() != first->getArrBasePtr() ||
				node->getTypeSizeBytes() != first->getTypeSizeBytes() ||
				node->getNameType() != first->getNameType())
				reason = "incompatible lane metadata";
			else if (!node->getSimdReduceOp().empty())
				reason = "horizontal reduction scratch output";
			else if (hasUnsupportedOrdinaryEdges(node))
				reason = "control or back edge";
		}

		if (!reason.empty())
		{
			rejected.push_back({entry.first, reason});
			continue;
		}
		candidates.push_back({entry.first, groupNodes, vectorInstruction, vectorOp,
			true, "", reductionRewrites.count(entry.first) != 0,
			accumulatorRewrites.count(entry.first) != 0});
	}

	auto isSharedScalarParent = [](const Candidate& candidate, dfgNode* parent) {
		for (dfgNode* laneNode : candidate.laneNodes)
		{
			const std::vector<dfgNode*> parents = laneNode->getAncestors();
			if (std::find(parents.begin(), parents.end(), parent) == parents.end())
				return false;
		}
		return true;
	};

	bool changed = true;
	while (changed)
	{
		changed = false;
		std::unordered_map<dfgNode*, std::pair<int, int> > candidateLane;
		for (size_t candidateIndex = 0; candidateIndex < candidates.size(); ++candidateIndex)
		{
			if (!candidates[candidateIndex].active)
				continue;
			for (size_t lane = 0; lane < candidates[candidateIndex].laneNodes.size(); ++lane)
				candidateLane[candidates[candidateIndex].laneNodes[lane]] =
					std::make_pair((int)candidateIndex, (int)lane);
		}

		for (size_t candidateIndex = 0; candidateIndex < candidates.size(); ++candidateIndex)
		{
			Candidate& candidate = candidates[candidateIndex];
			if (!candidate.active)
				continue;
			std::string reason;
			std::vector<std::pair<int, std::string> > firstRecParents;
			std::vector<std::pair<int, std::string> > firstRecChildren;
			for (size_t lane = 0; lane < candidate.laneNodes.size() && reason.empty(); ++lane)
			{
				dfgNode* node = candidate.laneNodes[lane];
				for (dfgNode* parent : node->getAncestors())
				{
					if (candidate.vectorAccumulator &&
						accumulatorRewrites.at(candidate.group).scaffolding.count(parent))
						continue;
					auto it = candidateLane.find(parent);
					if (it == candidateLane.end())
					{
						if (!scalarBroadcast || parent->getSimdGroup() >= 0 ||
							!isSharedScalarParent(candidate, parent))
						{
							reason = "scalar or unsupported parent";
							break;
						}
						continue;
					}
					if (it->second.first == (int)candidateIndex || it->second.second != (int)lane)
					{
						reason = "non-isomorphic parent wiring";
						break;
					}
				}
				if (!candidate.reductionProducer && !candidate.vectorAccumulator)
				{
					for (dfgNode* child : node->getChildren())
					{
						auto it = candidateLane.find(child);
						if (it == candidateLane.end())
						{
							reason = "scalar or unsupported child";
							break;
						}
						if (it->second.first == (int)candidateIndex || it->second.second != (int)lane)
						{
							reason = "non-isomorphic child wiring";
							break;
						}
					}
				}

				auto recurrenceSignature = [&](const std::vector<dfgNode*>& peers,
					const std::map<Instruction*, std::string>& types,
					const char* direction,
					std::vector<std::pair<int, std::string> >& signature) {
					for (dfgNode* peer : peers)
					{
						auto it = candidateLane.find(peer);
						if (it == candidateLane.end())
						{
							reason = std::string("scalar or unsupported recurrence ") + direction;
							return;
						}
						if (it->second.first == (int)candidateIndex || it->second.second != (int)lane)
						{
							reason = std::string("non-isomorphic recurrence ") + direction;
							return;
						}
						auto typeIt = types.find(peer->getNode());
						signature.push_back(std::make_pair(
							it->second.first,
							typeIt == types.end() ? "" : typeIt->second));
					}
					std::sort(signature.begin(), signature.end());
				};

				std::vector<std::pair<int, std::string> > recParents;
				recurrenceSignature(node->getRecAncestors(), node->RecAncestorType,
					"parent", recParents);
				if (!reason.empty())
					break;
				std::vector<std::pair<int, std::string> > recChildren;
				recurrenceSignature(node->getRecChildren(), node->RecChildrenType,
					"child", recChildren);
				if (!reason.empty())
					break;
				if (lane == 0)
				{
					firstRecParents = recParents;
					firstRecChildren = recChildren;
				}
				else if (recParents != firstRecParents || recChildren != firstRecChildren)
				{
					reason = "non-isomorphic recurrence signature";
					break;
				}
			}
			if (!reason.empty())
			{
				candidate.active = false;
				candidate.reason = reason;
				rejected.push_back({candidate.group, reason});
				changed = true;
			}
		}
	}

	std::unordered_map<dfgNode*, int> originalIndex;
	for (dfgNode* node : NodeList)
		originalIndex[node] = node->getIdx();
	const int originalNodeCount = (int)NodeList.size();
	std::unordered_set<dfgNode*> removedNodes;
	std::unordered_set<dfgNode*> reductionPrunedNodes;
	std::unordered_set<dfgNode*> accumulatorPrunedNodes;
	std::unordered_set<dfgNode*> postPackIteratorPrunedNodes;
	std::vector<Candidate*> packedCandidates;
	std::vector<Candidate*> packedAccumulators;
	for (Candidate& candidate : candidates)
	{
		if (!candidate.active)
			continue;
		std::vector<int> laneSources;
		for (dfgNode* node : candidate.laneNodes)
			laneSources.push_back(originalIndex[node]);
		dfgNode* representative = candidate.laneNodes.front();
		representative->setFinalIns(candidate.vectorInstruction);
		representative->setSimdPacked(std::move(laneSources));
		for (size_t lane = 1; lane < candidate.laneNodes.size(); ++lane)
			removedNodes.insert(candidate.laneNodes[lane]);
		packedCandidates.push_back(&candidate);

		if (candidate.reductionProducer)
		{
			ReductionRewrite& rewrite = reductionRewrites.at(candidate.group);
			dfgNode* producer = representative;
			rewrite.scalarTreeRoot->removeChild(rewrite.finalAccumulator);
			rewrite.finalAccumulator->removeAncestor(rewrite.scalarTreeRoot);

			dfgNode* vred = new dfgNode(this);
			vred->setIdx((int)NodeList.size());
			vred->setNameType("VRED");
			vred->setFinalIns(VRED);
			vred->setSimdReduction(rewrite.op);
			vred->BB = rewrite.finalAccumulator->BB;
			const int vredASAP = producer->getASAPnumber() + 1;
			vred->setASAPnumber(vredASAP);
			vred->setALAPnumber(std::max(vredASAP,
				rewrite.finalAccumulator->getALAPnumber() - 1));
			producer->addChildNode(vred);
			vred->addAncestorNode(producer);
			vred->addChildNode(rewrite.finalAccumulator);
			rewrite.finalAccumulator->addAncestorNode(vred);
			NodeList.push_back(vred);

			for (dfgNode* store : rewrite.stores)
				reductionPrunedNodes.insert(store);
			for (dfgNode* node : rewrite.scalarTree)
				reductionPrunedNodes.insert(node);
		}

		if (candidate.vectorAccumulator)
		{
			AccumulatorRewrite& rewrite = accumulatorRewrites.at(candidate.group);
			accumulatorPrunedNodes.insert(rewrite.scaffolding.begin(),
				rewrite.scaffolding.end());
			accumulatorPrunedNodes.insert(rewrite.exitTransfers.begin(),
				rewrite.exitTransfers.end());
			packedAccumulators.push_back(&candidate);

			// The scalar PHIs represented one independent lane recurrence each.
			// After packing, their only steady-state meaning is a next-iteration
			// edge from the vector update back to itself.
			representative->addChildNode(
				representative, EDGE_TYPE_DATA, true, false, true);
			representative->addAncestorNode(
				representative, EDGE_TYPE_DATA, true, false, true);
		}
	}
	removedNodes.insert(reductionPrunedNodes.begin(), reductionPrunedNodes.end());
	removedNodes.insert(accumulatorPrunedNodes.begin(), accumulatorPrunedNodes.end());

	if (!removedNodes.empty())
	{
		// A scalar broadcast parent has one edge to every scalar lane. The
		// representative keeps lane zero; the remaining edges are redundant
		// once the vector operation consumes that shared scalar input.
		for (dfgNode* node : NodeList)
		{
			if (removedNodes.count(node))
				continue;
			const std::vector<dfgNode*> ancestors = node->getAncestors();
			for (dfgNode* parent : ancestors)
			{
				if (!removedNodes.count(parent))
					continue;
				parent->removeChild(node);
				node->removeAncestor(parent);
			}
			const std::vector<dfgNode*> children = node->getChildren();
			for (dfgNode* child : children)
			{
				if (!removedNodes.count(child))
					continue;
				node->removeChild(child);
				child->removeAncestor(node);
			}
		}
		for (dfgNode* node : NodeList)
		{
			if (removedNodes.count(node))
				continue;
			for (dfgNode* parent : node->getAncestors())
				if (removedNodes.count(parent))
					report_fatal_error("SIMD pack left a surviving node with a removed parent");
			for (dfgNode* child : node->getChildren())
				if (removedNodes.count(child))
					report_fatal_error("SIMD pack left a surviving node with a removed child");
			for (dfgNode* parent : node->getRecAncestors())
				if (removedNodes.count(parent))
					report_fatal_error("SIMD pack left a surviving node with a removed recurrence parent");
			for (dfgNode* child : node->getRecChildren())
				if (removedNodes.count(child))
					report_fatal_error("SIMD pack left a surviving node with a removed recurrence child");
			node->parentClassification.clear();
			node->childClassification.clear();
		}

		edgeList.erase(
			std::remove_if(edgeList.begin(), edgeList.end(),
				[&removedNodes](Edge& edge) {
					return removedNodes.count(edge.getSrc()) || removedNodes.count(edge.getDest());
				}),
			edgeList.end());
		NodeList.erase(
			std::remove_if(NodeList.begin(), NodeList.end(),
				[&removedNodes](dfgNode* node) { return removedNodes.count(node) > 0; }),
			NodeList.end());
		for (auto it = OutLoopNodeMap.begin(); it != OutLoopNodeMap.end(); )
			if (removedNodes.count(it->second)) it = OutLoopNodeMap.erase(it); else ++it;
		for (auto it = OutLoopNodeMapReverse.begin(); it != OutLoopNodeMapReverse.end(); )
			if (removedNodes.count(it->first)) it = OutLoopNodeMapReverse.erase(it); else ++it;
	}

	// SIMD packing must precede iterator externalization for vector partial
	// accumulators: only packing removes the scalar PHI control scaffolding and
	// exposes the iterator as an independent component. Keep both transforms in
	// this method so the SIMD manifest is emitted in the final XML index space.
	if (iteratorExternalized)
	{
		for (size_t index = 0; index < NodeList.size(); ++index)
			NodeList[index]->setIdx((int)index);
		std::unordered_set<dfgNode*> beforeIterator(NodeList.begin(), NodeList.end());
		removeIteratorOnlyControl();
		for (dfgNode* node : beforeIterator)
			if (std::find(NodeList.begin(), NodeList.end(), node) == NodeList.end())
				postPackIteratorPrunedNodes.insert(node);
	}

	for (size_t index = 0; index < NodeList.size(); ++index)
		NodeList[index]->setIdx((int)index);

	std::map<int, int> finalIndexByOriginal;
	for (dfgNode* node : NodeList)
	{
		auto originalIt = originalIndex.find(node);
		if (originalIt != originalIndex.end())
			finalIndexByOriginal[originalIt->second] = node->getIdx();
	}
	for (Candidate* candidate : packedCandidates)
	{
		const int finalIndex = candidate->laneNodes.front()->getIdx();
		for (dfgNode* laneNode : candidate->laneNodes)
			finalIndexByOriginal[originalIndex[laneNode]] = finalIndex;
	}
	std::set<int> prunedOriginalIndices;
	for (dfgNode* node : reductionPrunedNodes)
		prunedOriginalIndices.insert(originalIndex.at(node));
	for (dfgNode* node : accumulatorPrunedNodes)
		prunedOriginalIndices.insert(originalIndex.at(node));
	for (dfgNode* node : postPackIteratorPrunedNodes)
	{
		auto originalIt = originalIndex.find(node);
		if (originalIt != originalIndex.end())
			prunedOriginalIndices.insert(originalIt->second);
	}
	if ((int)finalIndexByOriginal.size() + (int)prunedOriginalIndices.size() != originalNodeCount)
		report_fatal_error("SIMD manifest does not cover every original DFG node");

	for (dfgNode* node : NodeList)
	{
		for (dfgNode* parent : node->getAncestors())
			if (parent->getIdx() < 0 || parent->getIdx() >= (int)NodeList.size())
				report_fatal_error("SIMD pack produced an invalid parent index");
		for (dfgNode* child : node->getChildren())
			if (child->getIdx() < 0 || child->getIdx() >= (int)NodeList.size())
				report_fatal_error("SIMD pack produced an invalid child index");
	}

	std::ofstream manifestFile(name + "_SIMD_manifest.json");
	if (!manifestFile.is_open())
		report_fatal_error("Could not open SIMD manifest for writing");
	manifestFile << "{\n";
	manifestFile << "  \"schema_version\": 1,\n";
	manifestFile << "  \"transform\": \"vector_pack\",\n";
	manifestFile << "  \"status\": \"applied\",\n";
	manifestFile << "  \"lanes\": " << lanes << ",\n";
	manifestFile << "  \"original_node_count\": " << originalNodeCount << ",\n";
	manifestFile << "  \"final_node_count\": " << NodeList.size() << ",\n";
	manifestFile << "  \"post_pack_iterator_externalized\": "
			 << (iteratorExternalized ? "true" : "false") << ",\n";
	manifestFile << "  \"pruned_nodes\": [";
	bool firstPruned = true;
	for (int index : prunedOriginalIndices)
	{
		if (!firstPruned) manifestFile << ", ";
		firstPruned = false;
		manifestFile << index;
	}
	manifestFile << "],\n";
	std::set<std::string> prunedArtifactVars;
	for (dfgNode* node : reductionPrunedNodes)
		if (node->getArrBasePtr() != "NOT_A_MEM_OP")
			prunedArtifactVars.insert(node->getArrBasePtr());
	for (dfgNode* node : accumulatorPrunedNodes)
		if (node->getArrBasePtr() != "NOT_A_MEM_OP")
			prunedArtifactVars.insert(node->getArrBasePtr());
	manifestFile << "  \"pruned_artifact_vars\": [";
	bool firstVar = true;
	for (const std::string& var : prunedArtifactVars)
	{
		if (!firstVar) manifestFile << ", ";
		firstVar = false;
		manifestFile << jsonString(var);
	}
	manifestFile << "],\n";
	manifestFile << "  \"synthesized_nodes\": [";
	bool firstSynthesized = true;
	for (dfgNode* node : NodeList)
	{
		if (originalIndex.count(node))
			continue;
		if (!firstSynthesized) manifestFile << ", ";
		firstSynthesized = false;
		manifestFile << "{\"final_idx\": " << node->getIdx()
					 << ", \"op\": " << jsonString(HyCUBEInsStrings[node->getFinalIns()]);
		if (!node->getSimdReduceOp().empty())
			manifestFile << ", \"reduce_op\": " << jsonString(node->getSimdReduceOp());
		manifestFile << "}";
	}
	manifestFile << "],\n";
	manifestFile << "  \"exit_reductions\": [";
	for (size_t i = 0; i < packedAccumulators.size(); ++i)
	{
		Candidate* candidate = packedAccumulators[i];
		if (i != 0) manifestFile << ", ";
		manifestFile << "{\"source_final_idx\": "
				 << candidate->laneNodes.front()->getIdx()
				 << ", \"op\": \"VRED\", \"reduce_op\": "
				 << jsonString(accumulatorRewrites.at(candidate->group).op)
				 << ", \"accumulator_identity\": "
				 << jsonString(accumulatorRewrites.at(candidate->group).op == "AND"
					? "all_ones" : "zero")
				 << ", \"accumulator_init_execution\": \"loop_preheader\""
				 << ", \"accumulator_init_scheduled\": false"
				 << ", \"scalar_init_trace_var\": "
				 << jsonString(accumulatorRewrites.at(candidate->group).initTraceVar)
				 << ", \"execution\": \"loop_exit\""
				 << ", \"scalar_exit_trace_var\": "
				 << jsonString(accumulatorRewrites.at(candidate->group).exitTraceVar)
				 << ", \"combines_scalar_initializer\": true"
				 << ", \"stores_result\": true"
				 << ", \"scheduled\": false, \"lane_seed_trace_vars\": [";
		for (size_t lane = 0; lane < candidate->laneNodes.size(); ++lane)
		{
			if (lane != 0) manifestFile << ", ";
			manifestFile << jsonString(
				accumulatorRewrites.at(candidate->group).seedTraceVars.at(
					candidate->laneNodes[lane]));
		}
		manifestFile << "], \"lane_trace_vars\": [";
		for (size_t lane = 0; lane < candidate->laneNodes.size(); ++lane)
		{
			if (lane != 0) manifestFile << ", ";
			manifestFile << jsonString(
				accumulatorRewrites.at(candidate->group).laneTraceVars.at(
					candidate->laneNodes[lane]));
		}
		manifestFile << "]}";
	}
	manifestFile << "],\n";
	manifestFile << "  \"packed_groups\": [\n";
	for (size_t i = 0; i < packedCandidates.size(); ++i)
	{
		Candidate* candidate = packedCandidates[i];
		if (i != 0) manifestFile << ",\n";
		manifestFile << "    {\"simd_group\": " << candidate->group
				 << ", \"final_idx\": " << candidate->laneNodes.front()->getIdx()
				 << ", \"vector_op\": " << jsonString(candidate->vectorOp)
				 << ", \"lanes\": [";
		for (size_t lane = 0; lane < candidate->laneNodes.size(); ++lane)
		{
			if (lane != 0) manifestFile << ", ";
			manifestFile << originalIndex[candidate->laneNodes[lane]];
		}
		manifestFile << "]}";
	}
	manifestFile << "\n  ],\n";
	manifestFile << "  \"rejected_groups\": [";
	for (size_t i = 0; i < rejected.size(); ++i)
	{
		if (i != 0) manifestFile << ", ";
		manifestFile << "{\"simd_group\": " << rejected[i].group
				 << ", \"reason\": " << jsonString(rejected[i].reason) << "}";
	}
	manifestFile << "],\n";
	manifestFile << "  \"index_map\": [";
	bool firstMap = true;
	for (const std::pair<const int, int>& mapping : finalIndexByOriginal)
	{
		if (!firstMap) manifestFile << ", ";
		firstMap = false;
		manifestFile << "{\"original_idx\": " << mapping.first
				 << ", \"final_idx\": " << mapping.second << "}";
	}
	manifestFile << "]\n";
	manifestFile << "}\n";
	llvm::outs() << "[packSIMD] Packed " << packedCandidates.size()
				 << " groups; remaining nodes=" << NodeList.size() << "\n";
}

void DFG::writeBoundaryManifest()
{
	if (boundaryStates.empty() && stationaryPreloads.empty())
		return;

	for (size_t index = 0; index < NodeList.size(); ++index)
		NodeList[index]->setIdx((int)index);

	std::set<std::string> retainedTraceVars;
	for (const BoundaryState& state : boundaryStates)
	{
		if (!state.mappedNode ||
			std::find(NodeList.begin(), NodeList.end(), state.mappedNode) == NodeList.end())
			report_fatal_error("Boundary state refers to a removed mapped node");
		retainedTraceVars.insert(state.seedTraceVars.begin(), state.seedTraceVars.end());
		retainedTraceVars.insert(state.finalTraceVars.begin(), state.finalTraceVars.end());
	}
	for (const StationaryPreload& preload : stationaryPreloads)
	{
		if (!preload.mappedNode ||
			std::find(NodeList.begin(), NodeList.end(), preload.mappedNode) == NodeList.end())
			report_fatal_error("Stationary preload refers to a removed mapped node");
		retainedTraceVars.insert(preload.traceVar);
	}

	std::ofstream manifestFile(name + "_BOUNDARY_manifest.json");
	if (!manifestFile.is_open())
		report_fatal_error("Could not open boundary manifest for writing");
	manifestFile << "{\n";
	manifestFile << "  \"schema_version\": 2,\n";
	manifestFile << "  \"transform\": \"loop_boundary_canonicalization\",\n";
	manifestFile << "  \"status\": \"applied\",\n";
	bool resetsTregOnLaunch = false;
	for (const BoundaryState& state : boundaryStates)
		resetsTregOnLaunch |= state.storage == "treg";
	manifestFile << "  \"launch\": {\"execution\": \"before_each_loop_invocation\", "
		<< "\"actions\": [";
	bool firstAction = true;
	if (resetsTregOnLaunch)
	{
		manifestFile << "{\"operation\": \"reset\", \"storage\": \"treg\", "
			<< "\"scope\": \"all_pes\", \"value\": 0}";
		firstAction = false;
	}
	for (const StationaryPreload& preload : stationaryPreloads)
	{
		if (!firstAction) manifestFile << ", ";
		firstAction = false;
		manifestFile << "{\"operation\": \"preload\", \"storage\": \"regfile\", "
			<< "\"mapped_node\": " << preload.mappedNode->getIdx()
			<< ", \"operand\": \"I" << preload.operand << "\""
			<< ", \"row\": " << preload.row
			<< ", \"column\": " << preload.column
			<< ", \"value_bits\": " << preload.valueBits
			<< ", \"trace_address\": {\"variable\": "
			<< jsonString(preload.traceVar) << ", \"byte_offset\": "
			<< preload.byteOffset << "}, \"scheduled\": false}";
	}
	manifestFile << "]},\n";
	manifestFile << "  \"original_node_count\": " << boundaryOriginalNodeCount << ",\n";
	manifestFile << "  \"final_node_count\": " << NodeList.size() << ",\n";
	std::set<int> survivingOriginalIndices;
	manifestFile << "  \"index_map\": [";
	bool firstMap = true;
	for (dfgNode *node : NodeList)
	{
		auto original = boundaryOriginalIndex.find(node);
		if (original == boundaryOriginalIndex.end())
			continue;
		survivingOriginalIndices.insert(original->second);
		if (!firstMap) manifestFile << ", ";
		firstMap = false;
		manifestFile << "{\"original_idx\": " << original->second
			<< ", \"final_idx\": " << node->getIdx() << "}";
	}
	manifestFile << "],\n";
	manifestFile << "  \"pruned_nodes\": [";
	bool firstPruned = true;
	for (int original = 0; original < boundaryOriginalNodeCount; ++original)
	{
		if (survivingOriginalIndices.count(original))
			continue;
		if (!firstPruned) manifestFile << ", ";
		firstPruned = false;
		manifestFile << original;
	}
	manifestFile << "],\n";
	manifestFile << "  \"pruned_artifact_vars\": [";
	bool firstVar = true;
	for (const std::string& var : boundaryPrunedArtifactVars)
	{
		if (var.empty() || var == "NOT_A_MEM_OP" || retainedTraceVars.count(var))
			continue;
		if (!firstVar) manifestFile << ", ";
		firstVar = false;
		manifestFile << jsonString(var);
	}
	manifestFile << "],\n";
	manifestFile << "  \"states\": [\n";
	for (size_t stateIndex = 0; stateIndex < boundaryStates.size(); ++stateIndex)
	{
		const BoundaryState& state = boundaryStates[stateIndex];
		if (stateIndex != 0) manifestFile << ",\n";
		manifestFile << "    {\"mapped_node\": " << state.mappedNode->getIdx()
			<< ", \"lanes\": " << state.lanes
			<< ", \"value_bits\": " << state.valueBits
			<< ", \"storage\": " << jsonString(state.storage)
			<< ", \"seed\": {\"kind\": " << jsonString(state.seedKind);
		if (state.seedKind == "constant" || state.seedKind == "register_reset")
			manifestFile << ", \"value\": " << state.seedValue;
		manifestFile << ", \"execution\": \"loop_launch\", \"scheduled\": false";
		if (!state.seedTraceVars.empty())
		{
			manifestFile << ", \"trace_vars\": [";
			for (size_t i = 0; i < state.seedTraceVars.size(); ++i)
			{
				if (i != 0) manifestFile << ", ";
				manifestFile << jsonString(state.seedTraceVars[i]);
			}
			manifestFile << "]";
		}
		manifestFile << "}, \"drain\": {\"execution\": \"loop_exit\", "
			"\"scheduled\": false, \"trace_vars\": [";
		for (size_t i = 0; i < state.finalTraceVars.size(); ++i)
		{
			if (i != 0) manifestFile << ", ";
			manifestFile << jsonString(state.finalTraceVars[i]);
		}
		manifestFile << "]}}";
	}
	manifestFile << "\n  ],\n";
	manifestFile << "  \"stationary_preloads\": [";
	for (size_t index = 0; index < stationaryPreloads.size(); ++index)
	{
		const StationaryPreload& preload = stationaryPreloads[index];
		if (index != 0) manifestFile << ", ";
		manifestFile << "{\"mapped_node\": " << preload.mappedNode->getIdx()
			<< ", \"operand\": \"I" << preload.operand << "\""
			<< ", \"value_bits\": " << preload.valueBits
			<< ", \"row\": " << preload.row
			<< ", \"column\": " << preload.column
			<< ", \"trace_address\": {\"variable\": "
			<< jsonString(preload.traceVar) << ", \"byte_offset\": "
			<< preload.byteOffset << "}}";
	}
	manifestFile << "]\n";
	manifestFile << "}\n";
	llvm::outs() << "[boundary] Recorded " << boundaryStates.size()
		<< " loop-carried state value(s) and " << stationaryPreloads.size()
		<< " stationary preload(s) in " << name
		<< "_BOUNDARY_manifest.json\n";
}

std::unordered_set<dfgNode *> DFG::getLineage(dfgNode *n)
{

	std::unordered_set<dfgNode *> lin;

	std::queue<dfgNode *> q;
	q.push(n);
	lin.insert(n);

	while (!q.empty())
	{
		dfgNode *head = q.front();
		q.pop();
		for (dfgNode *anc : head->getAncestors())
		{
			if (head->ancestorBackEdgeMap[anc])
				continue;
			if (lin.find(anc) == lin.end())
			{
				q.push(anc);
				lin.insert(anc);
			}
		}
	}

	LLVM_DEBUG(dbgs() << "lineage = ");
	for (dfgNode *n : lin)
		LLVM_DEBUG(dbgs() << n->getIdx() << ",");
	LLVM_DEBUG(dbgs() << "\n");

	return lin;
}

void populateStructOffset(StructType *ST, std::unordered_map<int, int> &structOffset, const DataLayout &DL)
{
	int prev_offset = 0;
	int ctr = 0;
	for (auto type_el : ST->elements())
	{
		LLVM_DEBUG(dbgs() << "\t\t");
		LLVM_DEBUG(type_el->dump());
		structOffset[ctr] = prev_offset;
		prev_offset += DL.getTypeAllocSize(type_el);
		ctr++;
	}
}

int DFG::CalculateGEPBaseAddr(GetElementPtrInst *GEP)
{
	// dfgNode* gep_node = findNode(GEP); assert(gep_node);
	const DataLayout DL = GEP->getParent()->getParent()->getParent()->getDataLayout();
	int offset = 0;
	Type *currType = GEP->getSourceElementType();
	for (unsigned i = 1; i < GEP->getNumOperands(); i++)
	{
		LLVM_DEBUG(dbgs() << "currType = ");
		LLVM_DEBUG(currType->dump());
		Value *idxVal = GEP->getOperand(i);
		ConstantInt *CV = dyn_cast<ConstantInt>(idxVal);

		// Operand[1] indexes the source element type directly.
		if (i == 1)
		{
			int stride = (int)DL.getTypeAllocSize(currType);
			if (CV)
			{
				int cv_int = CV->getSExtValue();
				offset += cv_int * stride;
				LLVM_DEBUG(dbgs() << "\t offset_idx" << i << "=" << cv_int
								  << " stride=" << stride << "\n");
			}
			continue;
		}

		if (StructType *ST = dyn_cast<StructType>(currType))
		{
			// LLVM requires struct indices to be constants. If not, skip safely.
			if (!CV)
			{
				LLVM_DEBUG(dbgs() << "Non-constant struct index; skipping constant offset contribution\n");
				continue;
			}

			int cv_int = CV->getSExtValue();
			LLVM_DEBUG(dbgs() << "\t struct offset_idx" << i << "=" << cv_int << "\n");
			std::unordered_map<int, int> structOffset;
			populateStructOffset(ST, structOffset, DL);
			auto offIt = structOffset.find(cv_int);
			if (offIt != structOffset.end())
			{
				offset += offIt->second;
				currType = ST->getElementType(cv_int);
			}
			continue;
		}

		int stride = 0;
		if (ArrayType *AT = dyn_cast<ArrayType>(currType))
		{
			stride = (int)DL.getTypeAllocSize(AT->getElementType());
			currType = AT->getElementType();
		}
		else
		{
			stride = (int)DL.getTypeAllocSize(currType);
		}

		if (CV)
		{
			int cv_int = CV->getSExtValue();
			offset += cv_int * stride;
			LLVM_DEBUG(dbgs() << "\t offset_idx" << i << "=" << cv_int
							  << " stride=" << stride << "\n");
		}
	}

	LLVM_DEBUG(dbgs() << "\t offset = " << offset << "\n");

	return offset;
}

void DFG::GEPBaseAddrCheck(Function &F)
{

	for (auto &bb : F)
	{
		for (auto &ins : bb)
		{
			if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(&ins))
			{
				LLVM_DEBUG(dbgs() << "GEP = ");
				LLVM_DEBUG(GEP->dump());
				int offset = CalculateGEPBaseAddr(GEP);
				GEPOffsetMap[GEP] = offset;
				LLVM_DEBUG(dbgs() << "[dfg.cpp][GEPBaseAddrCheck] offset: " << offset << "\n");
			}
		}
	}
}

// Trace through GEPs and pointer loads to find the root GlobalVariable name.
// Handles patterns like volatile int **A where intermediate pointer loads
// produce unnamed temporaries that break simple getName() lookups.
static std::string traceBaseArrayNameImpl(Value *V, std::unordered_set<const Value *> &visited, unsigned depth) {
	if (!V)
		return "";
	if (depth > 64)
		return "";
	if (!visited.insert(V).second)
		return "";

	std::string result;
	if (isa<GlobalVariable>(V))
	{
		result = V->getName().str();
	}
	else if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(V))
	{
		result = traceBaseArrayNameImpl(GEP->getPointerOperand(), visited, depth + 1);
	}
	else if (LoadInst *LI = dyn_cast<LoadInst>(V))
	{
		result = traceBaseArrayNameImpl(LI->getPointerOperand(), visited, depth + 1);
	}
	// For loop-carried pointer PHIs (e.g. B_elem created by FlattenDoublePointers),
	// follow the first incoming value (from outside the loop / preheader) to find
	// the original global array name (e.g. B_elem -> B_elem_init -> B).
	else if (PHINode *PHI = dyn_cast<PHINode>(V))
	{
		if (PHI->getNumIncomingValues() > 0)
			result = traceBaseArrayNameImpl(PHI->getIncomingValue(0), visited, depth + 1);
	}
	else if (Argument *Arg = dyn_cast<Argument>(V))
	{
		result = Arg->hasName()
			? Arg->getName().str()
			: ("_arg" + std::to_string(Arg->getArgNo()));
	}
	else
	{
		result = V->getName().str();
	}

	visited.erase(V);
	return result;
}

static std::string traceBaseArrayName(Value *V) {
	std::unordered_set<const Value *> visited;
	return traceBaseArrayNameImpl(V, visited, 0);
}

// Traverse a GEP chain to find the root GlobalVariable (e.g. @A from
// %arrayidx7 = gep [8 x i32]* %arrayidx4, 0, %j
//   where %arrayidx4 = gep @A, 0, %i).
static Value* getGlobalBaseImpl(Value *V, std::unordered_set<const Value *> &visited, unsigned depth) {
	if (!V)
		return nullptr;
	if (depth > 64)
		return nullptr;
	if (!visited.insert(V).second)
		return nullptr;
	if (isa<GlobalVariable>(V))
	{
		visited.erase(V);
		return V;
	}
	if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(V))
	{
		Value *base = getGlobalBaseImpl(GEP->getPointerOperand(), visited, depth + 1);
		visited.erase(V);
		return base;
	}
	if (GEPOperator *GEP = dyn_cast<GEPOperator>(V))
	{
		Value *base = getGlobalBaseImpl(GEP->getPointerOperand(), visited, depth + 1);
		visited.erase(V);
		return base;
	}
	if (BitCastOperator *BC = dyn_cast<BitCastOperator>(V))
	{
		Value *base = getGlobalBaseImpl(BC->getOperand(0), visited, depth + 1);
		visited.erase(V);
		return base;
	}
	if (LoadInst *LI = dyn_cast<LoadInst>(V))
	{
		Value *base = getGlobalBaseImpl(LI->getPointerOperand(), visited, depth + 1);
		visited.erase(V);
		return base;
	}
	if (PHINode *PHI = dyn_cast<PHINode>(V))
	{
		for (unsigned i = 0; i < PHI->getNumIncomingValues(); ++i)
		{
			Value *base = getGlobalBaseImpl(PHI->getIncomingValue(i), visited, depth + 1);
			if (base)
			{
				visited.erase(V);
				return base;
			}
		}
		visited.erase(V);
		return nullptr;
	}
	visited.erase(V);
	return V;
}

static Value* getGlobalBase(Value *V) {
	std::unordered_set<const Value *> visited;
	return getGlobalBaseImpl(V, visited, 0);
}

void checkGEPDerivatives(Instruction *root, GetElementPtrInst *GEP, std::unordered_map<Value *, GetElementPtrInst *> &deriv)
{
	for (User *u : root->users())
	{
		if(u == GEP) continue;
		if (Instruction *ins = dyn_cast<Instruction>(u))
		{
			if (ins->mayReadOrWriteMemory())
				continue;
			if (deriv.find(ins) != deriv.end())
				continue;

			LLVM_DEBUG(dbgs() << "\t value = ");
			LLVM_DEBUG(ins->dump());
			deriv[ins] = GEP;
			checkGEPDerivatives(ins, GEP, deriv);
		}
	}
}

//int DFG::getMUnitTransID(BasicBlock *src, BasicBlock *dest)
//{
//
//	for (munitTransition mut : munitTransitionsALL)
//	{
//		if (src == mut.srcBB && dest == mut.destBB)
//		{
//			return mut.id;
//		}
//	}
//	assert(false);
//}

/*
 *
 * Outputs:
 * 1) outer_vals
 *
 * contains values loaded from outerLoopStore/outerLoopLoads. These loads are scalar
 * load/stores corresponds to live in and out values such as outer loop induction variables
 * Eg:
 *
 *  %i.022 = phi i32 [ 0, %entry ], [ %inc8, %for.end ] (%i.022 is an induction variable passed in
 * as a live in variable)
 *
 *  %add = add nsw i32 %mul, %sum.021 (%add is a temp variable generated within loop and passed out
 *  as a live out variable)
 *
 * 2) mem_ptrs
 *
 * contains pointer operands of array loads/stores (??). Pointer operand is a GEP instruction.
 *
 * Eg: Value name:arrayidx4,
 * GEP =   %arrayidx4 = getelementptr inbounds [4 x [4 x i32]], [4 x [4 x i32]]* @A, i32 0, i32 %i.022, i32 %j.020
 *
 * 3) acc
 *
 * contains the number of memory accesses for each array/ scalars.
 * Value represent the array base pointer or scalar pointer (??)
 * Eg:
 * base_ptr:A, accesses = 1 (array)
 * base_ptr:x, accesses = 1 (array)
 * base_ptr:add, accesses = 1 (outloopstore scalar)
 * base_ptr:i.022, accesses = 1 (outloopload scalar)
 *
 *
 * */

void DFG::getTransferVariables(std::unordered_set<Value *> &outer_vals,
		std::unordered_map<Value *, GetElementPtrInst *> &mem_ptrs,
		std::unordered_map<Value *, int> &acc,
		Function &F)
{

	assert(OutLoopNodeMap.size() == OutLoopNodeMapReverse.size());

	for (auto it = OutLoopNodeMapReverse.begin(); it != OutLoopNodeMapReverse.end(); it++)
	{
		LLVM_DEBUG(dbgs() << "onode = " << it->first->getIdx()  << "nametype = " << it->first->getNameType() << "\n");
		Value *val = it->second;
		outer_vals.insert(val);

		if(it->first->getNameType().compare("OutLoopSTORE")){
			outVals_inorout[val]=1;//load
		}else{
			outVals_inorout[val]=0;//store
		}
		acc[val] = acc[val] + 1;
	}

	/*
	 * gep_derivatives:
	 *
	 * Value: pointer operand of array loads/stores (??). Pointer operand is a GEP instruction.
	 * GetElementPtrInst: corresponding GEP instruction
	 * */

	std::unordered_map<Value *, GetElementPtrInst *> gep_derivatives;

	for (auto &bb : F)
	{
		for (auto &ins : bb)
		{
			if (gep_derivatives.find(&ins) != gep_derivatives.end())
				continue;
			if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(&ins))
			{
				LLVM_DEBUG(dbgs() << "searching for GEP = ");
				LLVM_DEBUG(GEP->dump());
				gep_derivatives[GEP] = GEP;
				checkGEPDerivatives(GEP, GEP, gep_derivatives);
			}
		}
	}
	/*
	 * sizeArrMap:
	 *
	 * Contains the array sizes passed from function annotations.
	 *
	 * Eg:
	 * __attribute__((annotate("fr:128,fi:128")))
	 * int fix_fft(short fr[], short fi[], short m1, short inverse){
	 * */
		for (dfgNode *node : NodeList)
		{
			if (node->getNode())
			{
				auto recordAccessBase = [&](Value *ptr) {
					if (sizeArrMap.find(ptr->getName().str()) != sizeArrMap.end())
					{
						acc[ptr] = acc[ptr] + 1;
						return;
					}

					auto gepIt = gep_derivatives.find(ptr);
					if (gepIt != gep_derivatives.end())
					{
						mem_ptrs[ptr] = gepIt->second;
						Value *basePtr = gepIt->second->getPointerOperand();
						acc[basePtr] = acc[basePtr] + 1;
						return;
					}

					Value *basePtr = getGlobalBase(ptr);
					if (!basePtr)
						basePtr = ptr;
					acc[basePtr] = acc[basePtr] + 1;
				};

				if (LoadInst *LDI = dyn_cast<LoadInst>(node->getNode()))
				{
					LLVM_DEBUG(dbgs() << "LOAD TRANSFER = ");
					LLVM_DEBUG(LDI->dump());
					Value *ld_ptr = LDI->getPointerOperand();
					LLVM_DEBUG(dbgs()<<"ld_ptr:" );
					LLVM_DEBUG(ld_ptr->dump());
					recordAccessBase(ld_ptr);

				}
				if (StoreInst *STI = dyn_cast<StoreInst>(node->getNode()))
				{
					LLVM_DEBUG(dbgs() << "STORE TRANSFER = ");
					LLVM_DEBUG(STI->dump());
					Value *st_ptr = STI->getPointerOperand();
					recordAccessBase(st_ptr);

				}
			}
		}


	// for(auto it = OutLoopNodeMapReverse.begin(); it != OutLoopNodeMapReverse.end(); it++){
	// 	dfgNode* on = it->first;
	// 	acc[it->second] = 1;
	// }

	LLVM_DEBUG(dbgs()<<"\nOutvals_inorout contains: \n" );
	for (auto it = outVals_inorout.begin(); it != outVals_inorout.end(); it++)
	{
		Value* outvl = it->first;
		LLVM_DEBUG(dbgs() << "outVal:");
		LLVM_DEBUG(outvl->dump());
		LLVM_DEBUG(dbgs() << "\n");
		LLVM_DEBUG(dbgs() << "load(1)/store(0):"<<it->second <<"\n");
	}

}
/*
 * Set base pointer names of DFG nodes
 * Record sizes of each array/ scalar
 *
 * base_ptr:i.022, size = 4
 * base_ptr:add, size = 4
 * base_ptr:A, size = 64
 * base_ptr:x, size = 16
 *
 *
 * */
void DFG::SetBasePointers(std::unordered_set<Value *> &outer_vals,
		std::unordered_map<Value *, GetElementPtrInst *> &mem_ptrs, std::map<dfgNode*,Value*> &OLNodesWithPtrTyUsage, Function &F)
{


	LLVM_DEBUG(dbgs()<<"array_pointer_sizes contains: \n" );
	for (auto it = array_pointer_sizes.begin(); it != array_pointer_sizes.end(); it++)
	{
		std::string base_ptr = it->first;
		int size = it->second;
		LLVM_DEBUG(dbgs() << "base_ptr:" << base_ptr << ", size = " << size << "\n");
	}

	LLVM_DEBUG(dbgs() << "OutLoopNodeMapReverse size = " << OutLoopNodeMapReverse.size() << "\n");
	LLVM_DEBUG(dbgs() << "OutLoopNodeMap size = " << OutLoopNodeMap.size() << "\n");

	auto ensureResolvedBasePtr = [&](const std::string &basePtrName, Value *ptr) {
		if (!basePtrName.empty())
			return;
		std::string ptrName = "<unnamed>";
		if (ptr && !ptr->getName().empty())
			ptrName = ptr->getName().str();
		throw DFGGenerationError(
			"UnsupportedArrayDimensions",
			"could not resolve base pointer for '" + ptrName + "' in function '" +
			F.getName().str() +
			"'; this pointer/array pattern is not supported (Morpher currently supports only up to 2D arrays)");
	};

	for (dfgNode *node : NodeList)
	{

		if (OutLoopNodeMapReverse.find(node) != OutLoopNodeMapReverse.end())
		{
			assert(outer_vals.find(OutLoopNodeMapReverse[node]) != outer_vals.end());
			LLVM_DEBUG(dbgs() << "Outer LOOP value :: ");
			LLVM_DEBUG(OutLoopNodeMapReverse[node]->dump());
			LLVM_DEBUG(dbgs() << "Outer LOOP name = " << OutLoopNodeMapReverse[node]->getName() << "\n");
			LLVM_DEBUG(dbgs() << "Outer LOOP node idx = " << node->getIdx() << "\n");
			// Use a synthetic name for unnamed values to avoid empty BasePointerName in XML/mem_alloc.
			{
				StringRef rawName = OutLoopNodeMapReverse[node]->getName();
				std::string outerName = rawName.empty()
					? ("_ol_" + std::to_string(node->getIdx()))
					: rawName.str();
				node->setArrBasePtr(outerName);
			}
			/*
			 *   %arrayidx = gep , []* @sum, ... //arrayidx is the address of sum[i]
			 *
			 *   for.body
			 *
			 *      store i32 %x, i32* %arrayidx    // store to address arrayidx
			 *
			 *
			 * Detect out loop nodes with pointer type usage in the loop body
			 * These nodes require special treatment in instrumentation
			 * because we need to record the local memory address, not the memory address
			 * generated in the ./final at runtime*/
			for (User *u : OutLoopNodeMapReverse[node]->users())
			{
				LLVM_DEBUG(dbgs() << "Users: ");
				LLVM_DEBUG(u->dump());
				LLVM_DEBUG(dbgs() << "\n");

				for (int i = 0; i < u->getNumOperands(); ++i)
				{
					if (u->getOperand(i) ==  OutLoopNodeMapReverse[node] && u->getOperand(i)->getType()->isPointerTy())
					{
						LLVM_DEBUG(dbgs() << "Have pointer type user\n");
						OLNodesWithPtrTyUsage[node]=OutLoopNodeMapReverse[node];
					}
				}

			}

			DataLayout DL = F.getParent()->getDataLayout();
			// Use getArrBasePtr() to stay consistent with the synthetic name set above.
			array_pointer_sizes[node->getArrBasePtr()] = DL.getTypeAllocSize(OutLoopNodeMapReverse[node]->getType());
		}
		else if (node->getNode() && node->getNode()->mayReadOrWriteMemory())
		{
			if (LoadInst *LDI = dyn_cast<LoadInst>(node->getNode()))
			{
				Value *pointer = LDI->getPointerOperand();
				if(sizeArrMap.find(pointer->getName().str()) != sizeArrMap.end()){
					array_pointer_sizes[pointer->getName().str()] = sizeArrMap[pointer->getName().str()];
					node->setArrBasePtr(pointer->getName().str());
				}
				else{
					std::string base_ptr_name;
						if (mem_ptrs.find(pointer) != mem_ptrs.end()) {
							// Use traceBaseArrayName so pointer-chain patterns (e.g., volatile int **A)
							// correctly resolve to the global variable name instead of an unnamed temp.
							base_ptr_name = traceBaseArrayName(mem_ptrs[pointer]->getPointerOperand());
						} else {
							// Pointer load from global (e.g., load i32** from @A in volatile int **A).
							base_ptr_name = traceBaseArrayName(pointer);
						}
						ensureResolvedBasePtr(base_ptr_name, pointer);
						node->setArrBasePtr(base_ptr_name);

					if (sizeArrMap.find(base_ptr_name) != sizeArrMap.end())
					{
						array_pointer_sizes[base_ptr_name] = sizeArrMap[base_ptr_name];
					}
					else
					{
						DataLayout DL = LDI->getParent()->getParent()->getParent()->getDataLayout();
							if (mem_ptrs.find(pointer) != mem_ptrs.end()) {
								array_pointer_sizes[base_ptr_name] = DL.getTypeAllocSize(mem_ptrs[pointer]->getSourceElementType());
						} else {
							// Size of the loaded type (pointer or data element).
							array_pointer_sizes[base_ptr_name] = DL.getTypeAllocSize(LDI->getType());
						}
					}
				}
			}
			else if (StoreInst *STI = dyn_cast<StoreInst>(node->getNode()))
			{
				Value *pointer = STI->getPointerOperand();
				if(sizeArrMap.find(pointer->getName().str()) != sizeArrMap.end()){
					array_pointer_sizes[pointer->getName().str()] = sizeArrMap[pointer->getName().str()];
					node->setArrBasePtr(pointer->getName().str());
				}
				else{
					std::string base_ptr_name;
						if (mem_ptrs.find(pointer) != mem_ptrs.end()) {
							base_ptr_name = traceBaseArrayName(mem_ptrs[pointer]->getPointerOperand());
						} else {
							base_ptr_name = traceBaseArrayName(pointer);
						}
						ensureResolvedBasePtr(base_ptr_name, pointer);
						node->setArrBasePtr(base_ptr_name);

					if (sizeArrMap.find(base_ptr_name) != sizeArrMap.end())
					{
						array_pointer_sizes[base_ptr_name] = sizeArrMap[base_ptr_name];
					}
					else
					{
						DataLayout DL = STI->getParent()->getParent()->getParent()->getDataLayout();
							if (mem_ptrs.find(pointer) != mem_ptrs.end()) {
								array_pointer_sizes[base_ptr_name] = DL.getTypeAllocSize(mem_ptrs[pointer]->getSourceElementType());
						} else {
							PointerType *PT = cast<PointerType>(STI->getValueOperand()->getType()->getPointerTo());
							array_pointer_sizes[base_ptr_name] = DL.getTypeAllocSize(STI->getValueOperand()->getType());
						}
					}
				}
			}
			else
			{
				//if the instruction could read or write memory it should be a load
				// or a store instruction.
				LLVM_DEBUG(node->getNode()->dump());
				assert(false);
			}
		}
		else if (node->getNode() && isa<GetElementPtrInst>(node->getNode()))
		{
			GetElementPtrInst *GEP = cast<GetElementPtrInst>(node->getNode());
				// Use traceBaseArrayName so pointer chains (e.g., volatile int **A) resolve
				// to the global name rather than an unnamed intermediate temporary.
				std::string base_ptr_name = traceBaseArrayName(GEP->getPointerOperand());
				ensureResolvedBasePtr(base_ptr_name, GEP->getPointerOperand());
				node->setArrBasePtr(base_ptr_name);

			if (sizeArrMap.find(base_ptr_name) != sizeArrMap.end())
			{
				array_pointer_sizes[base_ptr_name] = sizeArrMap[base_ptr_name];
			}
			else
			{
				DataLayout DL = GEP->getParent()->getParent()->getParent()->getDataLayout();
					array_pointer_sizes[base_ptr_name] = DL.getTypeAllocSize(GEP->getSourceElementType());
			}
		}


	}
	LLVM_DEBUG(dbgs() << "Outloop node instructions with pointer type usage in the loop body \n");
	for (auto i : OLNodesWithPtrTyUsage)
	{

		Value* ptr = i.second;
		LLVM_DEBUG(ptr->dump());
		LLVM_DEBUG(dbgs() << "\n");

	}

	LLVM_DEBUG(dbgs()<<"array_pointer_sizes contains: \n" );
	for (auto it = array_pointer_sizes.begin(); it != array_pointer_sizes.end(); it++)
	{
		std::string base_ptr = it->first;
		int size = it->second;
		LLVM_DEBUG(dbgs() << "base_ptr:" << base_ptr << ", size = " << size << "\n");
	}


}
/*
 * Instrument the code to save live in and live out data for simulation
 *
 *
 * */
//#define DEBUG_TYPE "instrumentation"
void DFG::InstrumentInOutVars(Function &F, std::unordered_map<Value *, int> mem_accesses, std::map<dfgNode*,Value*> &OLNodesWithPtrTyUsage, std::unordered_map<Value *, int> &spm_base_address){

	LLVM_DEBUG(dbgs() << "Outloop node instructions with pointer type usage in the loop body \n");
	for (auto i : OLNodesWithPtrTyUsage)
	{

		Value* ptr = i.second;
		LLVM_DEBUG(ptr->dump());
		LLVM_DEBUG(dbgs() << "\n");

	}
	LLVMContext &Ctx = F.getContext();

	//Instrumentation functions

	//this function should be called at the beginning of the loop.
	auto loopStartFn = F.getParent()->getOrInsertFunction(
			"loopStart",
			FunctionType::getVoidTy(Ctx),
			morpher::llvm_compat::getInt8PtrTy(Ctx) //loopname
	);

	//this should be called at the end of the loop
	auto clearPrintedArrs = F.getParent()->getOrInsertFunction(
			"clearPrintedArrs",
			FunctionType::getVoidTy(Ctx));

	auto printArrFunc = F.getParent()->getOrInsertFunction(
			"printArr",
			FunctionType::getVoidTy(Ctx),
			morpher::llvm_compat::getInt8PtrTy(Ctx),
			morpher::llvm_compat::getInt8PtrTy(Ctx),
			Type::getInt32Ty(Ctx),
			Type::getInt8Ty(Ctx),
			Type::getInt32Ty(Ctx));

	auto reportDynArrSize = F.getParent()->getOrInsertFunction(
			"reportDynArrSize", FunctionType::getVoidTy(Ctx),
			morpher::llvm_compat::getInt8PtrTy(Ctx),
			morpher::llvm_compat::getInt8PtrTy(Ctx),
			Type::getInt32Ty(Ctx),
			Type::getInt32Ty(Ctx));

	auto printDynArrSize = F.getParent()->getOrInsertFunction(
			"printDynArrSize", FunctionType::getVoidTy(Ctx));

	auto loopEndFn = F.getParent()->getOrInsertFunction(
			"loopEnd", FunctionType::getVoidTy(Ctx), morpher::llvm_compat::getInt8PtrTy(Ctx));

	//these should be called prior to the loop start/end to report all the variables that are being accessed in the loop
	auto live_in_report_FN = F.getParent()->getOrInsertFunction(
			"LiveInReport",
			FunctionType::getVoidTy(Ctx),
			morpher::llvm_compat::getInt8PtrTy(Ctx), //variable name
			morpher::llvm_compat::getInt8PtrTy(Ctx), //variable data
			Type::getInt32Ty(Ctx) //size
	);
	auto live_in_report_FN2 = F.getParent()->getOrInsertFunction(
			"LiveInReport2",
			FunctionType::getVoidTy(Ctx),
			morpher::llvm_compat::getInt8PtrTy(Ctx), //variable name
			PointerType::get(Type::getInt32Ty(Ctx), 0), //variable data
			Type::getInt32Ty(Ctx) //size
	);

	auto live_in_report_PtrTypeUsage = F.getParent()->getOrInsertFunction(
			"LiveInReportPtrTypeUsage",
			FunctionType::getVoidTy(Ctx),
			morpher::llvm_compat::getInt8PtrTy(Ctx), //variable name
			morpher::llvm_compat::getInt8PtrTy(Ctx), //spm base address (as string)
			morpher::llvm_compat::getInt8PtrTy(Ctx), //actual runtime pointer (GEP result, cast to i8*)
			morpher::llvm_compat::getInt8PtrTy(Ctx), //actual array base pointer (root GlobalVariable, cast to i8*)
			Type::getInt32Ty(Ctx)    //size
	);

	auto mem_access_report_FN = F.getParent()->getOrInsertFunction(
			"MemAccessReport",
			FunctionType::getVoidTy(Ctx),
			morpher::llvm_compat::getInt8PtrTy(Ctx), //loop name
			Type::getInt32Ty(Ctx),   //DFG node idx
			morpher::llvm_compat::getInt8PtrTy(Ctx), //base pointer name
			morpher::llvm_compat::getInt8PtrTy(Ctx), //spm base address (as string)
			morpher::llvm_compat::getInt8PtrTy(Ctx), //actual runtime pointer
			morpher::llvm_compat::getInt8PtrTy(Ctx), //actual array base pointer
			Type::getInt32Ty(Ctx),   //access size in bytes
			Type::getInt8Ty(Ctx)     //is store
	);

	auto live_out_report_FN = F.getParent()->getOrInsertFunction(
			"LiveOutReport",
			FunctionType::getVoidTy(Ctx),
			morpher::llvm_compat::getInt8PtrTy(Ctx), //variable name
			morpher::llvm_compat::getInt8PtrTy(Ctx), // variable data
			Type::getInt32Ty(Ctx) //size
	);

	auto live_in_report_intvar_FN = F.getParent()->getOrInsertFunction(
			"LiveInReportIntermediateVar",
			FunctionType::getVoidTy(Ctx),
			morpher::llvm_compat::getInt8PtrTy(Ctx), //variable name
			Type::getInt32Ty(Ctx) //variable data
	);

	auto live_out_report_intvar_FN = F.getParent()->getOrInsertFunction(
			"LiveOutReportIntermediateVar",
			FunctionType::getVoidTy(Ctx),
			morpher::llvm_compat::getInt8PtrTy(Ctx), //variable name
			Type::getInt32Ty(Ctx) //variable data
	);
	auto scalarTraceValue = [&](IRBuilder<> &builder, Value *V) -> Value * {
		Type *Ty = V->getType();
		Type *I32Ty = Type::getInt32Ty(Ctx);
		if (Ty->isIntegerTy())
			return builder.CreateIntCast(V, I32Ty, true);
		if (Ty->isFloatTy())
			return builder.CreateBitCast(V, I32Ty);
		if (Ty->isPointerTy())
			return builder.CreatePtrToInt(V, I32Ty);
		if (Ty->isDoubleTy()) {
			Value *I64 = builder.CreateBitCast(V, Type::getInt64Ty(Ctx));
			return builder.CreateTrunc(I64, I32Ty);
		}
		return ConstantInt::get(I32Ty, 0);
	};

	//Insertion of loop start functions to all entries
	for(auto trans : loopentryBB){
		BasicBlock* entryBB = trans.first;
		IRBuilder<> builder(entryBB->getTerminator());

		Value *loopName = builder.CreateGlobalStringPtr(this->kernelname);
		builder.CreateCall(loopStartFn,{loopName});
	}



	// Insertion of reporting functions at entry and exit basicblocks
	for(auto it = mem_accesses.begin(); it != mem_accesses.end(); it++){
		Instruction *BuilderInsertPoint = NodeList[0]->getNode();
		if (!BuilderInsertPoint)
			BuilderInsertPoint = &F.getEntryBlock().front();
		IRBuilder<> builder(BuilderInsertPoint);

		Value* ptr = it->first;
		dfgNode *outLoopNode = nullptr;
		auto outLoopIt = OutLoopNodeMap.find(ptr);
		if (outLoopIt != OutLoopNodeMap.end())
			outLoopNode = outLoopIt->second;
		LLVM_DEBUG(dbgs() << "\n---ptr_name = " << ptr->getName() << "\n");
		LLVM_DEBUG(ptr->dump());


		// Skip unnamed intermediate pointer values (e.g., from volatile int **A chains).
		// These appear in mem_accesses because GEP pointer operands are tracked in acc, but
		// unnamed temporaries have no entry in array_pointer_sizes. They are still needed in
		// acc for AllocateSPMBanks bank assignments, so we skip only the instrumentation here.
		if (ptr->getName().empty() && ptr->getType()->isPointerTy() && !outLoopNode) {
			LLVM_DEBUG(dbgs() << "Skipping unnamed intermediate pointer in InstrumentInOutVars\n");
			continue;
		}

		// Resolve the array name for this pointer. For intermediate pointer loads
		// like %9=load i32**,@C, acc[%9] is added by GEP-based loads that use
		// %9 as their base pointer. array_pointer_sizes stores the traced base
		// name ("C"), not the load-result value's own name. Use traceBaseArrayName
		// as fallback to resolve through pointer-load chains to the real base name.
		std::string arr_name = ptr->getName().str();
		if (outLoopNode && (arr_name.empty() || array_pointer_sizes.find(arr_name) == array_pointer_sizes.end())) {
			arr_name = outLoopNode->getArrBasePtr();
		}
		if (array_pointer_sizes.find(arr_name) == array_pointer_sizes.end()) {
			arr_name = traceBaseArrayName(ptr);
		}
		if (array_pointer_sizes.find(arr_name) == array_pointer_sizes.end()) {
			// Still not found: skip this entry (no known allocation for this pointer).
			LLVM_DEBUG(dbgs() << "Skipping unknown pointer in InstrumentInOutVars: "
			                  << ptr->getName() << "\n");
			continue;
		}
		// Intermediate pointer-type values (e.g., %9=load i32**,@A) defined
		// inside the loop body cannot be used at the loop entry/exit without
		// violating SSA dominance. However, OLOAD nodes with pointer type
		// (those in OLNodesWithPtrTyUsage) are computed OUTSIDE the inner loop
		// and ARE available at the entry — they need LiveInReportPtrTypeUsage.
		bool ptrIsOLNodeWithPtrTyUsage = false;
		for (auto& olpair : OLNodesWithPtrTyUsage) {
			if (olpair.second == ptr) {
				ptrIsOLNodeWithPtrTyUsage = true;
				break;
			}
		}
		if (ptr->getType()->isPointerTy() && !isa<GlobalValue>(ptr) && !ptrIsOLNodeWithPtrTyUsage) {
			LLVM_DEBUG(dbgs() << "Skipping loop-body pointer in instrumentation: "
			                  << ptr->getName() << "\n");
			continue;
		}

		int size = array_pointer_sizes[arr_name];

		LLVM_DEBUG(dbgs() << "size = " << size << "\n");

		Value* ptr_name_val = builder.CreateGlobalStringPtr(arr_name);
		Value *size_val = ConstantInt::get(Type::getInt32Ty(Ctx), size);
		Value *size_val2 = ConstantInt::get(Type::getInt32Ty(Ctx), size/4);
		bool isOLNodewithPtrTyUsage = false;
		for(auto trans : loopentryBB){
			BasicBlock* entryBB = trans.first;
			builder.SetInsertPoint(entryBB->getTerminator());

			for (auto i : OLNodesWithPtrTyUsage)
			{
				/*Instrument the out-loop-nodes with pointer type usage in the loop body.
				 * These nodes require special treatment in instrumentation
				 * because we need to record the local memory address, not the memory address
				 * generated in the ./final at runtime
				 * Works only for single dim arrays (required to flatten mem access in multi-dim arrays)
				 *
				 * */
				Value* ptrs = i.second;
				//				ptrs->dump();
				if(ptrs == ptr){
					Value* rootBase = getGlobalBase(ptrs);
					Value* ptr_spm_address_val = builder.CreateGlobalStringPtr(
							std::to_string(spm_base_address[rootBase]));
					Value* actual_ptr = builder.CreateBitCast(ptrs, morpher::llvm_compat::getInt8PtrTy(Ctx));
					Value* array_base = builder.CreateBitCast(rootBase, morpher::llvm_compat::getInt8PtrTy(Ctx));
					builder.CreateCall(live_in_report_PtrTypeUsage,
							{ptr_name_val, ptr_spm_address_val, actual_ptr, array_base, size_val});
					isOLNodewithPtrTyUsage = true;
					break;
				}
				isOLNodewithPtrTyUsage = false;
			}

			if(isOLNodewithPtrTyUsage == true){isOLNodewithPtrTyUsage = false;}
			else if(ptr->getType()->isPointerTy()){
				LLVM_DEBUG(dbgs() << "Adding LiveInReport() call\n");
				Value* bitcastedptr = builder.CreateBitCast(it->first, morpher::llvm_compat::getInt8PtrTy(Ctx));
				builder.CreateCall(live_in_report_FN,{ptr_name_val,bitcastedptr,size_val});
			//	Value* bitcastedptr2 = builder.CreateBitCast(it->first, Type::getInt32PtrTy(Ctx));
			//	builder.CreateCall(live_in_report_FN2,{ptr_name_val,bitcastedptr2,size_val2});
			}
			else{
				LLVM_DEBUG(dbgs() << "type = "); LLVM_DEBUG(ptr->getType()->dump());
				if (Instruction *PtrIns = dyn_cast<Instruction>(it->first)) {
					if (DT && !DT->dominates(PtrIns, entryBB->getTerminator())) {
						LLVM_DEBUG(dbgs() << "Skipping non-dominating intermediate live-in value: "
						                  << ptr->getName() << "\n");
						continue;
					}
				}
				/* For integer OLOADs used as dimension indices in multi-dim inner-loop GEPs,
				 * precompute  spm_base + idx * dim_stride  so the DFG ADD can combine it
				 * with the inner-loop shifted index without needing a 3-operand ADD. */
				Value* storeVal = nullptr;
				for (User *U : ptr->users()) {
					if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(U)) {
						if (loopBB.find(GEP->getParent()) == loopBB.end()) continue;
						int numVarOps = 0;
						for (unsigned ki = 1; ki < GEP->getNumOperands(); ki++)
							if (!isa<ConstantInt>(GEP->getOperand(ki))) numVarOps++;
						if (numVarOps < 2) continue;
						DataLayout DL = GEP->getParent()->getParent()->getParent()->getDataLayout();
						Type *currType = GEP->getSourceElementType();
						for (unsigned opIdx = 1; opIdx < GEP->getNumOperands(); opIdx++) {
							Value *opVal = GEP->getOperand(opIdx);
							int stride;
							if (opIdx == 1) {
								stride = (int)DL.getTypeAllocSize(currType);
							} else {
								if (ArrayType *AT = dyn_cast<ArrayType>(currType)) {
									stride = (int)DL.getTypeAllocSize(AT->getElementType());
									currType = AT->getElementType();
								} else {
									stride = (int)DL.getTypeAllocSize(currType);
								}
							}
							if (isa<ConstantInt>(opVal) || opVal != ptr) continue;
							Value *gepBase = getGlobalBase(GEP->getPointerOperand());
							auto baseIt = spm_base_address.find(gepBase);
							if (baseIt == spm_base_address.end()) break;
							Value* idx_int  = builder.CreateIntCast(it->first, Type::getInt32Ty(Ctx), true);
							Value* stride_c = ConstantInt::get(Type::getInt32Ty(Ctx), stride);
							Value* base_c   = ConstantInt::get(Type::getInt32Ty(Ctx), baseIt->second);
							storeVal = builder.CreateAdd(base_c, builder.CreateMul(idx_int, stride_c));
							break;
						}
						if (storeVal) break;
					}
				}
				if (!storeVal)
					storeVal = scalarTraceValue(builder, it->first);
				if(outVals_inorout.find(ptr)!=outVals_inorout.end()){
					if(outVals_inorout[ptr]==1){//outLoopLoad
						LLVM_DEBUG(dbgs() << "Adding live_in_report_intermediatevariable() call for OutloopLoad\n");
						builder.CreateCall(live_in_report_intvar_FN,{ptr_name_val,storeVal});
					}
				}else{
					builder.CreateCall(live_in_report_intvar_FN,{ptr_name_val,storeVal});
				}
			}

		}

		for(auto trans : loopexitBB){
			BasicBlock* exitBB = trans.second;
			builder.SetInsertPoint(exitBB->getTerminator());

			for (auto i : OLNodesWithPtrTyUsage){
				Value* ptrs = i.second;
				if(ptrs == ptr){
					LLVM_DEBUG(dbgs() << "No need to add LiveOutReport() call for outloop node with pointer type usage\n");
					isOLNodewithPtrTyUsage = true;
					break;
				}
				isOLNodewithPtrTyUsage = false;
			}
			if(isOLNodewithPtrTyUsage == true){isOLNodewithPtrTyUsage = false;break;}

			if(ptr->getType()->isPointerTy()){
				LLVM_DEBUG(dbgs() << "Adding LiveOutReport() call\n");
				Value* bitcastedptr = builder.CreateBitCast(it->first, morpher::llvm_compat::getInt8PtrTy(Ctx));
				builder.CreateCall(live_out_report_FN,{ptr_name_val,bitcastedptr,size_val});
			}
			else{
				LLVM_DEBUG(dbgs() << "type = "); LLVM_DEBUG(ptr->getType()->dump());

				if(outVals_inorout.find(ptr)!=outVals_inorout.end()){
					if(outVals_inorout[ptr]==0){//outLoopStore
						LLVM_DEBUG(dbgs() << "Adding live_out_report_intermediatevariable() call for OutloopStore\n");
						bool reportedLiveOut = false;
						for (auto &I : *exitBB){

							LLVM_DEBUG(I.dump());
							Value *val = dyn_cast<Value>(&I);
							LLVM_DEBUG(val->dump());
							if (I.getNumOperands() == 0)
								continue;
								if(I.getOperand(0)==ptr){
									LLVM_DEBUG(dbgs() << "lcssa use\n");
	//								ptr_name_val = builder.CreateGlobalStringPtr(val->getName());
									Type *valTy = val->getType();
									if (!valTy->isIntegerTy() && !valTy->isFloatingPointTy() && !valTy->isPointerTy()) {
										LLVM_DEBUG(dbgs() << "Skipping non-castable live-out value in exit block\n");
										continue;
									}
									Value* bitcastedptr = scalarTraceValue(builder, val);
									builder.CreateCall(live_out_report_intvar_FN,{ptr_name_val,bitcastedptr});
									reportedLiveOut = true;
									break;
								}
							}
						if (!reportedLiveOut)
							LLVM_DEBUG(dbgs() << "No scalar LCSSA live-out found for " << arr_name << "\n");


						for (User *u : ptr->users()){
							LLVM_DEBUG(dbgs() << "Users: ");
							LLVM_DEBUG(u->dump());
							LLVM_DEBUG(dbgs() << "\n");
						}
//						Value* bitcastedptr = builder.CreateIntCast(it->first, Type::getInt32Ty(Ctx),true);
//						builder.CreateCall(live_out_report_intvar_FN,{ptr_name_val,bitcastedptr});
					}
				}else{
					LLVM_DEBUG(dbgs() << "Adding live_out_report_intermediatevariable() call\n");

					if (Instruction *PtrIns = dyn_cast<Instruction>(it->first)) {
						if (DT && !DT->dominates(PtrIns, exitBB->getTerminator())) {
							LLVM_DEBUG(dbgs() << "Skipping non-dominating intermediate live-out value: "
							                  << ptr->getName() << "\n");
							continue;
						}
					}
					Value* bitcastedptr = scalarTraceValue(builder, it->first);
					builder.CreateCall(live_out_report_intvar_FN,{ptr_name_val,bitcastedptr});
				}
			}
		}

	}

	for (dfgNode *node : NodeList) {
		Instruction *ins = node->getNode();
		if (!ins)
			continue;

		Value *pointer = nullptr;
		Type *accessType = nullptr;
		bool isStore = false;
		if (LoadInst *LDI = dyn_cast<LoadInst>(ins)) {
			pointer = LDI->getPointerOperand();
			accessType = LDI->getType();
			isStore = false;
		} else if (StoreInst *STI = dyn_cast<StoreInst>(ins)) {
			pointer = STI->getPointerOperand();
			accessType = STI->getValueOperand()->getType();
			isStore = true;
		} else {
			continue;
		}

		Value *rootBase = getGlobalBase(pointer);
		if (!rootBase || !rootBase->getType()->isPointerTy())
			continue;

		auto baseIt = spm_base_address.find(rootBase);
		if (baseIt == spm_base_address.end())
			continue;

			IRBuilder<> builder(ins);
			if (isStore) {
				Instruction *next = ins->getNextNode();
				if (next) {
					builder.SetInsertPoint(next);
				}
			}
			DataLayout DL = F.getParent()->getDataLayout();
		uint32_t accessSize = (uint32_t)DL.getTypeAllocSize(accessType);
		std::string baseName = node->getArrBasePtr();
		if (baseName.empty() || baseName == "NOT_A_MEM_OP")
			baseName = traceBaseArrayName(rootBase);

		Value *loopNameVal = builder.CreateGlobalStringPtr(this->kernelname);
		Value *nodeIdxVal = ConstantInt::get(Type::getInt32Ty(Ctx), node->getIdx());
		Value *baseNameVal = builder.CreateGlobalStringPtr(baseName);
		Value *baseAddrVal = builder.CreateGlobalStringPtr(std::to_string(baseIt->second));
		Value *actualPtr = builder.CreateBitCast(pointer, morpher::llvm_compat::getInt8PtrTy(Ctx));
		Value *arrayBase = builder.CreateBitCast(rootBase, morpher::llvm_compat::getInt8PtrTy(Ctx));
		Value *sizeVal = ConstantInt::get(Type::getInt32Ty(Ctx), accessSize);
		Value *isStoreVal = ConstantInt::get(Type::getInt8Ty(Ctx), isStore ? 1 : 0);
		builder.CreateCall(mem_access_report_FN,
				{loopNameVal, nodeIdxVal, baseNameVal, baseAddrVal,
				 actualPtr, arrayBase, sizeVal, isStoreVal});
	}

	//Insertion of loop end function to all exits
	for(auto trans : loopexitBB){
		BasicBlock* exitBB = trans.second;
		IRBuilder<> builder(exitBB->getTerminator());

		Value *loopName = builder.CreateGlobalStringPtr(kernelname);
		builder.CreateCall(loopEndFn,{loopName});
	}

}
//#undef  DEBUG_TYPE
//#define DEBUG_TYPE LV_NAME

void DFG::UpdateSPMAllocation(std::unordered_map<Value *, int> &spm_base_address,
		std::unordered_map<Value *, SPM_BANK> &spm_base_allocation,
		std::unordered_map<Value *, GetElementPtrInst *> &arr_ptrs)
{
	std::ofstream mem_alloc_txt;
	mem_alloc_txt.open(this->kernelname + "_mem_alloc.txt");
	mem_alloc_txt << "var_name,base_addr\n";
	std::string var_name;
	int base_addr;
	std::unordered_map<std::string, int> base_address_map;
	for (dfgNode *node : NodeList)
	{
		if (OutLoopNodeMapReverse.find(node) != OutLoopNodeMapReverse.end() && OutLoopNodeMapReverse[node])
		{
			assert(spm_base_address.find(OutLoopNodeMapReverse[node]) != spm_base_address.end());
			LLVM_DEBUG(dbgs() << "SetOutLoopLOAD/STORE BaseAddresses :: setting outerloop value=" << OutLoopNodeMapReverse[node]->getName() << " to " << spm_base_address[OutLoopNodeMapReverse[node]] << "\n");
			node->setConstantVal(spm_base_address[OutLoopNodeMapReverse[node]]);
			node->setoutloopAddr(spm_base_address[OutLoopNodeMapReverse[node]]);//this is assigned to constval in Namenodes()
//			LLVM_DEBUG(dbgs() << "Const val: " << node->getConstantVal() << "/n";
			assert(spm_base_allocation.find(OutLoopNodeMapReverse[node]) != spm_base_allocation.end());
			if (spm_base_allocation[OutLoopNodeMapReverse[node]] == BANK0)
			{
				node->setLeftAlignedMemOp(1);
			}
			else
			{
				node->setLeftAlignedMemOp(2);
			}
			LLVM_DEBUG(dbgs() << "\t pointer " << OutLoopNodeMapReverse[node]->getName() << "\n");
			LLVM_DEBUG(dbgs() << "\t to address " << spm_base_address[OutLoopNodeMapReverse[node]] << "\n");
			// Use getArrBasePtr() to get the (possibly synthetic) name for unnamed values.
			var_name = node->getArrBasePtr();
			base_addr = spm_base_address[OutLoopNodeMapReverse[node]];
			base_address_map[var_name]=base_addr;
		}
		else if (node->getNode())
		{
			if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(node->getNode()))
			{
				Value *gep_pointer = GEP->getPointerOperand();
				if (spm_base_address.find(gep_pointer) != spm_base_address.end())
				{
					node->setGEPbaseAddr(spm_base_address[gep_pointer]);
					LLVM_DEBUG(dbgs() << "SetNewGEPBaseAddresses :: setting GEP base address for=");
					LLVM_DEBUG(GEP->dump());
					LLVM_DEBUG(dbgs() << "\t pointer " << gep_pointer->getName() << "\n");
					LLVM_DEBUG(dbgs() << "\t to address " << spm_base_address[gep_pointer] << "\n");
					// For double-pointer arrays, gep_pointer may be unnamed.
					// Trace to the root global variable name (e.g., %row -> @A -> "A").
					var_name = traceBaseArrayName(gep_pointer);
					base_addr = spm_base_address[gep_pointer];
					if (!var_name.empty())
						base_address_map[var_name]=base_addr;
					//					mem_alloc_txt << var_name<<","<< base_addr<<"\n";
				}
			}
				else if (LoadInst *LDI = dyn_cast<LoadInst>(node->getNode()))
				{
					Value *ptr = LDI->getPointerOperand();
					LLVM_DEBUG(ptr->dump());

					Value *base_ptr = nullptr;
					if (arr_ptrs.find(ptr) != arr_ptrs.end())
						base_ptr = arr_ptrs[ptr]->getPointerOperand();
					else
						base_ptr = getGlobalBase(ptr);

					if (!base_ptr) {
						// Pointer-type load (e.g., volatile int **A pattern): no SPM slot needed.
						if (LDI->getType()->isPointerTy())
							continue;
						assert(false && "could not resolve base pointer for data load in UpdateSPMAllocation");
					}

					assert(spm_base_allocation.find(base_ptr) != spm_base_allocation.end());

					if (spm_base_allocation[base_ptr] == BANK0)
					{
						node->setLeftAlignedMemOp(1);
					}
				else
				{
						node->setLeftAlignedMemOp(2);
					}

					LLVM_DEBUG(dbgs() << "\t pointer  " << base_ptr->getName() << "\n");
					LLVM_DEBUG(dbgs() << "\t to address " << spm_base_address[base_ptr] << "\n");
					var_name = traceBaseArrayName(base_ptr);
					base_addr = spm_base_address[base_ptr];
					if (!var_name.empty())
						base_address_map[var_name]=base_addr;
				}
				else if (StoreInst *STI = dyn_cast<StoreInst>(node->getNode()))
				{
					Value *ptr = STI->getPointerOperand();
					LLVM_DEBUG(ptr->dump());
					Value *base_ptr = nullptr;
					if (arr_ptrs.find(ptr) != arr_ptrs.end())
						base_ptr = arr_ptrs[ptr]->getPointerOperand();
					else
						base_ptr = getGlobalBase(ptr);
					assert(base_ptr != nullptr);
					assert(spm_base_allocation.find(base_ptr) != spm_base_allocation.end());

					if (spm_base_allocation[base_ptr] == BANK0)
					{
						node->setLeftAlignedMemOp(1);
					}
				else
				{
						node->setLeftAlignedMemOp(2);
					}

					LLVM_DEBUG(dbgs() << "\t pointer " << base_ptr->getName() << "\n");
					LLVM_DEBUG(dbgs() << "\t to address " << spm_base_address[base_ptr] << "\n");
					var_name = traceBaseArrayName(base_ptr);
					base_addr = spm_base_address[base_ptr];
					if (!var_name.empty())
						base_address_map[var_name]=base_addr;
				}
		}
	}

	LLVM_DEBUG(dbgs() << "\n\nName nodes begin\n");
	nameNodes();
	LLVM_DEBUG(dbgs() << "\nName nodes end\n\n");

	LLVM_DEBUG(dbgs() << "\n\n Writing mem alloc begin \n");

	for(auto i = base_address_map.begin(); i != base_address_map.end(); i++){
		LLVM_DEBUG(dbgs() << "\t pointer " << i->first<<","<< i->second<<"\n");
		mem_alloc_txt << i->first<<","<< i->second<<"\n";
	}
	if (!iteratorExternalized || loopexitKept)
	{
		mem_alloc_txt << "loopend" <<","<<(MEM_SIZE/2 - 1)<<"\n";
	}
	if (!iteratorExternalized || loopstartEventKept)
	{
		mem_alloc_txt << "loopstart" <<","<<(MEM_SIZE - 2)<<"\n";
	}
	mem_alloc_txt.close();
	LLVM_DEBUG(dbgs() << "\n Writing mem alloc end\n\n");
	//std::cout << "Data layout generated ("<<this->kernelname<<"_mem_alloc.txt) \n";
	// assert(false);
}

int DFG::insertshiftGEPsCorrect(){

	std::unordered_set<dfgNode *> newNodes;

	for(dfgNode* node : NodeList){
		if(node->getNode()){
			if(GetElementPtrInst* GEP = dyn_cast<GetElementPtrInst>(node->getNode())){
				DataLayout DL = GEP->getParent()->getParent()->getParent()->getDataLayout();

				/* Build a per-operand stride map by walking the GEP type chain.
				 *
				 * For  gep [8x[8xi32]]* @B, 0, %i, %k :
				 *   op[1] = 0  (constant, pointer-level dereference, skip)
				 *   op[2] = %i  → stride = sizeof([8xi32]) = 32  → LS(5)
				 *   op[3] = %k  → stride = sizeof(i32)    =  4  → LS(2)
				 *
				 * For OLOAD ancestors the LS is intentionally omitted: the
				 * instrumentation stores  spm_base + outer_idx * outer_stride
				 * directly into the trace, so the OLOAD value already carries
				 * the correct byte address contribution for the outer dimension.
				 * The inner-loop ancestor still gets the appropriate LS.
				 */
				std::map<Value*, int> operandStrideMap;
				Type *currType = GEP->getSourceElementType();
				for (unsigned opIdx = 1; opIdx < GEP->getNumOperands(); opIdx++) {
					Value *opVal = GEP->getOperand(opIdx);
					int stride;
					if (opIdx == 1) {
						/* First index: pointer-level dereference.
						 * stride = sizeof(entire source element type). */
						stride = (int)DL.getTypeAllocSize(currType);
						/* currType does NOT advance here — subsequent indices
						 * step into the source element type itself. */
					} else {
						if (ArrayType *AT = dyn_cast<ArrayType>(currType)) {
							stride = (int)DL.getTypeAllocSize(AT->getElementType());
							currType = AT->getElementType();
						} else {
							stride = (int)DL.getTypeAllocSize(currType);
						}
					}
					if (!isa<ConstantInt>(opVal))
						operandStrideMap[opVal] = stride;
				}

				/* Fallback single stride (old behavior) for non-array GEPs or
				 * ancestors that cannot be matched to a specific operand. */
				int array_size = DL.getTypeAllocSize(GEP->getSourceElementType());
				Type *T = GEP->getSourceElementType();
				int no_of_elements = isa<ArrayType>(GEP->getSourceElementType()) ?
						(int)cast<ArrayType>(T)->getNumElements() : 1;
				int fallback_size = array_size / no_of_elements;

				LLVM_DEBUG(dbgs() << "total array size=" << array_size
						<< " fallback element size(bytes)=" << fallback_size << "\n");
				LLVM_DEBUG(GEP->dump());

				bool hasOLOADancestor = false;
				for(dfgNode* anc : node->getAncestors()){
					// ignore predicates
					if(anc->childConditionalMap[node] != UNCOND) continue;

					/* OLOAD ancestors (getNode()==NULL, nameType=="OutLoopLOAD")
					 * represent outer-loop values. Their trace entry is precomputed
					 * by the instrumentation as  spm_base + idx * outer_stride, so
					 * they already carry the correct byte contribution.
					 * Adding a scale node on top would over-scale them.
					 * Note: GEPLEFTSHIFT/GEPSCALE nodes also have getNode()==NULL, so we
					 * must check nameType to distinguish them from real OLOADs. */
					if (anc->getNode() == nullptr &&
					    anc->getNameType() == "OutLoopLOAD") {
						hasOLOADancestor = true;
						LLVM_DEBUG(dbgs() << "Skipping LS for OLOAD ancestor of GEP\n");
						continue;
					}

					/* Determine the per-dimension stride for this ancestor. */
					Value *ancVal = anc->getNode();
					int stride = fallback_size;
					if (ancVal) {
						auto it = operandStrideMap.find(ancVal);
						if (it != operandStrideMap.end())
							stride = it->second;
					}

					if (stride > 1) {
						dfgNode *temp = new dfgNode(this);
						temp->setIdx(NodeList.size() + newNodes.size());
						newNodes.insert(temp);
						configureGEPScaleNode(temp, stride);
						temp->BB = node->BB;
						if (isPositivePowerOfTwo(stride))
							LLVM_DEBUG(dbgs() << "adding LS node (shift=" << Log2_32(stride) << ")...\n");
						else
							LLVM_DEBUG(dbgs() << "adding MUL scale node (const=" << stride << ")...\n");

						anc->addChildNode(temp);
						temp->addAncestorNode(anc);

						temp->addChildNode(node);
						node->addAncestorNode(temp);

						anc->removeChild(node);
						node->removeAncestor(anc);
					}
				}
				/* When an OLOAD ancestor is present, the OLOAD value already
				 * carries (array_spm_base + outer_idx * outer_stride).
				 * The ADD node must use two register inputs (OLOAD + LS),
				 * so constValid must be false -- clear the GEPbaseAddr constant. */
				if (hasOLOADancestor && node->hasConstantVal()) {
					LLVM_DEBUG(dbgs() << "Clearing GEPbaseAddr for GEP node with OLOAD ancestor\n");
					node->removeConstantVal();
				}
			}
		}
	}

	for (dfgNode *n : newNodes)
	{
		NodeList.push_back(n);
	}

	return 0;
}
/* clearGEPbaseAddrForOLOADnodes:
 * After GEPBaseAddrCheck/nameNodes have set GEPbaseAddr (constVal) on all GEP
 * nodes, clear it for GEP nodes that have an OLOAD ancestor.  When an OLOAD
 * ancestor is present its trace value already encodes  spm_base + idx*stride,
 * so the ADD must use two register inputs (OLOAD + LS) with constValid=false.
 * Keeping GEPbaseAddr would cause the mapper to emit constValid=true, which
 * silently ignores the LS input and produces wrong addresses. */
void DFG::clearGEPbaseAddrForOLOADnodes() {
	for (dfgNode *node : NodeList) {
		if (!node->getNode()) continue;
		if (!dyn_cast<GetElementPtrInst>(node->getNode())) continue;
		if (!node->hasConstantVal()) continue;
		for (dfgNode *anc : node->getAncestors()) {
			if (anc->childConditionalMap[node] != UNCOND) continue;
			if (anc->getNode() == nullptr &&
			    anc->getNameType() == "OutLoopLOAD") {
				/* OLOAD ancestor found -- clear constVal so constValid=false.
				 * Note: GEPLEFTSHIFT/GEPSCALE nodes also have getNode()==NULL; they must
				 * NOT trigger removal here or 1-D GEP base addresses would be lost. */
				LLVM_DEBUG(dbgs() << "clearGEPbaseAddrForOLOADnodes: removing constVal from GEP node\n");
				node->removeConstantVal();
				break;
			}
		}
	}
}
