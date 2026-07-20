#include "DfgGenInternal.h"

void ValidateSupportedArrayDimensions(Function &F)
{
	for (BasicBlock &BB : F)
	{
		for (Instruction &I : BB)
		{
			GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(&I);
			if (!GEP)
				continue;
			if (isa<StructType>(GEP->getSourceElementType()))
				continue;

			unsigned effectiveArrayDims = getEffectiveArrayDims(GEP);
			unsigned pointerDepth = morpher::llvm_compat::getLogicalPointerDepthForGEP(GEP);
			if (effectiveArrayDims <= 2 && pointerDepth <= 2)
				continue;

			std::string displayName = GEP->getPointerOperand()->getName().str();
			if (displayName.empty())
				displayName = GEP->getName().str();
			if (displayName.empty())
				displayName = "<unnamed>";

			std::string details = "pointer '" + displayName + "' in function '" + F.getName().str() +
				"' is not supported (effective_dims=" + std::to_string(effectiveArrayDims) +
				", pointer_depth=" + std::to_string(pointerDepth) +
				"); Morpher currently supports only up to 2D arrays";
			DFG_CHECK(false, "UnsupportedArrayDimensions", details);
		}
	}
}

static void collectInnerLoops(Loop *L, SmallVectorImpl<Loop *> &result) {
	if (L->getSubLoops().empty())
		result.push_back(L);
	else
		for (Loop *sub : L->getSubLoops())
			collectInnerLoops(sub, result);
}

/// Hoist loop-invariant double-pointer row dereferences to the loop preheader.
///
/// Targets non-volatile pointer loads of the form:
///
///   (loop body)
///     %base    = load i32**, <global>           -- may be an in-loop load
///     %row_gep = gep i32*, i32** %base, <idx>  -- idx is loop-invariant (NOT IV)
///     %row     = load i32*, i32** %row_gep      -- same value every iteration!
///
/// Because the row index is loop-invariant, the row pointer %row is identical
/// on every iteration.  We move the load to the preheader, eliminating the
/// in-loop memory load without introducing any loop-carried PHI.
///
/// Validity condition (type-based alias check): no store in the loop writes a
/// pointer-typed value, so no store can modify the i32** row-pointer array.
/// In tensor/linear-algebra kernels all in-loop stores write scalars -- the
/// check always passes and hoisting is always safe.
///
/// Two sub-cases handled:
///   arr[fixed_row][IV_col]    -- hoist row_load; body: gep(row, IV) stride-1
///   arr[fixed_row][fixed_col] -- hoist row_load + elem_gep; body: pure OLOAD
///
/// Runs BEFORE FlattenDoublePointers (which handles arr[IV_row][fixed_col]).
void HoistInvariantRowPointers(Function &F, LoopInfo &LI) {
	SmallVector<Loop *, 8> innerLoops;
	for (Loop *L : LI)
		collectInnerLoops(L, innerLoops);

	for (Loop *L : innerLoops) {
		BasicBlock *preheader = L->getLoopPreheader();
		if (!preheader) continue;

		// Find the loop IV so we can skip arr[IV][*] patterns -- those are
		// FlattenDoublePointers' responsibility.
		PHINode *IV = nullptr;
		for (auto &I : *L->getHeader()) {
			PHINode *phi = dyn_cast<PHINode>(&I);
			if (!phi) break;
			if (!phi->getType()->isIntegerTy()) continue;
			if (phi->getNumIncomingValues() != 2) continue;
			unsigned inside = 0;
			for (unsigned i = 0; i < phi->getNumIncomingValues(); i++)
				if (L->contains(phi->getIncomingBlock(i))) inside++;
			if (inside == 1) { IV = phi; break; }
		}

		// ------------------------------------------------------------------
		// Safety check: if any store in the loop writes a pointer-typed value
		// we conservatively skip.  A store of i32* or i32** could potentially
		// update the row-pointer array, invalidating the hoist.
		// In all tensor/linear-algebra code, in-loop stores write scalars
		// (e.g. store volatile i32), so this check always passes there.
		// ------------------------------------------------------------------
		bool hasPtrStore = false;
		for (BasicBlock *BB : L->getBlocks()) {
			for (Instruction &I : *BB)
				if (auto *SI = dyn_cast<StoreInst>(&I))
					if (SI->getValueOperand()->getType()->isPointerTy())
						{ hasPtrStore = true; break; }
			if (hasPtrStore) break;
		}
		if (hasPtrStore) continue;

		// ------------------------------------------------------------------
		// Collect hoist candidates: non-volatile pointer loads whose GEP has
		// a single loop-invariant index that is NOT the IV.
		// ------------------------------------------------------------------
		struct HoistPattern {
			LoadInst          *row_load;
			GetElementPtrInst *row_gep;
			Value             *gep_base;
		};
		SmallVector<HoistPattern, 4> patterns;

		for (BasicBlock *BB : L->getBlocks()) {
			for (Instruction &I : *BB) {
				auto *row_load = dyn_cast<LoadInst>(&I);
				if (!row_load || row_load->isVolatile()) continue;
				if (!row_load->getType()->isPointerTy()) continue;

				auto *row_gep = dyn_cast<GetElementPtrInst>(
					row_load->getPointerOperand());
				if (!row_gep || row_gep->getNumIndices() != 1) continue;

				Value *row_idx = *row_gep->idx_begin();
				// arr[IV][*]: FlattenDoublePointers handles this -- skip.
				if (IV && row_idx == IV) continue;
				if (!L->isLoopInvariant(row_idx)) continue;

				// Base must be a pointer-to-pointer (i32**).
				Value *gep_base = row_gep->getPointerOperand();
				if (!row_gep->getSourceElementType()->isPointerTy()) continue;

				patterns.push_back({row_load, row_gep, gep_base});
			}
		}
		if (patterns.empty()) continue;

		IRBuilder<> preBuilder(preheader->getTerminator());

		for (auto &pat : patterns) {
			// ----------------------------------------------------------------
			// Preheader: recreate the base load if LICM left it in-loop,
			// then hoist the row GEP and row pointer load.
			// ----------------------------------------------------------------
			Value *preheader_base = nullptr;
			if (auto *inLoopLI = dyn_cast<LoadInst>(pat.gep_base)) {
				if (L->contains(inLoopLI->getParent()))
					preheader_base = morpher::llvm_compat::createTypedLoad(
						preBuilder, inLoopLI->getType(), inLoopLI->getPointerOperand(), "hoist_base");
			}
			if (!preheader_base) {
				if (auto *baseInst = dyn_cast<Instruction>(pat.gep_base))
					if (L->contains(baseInst->getParent())) continue;
				preheader_base = pat.gep_base;
			}

			Value *row_idx     = *pat.row_gep->idx_begin();
			Value *hoist_addr  = morpher::llvm_compat::createTypedGEP(
				preBuilder, pat.row_gep->getSourceElementType(), preheader_base, row_idx, "hoist_row_addr");
			Value *hoisted_row = morpher::llvm_compat::createTypedLoad(
				preBuilder, pat.row_load->getType(), hoist_addr, "hoist_row");

			// Replace every in-loop use of row_load with the hoisted pointer.
			pat.row_load->replaceAllUsesWith(hoisted_row);
			pat.row_load->eraseFromParent();
			if (pat.row_gep->use_empty())
				pat.row_gep->eraseFromParent();
			if (auto *baseInst = dyn_cast<Instruction>(pat.gep_base))
				if (baseInst->use_empty())
					baseInst->eraseFromParent();

			// ----------------------------------------------------------------
			// Second-level hoist: any elem GEP that uses hoisted_row and has
			// all loop-invariant indices can itself be moved to the preheader.
			// This covers arr[fixed_row][fixed_col] -- the element pointer is
			// constant across the entire loop, becoming a pure OLOAD in the DFG.
			// ----------------------------------------------------------------
			SmallVector<GetElementPtrInst *, 4> elemGEPs;
			for (User *U : hoisted_row->users()) {
				auto *gep = dyn_cast<GetElementPtrInst>(U);
				if (!gep || !L->contains(gep->getParent())) continue;
				bool allInvariant = true;
				for (auto it = gep->idx_begin(); it != gep->idx_end(); ++it)
					if (!L->isLoopInvariant(*it)) { allInvariant = false; break; }
				if (allInvariant)
					elemGEPs.push_back(gep);
			}
			for (auto *elemGEP : elemGEPs) {
				SmallVector<Value *, 2> idxs(
					elemGEP->idx_begin(), elemGEP->idx_end());
				Value *hoisted_elem = morpher::llvm_compat::createTypedGEP(
					preBuilder, elemGEP->getSourceElementType(), hoisted_row, idxs, "hoist_elem_addr");
				elemGEP->replaceAllUsesWith(hoisted_elem);
				elemGEP->eraseFromParent();
			}
		}
	}
}

/// Fold volatile int** double-pointer 2D array accesses into an element-pointer
/// induction variable, eliminating both the in-loop row-pointer load and the
/// in-loop element GEP node.
///
/// For each innermost loop, we detect the pattern where a row-pointer is
/// loaded inside the loop body using the loop IV as the GEP index:
///
///   (loop body)
///     %base     = load i32**, <global>           -- may be in-loop load
///     %row_gep  = gep i32*, i32** %base, <IV>   -- indexed by loop IV
///     %row      = load i32*, i32** %row_gep      -- in-loop memory load!
///     %elem_gep = gep i32,   i32*  %row, <col>  -- in-loop element GEP!
///
/// And replace with an element-pointer induction variable (no MUL, no in-loop GEP):
///
///   (preheader)
///     %base_pre   = load i32**, <global>
///     %row_init   = load i32*, gep(%base_pre, IV_start)  -- B[k_start]
///     %elem_init  = gep i32, i32* %row_init, <col>       -- B[k_start][j]
///   (header phi)
///     %B_elem = phi [%elem_init, preheader], [%B_elem_next, latch]
///   (loop body)
///     -- B_elem used directly as the element pointer (load B_elem)
///   (latch, before branch)
///     %B_elem_next = gep i32, i32* %B_elem, <N>          -- advance to B[k+1][j]
///
/// By folding the column index into the initial pointer in the preheader,
/// the in-loop element GEP (LS node on CGRA) is eliminated entirely.
/// This matches the DFG node count of an equivalent flat int* access pattern.
void FlattenDoublePointers(Function &F, LoopInfo &LI) {
	SmallVector<Loop *, 8> innerLoops;
	for (Loop *L : LI)
		collectInnerLoops(L, innerLoops);

	for (Loop *L : innerLoops) {
		BasicBlock *preheader = L->getLoopPreheader();
		BasicBlock *latch     = L->getLoopLatch();
		if (!preheader || !latch) continue;

		// ------------------------------------------------------------------
		// 1. Find the loop induction variable: the integer PHI in the header
		//    with one incoming value from outside the loop.
		// ------------------------------------------------------------------
		PHINode *IV        = nullptr;
		Value   *IV_start  = nullptr;   // initial value of IV (from preheader)
		for (auto &I : *L->getHeader()) {
			PHINode *phi = dyn_cast<PHINode>(&I);
			if (!phi) break;
			if (!phi->getType()->isIntegerTy()) continue;
			if (phi->getNumIncomingValues() != 2) continue;
			unsigned inside = 0;
			for (unsigned i = 0; i < phi->getNumIncomingValues(); i++)
				if (L->contains(phi->getIncomingBlock(i))) inside++;
			if (inside == 1) {
				IV = phi;
				// Identify which incoming value is from outside the loop
				for (unsigned i = 0; i < phi->getNumIncomingValues(); i++)
					if (!L->contains(phi->getIncomingBlock(i)))
						IV_start = phi->getIncomingValue(i);
				break;
			}
			}
			if (!IV || !IV_start) continue;

			// Require a canonical +1 induction update inside the loop. If not,
			// skip flattening to avoid deriving an incorrect row stride advance.
			Value *IV_next = nullptr;
			for (unsigned i = 0; i < IV->getNumIncomingValues(); i++)
				if (L->contains(IV->getIncomingBlock(i)))
					IV_next = IV->getIncomingValue(i);
			if (!IV_next) continue;
			auto *stepI = dyn_cast<BinaryOperator>(IV_next);
			if (!stepI || stepI->getOpcode() != Instruction::Add) continue;
			ConstantInt *stepC = nullptr;
			if (stepI->getOperand(0) == IV)
				stepC = dyn_cast<ConstantInt>(stepI->getOperand(1));
			else if (stepI->getOperand(1) == IV)
				stepC = dyn_cast<ConstantInt>(stepI->getOperand(0));
			if (!stepC || stepC->getSExtValue() != 1) continue;

			// ------------------------------------------------------------------
			// 2. Find N_val: the row stride (number of i32 elements per row).
			//    For polybench: the loop runs k in [k_start, n-1), so n equals
		//    the loop upper bound.  If bound = add(n, -1) then N_val = n.
		// ------------------------------------------------------------------
			Value *N_val = nullptr;
			{
				auto *br = dyn_cast<BranchInst>(latch->getTerminator());
				if (!br || !br->isConditional()) continue;
				auto *cmp = dyn_cast<ICmpInst>(br->getCondition());
				if (!cmp) continue;
				ICmpInst::Predicate pred = cmp->getPredicate();
				bool predOK = pred == ICmpInst::ICMP_SLT || pred == ICmpInst::ICMP_SLE ||
						pred == ICmpInst::ICMP_ULT || pred == ICmpInst::ICMP_ULE ||
						pred == ICmpInst::ICMP_NE;
				if (!predOK) continue;

				auto isIVLike = [&](Value *V) {
					return V == IV || V == IV_next;
				};

				Value *lhs = cmp->getOperand(0);
				Value *rhs = cmp->getOperand(1);
				Value *boundOp = nullptr;
				if (isIVLike(lhs) && L->isLoopInvariant(rhs))
					boundOp = rhs;
				else if (isIVLike(rhs) && L->isLoopInvariant(lhs))
					boundOp = lhs;
				else
					continue;

				// Check if bound = add(n, -1) -> N_val = n
				if (auto *addI = dyn_cast<BinaryOperator>(boundOp)) {
					if (addI->getOpcode() == Instruction::Add) {
						for (unsigned j = 0; j < 2; j++) {
							auto *C = dyn_cast<ConstantInt>(addI->getOperand(j));
							if (C && C->getSExtValue() == -1) {
								N_val = addI->getOperand(1 - j);
								break;
							}
						}
					}
				}
				if (!N_val) N_val = boundOp;
			}
			if (!N_val) continue;

		// ------------------------------------------------------------------
		// 3. Scan loop blocks for the row-pointer load pattern:
		//      row_load = load i32*, gep(base_ptr, IV)
		//    with exactly one downstream user that is a GEP (elem_gep).
		// ------------------------------------------------------------------
		struct Pattern {
			GetElementPtrInst *row_gep;
			LoadInst          *row_load;
			Value             *gep_base;  // i32** base pointer (may be in-loop)
			GetElementPtrInst *elem_gep;  // gep i32, i32* row_load, col
		};
		SmallVector<Pattern, 4> patterns;

		for (BasicBlock *BB : L->getBlocks()) {
			for (Instruction &I : *BB) {
				auto *row_load = dyn_cast<LoadInst>(&I);
				if (!row_load || row_load->isVolatile()) continue;
				if (!row_load->getType()->isPointerTy()) continue;

				auto *row_gep =
					dyn_cast<GetElementPtrInst>(row_load->getPointerOperand());
				if (!row_gep || row_gep->getNumIndices() != 1) continue;
				if (*row_gep->idx_begin() != IV) continue;

				// GEP base must be pointer-to-pointer (volatile int**)
				Value *gep_base = row_gep->getPointerOperand();
					if (!row_gep->getSourceElementType()->isPointerTy()) continue;

				// row_load must have exactly one user that is a GEP
				if (!row_load->hasOneUse()) continue;
				auto *elem_gep =
					dyn_cast<GetElementPtrInst>(*row_load->user_begin());
				if (!elem_gep || elem_gep->getNumIndices() != 1) continue;

				patterns.push_back({row_gep, row_load, gep_base, elem_gep});
			}
		}
		if (patterns.empty()) continue;

		LLVMContext    &Ctx        = F.getContext();
		Type           *i32Ty      = Type::getInt32Ty(Ctx);
		IRBuilder<>     preBuilder(preheader->getTerminator());
		IRBuilder<>     latchBuilder(latch->getTerminator());

		for (auto &pat : patterns) {
			// ----------------------------------------------------------------
			// 4a. col_idx must be loop-invariant to fold into the preheader.
			//     Skip this pattern if the column index is loop-variant.
			// ----------------------------------------------------------------
			Value *col_idx = *pat.elem_gep->idx_begin();
			if (!L->isLoopInvariant(col_idx)) continue;

			// ----------------------------------------------------------------
			// 4b. Preheader: load B[k_start] then fold col_idx into it to get
			//     the element pointer for the first iteration B[k_start][j].
			//     If gep_base (the i32** pointer) is an in-loop load (because
			//     LICM did not hoist it), re-create that load in the preheader.
			// ----------------------------------------------------------------
			Value *preheader_base = nullptr;
			if (auto *inLoopLI = dyn_cast<LoadInst>(pat.gep_base)) {
				if (L->contains(inLoopLI->getParent()))
					preheader_base = morpher::llvm_compat::createTypedLoad(
						preBuilder, inLoopLI->getType(), inLoopLI->getPointerOperand(), "row_outer_ptr");
			}
			if (!preheader_base) {
				if (auto *baseInst = dyn_cast<Instruction>(pat.gep_base))
					if (L->contains(baseInst->getParent())) continue;
				preheader_base = pat.gep_base;
			}

			// row_init = B[k_start]: row pointer for the first iteration
			Value *Bkinit_addr = morpher::llvm_compat::createTypedGEP(
				preBuilder, pat.row_gep->getSourceElementType(), preheader_base, IV_start, "row_init_ptr");
			Value *row_init = morpher::llvm_compat::createTypedLoad(
				preBuilder, pat.row_load->getType(), Bkinit_addr, "row_init");

			// B_elem_init = B[k_start][j]: fold col_idx into the initial ptr.
			// This eliminates the in-loop element GEP node entirely.
			Value *B_elem_init = morpher::llvm_compat::createTypedGEP(
				preBuilder, pat.elem_gep->getSourceElementType(), row_init, col_idx, "elem_init_ptr");

			// ----------------------------------------------------------------
			// 4c. Create an element-pointer PHI in the loop header.
			//     B_elem tracks B[k][j] directly: no row pointer, no in-loop GEP.
			//     It is initialized to B[k_start][j] and advanced by N elements
			//     per iteration (one full row stride) via a GEP in the latch.
			// ----------------------------------------------------------------
			PHINode *B_elem = PHINode::Create(
				B_elem_init->getType(), 2, "B_elem",
				L->getHeader()->getFirstNonPHI());
			B_elem->addIncoming(B_elem_init, preheader);

			// Latch: B_elem_next = gep i32, i32* B_elem, N_val
			// (advances to the same column in the next row: B[k+1][j])
			Value *N_cast = N_val;
			if (N_val->getType() != i32Ty)
				N_cast = latchBuilder.CreateSExtOrTrunc(N_val, i32Ty, "N_cast");
			Value *B_elem_next = morpher::llvm_compat::createTypedGEP(
				latchBuilder, pat.elem_gep->getSourceElementType(), B_elem, N_cast, "B_elem_next");
			B_elem->addIncoming(B_elem_next, latch);

			// ----------------------------------------------------------------
			// 4d. Replace elem_gep with B_elem directly -- no new in-loop GEP.
			//     The volatile load of B[k][j] becomes simply: load B_elem.
			// ----------------------------------------------------------------
			pat.elem_gep->replaceAllUsesWith(B_elem);
			pat.elem_gep->eraseFromParent();

			// row_load (now unused) and its GEP can be erased
			pat.row_load->eraseFromParent();
			if (pat.row_gep->use_empty())
				pat.row_gep->eraseFromParent();

			// Erase the in-loop base load if it became dead
			if (auto *baseInst = dyn_cast<Instruction>(pat.gep_base))
				if (baseInst->use_empty())
					baseInst->eraseFromParent();
		}
	}
}


void ParseSizeAttr(Function &F, std::map<std::string, int> *sizeArrMap)
{
	auto global_annos = F.getParent()->getNamedGlobal("llvm.global.annotations");
	if (global_annos)
	{
		auto a = cast<ConstantArray>(global_annos->getOperand(0));
		for (int i = 0; i < a->getNumOperands(); i++)
		{
			auto e = cast<ConstantStruct>(a->getOperand(i));

			auto *annotatedValue = e->getOperand(0)->stripPointerCasts();
			if (auto fn = dyn_cast<Function>(annotatedValue))
			{
				auto *annotationGlobal = dyn_cast<GlobalVariable>(e->getOperand(1)->stripPointerCasts());
				if (!annotationGlobal)
					continue;
				auto *annotationData = cast<ConstantDataArray>(annotationGlobal->getOperand(0));
				auto anno = annotationData->getAsCString();
				fn->addFnAttr("size", anno); // <-- add function annotation here
			}
		}
	}

	if (F.hasFnAttribute("size"))
	{
		Attribute attr = F.getFnAttribute("size");
		LLVM_DEBUG(dbgs() << "Size attribute : " << attr.getValueAsString() << "\n");
		StringRef sizeAttrStr = attr.getValueAsString();
		SmallVector<StringRef, 8> sizeArr;
		sizeAttrStr.split(sizeArr, ',');

		for (int i = 0; i < sizeArr.size(); ++i)
		{
			std::pair<StringRef, StringRef> splitDuple = sizeArr[i].split(':');
			uint32_t size;
			splitDuple.second.getAsInteger(10, size);
			LLVM_DEBUG(dbgs() << "ParseAttr:: name:" << splitDuple.first << ",size:" << size << "\n");
			(*sizeArrMap)[splitDuple.first.str()] = size;
		}
	}

	if (F.hasFnAttribute("size"))
	{
		LLVM_DEBUG(dbgs() << F.getName() << " has my attribute!\n");
	}
}

void RemoveSelectLeafs(Function &F)
{
	for (BasicBlock &BB : F)
	{
		for (Instruction &I : BB)
		{
			Instruction *ins = &I;
			if (SelectInst *SLI = dyn_cast<SelectInst>(ins))
			{
				IRBuilder<> builder(SLI);
				if (Instruction *Cond = dyn_cast<Instruction>(SLI->getCondition()))
				{
					if (Cond->getParent() == SLI->getParent())
						continue;
					LLVM_DEBUG(dbgs() << "Found Alone Select=");
					LLVM_DEBUG(SLI->dump());
					LLVM_DEBUG(dbgs() << "Condition=");
					LLVM_DEBUG(Cond->dump());
					builder.SetInsertPoint(BB.getFirstNonPHI());
					Value *OR0 = builder.CreateXor(Cond, (uint64_t)0);
					assert(OR0 != Cond);
					Instruction *ORIns = cast<Instruction>(OR0);
					LLVM_DEBUG(dbgs() << "OR = ");
					LLVM_DEBUG(ORIns->dump());

					SLI->setOperand(0, OR0);
					LLVM_DEBUG(BB.dump());
				}
			}
		}
	}
}

bool ExpandConstantFunnelShifts(Function &F)
{
	bool changed = false;
	SmallVector<IntrinsicInst *, 8> funnelShifts;
	for (BasicBlock &BB : F)
		for (Instruction &I : BB)
			if (auto *II = dyn_cast<IntrinsicInst>(&I))
				if (II->getIntrinsicID() == Intrinsic::fshl ||
				    II->getIntrinsicID() == Intrinsic::fshr)
					funnelShifts.push_back(II);

	for (IntrinsicInst *II : funnelShifts)
	{
		auto *intTy = dyn_cast<IntegerType>(II->getType());
		auto *shiftAmount = dyn_cast<ConstantInt>(II->getArgOperand(2));
		if (!intTy || !shiftAmount)
			continue;

		unsigned bitWidth = intTy->getBitWidth();
		uint64_t amount = shiftAmount->getZExtValue() % bitWidth;
		Value *replacement = nullptr;
		IRBuilder<> builder(II);
		if (amount == 0)
		{
			replacement = II->getIntrinsicID() == Intrinsic::fshl
				? II->getArgOperand(0)
				: II->getArgOperand(1);
		}
		else if (II->getIntrinsicID() == Intrinsic::fshl)
		{
			Value *left = builder.CreateShl(II->getArgOperand(0), amount);
			Value *right = builder.CreateLShr(II->getArgOperand(1), bitWidth - amount);
			replacement = builder.CreateOr(left, right);
		}
		else
		{
			Value *left = builder.CreateLShr(II->getArgOperand(0), amount);
			Value *right = builder.CreateShl(II->getArgOperand(1), bitWidth - amount);
			replacement = builder.CreateOr(left, right);
		}

		II->replaceAllUsesWith(replacement);
		II->eraseFromParent();
		changed = true;
	}
	return changed;
}

bool ExpandIntegerMinMaxIntrinsics(Function &F)
{
	bool changed = false;
	SmallVector<IntrinsicInst *, 8> minMaxes;
	for (BasicBlock &BB : F)
		for (Instruction &I : BB)
			if (auto *II = dyn_cast<IntrinsicInst>(&I))
				if (II->getIntrinsicID() == Intrinsic::smax ||
				    II->getIntrinsicID() == Intrinsic::smin ||
				    II->getIntrinsicID() == Intrinsic::umax ||
				    II->getIntrinsicID() == Intrinsic::umin)
					minMaxes.push_back(II);

	for (IntrinsicInst *II : minMaxes)
	{
		IRBuilder<> builder(II);
		Value *lhs = II->getArgOperand(0);
		Value *rhs = II->getArgOperand(1);
		CmpInst::Predicate pred;
		switch (II->getIntrinsicID())
		{
		case Intrinsic::smax:
			pred = CmpInst::ICMP_SGT;
			break;
		case Intrinsic::smin:
			pred = CmpInst::ICMP_SLT;
			break;
		case Intrinsic::umax:
			pred = CmpInst::ICMP_UGT;
			break;
		case Intrinsic::umin:
			pred = CmpInst::ICMP_ULT;
			break;
		default:
			continue;
		}
		Value *cmp = builder.CreateICmp(pred, lhs, rhs);
		Value *replacement = builder.CreateSelect(cmp, lhs, rhs);
		II->replaceAllUsesWith(replacement);
		II->eraseFromParent();
		changed = true;
	}
	return changed;
}

void InsertORtoSingularConditionalBB(Function &F)
{
	for (BasicBlock &BB : F)
	{
		for (Instruction &I : BB)
		{
			Instruction *ins = &I;
			if (BranchInst *BRI = dyn_cast<BranchInst>(ins))
			{
				if (!BRI->isConditional())
					continue;
				IRBuilder<> builder(BRI);
				if (Instruction *Cond = dyn_cast<Instruction>(BRI->getCondition()))
				{
					if (Cond->getParent() == BRI->getParent())
						continue;
					//Coming here if the condition is from a another basicblock;
					LLVM_DEBUG(dbgs() << "Found Alone Branch=");
					LLVM_DEBUG(BRI->dump());
					LLVM_DEBUG(dbgs() << "Pointer=");
					LLVM_DEBUG(Cond->dump());
					builder.SetInsertPoint(BB.getFirstNonPHI());
					Value *OR0 = builder.CreateXor(Cond, (uint64_t)0);
					assert(OR0 != Cond);
					Instruction *ORIns = cast<Instruction>(OR0);
					LLVM_DEBUG(dbgs() << "OR = ");
					LLVM_DEBUG(ORIns->dump());

					BRI->setOperand(0, OR0);
					LLVM_DEBUG(BB.dump());
				}
			}
		}
	}
	//	    		assert(false);
}

void ReplaceCMPs(Function &F)
{
	dfgNode *node;
	std::vector<Instruction *> instructions;

	for (auto &BB : F)
	{
		for (auto &I : BB)
		{
			instructions.push_back(&I);
		}
	}

	for (auto I : instructions)
	{
		if (CmpInst *CI = dyn_cast_or_null<CmpInst>(I))
		{
			LLVM_DEBUG(I->dump());

			IRBuilder<> builder(CI);
			assert(CI->getNumOperands() == 2);
			switch (CI->getPredicate())
			{
			case CmpInst::ICMP_EQ:
				//TODO : DAC18
			case CmpInst::FCMP_OEQ:
			case CmpInst::FCMP_UEQ:

				break;
			case CmpInst::ICMP_NE:
				//TODO : DAC18
			{
				CmpInst *cmpEqNew = cast<CmpInst>(builder.CreateICmpEQ(CI->getOperand(0), CI->getOperand(1)));
				Instruction *notIns = cast<Instruction>(builder.CreateNot(cmpEqNew));
				BasicBlock::iterator ii(CI);
				notIns->removeFromParent();
				ReplaceInstWithInst(CI->getParent(), ii, notIns);
				break;
			}
			case CmpInst::FCMP_ONE:
			case CmpInst::FCMP_UNE:
			{
				CmpInst *cmpEqNew = cast<CmpInst>(builder.CreateFCmpOEQ(CI->getOperand(0), CI->getOperand(1)));
				Instruction *notIns = cast<Instruction>(builder.CreateNot(cmpEqNew));
				BasicBlock::iterator ii(CI);
				notIns->removeFromParent();
				ReplaceInstWithInst(CI->getParent(), ii, notIns);
				break;
			}

			case CmpInst::ICMP_SGE:
			case CmpInst::ICMP_UGE:
				//TODO : DAC18
			case CmpInst::FCMP_OGE:
			case CmpInst::FCMP_UGE:
			{
				CmpInst *cmpEqNew;
				if (CI->getPredicate() == CmpInst::ICMP_SGE)
				{
					cmpEqNew = cast<CmpInst>(builder.CreateICmpSLT(CI->getOperand(1), CI->getOperand(0)));
				}
				else
				{ // ICMP_UGE
					cmpEqNew = cast<CmpInst>(builder.CreateICmpULT(CI->getOperand(1), CI->getOperand(0)));
				}
				Instruction *notIns = cast<Instruction>(builder.CreateNot(cmpEqNew));
				BasicBlock::iterator ii(CI);
				notIns->removeFromParent();
				ReplaceInstWithInst(CI->getParent(), ii, notIns);
				break;
			}
			case CmpInst::ICMP_SGT:
			case CmpInst::ICMP_UGT:
				//TODO : DAC18
			case CmpInst::FCMP_OGT:
			case CmpInst::FCMP_UGT:

				break;
			case CmpInst::ICMP_SLT:
			case CmpInst::ICMP_ULT:
				//TODO : DAC18
			case CmpInst::FCMP_OLT:
			case CmpInst::FCMP_ULT:

				break;
			case CmpInst::ICMP_SLE:
			case CmpInst::ICMP_ULE:
				//TODO : DAC18
			case CmpInst::FCMP_OLE:
			case CmpInst::FCMP_ULE:
			{
				CmpInst *cmpEqNew;
				if (CI->getPredicate() == CmpInst::ICMP_SLE)
				{
					cmpEqNew = cast<CmpInst>(builder.CreateICmpSGT(CI->getOperand(1), CI->getOperand(0)));
				}
				else
				{ // ICMP_ULE
					cmpEqNew = cast<CmpInst>(builder.CreateICmpUGT(CI->getOperand(1), CI->getOperand(0)));
				}
				Instruction *notIns = cast<Instruction>(builder.CreateNot(cmpEqNew));
				BasicBlock::iterator ii(CI);
				notIns->removeFromParent();
				ReplaceInstWithInst(CI->getParent(), ii, notIns);
				break;
			}
			break;
			default:
				DFG_CHECK(false, "ReplaceCMPs",
					"unsupported comparison predicate (predicate code " +
					std::to_string(CI->getPredicate()) + ")");
				break;
			}
		}
	} //iter thru BB
}
